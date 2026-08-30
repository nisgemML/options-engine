cmake_minimum_required(VERSION 3.22)
project(trading_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Release build optimizations
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -march=native -g -DNDEBUG")

# Common flags
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -fno-exceptions          # latency: no exception overhead
    -fno-rtti               # latency: no RTTI overhead
)

# Thread sanitizer support
option(TSAN "Enable ThreadSanitizer" OFF)
if(TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()

option(ASAN "Enable AddressSanitizer" OFF)
if(ASAN)
    add_compile_options(-fsanitize=address -g -O1)
    add_link_options(-fsanitize=address)
endif()

find_package(Threads REQUIRED)

# ── Core library ─────────────────────────────────────────────────────────────
add_library(engine_core STATIC
    src/core/order_book.cpp
    src/core/matching_engine.cpp
    src/core/market_data.cpp
    src/core/execution_layer.cpp
    src/util/logger.cpp
    src/util/allocator.cpp
)

target_include_directories(engine_core PUBLIC include)
target_link_libraries(engine_core PUBLIC Threads::Threads)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(trading_engine src/main.cpp)
target_link_libraries(trading_engine PRIVATE engine_core)

# ── Benchmarks ────────────────────────────────────────────────────────────────
add_executable(bench_latency bench/bench_latency.cpp)
target_link_libraries(bench_latency PRIVATE engine_core)

add_executable(bench_throughput bench/bench_throughput.cpp)
target_link_libraries(bench_throughput PRIVATE engine_core)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE engine_core)
add_test(NAME OrderBook COMMAND test_order_book)

# Fast unit tests — in ctest, <1ms
add_executable(test_spsc_unit tests/test_spsc_unit.cpp)
target_link_libraries(test_spsc_unit PRIVATE engine_core)
add_test(NAME SPSCUnit COMMAND test_spsc_unit)

# Stress tests — NOT in ctest, run manually (~30s)
add_executable(test_spsc_stress tests/test_spsc.cpp)
target_link_libraries(test_spsc_stress PRIVATE engine_core)

add_executable(test_matching tests/test_matching.cpp)
target_link_libraries(test_matching PRIVATE engine_core)
add_test(NAME Matching COMMAND test_matching)

add_executable(test_allocator tests/test_allocator.cpp)
target_link_libraries(test_allocator PRIVATE engine_core)
add_test(NAME Allocator COMMAND test_allocator)

add_executable(test_mpmc tests/test_mpmc.cpp)
target_link_libraries(test_mpmc PRIVATE engine_core)
add_test(NAME MPMC COMMAND test_mpmc)

add_executable(bench_avx2 bench/bench_avx2.cpp)
target_link_libraries(bench_avx2 PRIVATE engine_core)

add_executable(bench_multisymbol bench/bench_multisymbol.cpp)
target_link_libraries(bench_multisymbol PRIVATE engine_core)

add_executable(test_conservation tests/test_conservation.cpp)
target_link_libraries(test_conservation PRIVATE engine_core)
add_test(NAME Conservation COMMAND test_conservation 1 1000000)

add_executable(test_histogram tests/test_histogram.cpp)
target_link_libraries(test_histogram PRIVATE engine_core)
add_test(NAME Histogram COMMAND test_histogram)

add_executable(bench_cache bench/bench_cache.cpp)
target_link_libraries(bench_cache PRIVATE engine_core)

add_executable(bench_replay bench/bench_replay.cpp)
target_link_libraries(bench_replay PRIVATE engine_core)
