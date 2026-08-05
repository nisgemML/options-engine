// bench/bench_replay.cpp — generate a synthetic trace and replay it,
// reporting a full latency histogram using LatencyHistogram.
//
// This is the kind of benchmark you'd run before and after a refactor
// to validate that latency characteristics haven't regressed.

#include "core/matching_engine.hpp"
#include "core/replay.hpp"
#include "util/histogram.hpp"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include <cstring>

using namespace engine;

// Generate a synthetic trace that mimics realistic order flow:
//   - Resting limit orders from market makers (70%)
//   - Aggressive limit/market orders from takers  (20%)
//   - Cancellations of resting orders             (10%)
static void generate_trace(const char* path, int n_events) {
    TraceWriter writer(path);
    std::mt19937_64 rng(0xDEADBEEF);

    // Simulated clock starting at "9:30 AM"
    uint64_t ts_ns    = 9ULL * 3600 * 1'000'000'000ULL;
    uint64_t id_gen   = 1;

    // Track live order ids for cancellation.
    std::vector<OrderId> live;
    live.reserve(10000);

    auto rand_price = [&](Side s) -> Price {
        double mid   = 100.0;
        double spread = 0.02;
        double jitter = (rng() % 21 - 10) * 0.005;
        double base  = (s == Side::Buy) ? mid - spread/2 : mid + spread/2;
        return to_price(base + jitter);
    };

    for (int i = 0; i < n_events; ++i) {
        // Advance simulated clock: exponentially distributed inter-arrival.
        ts_ns += 500 + (rng() % 2000);   // 0.5–2.5 µs between events

        int roll = rng() % 10;
        MarketDataMsg msg{};
        msg.symbol = 0;

        if (roll < 7 || live.empty()) {
            // New resting limit order.
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            msg.msg_type   = MarketDataMsg::Type::NewOrder;
            msg.order_id   = id_gen++;
            msg.side       = s;
            msg.price      = rand_price(s);
            msg.qty        = 100 + (rng() % 10) * 100;
            msg.order_type = OrderType::Limit;
            writer.write(ts_ns, msg);
            live.push_back(msg.order_id);

        } else if (roll < 9) {
            // Aggressive order (crosses the spread).
            Side s = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            msg.msg_type   = MarketDataMsg::Type::NewOrder;
            msg.order_id   = id_gen++;
            msg.side       = s;
            // Price well through the spread to guarantee crossing.
            msg.price      = (s == Side::Buy) ? to_price(101.0) : to_price(99.0);
            msg.qty        = 100 + (rng() % 5) * 100;
            msg.order_type = (rng() % 3 == 0) ? OrderType::Market : OrderType::Limit;
            writer.write(ts_ns, msg);

        } else {
            // Cancel a random live order.
            std::size_t idx = rng() % live.size();
            msg.msg_type = MarketDataMsg::Type::CancelOrder;
            msg.order_id = live[idx];
            writer.write(ts_ns, msg);
            live.erase(live.begin() + static_cast<long>(idx));
        }
    }

    printf("Generated trace: %lu events → %s\n", writer.events_written(), path);
}

int main(int argc, char** argv) {
    const int n_events = (argc > 1) ? std::atoi(argv[1]) : 500'000;
    const char* trace_path = "/tmp/engine_trace.bin";

    printf("=== Order Flow Replay Benchmark ===\n\n");

    // ── Generate trace ──────────────────────────────────────────────────────
    printf("Generating %d synthetic events...\n", n_events);
    generate_trace(trace_path, n_events);

    // ── Set up engine ────────────────────────────────────────────────────────
    MatchingEngine engine;
    engine.register_symbol(0);
    engine.start();

    // ── Replay at max speed ──────────────────────────────────────────────────
    printf("\n[Mode: MaxSpeed]\n");
    {
        OrderFlowReplay::Config cfg;
        cfg.mode          = OrderFlowReplay::Mode::MaxSpeed;
        cfg.warmup_events = 10'000;
        cfg.verbose       = false;

        OrderFlowReplay replay(engine, cfg);
        ReplayResult result;
        replay.replay(trace_path, result);
        result.print();
    }

    // ── Replay at 1M msgs/sec (throttled) ───────────────────────────────────
    printf("\n[Mode: Throttled @ 1M msgs/sec]\n");
    {
        OrderFlowReplay::Config cfg;
        cfg.mode          = OrderFlowReplay::Mode::Throttled;
        cfg.throttle_mps  = 1'000'000;
        cfg.warmup_events = 5'000;

        OrderFlowReplay replay(engine, cfg);
        ReplayResult result;
        replay.replay(trace_path, result);
        result.print();
    }

    engine.stop();
    return 0;
}
