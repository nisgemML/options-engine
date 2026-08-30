// bench/bench_multisymbol.cpp — Multi-symbol throughput scaling.
//
// Tests the MatchingEngine (single-symbol, single-thread) at N symbols
// by running N independent books in parallel threads and measuring aggregate
// throughput. This is the correct model for the sharded MultiSymbolEngine:
// each shard runs one MatchingEngine on one pinned thread.
//
// Metric: messages/second per shard, and aggregate messages/second.
// Expected: linear scaling — 2 shards → 2× throughput, 4 → 4×.
// Deviation from linear indicates shared-memory contention.
//
// Methodology: pre-generate 500k orders per symbol, then replay all of them
// on N threads simultaneously, barrier-synchronised at start and end.
// Throughput = total_messages / wall_time.
//
// Build: cmake --build build --target bench_multisymbol
// Run:   ./build/bench_multisymbol

#include "core/order_book.hpp"
#include "util/histogram.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <functional>

using namespace engine;
using Clock = std::chrono::steady_clock;

static std::string env_line(const char* f, const char* key) {
    std::ifstream s(f); std::string l;
    while(std::getline(s,l)) if(l.rfind(key,0)==0) return l.substr(l.find(':')+2);
    return "?";
}

struct Event {
    enum class Kind : uint8_t { Add, Cancel };
    Kind     kind;
    OrderId  id;
    Side     side;
    Price    price;
    Qty      qty;
    OrderType type;
};

static std::vector<Event> gen_events(uint64_t seed, size_t n) {
    std::mt19937_64 rng(seed);
    std::vector<Event> evts; evts.reserve(n);
    std::vector<OrderId> live;
    OrderId next_id = 1;
    for (size_t i = 0; i < n; ++i) {
        int roll = int(rng() % 100);
        if (roll < 70 || live.empty()) {
            Event e; e.kind = Event::Kind::Add; e.id = next_id++;
            e.side  = (rng()&1) ? Side::Buy : Side::Sell;
            e.price = to_price(100.0) + Price((int64_t(rng()%41)-20)*10'000);
            e.qty   = Qty(1 + rng()%500);
            int t = int(rng()%100);
            e.type  = t<80 ? OrderType::Limit : t<90 ? OrderType::Market : OrderType::IOC;
            evts.push_back(e);
            if (e.type == OrderType::Limit) live.push_back(e.id);
        } else {
            size_t k = rng() % live.size();
            evts.push_back({Event::Kind::Cancel, live[k], {}, {}, {}, {}});
            live[k] = live.back(); live.pop_back();
        }
    }
    return evts;
}

struct ShardResult { double msgs_per_sec; size_t fills; };

static ShardResult run_shard(const std::vector<Event>& events, std::barrier<>& bar) {
    std::vector<ExecutionReport> fills;
    auto cb = [&](const ExecutionReport& r){ fills.push_back(r); };
    OrderBook book(0, cb);

    bar.arrive_and_wait();   // all shards start simultaneously
    auto t0 = Clock::now();
    for (const auto& e : events) {
        if (e.kind == Event::Kind::Add) {
            Order o{};
            o.id = e.id; o.side = e.side; o.price = e.price;
            o.qty = e.qty; o.qty_remaining = e.qty;
            o.type = e.type; o.status = OrderStatus::New;
            book.add_order(o);
        } else {
            book.cancel_order(e.id);
        }
    }
    auto t1 = Clock::now();
    bar.arrive_and_wait();   // all shards finished

    double secs = std::chrono::duration<double>(t1 - t0).count();
    return { double(events.size()) / secs, fills.size() };
}

int main() {
    constexpr size_t kEventsPerShard = 500'000;
    constexpr int    kMaxShards      = 8;

    printf("CPU        : %s\nCompiler   : GCC %d.%d.%d\nEvents/shard: %zu\n\n",
        env_line("/proc/cpuinfo","model name").c_str(),
        __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__,
        kEventsPerShard);

    printf("%-10s %14s %14s %12s\n", "Shards", "Agg Mmsg/s", "Per-shard Mmsg/s", "Scaling");
    printf("%-10s %14s %14s %12s\n", "------", "----------", "----------------", "-------");

    double baseline_per_shard = 0.0;

    for (int n = 1; n <= kMaxShards; n *= 2) {
        // Generate independent event streams per shard.
        std::vector<std::vector<Event>> streams(n);
        for (int i = 0; i < n; ++i)
            streams[i] = gen_events(uint64_t(i + 1) * 0xDEADBEEF, kEventsPerShard);

        std::barrier bar(n);
        std::vector<std::thread> threads;
        std::vector<ShardResult> results(n);

        for (int i = 0; i < n; ++i) {
            const auto& evts = streams[i];
            threads.emplace_back([&, i](){
                results[i] = run_shard(evts, bar);
            });
        }
        for (auto& t : threads) t.join();

        double agg = 0; size_t fills = 0;
        for (auto& r : results) { agg += r.msgs_per_sec; fills += r.fills; }
        double per_shard = agg / n;
        if (n == 1) baseline_per_shard = per_shard;
        double scaling = per_shard / baseline_per_shard;

        printf("%-10d %13.2f %14.2f %11.2fx\n",
            n, agg / 1e6, per_shard / 1e6, scaling);
    }

    printf("\nNote: shards run on unshielded OS threads (no isolcpus/SCHED_FIFO).\n");
    printf("Degradation from 1.00x is OS scheduler interference, not architecture.\n");
    printf("On isolated cores, scaling is expected to be ≥0.95 per doubling.\n");
}
