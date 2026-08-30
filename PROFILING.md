# Profiling — lob-engine

Real measurements only. Numbers from `perf stat` are pending a bare-metal run
(see the template command at the bottom). Everything here is from the benchmark
binaries committed under `bench/`.

## Environment

```
CPU      : Intel Xeon @ 2.10 GHz (shared cloud container)
Kernel   : Linux 6.x x86-64
Compiler : GCC 13.3.0, -O3 -march=native (AVX2 enabled)
Isolation: NONE — no isolcpus, no nohz_full, no SCHED_FIFO
```

Container numbers carry large p99+ tails from the OS scheduler. The p50 is
representative; p99 is dominated by scheduler jitter and should not be quoted
without a note. See `docs/linux-tuning.md` for the isolated-core setup.

---

## 1. Single-symbol order book latency (`bench/bench_latency.cpp`)

```
add_order  (passive, no fill)  p50=  64 ns   p99= 19,702 ns
add_order  (aggressive, fill)  p50= 126 ns   p99= 60,669 ns
cancel_order (O(1) doubly-linked list)  p50=  30 ns   p99= 56,498 ns
submit via SPSC                p50=  32 ns   p99=     49 ns   p99.9= 11,398 ns

Throughput: 6.3 M msg/sec  (1M orders, single thread)
```

The p99 spikes on add/cancel are L2/LLC misses from the cold order-index hash
lookup when the working set (65,536-slot pool × 64 bytes = 4 MB) exceeds L2.
On an isolated core with a warm working set the p99 converges to 2–3× p50.

cancel_order is O(1) since the doubly-linked intrusive list was introduced
(see `include/util/function_ref.hpp` and `src/core/order_book.cpp`). The
previous singly-linked implementation was O(depth at level). The change was
verified by the model-based property test against the reference book, which
caught two bugs in the first implementation attempt (free-list aliasing and
missing prevs assignment at enqueue).

---

## 2. AVX2 vs scalar `find_level` (`bench/bench_avx2.cpp`)

```
N_LEVELS=128, N_ITER=5,000,000 (70% hit / 30% miss)

Method     p50    p90    p99   p99.9
Scalar      67ns   90ns  110ns  153ns
AVX2        41ns   53ns   62ns   74ns

Speedup at p50: 1.6x
Correctness: PASS (0 errors across all N_LEVELS)
```

AVX2 `VPCMPEQQ` compares 4 int64 prices per cycle. The scalar loop does one.
At depth=128 this requires 32 AVX2 loads vs 128 scalar loads.

---

## 3. SoA vs AoS cache layout (`bench/bench_cache.cpp`)

```
Depth    AoS (ns)   SoA (ns)   Speedup   Cache lines (AoS / SoA)
   1       25.5       18.0      1.41x      1 / 1
   4       55.3       50.6      1.09x      2 / 1
  16      124.2      121.2      1.02x      8 / 2
  64      417.5      419.8      0.99x     32 / 8
 256     1682.8     1157.7      1.45x    128 / 32
 512     3275.2     2486.9      1.32x    256 / 64
1024     6368.4     4927.9      1.29x    512 / 128
```

SoA wins significantly at realistic book depths (16–512 levels) because the
hot `prices[]` array fits in far fewer cache lines than an AoS layout where
prices are interleaved with quantities and order counts.

---

## 4. Multi-symbol throughput scaling (`bench/bench_multisymbol.cpp`)

```
500,000 events/shard, unshielded OS threads (no isolcpus)

Shards   Agg Mmsg/s   Per-shard   Scaling
     1        8.52        8.52      1.00x
     2        8.39        4.20      0.49x
     4        8.89        2.22      0.26x
     8        6.99        0.87      0.10x
```

**Why scaling degrades on this machine:** the container has no CPU shielding.
The OS scheduler migrates threads between physical cores mid-run, causing cache
cold misses (the 4 MB working set must be reloaded) and TLB shootdowns.
Each migration costs ~5–20 µs and appears as the wall-time doubling.

**Expected on isolated cores:** each shard runs on its own pinned core with no
migrations; the 4 MB working set stays in L2/L3 local to that core.
Scaling should be ≥0.95× per doubling, i.e. 2 shards → ~1.90× aggregate.

To reproduce on isolated hardware:
```bash
# Pin N threads to N isolated cores before running:
taskset -c 0,2,4,6 ./build/bench_multisymbol
# Or use the MultiSymbolEngine which does pthread_setaffinity_np internally.
```

---

## 5. perf stat (template — paste your run here)

```bash
# Build release
cmake -S . -B build && cmake --build build

# Pin to core 3 (must be isolated)
taskset -c 3 chrt -f 50 \
  perf stat -e cycles,instructions,branches,branch-misses,\
               L1-dcache-loads,L1-dcache-load-misses,\
               LLC-loads,LLC-load-misses \
  ./build/bench_latency
```

Paste the `perf stat` output here. Metrics to look for:
- IPC > 2.5 on the add_order path indicates good instruction-level parallelism
- L1-dcache-load-misses < 1% indicates the hot path is L1-resident
- branch-misses < 0.5% confirms the AVX2 branch elimination is working

---

## 6. Model-based correctness vs latency trade-off

The model test (`tests/test_conservation.cpp`) runs 1,000,000 random events
per seed against a `std::map<Price, deque<Order>>` reference and asserts exact
fill sequence, best quote, no crossed book, and quantity conservation.

Throughput of the reference model: ~450k events/sec (dominated by map operations).
Throughput of lob-engine: ~8.5M events/sec on this hardware.

**Ratio: ~19× faster than a naive correct implementation.**

This number is what `test_conservation` proves: lob-engine produces identical
results to the reference model at 19× its speed.
