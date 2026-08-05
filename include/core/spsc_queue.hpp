#pragma once

// Single-Producer Single-Consumer lock-free ring buffer.
//
// Design rationale:
//   The minimum synchronization this problem requires is *two* atomic
//   variables — one producer-owned (write_pos) and one consumer-owned
//   (read_pos).  No mutex, no CAS loop, no ABA problem.
//
//   Memory ordering:
//     • Producer: store element, then release-store write_pos.
//       The release guarantees the payload write is visible before the
//       index update.
//     • Consumer: acquire-load write_pos, then load element.
//       The acquire pairs with the producer's release — correct
//       happens-before without any extra fence.
//     • read_pos is only written by the consumer and only read by the
//       producer (to check fullness).  A relaxed-store on the consumer
//       side would technically suffice because the producer only needs
//       to observe progress, not synchronise any payload.  We use
//       release-store anyway for symmetry and to give the producer a
//       happens-before edge on anything the consumer wrote before
//       advancing the cursor — a negligible cost, no extra fence on x86.
//
//   False sharing: head and tail live on separate cache lines.

#include <atomic>
#include <array>
#include <optional>
#include <cstdint>
#include <cassert>

namespace engine {

template<typename T, std::size_t Capacity>
    requires (Capacity > 1 && (Capacity & (Capacity - 1)) == 0)  // power-of-two
class SPSCQueue {
public:
    static constexpr std::size_t kCapacity = Capacity;
    static constexpr std::size_t kMask     = Capacity - 1;

    SPSCQueue() noexcept : write_pos_(0), read_pos_(0) {}

    // Producer side — called from exactly one thread.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t wp = write_pos_.load(std::memory_order_relaxed);
        const std::size_t next = (wp + 1) & kMask;

        // Check full: we need the consumer's read_pos.
        // acquire so we see the latest consumer progress.
        if (next == read_pos_.load(std::memory_order_acquire))
            return false;   // full

        buffer_[wp] = item;

        // Release: makes the payload write visible before the index update.
        write_pos_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side — called from exactly one thread.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t rp = read_pos_.load(std::memory_order_relaxed);

        // Acquire: pairs with producer's release-store.
        if (rp == write_pos_.load(std::memory_order_acquire))
            return false;   // empty

        out = buffer_[rp];

        // Release: lets producer see updated read cursor.
        read_pos_.store((rp + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return read_pos_.load(std::memory_order_acquire) ==
               write_pos_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t w = write_pos_.load(std::memory_order_relaxed);
        const std::size_t r = read_pos_.load(std::memory_order_relaxed);
        return (w - r) & kMask;
    }

private:
    // Hot data: buffer accessed by both threads but on different indices.
    std::array<T, Capacity> buffer_;

    // Separate cache lines to eliminate false sharing between producer and consumer.
    alignas(64) std::atomic<std::size_t> write_pos_;
    alignas(64) std::atomic<std::size_t> read_pos_;
};

} // namespace engine
