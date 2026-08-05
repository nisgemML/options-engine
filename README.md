# Low-Latency Trading Engine

A from-scratch exchange simulation in C++20 targeting sub-microsecond matching
latency on Linux x86-64. Full exchange architecture: market data ingestion,
limit order book, matching engine, execution layer — all connected by lock-free
SPSC queues with no heap allocation on the hot path.

---

## Benchmark Results

Measured on a **shared container without core isolation or `SCHED_FIFO`**
(the numbers you can reproduce by cloning and running `./scripts/build.sh --bench`).
See [docs/linux-tuning.md](docs/linux-tuning.md) for the production setup
(`isolcpus`, `nohz_full`, `SCHED_FIFO`) which reduces p99 by 10–100×.

### Order Book — single-threaded, no IPC overhead

| Operation | p50 | p90 | p99 | Throughput |
|-----------|-----|-----|-----|------------|
| `add_order` (passive, no fill) | ~140 ns | varies | varies | — |
| `add_order` (aggressive, fill) | ~220 ns | varies | varies | — |
| `cancel_order` | ~40 ns | varies | varies | — |
| 1M sequential `add_order` | — | — | — | **~6 M msg/sec** |

p50 latency is stable (cache-warm matching path). p99 varies with container
scheduling noise — on a dedicated isolated core it converges to 2–3× p50.

### End-to-End — SPSC submit to matching engine

| Metric | Value |
|--------|-------|
| Submit latency (SPSC push only) p50 | ~190 ns |
| True order-to-fill latency | submit_lat + match_lat ≈ 300–400 ns |

The matching engine runs asynchronously on its own thread. End-to-end latency
is the sum of the SPSC push (above) and the order-book match (above).

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
│  │  OrderBook[N] │  │  ← SoA layout, pool-allocated slots, O(1) cancel
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

### Lock-free concurrency — SPSC ring buffers

The minimum synchronization this problem requires is two atomic variables:
one producer-owned (`write_pos`) and one consumer-owned (`read_pos`). No
mutex, no CAS loop, no ABA problem.

```cpp
// Producer: write payload, then release-store the index.
buffer_[wp] = item;
write_pos_.store(next, std::memory_order_release);

// Consumer: acquire-load the index (pairs with above), then read payload.
if (rp == write_pos_.load(std::memory_order_acquire)) return false;
out = buffer_[rp];
```

The `release`/`acquire` pair establishes happens-before with no explicit fence.
Producer and consumer heads live on **separate 64-byte cache lines** to prevent
false sharing. An MPMC variant (`include/core/mpmc_queue.hpp`) using per-slot
sequence numbers handles the multiple-producer case.

### Cache-aware order book — struct-of-arrays

The classic `std::map<Price, std::list<Order*>>` LOB requires a tree traversal
and pointer-chase per match — each a likely cache miss. Profiling showed L1
miss rate as the primary bottleneck.

The fix: struct-of-arrays layout separates the hot field (price) from cold
fields (qty, count):

```
prices[]       [99.95] [99.90] [99.85] ...   ← iterated every match
qtys[]         [ 1000] [  500] [  200] ...   ← touched only on a confirmed cross
order_counts[] [    3] [    2] [    1] ...
```

The hardware prefetcher streams `prices[]` ahead of the comparison loop.
`qtys[]` stays cache-cold until actually needed. `bench/bench_cache.cpp`
measures and prints the SoA vs AoS speedup at each book depth.

**The level-index stability bug:** The original design stored a `level_idx`
array index in each order slot for O(1) cancel. But `remove_level()` uses
`memmove` to keep the arrays sorted — invalidating stored indices. Fixed by
storing the order's **price** instead (stable across shifts) and doing an
O(depth) `find_level(price)` on cancel. This is faster in practice because
depth is small and the scan is cache-resident.

### Pool allocator — deterministic O(1) allocation

`malloc` is non-deterministic and touches shared heap state. Every `Order`
lives in a pre-allocated `mmap`'d slab pinned with `mlock`. Allocation is
a free-list head load + pointer swap — ~3–5 ns, no system calls.

```
slab_  →  [Order][Order][Order]...[Order]
           each free slot's first bytes hold a pointer to next free slot
```

### CPU isolation

The matching engine thread calls `pthread_setaffinity_np` to pin itself and
`pthread_setschedparam(SCHED_FIFO, 50)` to prevent preemption. It busy-polls
its SPSC queue with `__builtin_ia32_pause()` (the x86 PAUSE hint) in the empty
case — no condition variable, no OS involvement.

---

## Project Layout

