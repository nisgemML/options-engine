#pragma once

// MPMCQueue — bounded, lock-free, multiple-producer multiple-consumer queue.
//
// Algorithm: Dmitry Vyukov's "bounded MPMC queue"
//   http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
//
// Design rationale vs the SPSC queue:
//
//   The SPSC queue uses two atomic counters and release/acquire pairs —
//   it's the minimum possible synchronization for one producer, one consumer.
//
//   MPMC requires per-slot sequence numbers to safely coordinate N producers
//   and M consumers.  Each slot carries a `sequence` counter that tracks
//   whether the slot is:
//     (a) available for a producer to write into, or
//     (b) available for a consumer to read from.
//
//   The CAS in try_push:
//     "I claim this slot if its sequence == my position"
//   The CAS in try_pop:
//     "I claim this slot if its sequence == my position + 1"
//
//   This means each slot transitions through a well-defined sequence:
//     EMPTY (seq=pos) → WRITTEN (seq=pos+1) → EMPTY (seq=pos+Capacity)
//   and the CAS ensures only one producer/consumer "wins" each transition.
//
// Memory ordering:
//   - sequence_.load(acquire) in push: must see the consumer's release-store
//     that marked the slot as empty after the previous pop.
//   - sequence_.store(release) after writing payload: makes the payload
//     visible to any consumer that acquire-loads sequence_ next.
//   - Same reasoning applies symmetrically to pop.
//
// Performance vs SPSC:
//   Each operation does one CAS on a per-slot atomic (not a shared counter),
//   so producers compete only when they happen to claim the same slot.
//   At low to moderate contention, this is nearly as fast as SPSC.
//   At high contention, the CAS retry loop is the bottleneck.
//
// When to use MPMC vs SPSC:
//   - SPSC: exactly one producer thread and one consumer thread. Always prefer
//     this when the threading model allows it — zero CAS, minimum barriers.
//   - MPMC: multiple feed parsers writing to one matching engine input queue,
//     or one matching engine writing to multiple downstream consumers.

#include <atomic>
#include <array>
#include <cstddef>
#include <cassert>

namespace engine {

template<typename T, std::size_t Capacity>
    requires (Capacity > 1 && (Capacity & (Capacity - 1)) == 0)
class MPMCQueue {
public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask     = Capacity - 1;

    MPMCQueue() noexcept {
        // Initialize each slot's sequence to its index.
        // A slot at index i is initially "ready for producer to claim at position i".
        for (std::size_t i = 0; i < Capacity; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);

        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    // Producer: try to enqueue one item.
    // Returns false if the queue is full.
    // May be called from multiple threads simultaneously.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        Slot* slot;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            slot = &slots_[pos & kMask];
            const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq)
                                      - static_cast<std::ptrdiff_t>(pos);

            if (diff == 0) {
                // Slot is empty and matches our position.
                // Race to claim it with a CAS on enqueue_pos_.
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    break;  // We claimed the slot.
                }
                // Another producer won — retry with the updated pos.
            } else if (diff < 0) {
                // Queue is full (slot has not been consumed yet).
                return false;
            } else {
                // Another producer is ahead — reload enqueue_pos_ and retry.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        slot->data = item;

        // Release: makes payload visible to consumers.
        slot->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Consumer: try to dequeue one item.
    // Returns false if the queue is empty.
    // May be called from multiple threads simultaneously.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        Slot* slot;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            slot = &slots_[pos & kMask];
            const std::size_t seq = slot->sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq)
                                      - static_cast<std::ptrdiff_t>(pos + 1);

            if (diff == 0) {
                // Slot has been written and matches our position.
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    break;
                }
            } else if (diff < 0) {
                // Queue is empty.
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        out = slot->data;

        // Release: marks slot as available for producers at position pos+Capacity.
        slot->sequence.store(pos + kMask + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        const std::size_t ep = enqueue_pos_.load(std::memory_order_relaxed);
        const std::size_t dp = dequeue_pos_.load(std::memory_order_relaxed);
        return ep == dp;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t ep = enqueue_pos_.load(std::memory_order_relaxed);
        const std::size_t dp = dequeue_pos_.load(std::memory_order_relaxed);
        return ep > dp ? ep - dp : 0;
    }

private:
    struct Slot {
        std::atomic<std::size_t> sequence;
        T data;
        // Pad to cache line to prevent false sharing between adjacent slots.
        // This is critical for MPMC (multiple threads touching adjacent slots)
        // but wasteful for SPSC (only one thread per side).
        char _pad[64 - sizeof(std::atomic<std::size_t>) - sizeof(T) <= 0
                  ? 1 : 64 - sizeof(std::atomic<std::size_t>) - sizeof(T)];
    };

    // Pad the position counters to separate cache lines.
    // enqueue_pos_ is written by producers; dequeue_pos_ by consumers.
    // Without padding, both live on the same cache line → false sharing
    // between producer and consumer threads → unnecessary MESI invalidations.
    alignas(64) std::atomic<std::size_t> enqueue_pos_;
    alignas(64) std::atomic<std::size_t> dequeue_pos_;
    alignas(64) std::array<Slot, Capacity> slots_;
};

} // namespace engine
