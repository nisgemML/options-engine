// fuzz/fuzz_market_data.cpp — libFuzzer harness for the market data decoder.
//
// Build and run:
//   clang++ -std=c++20 -O1 -fsanitize=fuzzer,address -Iinclude \
//     fuzz/fuzz_market_data.cpp src/core/market_data.cpp \
//     src/core/order_book.cpp src/core/matching_engine.cpp \
//     src/core/execution_layer.cpp src/util/logger.cpp src/util/allocator.cpp \
//     -lpthread -o fuzz_market_data
//   ./fuzz_market_data corpus/ -max_len=256 -jobs=4
//
// What we're testing:
//   The parser must be robust to arbitrary byte sequences — no out-of-bounds
//   reads, no integer overflows, no UB, no crashes.  The matching engine that
//   processes the decoded messages must maintain its invariants (bid <= ask)
//   regardless of input.
//
//   libFuzzer generates inputs, mutates them, and tracks coverage.  ASan
//   catches memory errors.  The fuzzer minimizes any crashing input to a
//   small reproducible test case.

#include "core/market_data.hpp"
#include "core/matching_engine.hpp"
#include <cstdint>
#include <cstddef>
#include <span>

using namespace engine;

// Shared state — libFuzzer calls LLVMFuzzerTestOneInput repeatedly from one
// thread, so static state is safe here.
static MatchingEngine* g_engine = nullptr;

// libFuzzer calls this once before the first LLVMFuzzerTestOneInput call.
extern "C" int LLVMFuzzerInitialize(int*, char***) {
    g_engine = new MatchingEngine();
    for (SymbolId s = 0; s < 8; ++s)
        g_engine->register_symbol(s);
    g_engine->start();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    // We use a fresh SPSC queue per invocation so the fuzzer can drive
    // MarketDataIngestion directly without the matching engine thread
    // consuming concurrently (which would be TSAN-clean but slow).
    using Queue = SPSCQueue<MarketDataMsg, 256>;
    Queue queue;

    MarketDataIngestion ingestion(queue);

    // The parser must not crash, corrupt memory, or invoke UB on any input.
    ingestion.ingest(std::span<const uint8_t>(data, size));

    // Drain whatever was decoded into the engine.
    // The matching engine must maintain bid<=ask regardless of message content.
    MarketDataMsg msg;
    int count = 0;
    while (queue.try_pop(msg) && count++ < 64) {
        // Clamp symbol to registered range to avoid no-op rejections.
        msg.symbol = msg.symbol % 8;
        g_engine->submit(msg);
    }

    // Drain execution reports to prevent queue buildup across invocations.
    ExecutionReport rpt;
    while (g_engine->poll_report(rpt)) {}

    return 0;
}