```
trading-engine/
├── include/
│   ├── core/
│   │   ├── types.hpp           # Price, Qty, Order, ExecutionReport, ...
│   │   ├── spsc_queue.hpp      # Lock-free SPSC ring buffer (release/acquire)
│   │   ├── mpmc_queue.hpp      # Lock-free MPMC (Vyukov's per-slot sequence)
│   │   ├── order_book.hpp      # SoA limit order book
│   │   ├── matching_engine.hpp # Orchestrator + thread management
│   │   ├── market_data.hpp     # Wire format decoder + ingestion
│   │   ├── execution_layer.hpp # Position tracking + P&L
│   │   └── replay.hpp          # Binary trace writer/replayer
│   └── util/
│       ├── allocator.hpp       # mmap pool allocator (mlock'd slab)
│       ├── histogram.hpp       # Lock-free latency histogram (no alloc)
│       ├── logger.hpp          # Lock-free async logger via SPSC queue
│       └── perf_counters.hpp   # perf_event_open RAII wrapper
├── src/
│   └── core/ util/             # Implementations
├── tests/
│   ├── test_order_book.cpp     # Unit + 100K-event property-based invariant test
│   ├── test_spsc.cpp           # 2M-item concurrent correctness (TSan-clean)
│   ├── test_mpmc.cpp           # N×M producer/consumer correctness
│   ├── test_matching.cpp       # End-to-end integration tests
│   ├── test_allocator.cpp      # Pool allocator stress (1M alloc/free cycles)
│   └── test_histogram.cpp      # Concurrent recording + percentile correctness
├── bench/
│   ├── bench_latency.cpp       # p50/p99 at order-book and end-to-end level
│   ├── bench_throughput.cpp    # Sustained msgs/sec (producer/consumer threads)
│   ├── bench_replay.cpp        # Trace-replay with LatencyHistogram output
│   └── bench_cache.cpp         # SoA vs AoS measured speedup at each book depth
├── fuzz/
│   ├── fuzz_market_data.cpp    # libFuzzer harness for wire parser
│   └── gen_corpus.py           # Generates seed corpus (valid + malformed inputs)
├── docs/
│   ├── design.md               # Detailed rationale for every non-obvious decision
│   └── linux-tuning.md          # isolcpus, SCHED_FIFO, C-states, DPDK checklist
├── scripts/
│   ├── build.sh                # Build + test + optional benchmark driver
│   └── profile.sh              # perf record + FlameGraph generation
└── .github/workflows/ci.yml    # Release, TSan, ASan, clang-tidy, bench smoke
```

---

## Building

**Requirements:** GCC ≥ 12 or Clang ≥ 16, CMake ≥ 3.22, Ninja, Linux x86-64.

```bash
# Release build + all tests
./scripts/build.sh

# Release build + run benchmarks
./scripts/build.sh --bench

# ThreadSanitizer build (validates SPSC and MPMC concurrent correctness)
./scripts/build.sh --tsan

# AddressSanitizer build
./scripts/build.sh --asan

# Manual workflow
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

---

## Testing

| Suite | Assertions | What it covers |
|-------|-----------|----------------|
| `test_order_book` | 22 | Price-time priority, FIFO, cancel, partial fills, 100K-event property invariant (`bid ≤ ask` holds throughout) |
| `test_spsc` | 12 | 2M-item FIFO ordering, 1M wrap-around stress, concurrent producer/consumer — TSan-clean |
| `test_mpmc` | 24 | 1P×1C, 2P×1C, 1P×2C, 4P×4C, 8P×4C — every item received exactly once, no duplicates |
| `test_matching` | 8 | End-to-end cross, cancel-before-match, multi-symbol isolation, market order fill |
| `test_allocator` | 84 | Exhaust/recover, free-list integrity, 1M alloc/free cycling stress |
| `test_histogram` | 21 | Bucket indexing, percentile accuracy, concurrent recording from 8 threads |
| **Total** | **171** | **0 failures** |

### Fuzz testing

```bash
clang++ -std=c++20 -O1 -fsanitize=fuzzer,address -Iinclude \
  fuzz/fuzz_market_data.cpp src/**/*.cpp -lpthread -o fuzz_market_data
python3 fuzz/gen_corpus.py          # generate seed corpus
./fuzz_market_data fuzz/corpus/ -max_len=512 -jobs=4
```

---

## What Is Not Here (Intentionally)

**Network transport:** `MarketDataIngestion` accepts `span<const uint8_t>`.
Plugging in DPDK or kernel-bypass UDP is a one-function change orthogonal to
matching correctness.

**Persistence:** A production system would WAL every event to pmem or NVMe
for crash recovery. Left out to keep the matching path unobscured.

**Multi-symbol parallelism:** Each `OrderBook` has no shared state. Adding
per-symbol threads (one core per instrument) is a mechanical change to
`MatchingEngine::register_symbol`. The single-threaded baseline is correct
first and scalable second.

**Risk / pre-trade checks:** Fat-finger limits and position limits live between
ingestion and matching. Architecturally uninteresting — just comparisons against
pre-computed limits.
