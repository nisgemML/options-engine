# Limitations

Scope is deliberate. Listed here so nobody has to discover it.

- **Single-instrument core.** `MultiSymbolEngine` shards books by symbol
  onto threads, but there is no cross-symbol logic: no combo/spread books,
  no position limits by underlying, nothing options-specific.
- **Fixed capacity.** 4,096 price levels and 65,536 live orders per book.
  Beyond that, `add_order` rejects. Sized for equity/option books, not
  Treasury futures ladders.
- **No network, persistence, recovery, or risk checks.** Ingress is an
  in-process SPSC queue. A crash loses the book.
- **Modify is decrease-only-safe.** `modify_order` edits quantity in place
  and keeps queue position. That is correct for a reduction; an increase
  should lose priority and currently does not. The model test only
  exercises decreases.
- **Cancel is O(1)** via a doubly-linked intrusive list (`slots_.prevs[]`).
  The implementation was verified by the model-based property test
  (`test_conservation.cpp`) which caught two bugs in the first attempt.
- **Hash index is linear-probe with backward-shift**, not Robin Hood.
  Expected probe length 1.5 at 50% load; degrades under adversarial keys.
- **Benchmarks are L1-resident and single-symbol.** Market-data-driven
  runs (`bench_replay`) exercise realistic order flow but still one symbol.
- **No isolated-core numbers recorded yet.** Everything in
  `BENCHMARK_RESULTS.md` is from a shared container.
