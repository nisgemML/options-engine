#pragma once

// LatencyHistogram — lock-free, allocation-free HDR-style histogram.
//
// Motivation:
//   In production, you need continuous latency visibility without the act of
//   measuring itself adding latency.  std::map, mutex-protected buckets, and
//   anything that calls malloc on the measurement path are all disqualified.
//
// Design:
//   Fixed-size array of atomic counters, one per bucket.
//   Buckets use a two-level scheme:
//     - Sub-microsecond resolution below 1 µs  (1 ns buckets, 0–999 ns)
//     - 100 ns resolution from 1–10 µs
//     - 1 µs resolution from 10–100 µs
//     - 10 µs resolution from 100 µs–1 ms
//     - Overflow bucket for everything ≥ 1 ms
//
//   Recording is a single fetch_add on one atomic — ~3–5 ns.
//   Reading (for reporting) does a snapshot of all counters.
//
// Thread safety:
//   Any number of threads may call record() concurrently.
//   snapshot() and report() are safe to call from any thread, but
//   they read a non-atomic snapshot — counts may be slightly off during
//   a concurrent burst of records.  This is acceptable for monitoring.

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <numeric>

namespace engine {

class LatencyHistogram {
public:
    // ── Bucket layout ─────────────────────────────────────────────────────────
    //   [0,   1000)   ns — 1000 buckets of 1 ns   (index = ns)
    //   [1000, 10000) ns — 90 buckets of 100 ns   (index = 1000 + ns/100 - 10)
    //   [10000,100000)ns — 90 buckets of 1000 ns  (index = 1090 + ns/1000 - 10)
    //   [100000,1e6)  ns — 90 buckets of 10000 ns (index = 1180 + ns/10000 - 10)
    //   [1e6, ∞)      ns — 1 overflow bucket       (index = 1270)
    //
    // Total: 1271 buckets — fits in 10 KB of atomics.

    static constexpr int kSubMicroBuckets  = 1000;  // 0–999 ns, 1 ns each
    static constexpr int kLowMicroBuckets  =   90;  // 1–10 µs, 100 ns each
    static constexpr int kMidMicroBuckets  =   90;  // 10–100 µs, 1 µs each
    static constexpr int kHighMicroBuckets =   90;  // 100–1000 µs, 10 µs each
    static constexpr int kOverflowBucket   =    1;

    static constexpr int kTotalBuckets =
        kSubMicroBuckets + kLowMicroBuckets + kMidMicroBuckets +
        kHighMicroBuckets + kOverflowBucket;  // 1271

    // ── Recording ─────────────────────────────────────────────────────────────

