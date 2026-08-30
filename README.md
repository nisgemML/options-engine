# lob-engine

A from-scratch C++20 limit-order-book matching engine for Linux x86-64:
struct-of-arrays price levels, AVX2 level search, intrusive per-level order
lists, a fixed pool with free-list, and lock-free SPSC ingress. Zero heap
allocation, zero mutexes, zero syscalls on the matching path.

This is a single-symbol book plus the plumbing around it. What it deliberately
does not do is listed in [LIMITATIONS.md](LIMITATIONS.md).

> Formerly published as `options-engine`. The core is instrument-agnostic and
> the old name overstated it; renamed to say what it is.

---

## Correctness

The book is checked against an independent reference model
(`std::map<Price, deque<Order>>`, written from the spec) on **1,000,000
random events per seed, across 5 seeds in CI** (plus a 10,000,000-event
run in a dedicated CI job) — limit, market, IOC, FOK, cancel, modify
(both increases and decreases) — with equality asserted after every event
on:

- the exact fill sequence (aggressor, passive, price, qty), which is what
  enforces price-time priority, including priority loss on a modify-up;
- best bid/ask;
- no crossed book;
- quantity conservation: `submitted == 2·filled + resting + cancelled + rejected + expired`.

The reference model also mirrors the engine's fixed order-capacity limit
(`OrderBook::kMaxOrders`), so a long run diverging only because it hit
that documented boundary isn't mistaken for a bug.

`tests/test_conservation.cpp`. Runs in CI under ASan, UBSan, and TSan.

This test found five bugs, all now fixed:
1. an unfilled **market order was booked as a resting limit** at its price;
2. **IOC/FOK ignored their limit price** and swept through the book like a market order;
3. **FOK was not atomic** — it executed partial fills and then reported a reject;
4. **`modify_order` kept queue position on a quantity increase**, instead of
   losing priority as real exchange semantics require. Found by extending
   the fuzzer's modify generator to exercise increases (previously
   decrease-only) and the reference model's `modify_in` to match — the old
   code then failed the fill-sequence check at a fixed, reproducible seed;
5. **capacity rejection could orphan an empty price level.** `add_order`
   inserts a new price level *before* checking whether a slot is available
   for the order; if the slot pool was full (`kMaxOrders` = 65,536), the
   function returned "rejected" but left the just-created, empty level
   sitting in the book — corrupting `best_quote()` and permanently
   consuming one of the 4,096 level slots. Found only once the model was
   made capacity-aware (previously it had no order cap, so it never ran
   long enough in the same process to hit this) and the fuzzer was run
   past 65,536 concurrently-resting orders — the 1M-event/seed default
   never gets there; the 10M-event CI job does.

---

## Benchmark Results

Every number below is pasted from a committed run in
[BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md); `scripts/update_readme.py` refuses
to let the two drift. Container numbers are what we have; isolated-core numbers
are pending and are **not** predicted here.

<!-- BENCH:START -->
### Order flow replay — 500K synthetic events (Ubuntu 24.04 LTS, GCC 13.3.0, -O3 -march=native (AVX2), x86-64 container (no `isolcpus`, no `SCHED_FIFO`))

| Metric | Value |
|--------|-------|
| Submit latency p50 | **38 ns** |
| Submit latency p99 | 50 ns |
| Submit latency p99.9 | 238 ns |
<!-- BENCH:END -->

p99.9 is scheduler jitter from the container. See
[docs/linux-tuning.md](docs/linux-tuning.md) for the isolated-core setup;
the table will be replaced by an isolated run when one is recorded.

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

## Project Layout

```
lob-engine/
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
| `test_order_book` | 37 | Price-time priority, FIFO, cancel, partial fills, modify increase/decrease priority, IOC remainder expiry, FOK atomicity, 100K-event property invariant |
| `test_spsc` | 12 | 2M-item FIFO ordering, wrap-around stress, concurrent — TSan-clean |
| `test_mpmc` | 24 | 1P×1C through 8P×4C — every item received exactly once |
| `test_matching` | 8 | End-to-end cross, cancel-before-match, multi-symbol isolation |
| `test_allocator` | 84 | Exhaust/recover, free-list integrity, 1M alloc/free cycles |
| `test_histogram` | 21 | Bucket indexing, percentile accuracy, concurrent recording |
| `test_conservation` | 1M events × 5 seeds | Model-based differential fuzzer — see Correctness above |
| **Total (unit/property)** | **186** | **0 failures** |

---

## What Is Not Here (Intentionally)

**Network transport:** `MarketDataIngestion` accepts `span<const uint8_t>`.
Plugging in DPDK or kernel-bypass UDP is a one-function change.

**Persistence:** WAL to pmem/NVMe left out to keep the matching path unobscured.

**Multi-symbol parallelism** is implemented (`MultiSymbolEngine`, one
pinned thread per shard — see `bench/bench_multisymbol.cpp` and
PROFILING.md §4). What's still out of scope is anything *cross-symbol*:
combo/spread books, position limits by underlying — see LIMITATIONS.md.

**Risk / pre-trade checks:** Fat-finger and position limits live between
ingestion and matching — architecturally uninteresting comparisons.
