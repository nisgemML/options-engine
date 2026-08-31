// Deterministic replay harness.
//
// Distinct from include/core/replay.hpp's OrderFlowReplay: that one
// replays a MarketDataMsg wire-format trace at controlled pace to measure
// latency (submit timestamps, throttling, a LatencyHistogram) and never
// captures or compares the resulting fills. This tool exists for the
// opposite purpose — verifying determinism, not measuring speed — so it
// captures the full fill sequence and byte-diffs it against a second,
// independent replay. Deliberately a separate, smaller binary trace
// format (raw Order-level events, no wire encoding) rather than bolting
// fill-capture onto OrderFlowReplay, since that would couple a
// correctness check to a benchmarking tool's timing logic.
//
//   replay_trace record <seed> <num_events> <trace_file>
//       Generates a random event stream (same style of traffic as
//       tests/test_conservation.cpp — limit/market/IOC/FOK adds, cancels,
//       modifies), runs it through a real OrderBook, and writes two files:
//         <trace_file>        — the raw event stream (binary, fixed-size records)
//         <trace_file>.fills  — the resulting fill sequence (binary)
//
//   replay_trace replay <trace_file>
//       Reads <trace_file>, replays it through a FRESH OrderBook, and
//       byte-compares the fills produced against <trace_file>.fills.
//       Prints PASS/FAIL. Exit code 0 on PASS, 1 on FAIL or I/O error.
//
// This is what "bit-identical fills from a recorded trace" (as opposed to
// test_conservation.cpp's live differential run) actually means: the engine
// is deterministic given a fixed input sequence, checked by literally
// replaying it and diffing raw bytes, not just re-deriving the same
// pseudo-random stream twice.
//
// Kept deliberately separate from the reference-model differential test:
// this only proves determinism (same input -> same output, byte-for-byte),
// not correctness against a spec. test_conservation.cpp still owns the
// correctness claim.
//
// Two real bugs were found and fixed building this, both by running it
// under UBSan rather than trusting that it compiled and printed PASS:
//   1. The original on-disk structs used #pragma pack(1); reference-
//      binding a uint64_t field at a 4-byte-aligned offset (`for (const
//      auto& e : stream)`) is undefined behavior. Fixed by dropping the
//      pack and asserting the natural (padded) sizes instead.
//   2. Once unpacked, TraceFill instances built via `TraceFill{a,b,c,d}`
//      (all named members supplied) do NOT guarantee the compiler-
//      inserted padding bytes are zero — only value-initializing the
//      whole object first (`TraceFill f{};` then assign each field) does.
//      Without that, two independent process runs of the identical
//      seed/event-count produced trace files with different garbage in
//      the padding, which made the byte-for-byte fill comparison flaky
//      independent of any real logic bug. Confirmed fixed by recording
//      the same input twice in separate processes and `cmp`-ing the
//      output files directly, not just trusting `replay`'s own verdict.

#include "core/order_book.hpp"
#include "core/types.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct TraceEvent {
    uint8_t  kind;        // 0 = Add, 1 = Cancel, 2 = Modify
    uint8_t  side;        // Order::side, Add only
    uint8_t  type;        // Order::type, Add only
    uint8_t  _pad = 0;
    uint64_t id;
    int64_t  price;       // Add only
    uint32_t qty;         // Add: qty. Modify: new_qty. Cancel: unused.
    uint32_t account_id;  // Add only
};
struct TraceFill {
    uint64_t aggr_id;
    uint64_t passive_id;
    int64_t  price;
    uint32_t qty;
};
// Deliberately NOT byte-packed (no #pragma pack): on-disk records are read
// directly via fread into a vector<TraceEvent>/vector<TraceFill> and then
// reference-bound (`for (const auto& e : stream)`), so packing would put
// the uint64_t/int64_t fields at 4-byte-aligned offsets — technically UB
// to reference-bind at an under-aligned address. UBSan caught this
// immediately when it was packed (misaligned-address error at that exact
// loop). Natural alignment costs a few bytes of padding per record;
// correctness wins that trade every time. Sizes below are asserted, not
// assumed — if this fails after an edit, the on-disk format changed;
// bump the assert and confirm that's intended (it's fine, this format
// isn't meant to be portable across machines/compilers, only a
// same-run record/replay round trip).
static_assert(sizeof(TraceEvent) == 32, "trace record layout changed — bump the assert and confirm on-disk format is still what you expect");
static_assert(sizeof(TraceFill)  == 32, "fill record layout changed — bump the assert and confirm on-disk format is still what you expect");

