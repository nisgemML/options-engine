// tests/test_histogram.cpp — correctness tests for LatencyHistogram.

#include "util/histogram.hpp"
#include <cstdio>
#include <cassert>
#include <cmath>
#include <vector>
#include <thread>

using namespace engine;

static int passed = 0, failed = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); ++failed; } \
        else { ++passed; } \
    } while(0)

#define CHECK_NEAR(a, b, tol, msg) CHECK(std::abs((double)(a)-(double)(b)) <= (tol), msg)

static void test_bucket_index() {
    // Sub-microsecond: direct index.
    CHECK(LatencyHistogram::bucket_index(0)   == 0,   "0 ns → bucket 0");
    CHECK(LatencyHistogram::bucket_index(500) == 500, "500 ns → bucket 500");
    CHECK(LatencyHistogram::bucket_index(999) == 999, "999 ns → bucket 999");

    // 1–10 µs: 100 ns resolution.
    int b1000 = LatencyHistogram::bucket_index(1000);
    int b1999 = LatencyHistogram::bucket_index(1999);
    CHECK(b1000 < b1999, "1000 ns < 1999 ns (different buckets)");
    CHECK(b1000 == LatencyHistogram::bucket_index(1050), "1000 and 1050 same bucket");

    // Overflow.
    int ov1 = LatencyHistogram::bucket_index(1'000'000);
    int ov2 = LatencyHistogram::bucket_index(99'000'000);
    CHECK(ov1 == ov2, "All ≥ 1ms in overflow bucket");
    CHECK(ov1 == LatencyHistogram::kTotalBuckets - 1, "Overflow is last bucket");
}

static void test_percentiles_uniform() {
    LatencyHistogram h;
    // Record 1000 samples: 0, 1, 2, ..., 999 ns.
    for (int i = 0; i < 1000; ++i) h.record(static_cast<uint64_t>(i));

    auto s = h.take_snapshot();
    CHECK(s.total == 1000, "Total count correct");

    // p50 ≈ 500 ns (within one bucket).
    CHECK_NEAR(s.percentile(0.50), 500, 5, "p50 ≈ 500 ns");
    // p99 ≈ 990 ns.
    CHECK_NEAR(s.percentile(0.99), 990, 5, "p99 ≈ 990 ns");
    // Mean ≈ 499.5 ns.
    CHECK_NEAR(s.mean_ns(), 499.5, 2.0, "Mean ≈ 499.5 ns");
    // Max = 999.
    CHECK(s.max_ns == 999, "Max = 999 ns");
}

static void test_percentiles_bimodal() {
    LatencyHistogram h;
    // 90% at ~100 ns, 10% at ~10 µs.
    for (int i = 0; i < 900; ++i) h.record(100);
    for (int i = 0; i < 100; ++i) h.record(10'000);

    auto s = h.take_snapshot();
    CHECK(s.total == 1000, "Total 1000");
    // p50 and p90 should both be in the 100 ns bucket.
    CHECK(s.percentile(0.50) <= 200, "p50 in fast bucket");
    CHECK(s.percentile(0.90) <= 10000, "p90 at boundary of fast/slow buckets");
    // p99 should be in the 10 µs range.
    CHECK(s.percentile(0.99) >= 5000, "p99 in slow bucket");
}

static void test_reset() {
    LatencyHistogram h;
    for (int i = 0; i < 100; ++i) h.record(500);
    CHECK(h.count() == 100, "Count before reset");
    h.reset();
    CHECK(h.count() == 0, "Count after reset");
    auto s = h.take_snapshot();
    CHECK(s.total == 0, "Snapshot total after reset");
    CHECK(s.max_ns == 0, "Max after reset");
}

static void test_concurrent_records() {
    LatencyHistogram h;
    static constexpr int kThreads = 8;
    static constexpr int kPerThread = 100'000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i)
                h.record(static_cast<uint64_t>((t * 100 + i) % 5000));
        });
    }
    for (auto& t : threads) t.join();

    CHECK(h.count() == kThreads * kPerThread, "Concurrent records counted correctly");
}

int main() {
    printf("=== LatencyHistogram Tests ===\n\n");

    test_bucket_index();
    test_percentiles_uniform();
    test_percentiles_bimodal();
    test_reset();

    printf("Running concurrent record stress test (%d threads × %d samples)...\n",
           8, 100'000);
    test_concurrent_records();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
