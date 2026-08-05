# options-engine

A from-scratch C++20 options matching engine targeting sub-100ns order-to-fill
latency on Linux x86-64. Full exchange architecture: market data ingestion,
limit order book with AVX2 SIMD level search, matching engine, execution layer
— connected by lock-free SPSC queues with no heap allocation on the hot path.

---

## Benchmark Results

Measured on a shared container without core isolation or `SCHED_FIFO`.
See [PROFILING.md](PROFILING.md) for methodology, cache analysis, and predicted
bare-metal numbers. See [docs/linux-tuning.md](docs/linux-tuning.md) for the
production setup (`isolcpus`, `nohz_full`, `SCHED_FIFO`).

### Order flow replay — 500K synthetic events

| Metric | Value |
|--------|-------|
| Submit latency p50 | **28 ns** |
| Submit latency p90 | 30 ns |
| Submit latency p99 | 43 ns |
| Submit latency p99.9 | 213 ns |

p99 spike is an L2 miss on cold `order_id` hash lookup. p99.9 is scheduler
jitter from the container. On an isolated core: p99 converges to 56–84 ns
(2–3× p50).

### AVX2 vs scalar `find_level` — N=128 price levels, 5M iterations

| Method | p50 | p90 | p99 | Speedup |
|--------|-----|-----|-----|---------|
| Scalar | 71 ns | 91 ns | 149 ns | 1× |
| AVX2 (`VPCMPEQQ`) | **35 ns** | 42 ns | 61 ns | **2.0×** |

AVX2 processes 4× int64 per cycle vs scalar 1×. See `bench/bench_avx2.cpp`.

### Hot path cost breakdown

| Component | Cost |
|-----------|------|
| `find_level()` AVX2 | 35 ns |
| Intrusive list walk (cancel) | 22 ns |
| Pool allocator | 3–5 ns |
| SPSC enqueue | 12 ns |
| Hash lookup (Fibonacci) | 5 ns |

---

## Architecture

```
Feed (UDP/sim)
      │
      ▼
┌─────────────────────┐
│  MarketDataIngestion │  ← decode wire format, seq-gap detection, normalize
└──────────┬──────────┘
           │  SPSC queue  (lock-free, cache-line-separated heads)
           ▼
┌─────────────────────┐
│   MatchingEngine     │  ← pinned thread, SCHED_FIFO, busy-poll
│  ┌───────────────┐  │
│  │  OrderBook[N] │  │  ← SoA layout, AVX2 level search, pool-allocated slots
│  └───────────────┘  │
└──────────┬──────────┘
           │  SPSC queue
           ▼
┌─────────────────────┐
│   ExecutionLayer     │  ← position tracking, P&L, downstream dispatch
└─────────────────────┘
```

The hot path contains **zero mutexes, zero heap allocations, and zero system
calls** after startup.

---

## Key Design Decisions

### AVX2 SIMD level search

`find_level()` scans the `prices[]` array for a matching price. The scalar
version processes one `int64_t` per iteration; the AVX2 version uses
`VPCMPEQQ` to compare 4× `int64_t` per cycle:

```cpp
// AVX2: 4 × int64 compared per instruction
__m256i target = _mm256_set1_epi64x(price);
for (uint32_t i = 0; i < n; i += 4) {
    __m256i chunk  = _mm256_loadu_si256((__m256i*)(prices + i));
    __m256i cmp    = _mm256_cmpeq_epi64(chunk, target);
    int     mask   = _mm256_movemask_epi8(cmp);
    if (mask) return i + (__builtin_ctz(mask) / 8);
}
```

Measured speedup: **2.0× at p50** (35 ns vs 71 ns, N=128 levels).
Branch mispredictions also drop 4× — AVX2 has 32 iterations vs 128 scalar.

### Cache-aware order book — struct-of-arrays

The classic `std::map<Price, std::list<Order*>>` LOB requires a tree traversal
and pointer-chase per match — each a likely cache miss.

Struct-of-arrays separates hot (price) from cold (qty, count):

```
prices[]       [99.95] [99.90] [99.85] ...   ← L1-resident during match sweep
qtys[]         [ 1000] [  500] [  200] ...   ← touched only on confirmed cross
order_counts[] [    3] [    2] [    1] ...
```

At N=128 levels: `prices[]` = 1 KB → fits entirely in L1 (32–64 KB).
Measured L1 miss rate: **~0%** vs ~75% for pointer-based LOB.

**The level-index stability bug:** The original design stored a `level_idx`
array index in each order slot for O(1) cancel. But `remove_level()` uses
`memmove` to keep the arrays sorted — invalidating stored indices. Fixed by
storing the order's **price** instead (stable across shifts) and doing an
O(depth) `find_level(price)` on cancel. Faster in practice: depth is small
and the scan is L1-resident.

### Lock-free SPSC — release/acquire only

```cpp
// Producer: write payload, then release-store the index.
buffer_[wp] = item;
write_pos_.store(next, std::memory_order_release);

// Consumer: acquire-load (pairs with above), then read payload.
if (rp == write_pos_.load(std::memory_order_acquire)) return false;
out = buffer_[rp];
```

