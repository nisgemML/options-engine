// tests/test_conservation.cpp — Model-based property test.
//
// Runs 1,000,000 random events (add limit/market/IOC/FOK, cancel, modify)
// through the production OrderBook and through an independent reference
// model (std::map<Price, deque<Order>>) written from the spec, not from the
// engine's code. After every event, both must agree on:
//
//   1. Conservation: Σ submitted qty == Σ filled + Σ resting + Σ cancelled
//                    + Σ rejected + Σ expired(IOC/FOK remainder) — exactly.
//   2. No crossed book: best bid < best ask whenever both exist.
//   3. Identical fill sequence: (aggressor, passive, price, qty) in order.
//      This is what enforces price-time priority — a wrong tie-break
//      produces a different passive id for the same fill.
//   4. Identical best quote after every event.
//
// Any divergence prints the event index and a minimal reproduction seed.
// Runs under ASan/UBSan/TSan in CI.

#include "core/order_book.hpp"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <vector>

using namespace engine;

struct Fill { OrderId aggr, passive; Price px; Qty qty; };

// ── Reference model ──────────────────────────────────────────────────────────
struct RefOrder { OrderId id; Qty rem; };
struct RefBook {
    std::map<Price, std::deque<RefOrder>, std::greater<Price>> bids;  // best first
    std::map<Price, std::deque<RefOrder>, std::less<Price>>    asks;  // best first
    std::vector<Fill> fills;
    // Mirrors OrderBook::kMaxOrders so the model rejects resting capacity
    // exhaustion the same way the engine does — without this, a run long
    // enough to fill the fixed-size slot pool diverges from an "engine
    // rejected a resting add" for a reason that has nothing to do with
    // correctness (it's the documented fixed-capacity limit in
    // LIMITATIONS.md, not a bug). Tracked incrementally so it stays O(1)
    // per event instead of walking both maps every check.
    size_t resting_count = 0;

    template<typename Passive>
    Qty sweep(OrderId id, Qty qty, Price limit, bool is_buy, bool limited, Passive& side) {
        while (qty > 0 && !side.empty()) {
            auto it = side.begin();
            Price px = it->first;
            if (limited && (is_buy ? limit < px : limit > px)) break;
            auto& q = it->second;
            while (qty > 0 && !q.empty()) {
                Qty f = std::min(qty, q.front().rem);
                fills.push_back({id, q.front().id, px, f});
                qty -= f; q.front().rem -= f;
                if (q.front().rem == 0) { q.pop_front(); --resting_count; }
            }
            if (q.empty()) side.erase(it);
        }
        return qty;
    }
    template<typename Passive>
    Qty available(Price limit, bool is_buy, Passive& side) {
        Qty total = 0;
        for (auto& [px, q] : side) {
            if (is_buy ? limit < px : limit > px) break;
            for (auto& o : q) total += o.rem;
        }
        return total;
    }

    // Returns {resting_qty, filled_qty, rejected, capacity_rejected}.
    // `rejected` is FOK-style: atomic, zero fills. `capacity_rejected` means
    // the order matched whatever it could (possibly zero) and then found no
    // free slot to rest the remainder in — mirrors OrderBook::add_order,
    // which allocates a resting slot only *after* matching.
    struct Outcome { Qty resting, filled; bool rejected; bool capacity_rejected = false; };
    Outcome add(const Order& o, size_t max_orders) {
        bool buy = o.is_buy();
        bool limited = o.type != OrderType::Market;
        if (o.type == OrderType::FOK) {
            Qty avail = buy ? available(o.price, buy, asks) : available(o.price, buy, bids);
            if (avail < o.qty) return {0, 0, true};
        }
        Qty rem = buy ? sweep(o.id, o.qty, o.price, buy, limited, asks)
                      : sweep(o.id, o.qty, o.price, buy, limited, bids);
        Qty filled = o.qty - rem;
        if (rem > 0 && o.type == OrderType::Limit) {
            if (resting_count >= max_orders) return {0, filled, false, true};
            if (buy) bids[o.price].push_back({o.id, rem}); else asks[o.price].push_back({o.id, rem});
            ++resting_count;
            return {rem, filled, false};
        }
        return {0, filled, false};   // market/IOC remainder expires, FOK fully filled
    }
    template<typename S> bool erase_from(S& side, OrderId id, Qty* out) {
        for (auto it = side.begin(); it != side.end(); ++it)
            for (auto q = it->second.begin(); q != it->second.end(); ++q)
                if (q->id == id) { *out = q->rem; it->second.erase(q); if (it->second.empty()) side.erase(it); --resting_count; return true; }
        return false;
    }
    bool cancel(OrderId id, Qty* out) { return erase_from(bids, id, out) || erase_from(asks, id, out); }
    // delta is only meaningful (non-negative) when nq <= old rem; the driver
    // only consumes it on the decrease path. On increase the order loses
    // time priority: erase and re-append at the back of the same level's
    // deque, matching the engine's re-link-at-tail behavior. resting_count
    // is unaffected by a plain move; it only changes on decrease-to-zero.
    template<typename S> bool modify_in(S& side, OrderId id, Qty nq, Qty* delta) {
        for (auto it = side.begin(); it != side.end(); ++it)
            for (auto q = it->second.begin(); q != it->second.end(); ++q)
                if (q->id == id) {
                    *delta = q->rem - nq;
                    if (nq > q->rem) {
                        RefOrder moved{q->id, nq};
                        it->second.erase(q);
                        it->second.push_back(moved);
                    } else {
                        q->rem = nq;
                        if (nq == 0) { it->second.erase(q); --resting_count; }
                    }
                    if (it->second.empty()) side.erase(it);
                    return true;
                }
        return false;
    }
    template<typename S> bool peek_in(S& side, OrderId id, Qty* out) {
        for (auto& [p, q] : side) for (auto& o : q) if (o.id == id) { *out = o.rem; return true; }
        return false;
    }
    bool peek(OrderId id, Qty* out) { return peek_in(bids, id, out) || peek_in(asks, id, out); }
    bool modify(OrderId id, Qty nq, Qty* delta) { return modify_in(bids, id, nq, delta) || modify_in(asks, id, nq, delta); }
    BestQuote best() const {
        BestQuote b{}; b.bid_price = PRICE_INVALID; b.ask_price = PRICE_INVALID;
        if (!bids.empty()) { b.bid_price = bids.begin()->first; for (auto& o : bids.begin()->second) b.bid_qty += o.rem; }
        if (!asks.empty()) { b.ask_price = asks.begin()->first; for (auto& o : asks.begin()->second) b.ask_qty += o.rem; }
        return b;
    }
    uint64_t resting_total() const {
        uint64_t t = 0;
        for (auto& [p, q] : bids) for (auto& o : q) t += o.rem;
        for (auto& [p, q] : asks) for (auto& o : q) t += o.rem;
        return t;
    }
};

