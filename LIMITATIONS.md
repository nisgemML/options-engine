# Limitations

Scope is deliberate. Listed here so nobody has to discover it.

- **Single-instrument core.** `MultiSymbolEngine` shards books by symbol
  onto threads, but there is no cross-symbol logic: no combo/spread books,
  no position limits by underlying, nothing options-specific.
- **Fixed capacity.** 4,096 price levels and 65,536 live orders per book.
  Beyond that, `add_order` rejects, and does so cleanly — without leaving
  an orphaned empty price level behind (see the "bugs found" list in the
  README's Correctness section).
- **No network, persistence, recovery, or risk checks.** Ingress is an
  in-process SPSC queue. A crash loses the book.
- **Modify preserves price-time priority correctly.** `modify_order` edits
  quantity in place and keeps queue position on a decrease. On an increase
  it unlinks the order and re-appends it at the tail of the same price
  level's FIFO, so it loses time priority — matching real exchange
  semantics. Verified by `test_modify_increase_loses_priority` /
  `test_modify_decrease_keeps_priority` in `test_order_book.cpp`, and by
  the model-based fuzzer (`test_conservation.cpp`), which now exercises
  both directions and checks the exact fill sequence, not just quantities.
- **Cancel is O(1)** via a doubly-linked intrusive list (`slots_.prevs[]`).
  The implementation was verified by the model-based property test
  (`test_conservation.cpp`) which caught two bugs in the first attempt.
- **Hash index is linear-probe with backward-shift**, not Robin Hood.
  Expected probe length 1.5 at 50% load; degrades under adversarial keys.
- **Benchmarks are L1-resident and single-symbol.** Market-data-driven
  runs (`bench_replay`) exercise realistic order flow but still one symbol.
- **Self-trade prevention is opt-in and deliberately conservative.**
  `Order::account_id` defaults to 0, which never triggers STP — every
  existing caller (tests, benchmarks, `MatchingEngine`) is unaffected. Set
  it on both sides and, when the aggressor would trade against its own
  resting order, matching stops at that order rather than skipping past
  it — skipping would jump a same-account order ahead of a different
  account's order still queued behind it, which is a worse violation than
  not implementing STP at all. Since matching stopped there, the
  aggressor's remainder is cancelled rather than rested (resting it would
  leave the book crossed against the price it just refused to trade at).
  It does **not** implement "skip and match the next order" or "cancel
  the resting order" policies real venues also offer. Covered by
  `test_self_trade_prevention` and by the fuzzer (`test_conservation.cpp`
  assigns ~30% of generated orders one of 8 account ids).
- **No isolated-core numbers recorded yet.** Everything in
  `BENCHMARK_RESULTS.md` is from a shared container.