[[noreturn]] void die(const char* msg) {
    std::fprintf(stderr, "error: %s\n", msg);
    std::exit(1);
}

// Runs `stream` through a fresh OrderBook, collecting fills into
// `out_fills`. Used both to produce the trace (record mode, driven off a
// freshly-generated stream) and to reproduce it (replay mode, driven off
// the stream read back from disk) — same function, same code path, so
// there's no room for the two modes to drift apart.
void run(const std::vector<TraceEvent>& stream, std::vector<TraceFill>& out_fills) {
    SymbolId sym = 1;
    auto on_match = [&](const ExecutionReport& r) {
        if (r.exec_type == ExecType::Fill || r.exec_type == ExecType::PartialFill) {
            // TraceFill f{} zero-initializes the whole object, including
            // compiler-inserted padding, before any member is set —
            // required so the padding is deterministically 0 in every
            // process, not indeterminate garbage. Brace-init with all
            // members supplied (`{a,b,c,d}`) does NOT guarantee that: it
            // only guarantees the named members, leaving padding
            // unspecified — which made replay's raw memcmp() flaky
            // (comparing garbage against different garbage across two
            // independent process runs) until this was caught running
            // under UBSan/valgrind-style scrutiny.
            TraceFill f{};
            f.aggr_id = r.order_id; f.passive_id = r.contra_order_id;
            f.price = r.exec_price; f.qty = r.exec_qty;
            out_fills.push_back(f);
        }
    };
    OrderBook book(sym, on_match);

    for (const auto& e : stream) {
        if (e.kind == 0) {
            Order o{};
            o.id            = e.id;
            o.symbol        = sym;
            o.side          = static_cast<Side>(e.side);
            o.type          = static_cast<OrderType>(e.type);
            o.price         = e.price;
            o.qty           = e.qty;
            o.qty_remaining = e.qty;
            o.status        = OrderStatus::New;
            o.account_id    = e.account_id;
            book.add_order(o);
        } else if (e.kind == 1) {
            book.cancel_order(e.id);
        } else {
            book.modify_order(e.id, e.qty);
        }
    }
}