    void record(uint64_t latency_ns) noexcept {
        const int idx = bucket_index(latency_ns);
        buckets_[idx].fetch_add(1, std::memory_order_relaxed);
        // Also track total and sum for mean computation.
        total_.fetch_add(1, std::memory_order_relaxed);
        sum_ns_.fetch_add(latency_ns, std::memory_order_relaxed);

        // Lock-free max update.
        uint64_t cur_max = max_ns_.load(std::memory_order_relaxed);
        while (latency_ns > cur_max &&
               !max_ns_.compare_exchange_weak(cur_max, latency_ns,
                   std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    // ── Snapshot ──────────────────────────────────────────────────────────────

    struct Snapshot {
        uint64_t counts[kTotalBuckets];
        uint64_t total;
        uint64_t sum_ns;
        uint64_t max_ns;

        // Compute a percentile from the snapshot.
        // p in [0.0, 1.0].
        [[nodiscard]] uint64_t percentile(double p) const noexcept {
            if (total == 0) return 0;
            const uint64_t target = static_cast<uint64_t>(p * total);
            uint64_t cumulative = 0;
            for (int i = 0; i < kTotalBuckets; ++i) {
                cumulative += counts[i];
                if (cumulative > target)
                    return bucket_lower_ns(i);
            }
            return max_ns;
        }

        [[nodiscard]] double mean_ns() const noexcept {
            return total > 0 ? static_cast<double>(sum_ns) / total : 0.0;
        }

        void print(const char* label = "Latency") const noexcept {
            printf("%-20s  n=%-9lu  mean=%6.0f ns  "
                   "p50=%5lu ns  p90=%5lu ns  p99=%6lu ns  "
                   "p99.9=%7lu ns  max=%7lu ns\n",
                   label, total, mean_ns(),
                   percentile(0.50),
                   percentile(0.90),
                   percentile(0.99),
                   percentile(0.999),
                   max_ns);
        }

        // Print a compact ASCII histogram for the first N non-zero buckets.
        void print_histogram(int max_bars = 40) const noexcept {
            uint64_t peak = *std::max_element(counts, counts + kTotalBuckets);
            if (peak == 0) return;

            printf("\n  Latency distribution (each bar = %lu samples):\n",
                   std::max(peak / max_bars, 1UL));

            for (int i = 0; i < kTotalBuckets; ++i) {
                if (counts[i] == 0) continue;
                int bars = static_cast<int>(counts[i] * max_bars / peak);
                uint64_t lo = bucket_lower_ns(i);
                uint64_t hi = bucket_upper_ns(i);

                if (lo < 1000)
                    printf("  %4lu–%4lu ns │", lo, hi);
                else if (lo < 10000)
                    printf("  %4.1f–%4.1f µs │", lo/1000.0, hi/1000.0);
                else
                    printf("  %4.0f–%4.0f µs │", lo/1000.0, hi/1000.0);

                for (int b = 0; b < bars; ++b) printf("█");
                printf(" %lu\n", counts[i]);
            }
            printf("\n");
        }
    };

    Snapshot take_snapshot() const noexcept {
        Snapshot s{};
        for (int i = 0; i < kTotalBuckets; ++i)
            s.counts[i] = buckets_[i].load(std::memory_order_relaxed);
        s.total  = total_.load(std::memory_order_relaxed);
        s.sum_ns = sum_ns_.load(std::memory_order_relaxed);
        s.max_ns = max_ns_.load(std::memory_order_relaxed);
        return s;
    }

    void reset() noexcept {
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
        total_.store(0, std::memory_order_relaxed);
        sum_ns_.store(0, std::memory_order_relaxed);
        max_ns_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t count() const noexcept {
        return total_.load(std::memory_order_relaxed);
    }

    // ── Bucket index computation ───────────────────────────────────────────────

    [[nodiscard]] static constexpr int bucket_index(uint64_t ns) noexcept {
        if (ns < 1000) {
            return static_cast<int>(ns);                        // 0–999 ns
        } else if (ns < 10000) {
            return kSubMicroBuckets + static_cast<int>(ns / 100) - 10;
        } else if (ns < 100000) {
            return kSubMicroBuckets + kLowMicroBuckets
                   + static_cast<int>(ns / 1000) - 10;
        } else if (ns < 1000000) {
            return kSubMicroBuckets + kLowMicroBuckets + kMidMicroBuckets
                   + static_cast<int>(ns / 10000) - 10;
        } else {
            return kTotalBuckets - 1;                           // overflow
        }
    }

    [[nodiscard]] static constexpr uint64_t bucket_lower_ns(int idx) noexcept {
        if (idx < kSubMicroBuckets) return static_cast<uint64_t>(idx);
        idx -= kSubMicroBuckets;
        if (idx < kLowMicroBuckets)
            return static_cast<uint64_t>(idx + 10) * 100;
        idx -= kLowMicroBuckets;
        if (idx < kMidMicroBuckets)
            return static_cast<uint64_t>(idx + 10) * 1000;
        idx -= kMidMicroBuckets;
        if (idx < kHighMicroBuckets)
            return static_cast<uint64_t>(idx + 10) * 10000;
        return 1000000;
    }

    [[nodiscard]] static constexpr uint64_t bucket_upper_ns(int idx) noexcept {
        return bucket_lower_ns(idx + 1) - 1;
    }

private:
    std::array<std::atomic<uint64_t>, kTotalBuckets> buckets_{};
    alignas(64) std::atomic<uint64_t> total_{0};
    alignas(64) std::atomic<uint64_t> sum_ns_{0};
    alignas(64) std::atomic<uint64_t> max_ns_{0};
};

} // namespace engine
