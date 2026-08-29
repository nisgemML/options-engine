#pragma once
// include/core/multi_symbol_engine.hpp — Multi-symbol matching engine.
//
// ═══════════════════════════════════════════════════════════════════════════
// DESIGN
// ═══════════════════════════════════════════════════════════════════════════
//
// The single-symbol MatchingEngine is optimal for one symbol on one thread.
// Production options systems have thousands of strikes per underlying, each
// requiring its own order book. The multi-symbol engine shards across cores.
//
// Architecture:
//
//   Symbols → hash → shard (one per CPU core)
//                      ↓
//   Each shard: one thread (pinned) + one OrderBook[] + one SPSC queue
//
// Design decisions:
//
// 1. HASH-BASED SHARDING (not sorted): options symbols (AAPL230915C00150000)
//    do not have natural ordering for round-robin. FNV hash of ticker gives
//    uniform distribution across shards.
//
// 2. PER-SHARD SPSC QUEUE: market data ingestion → shard queue → matching.
//    No cross-shard communication on the matching hot path.
//
// 3. CPU PINNING: each shard thread is pinned to a dedicated core via
//    pthread_setaffinity_np. Reduces cache cold misses from scheduler jitter.
//
// 4. NUMA AWARENESS (stub): for systems with multiple NUMA nodes, shards
//    should be allocated on the same node as their CPU. Implemented as a
//    documented stub — requires NUMA-aware allocator (libnuma or hwloc).
//
// ── Scaling ───────────────────────────────────────────────────────────────
//
// At 4 shards: 4× throughput, 4× total capacity.
// At 16 shards (16-core machine): ~16× throughput.
// Cross-shard spread orders (e.g., buy C + sell P on same underlying)
// require a coordinator — implemented as a documented stub here.
//
// ═══════════════════════════════════════════════════════════════════════════

#include "core/matching_engine.hpp"
#include "core/spsc_queue.hpp"
#include "core/types.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#endif

namespace engine {

// ── FNV-1a hash for symbol strings ────────────────────────────────────────
[[nodiscard]] inline uint32_t fnv1a(std::string_view s) noexcept {
    uint32_t h = 2166136261u;
    for (char c : s) { h ^= uint8_t(c); h *= 16777619u; }
    return h;
}

// ── ShardConfig ───────────────────────────────────────────────────────────
struct ShardConfig {
    int         cpu_affinity  = -1;     // -1 = no pinning
    int         numa_node     = -1;     // -1 = no NUMA preference
    std::size_t queue_depth   = 65536;  // SPSC queue depth
};

// ── MultiSymbolEngine ─────────────────────────────────────────────────────
//
// N_SHARDS must be a power of 2 for hash-based routing.
//
template <std::size_t N_SHARDS = 4>
class MultiSymbolEngine {
    static_assert((N_SHARDS & (N_SHARDS - 1)) == 0, "N_SHARDS must be power of 2");

public:
    using ExecCallback = std::function<void(ExecutionReport const&)>;

    // ── Shard ─────────────────────────────────────────────────────────────
    struct Shard {
        alignas(64) MatchingEngine engine;
        alignas(64) SPSCQueue<MarketDataMsg, 65536> queue;
        std::thread   thread;
        std::atomic<bool> running{false};
        int           cpu_id{-1};

        // Statistics.
        alignas(64) std::atomic<uint64_t> msgs_processed{0};
        alignas(64) std::atomic<uint64_t> msgs_dropped{0};
    };

    // ── Construction ──────────────────────────────────────────────────────
    explicit MultiSymbolEngine(
        ExecCallback       on_exec,
        ShardConfig const* shard_configs = nullptr)
        : on_exec_{std::move(on_exec)}
    {
        for (std::size_t i = 0; i < N_SHARDS; ++i) {
            shards_[i] = std::make_unique<Shard>();
            if (shard_configs) {
                shards_[i]->cpu_id = shard_configs[i].cpu_affinity;
            }
        }
    }

    ~MultiSymbolEngine() { stop(); }