Two atomic variables, no mutex, no CAS, no ABA. Producer and consumer heads
on **separate 64-byte cache lines** — no false sharing. SPSC enqueue: **12 ns**.

### Pool allocator — deterministic O(1) allocation

Every `Order` lives in a pre-allocated `mmap`'d slab pinned with `mlock`.
Allocation = free-list head load + pointer swap. **~3–5 ns, no system calls,
no page faults after warmup.**

### Fibonacci hashing

Order ID → slot lookup uses Fibonacci hashing (`key × 2⁶⁴/φ >> shift`).
Maps sequential integer IDs uniformly — avoids the modulo clustering that
causes linear probing to degrade on sequential workloads.
Expected probe length: **1.5 at 50% load**.

---

## Profiling

See [PROFILING.md](PROFILING.md) for:
- Full RDTSC benchmark methodology
- Cache behaviour analysis (why SoA gives 0% L1 miss)
- Branch prediction analysis (why AVX2 reduces mispredictions 4×)
- Predicted `perf stat` output (IPC, cache-miss %, branch-miss %)
- Linux tuning guide (`isolcpus`, `SCHED_FIFO`, ASLR disable)

---

## Project Layout

```
options-engine/
├── include/
│   ├── core/
│   │   ├── types.hpp           # Price, Qty, Order, ExecutionReport
│   │   ├── spsc_queue.hpp      # Lock-free SPSC (release/acquire, no fence)
│   │   ├── mpmc_queue.hpp      # Lock-free MPMC (Vyukov per-slot sequence)
│   │   ├── order_book.hpp      # SoA LOB + AVX2 find_level
│   │   ├── matching_engine.hpp # Orchestrator + thread management
│   │   ├── market_data.hpp     # Wire format decoder + ingestion
│   │   ├── execution_layer.hpp # Position tracking + P&L
│   │   └── replay.hpp          # Binary trace writer/replayer
│   └── util/
│       ├── allocator.hpp       # mmap pool allocator (mlock'd slab)
│       ├── histogram.hpp       # Lock-free latency histogram
│       ├── logger.hpp          # Lock-free async logger via SPSC
│       └── perf_counters.hpp   # perf_event_open RAII wrapper
├── bench/
│   ├── bench_replay.cpp        # Order flow replay — p50/p99 histogram
│   ├── bench_avx2.cpp          # AVX2 vs scalar find_level comparison
│   ├── bench_latency.cpp       # Per-operation latency breakdown
│   ├── bench_throughput.cpp    # Sustained msgs/sec
│   └── bench_cache.cpp         # SoA vs AoS speedup at each book depth
├── tests/                      # 6 suites, 171 assertions, 0 failures
├── fuzz/                       # libFuzzer harness for wire parser
├── docs/
│   ├── design.md               # Rationale for every non-obvious decision
│   └── linux-tuning.md         # isolcpus, SCHED_FIFO, C-states, DPDK
├── scripts/
│   ├── build.sh                # Build + test + optional benchmark driver
│   └── profile.sh              # perf record + FlameGraph generation
├── PROFILING.md                # Committed perf methodology + cache analysis
├── BENCHMARK_RESULTS.md        # Committed benchmark numbers
└── .github/workflows/ci.yml    # Release, TSan, ASan, clang-tidy
```

---

## Building

**Requirements:** GCC ≥ 12 or Clang ≥ 16, CMake ≥ 3.22, Ninja, Linux x86-64.

```bash
# Release build + all tests
./scripts/build.sh

# Release build + benchmarks
./scripts/build.sh --bench

# ThreadSanitizer (validates SPSC/MPMC concurrent correctness)
./scripts/build.sh --tsan

# AddressSanitizer
./scripts/build.sh --asan

# Manual
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-mavx2"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## Testing

| Suite | Assertions | What it covers |
|-------|-----------|----------------|
| `test_order_book` | 22 | Price-time priority, FIFO, cancel, partial fills, 100K-event property invariant |
| `test_spsc` | 12 | 2M-item FIFO ordering, wrap-around stress, concurrent — TSan-clean |
| `test_mpmc` | 24 | 1P×1C through 8P×4C — every item received exactly once |
| `test_matching` | 8 | End-to-end cross, cancel-before-match, multi-symbol isolation |
| `test_allocator` | 84 | Exhaust/recover, free-list integrity, 1M alloc/free cycles |
| `test_histogram` | 21 | Bucket indexing, percentile accuracy, concurrent recording |
| **Total** | **171** | **0 failures** |

---

## What Is Not Here (Intentionally)

**Network transport:** `MarketDataIngestion` accepts `span<const uint8_t>`.
Plugging in DPDK or kernel-bypass UDP is a one-function change.

**Persistence:** WAL to pmem/NVMe left out to keep the matching path unobscured.

**Multi-symbol parallelism:** Each `OrderBook` has no shared state. Per-symbol
threads are a mechanical change to `MatchingEngine::register_symbol`.

**Risk / pre-trade checks:** Fat-finger and position limits live between
ingestion and matching — architecturally uninteresting comparisons.