int cmd_record(uint64_t seed, uint64_t num_events, const std::string& trace_path) {
    std::mt19937_64 rng(seed);
    std::vector<TraceEvent> stream;
    stream.reserve(num_events);
    std::vector<uint64_t> live;
    uint64_t next_id = 1;

    for (uint64_t i = 0; i < num_events; ++i) {
        uint32_t roll = uint32_t(rng() % 100);
        TraceEvent e{};
        if (roll < 70 || live.empty()) {
            e.kind       = 0;
            e.id         = next_id++;
            e.side       = uint8_t((rng() & 1) ? 0 : 1);
            e.price      = to_price(100.0) + int64_t((int64_t(rng() % 41) - 20) * 10'000);
            e.qty        = uint32_t(1 + rng() % 500);
            uint32_t t   = uint32_t(rng() % 100);
            e.type       = uint8_t(t < 80 ? 0 : t < 88 ? 1 : t < 95 ? 2 : 3);  // Limit/Market/IOC/FOK
            e.account_id = (rng() % 100 < 30) ? uint32_t(1 + rng() % 8) : 0;
            if (e.type == 0) live.push_back(e.id);  // optimistic; cancels/modifies on
                                                      // already-filled ids are harmless
                                                      // no-ops on both record and replay
        } else if (roll < 90) {
            e.kind = 1;
            e.id   = live[rng() % live.size()];
        } else {
            e.kind = 2;
            e.id   = live[rng() % live.size()];
            e.qty  = uint32_t(rng() % 800);
        }
        stream.push_back(e);
    }

    std::vector<TraceFill> fills;
    run(stream, fills);

    FILE* tf = std::fopen(trace_path.c_str(), "wb");
    if (!tf) die("could not open trace file for writing");
    std::fwrite(stream.data(), sizeof(TraceEvent), stream.size(), tf);
    std::fclose(tf);

    std::string fills_path = trace_path + ".fills";
    FILE* ff = std::fopen(fills_path.c_str(), "wb");
    if (!ff) die("could not open fills file for writing");
    std::fwrite(fills.data(), sizeof(TraceFill), fills.size(), ff);
    std::fclose(ff);

    std::printf("recorded %zu events, %zu fills -> %s (+ .fills)\n",
                stream.size(), fills.size(), trace_path.c_str());
    return 0;
}

int cmd_replay(const std::string& trace_path) {
    FILE* tf = std::fopen(trace_path.c_str(), "rb");
    if (!tf) die("could not open trace file for reading");
    std::fseek(tf, 0, SEEK_END);
    long tsize = std::ftell(tf);
    std::fseek(tf, 0, SEEK_SET);
    if (tsize < 0 || tsize % sizeof(TraceEvent) != 0) die("trace file size is not a multiple of record size");
    std::vector<TraceEvent> stream(size_t(tsize) / sizeof(TraceEvent));
    if (!stream.empty() &&
        std::fread(stream.data(), sizeof(TraceEvent), stream.size(), tf) != stream.size())
        die("short read on trace file");
    std::fclose(tf);

    std::string fills_path = trace_path + ".fills";
    FILE* ff = std::fopen(fills_path.c_str(), "rb");
    if (!ff) die("could not open companion .fills file for reading");
    std::fseek(ff, 0, SEEK_END);
    long fsize = std::ftell(ff);
    std::fseek(ff, 0, SEEK_SET);
    if (fsize < 0 || fsize % sizeof(TraceFill) != 0) die("fills file size is not a multiple of record size");
    std::vector<TraceFill> expected(size_t(fsize) / sizeof(TraceFill));
    if (!expected.empty() &&
        std::fread(expected.data(), sizeof(TraceFill), expected.size(), ff) != expected.size())
        die("short read on fills file");
    std::fclose(ff);

    std::vector<TraceFill> actual;
    run(stream, actual);

    bool same_size = actual.size() == expected.size();
    bool identical = same_size &&
        std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(TraceFill)) == 0;

    if (identical) {
        std::printf("PASS: %zu events replayed, %zu fills bit-identical to recorded trace\n",
                    stream.size(), actual.size());
        return 0;
    }

    std::fprintf(stderr, "FAIL: replay diverged from recorded trace (%zu events)\n", stream.size());
    std::fprintf(stderr, "  expected %zu fills, got %zu\n", expected.size(), actual.size());
    if (same_size) {
        for (size_t i = 0; i < actual.size(); ++i) {
            if (std::memcmp(&actual[i], &expected[i], sizeof(TraceFill)) != 0) {
                std::fprintf(stderr, "  first divergence at fill #%zu\n", i);
                std::fprintf(stderr, "    expected: aggr=%llu passive=%llu px=%lld qty=%u\n",
                    (unsigned long long)expected[i].aggr_id, (unsigned long long)expected[i].passive_id,
                    (long long)expected[i].price, expected[i].qty);
                std::fprintf(stderr, "    actual:   aggr=%llu passive=%llu px=%lld qty=%u\n",
                    (unsigned long long)actual[i].aggr_id, (unsigned long long)actual[i].passive_id,
                    (long long)actual[i].price, actual[i].qty);
                break;
            }
        }
    }
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "record") == 0) {
        if (argc != 5) die("usage: replay_trace record <seed> <num_events> <trace_file>");
        return cmd_record(std::strtoull(argv[2], nullptr, 10),
                           std::strtoull(argv[3], nullptr, 10),
                           argv[4]);
    }
    if (argc >= 2 && std::strcmp(argv[1], "replay") == 0) {
        if (argc != 3) die("usage: replay_trace replay <trace_file>");
        return cmd_replay(argv[2]);
    }
    std::fprintf(stderr,
        "usage:\n"
        "  replay_trace record <seed> <num_events> <trace_file>\n"
        "  replay_trace replay <trace_file>\n");
    return 1;
}