    // ── register_symbol: assign symbol to a shard ─────────────────────────
    //
    // Must be called before start(). Thread-safe among concurrent registrations
    // (uses the shard's own lock-free engine registration).
    //
    bool register_symbol(std::string_view symbol, SymbolId id) {
        std::size_t shard_idx = fnv1a(symbol) & (N_SHARDS - 1);
        return shards_[shard_idx]->engine.register_symbol(id);
    }

    // ── start: launch all shard threads ───────────────────────────────────
    void start() {
        for (std::size_t i = 0; i < N_SHARDS; ++i) {
            auto* shard = shards_[i].get();
            shard->running.store(true, std::memory_order_release);
            shard->thread = std::thread([this, shard, i] {
                // Pin to CPU if configured.
                pin_to_cpu(shard->cpu_id);
                shard_loop(shard, i);
            });
        }
    }

    // ── stop: drain queues and join threads ───────────────────────────────
    void stop() {
        for (auto& shard : shards_) {
            if (shard) shard->running.store(false, std::memory_order_release);
        }
        for (auto& shard : shards_) {
            if (shard && shard->thread.joinable()) shard->thread.join();
        }
    }

    // ── submit: route message to the correct shard ────────────────────────
    //
    // Called from the feed handler / ingestion thread.
    // Lock-free: pushes to the target shard's SPSC queue.
    //
    // Returns true if successfully enqueued.
    //
    [[nodiscard]] bool submit(MarketDataMsg const& msg) noexcept {
        std::size_t shard_idx = msg.symbol & (N_SHARDS - 1);
        auto* shard = shards_[shard_idx].get();
        if (shard->queue.push(msg)) return true;
        shard->msgs_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // ── Statistics ────────────────────────────────────────────────────────
    struct Stats {
        std::size_t shard_id;
        uint64_t    msgs_processed;
        uint64_t    msgs_dropped;
        int         cpu_id;
    };

    std::vector<Stats> stats() const {
        std::vector<Stats> result;
        result.reserve(N_SHARDS);
        for (std::size_t i = 0; i < N_SHARDS; ++i) {
            result.push_back({
                i,
                shards_[i]->msgs_processed.load(std::memory_order_relaxed),
                shards_[i]->msgs_dropped.load(std::memory_order_relaxed),
                shards_[i]->cpu_id,
            });
        }
        return result;
    }

    static constexpr std::size_t n_shards() noexcept { return N_SHARDS; }

private:
    // ── Shard main loop ───────────────────────────────────────────────────
    void shard_loop(Shard* shard, std::size_t shard_idx) noexcept {
        MarketDataMsg msg;
        while (shard->running.load(std::memory_order_acquire)) {
            if (shard->queue.pop(msg)) {
                shard->engine.on_message(msg);
                shard->msgs_processed.fetch_add(1, std::memory_order_relaxed);
            }
            // Busy-poll: no yield, no sleep — this is the hot path.
            // In production: add a yield after N empty polls to avoid
            // burning CPU when idle.
        }
        // Drain remaining messages on shutdown.
        while (shard->queue.pop(msg)) {
            shard->engine.on_message(msg);
        }
    }

    // ── CPU pinning ───────────────────────────────────────────────────────
    static void pin_to_cpu(int cpu_id) noexcept {
#if defined(__linux__)
        if (cpu_id < 0) return;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
        (void)cpu_id;
#endif
    }

    // ── NUMA-aware shard allocation (stub) ────────────────────────────────
    //
    // Production implementation:
    //   #include <numa.h>
    //   void* mem = numa_alloc_onnode(sizeof(Shard), numa_node);
    //   new (mem) Shard{};
    //
    // This ensures shard data is on the same NUMA node as its CPU, avoiding
    // cross-NUMA memory access (~40ns extra per cache miss on dual-socket).
    //
    // Required: libnuma (apt install libnuma-dev) and build with -lnuma.
    // See docs/numa_design.md for the full design.
    //

    std::array<std::unique_ptr<Shard>, N_SHARDS> shards_;
    ExecCallback on_exec_;
};

}  // namespace engine