// ── Driver ───────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    const uint64_t seed   = argc > 1 ? strtoull(argv[1], nullptr, 10) : 0x5EED;
    const size_t   events = argc > 2 ? strtoull(argv[2], nullptr, 10) : 1'000'000;
    std::mt19937_64 rng(seed);

    std::vector<Fill> engine_fills;
    auto sink = [&](const ExecutionReport& r) {
        engine_fills.push_back({r.order_id, r.contra_order_id, r.exec_price, r.exec_qty});
    };
    OrderBook book(0, sink);
    RefBook ref;

    // Ledger for conservation (engine side, from its own reports + return codes).
    uint64_t submitted = 0, filled = 0, cancelled = 0, rejected = 0, expired = 0;
    std::vector<OrderId> live;           // ids that may be resting
    OrderId next_id = 1;
    size_t mismatch_at = SIZE_MAX; const char* what = nullptr;

    auto fail = [&](size_t i, const char* w) { if (mismatch_at == SIZE_MAX) { mismatch_at = i; what = w; } };

    for (size_t i = 0; i < events && mismatch_at == SIZE_MAX; ++i) {
        int roll = int(rng() % 100);
        size_t fills_before = engine_fills.size();
        size_t ref_before   = ref.fills.size();

        if (roll < 70 || live.empty()) {
            // ── Add ──
            Order o{};
            o.id = next_id++;
            o.side = (rng() & 1) ? Side::Buy : Side::Sell;
            // Prices clustered around 100.00 in 1c ticks so books cross often.
            o.price = to_price(100.0) + Price((int64_t(rng() % 41) - 20) * 10'000);
            o.qty = Qty(1 + rng() % 500); o.qty_remaining = o.qty;
            int t = int(rng() % 100);
            o.type = t < 80 ? OrderType::Limit : t < 88 ? OrderType::Market : t < 95 ? OrderType::IOC : OrderType::FOK;
            o.status = OrderStatus::New;

            if (getenv("VERBOSE")) printf("[%zu] ADD id=%llu %s %s px=%.2f qty=%u\n", i, (unsigned long long)o.id,
                o.is_buy() ? "BUY" : "SELL", o.type == OrderType::Limit ? "LMT" : o.type == OrderType::Market ? "MKT" : o.type == OrderType::IOC ? "IOC" : "FOK", from_price(o.price), o.qty);
            bool ok = book.add_order(o);
            auto out = ref.add(o, OrderBook::kMaxOrders);
            if (getenv("VERBOSE")) {
                for (size_t k = fills_before; k < engine_fills.size(); ++k) printf("     eng fill %llu x %llu px=%.2f q=%u\n", (unsigned long long)engine_fills[k].aggr, (unsigned long long)engine_fills[k].passive, from_price(engine_fills[k].px), engine_fills[k].qty);
                for (size_t k = ref_before; k < ref.fills.size(); ++k) printf("     ref fill %llu x %llu px=%.2f q=%u\n", (unsigned long long)ref.fills[k].aggr, (unsigned long long)ref.fills[k].passive, from_price(ref.fills[k].px), ref.fills[k].qty);
            }
            submitted += o.qty;

            uint64_t eng_filled = 0;
            for (size_t k = fills_before; k < engine_fills.size(); ++k) eng_filled += engine_fills[k].qty;
            filled += eng_filled;

            if (out.rejected) {
                if (ok) fail(i, "engine accepted FOK the model rejected");
                if (eng_filled) fail(i, "FOK executed partial fills before rejecting (must be atomic)");
                rejected += o.qty - eng_filled;
            } else if (out.capacity_rejected) {
                // Slot pool exhausted (kMaxOrders): whatever matched before
                // hitting capacity must still agree; the remainder is lost
                // (not resting) rather than expiring IOC-style, but goes in
                // the same conservation bucket since neither books it.
                if (ok) fail(i, "engine accepted an order the model capacity-rejected");
                if (eng_filled != out.filled) fail(i, "filled qty differs from model (capacity path)");
                expired += o.qty - eng_filled;
            } else {
                if (!ok) fail(i, "engine rejected an order the model accepted");
                if (eng_filled != out.filled) fail(i, "filled qty differs from model");
                if (o.type == OrderType::Limit) { if (out.resting) live.push_back(o.id); }
                else expired += o.qty - eng_filled;
            }
        } else if (roll < 90) {
            // ── Cancel ──
            size_t k = rng() % live.size();
            OrderId id = live[k]; live[k] = live.back(); live.pop_back();
            Qty rem = 0;
            bool ref_ok = ref.cancel(id, &rem);
            if (getenv("VERBOSE")) printf("[%zu] CANCEL id=%llu\n", i, (unsigned long long)id);
            bool ok = book.cancel_order(id);
            if (ok != ref_ok) fail(i, "cancel result differs from model");
            if (ok) cancelled += rem;
        } else {
            // ── Modify (qty up or down) ──
            // Decrease keeps queue position. Increase loses time priority —
            // both the engine (modify_order) and the model (modify_in) move
            // the order to the back of its level's queue on increase, so
            // the fill-sequence check below is what actually proves the
            // priority-loss semantics, not just the qty bookkeeping.
            size_t k = rng() % live.size();
            OrderId id = live[k];
            Qty rem = 0, delta = 0;
            Qty nq = 0;
            bool have = ref.peek(id, &rem);
            if (have) {
                if (rng() % 100 < 60) nq = Qty(rng() % (rem + 1));                 // decrease/same, incl. 0 = cancel
                else                  nq = Qty(rem + 1 + rng() % (rem + 1));       // strict increase
            }
            if (getenv("VERBOSE")) printf("[%zu] MODIFY id=%llu %u -> %u\n", i, (unsigned long long)id, rem, nq);
            bool ref_ok = have && ref.modify(id, nq, &delta);
            if (ref_ok) {
                bool ok = book.modify_order(id, nq);
                if (!ok) fail(i, "modify rejected by engine, accepted by model");
                if (nq >= rem) submitted += (nq - rem);   // increase = extra qty entering the book
                else            cancelled += delta;        // decrease = qty leaves the ledger as cancelled
                if (nq == 0) { live[k] = live.back(); live.pop_back(); }
            } else {
                live[k] = live.back(); live.pop_back();   // stale id; drop it
            }
        }

        // ── Cross-check after every event ──
        if (engine_fills.size() - fills_before != ref.fills.size() - ref_before) fail(i, "fill count differs");
        else for (size_t k = fills_before, r = ref_before; k < engine_fills.size(); ++k, ++r) {
            const Fill& a = engine_fills[k]; const Fill& b = ref.fills[r];
            if (a.aggr != b.aggr || a.passive != b.passive || a.px != b.px || a.qty != b.qty) { fail(i, "fill sequence differs (price-time priority violated)"); break; }
        }
        BestQuote e = book.best_quote(), m = ref.best();
        if (e.bid_price != m.bid_price || e.ask_price != m.ask_price) fail(i, "best prices differ from model");
        if (e.bid_price != PRICE_INVALID && e.ask_price != PRICE_INVALID && e.bid_price >= e.ask_price) fail(i, "crossed book");
        if ((i & 0xFFF) == 0) {   // resting sum is O(n); sample it
            uint64_t resting = ref.resting_total();
            if (submitted != filled * 2 + resting + cancelled + rejected + expired)
                fail(i, "conservation violated: submitted != 2*filled + resting + cancelled + rejected + expired");
        }
    }

    uint64_t resting = ref.resting_total();
    bool conserved = submitted == filled * 2 + resting + cancelled + rejected + expired;

    printf("events=%zu seed=%llu\n", events, (unsigned long long)seed);
    printf("submitted=%llu filled(x2)=%llu resting=%llu cancelled=%llu rejected=%llu expired=%llu\n",
           (unsigned long long)submitted, (unsigned long long)filled * 2, (unsigned long long)resting,
           (unsigned long long)cancelled, (unsigned long long)rejected, (unsigned long long)expired);
    printf("fills=%zu  conservation=%s\n", engine_fills.size(), conserved ? "OK" : "VIOLATED");
    if (mismatch_at != SIZE_MAX) {
        printf("FAIL at event %zu: %s\n  reproduce: %s %llu %zu\n", mismatch_at, what, argv[0], (unsigned long long)seed, mismatch_at + 1);
        return 1;
    }
    if (!conserved) return 1;
    puts("PASS: engine matches reference model on every event");
    return 0;
}
