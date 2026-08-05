#pragma once

// PerfCounters — RAII wrapper around Linux perf_event for hardware counters.
//
// Allows inline measurement of:
//   - CPU cycles
//   - Instructions retired
//   - L1 cache misses
//   - LLC (last-level cache) misses
//   - Branch mispredictions
//
// Usage:
//   PerfCounters perf;
//   perf.start();
//   // ... code to measure ...
//   auto r = perf.stop();
//   printf("cycles=%lu  L1_misses=%lu  IPC=%.2f\n",
//          r.cycles, r.l1_misses, r.ipc());
//
// Why this over `perf stat`:
//   perf stat measures the entire process lifetime.  This wrapper lets you
//   isolate a specific code path — e.g. measure the cost of a single
//   order book lookup, or compare two implementations of the same function
//   under identical conditions.

#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>

namespace engine {

struct PerfResult {
    uint64_t cycles;
    uint64_t instructions;
    uint64_t l1_misses;
    uint64_t llc_misses;
    uint64_t branch_misses;
    uint64_t elapsed_ns;

    [[nodiscard]] double ipc() const noexcept {
        return cycles > 0 ? static_cast<double>(instructions) / cycles : 0.0;
    }

    [[nodiscard]] double l1_miss_rate() const noexcept {
        return instructions > 0
            ? static_cast<double>(l1_misses) / instructions * 100.0 : 0.0;
    }

    [[nodiscard]] double ns_per_cycle() const noexcept {
        return cycles > 0 ? static_cast<double>(elapsed_ns) / cycles : 0.0;
    }

    void print(const char* label = "") const noexcept {
        printf("PerfCounters [%s]\n", label);
        printf("  Cycles        : %lu\n",  cycles);
        printf("  Instructions  : %lu  (IPC: %.2f)\n", instructions, ipc());
        printf("  L1 misses     : %lu  (%.2f%%)\n", l1_misses, l1_miss_rate());
        printf("  LLC misses    : %lu\n",  llc_misses);
        printf("  Branch misses : %lu\n",  branch_misses);
        printf("  Elapsed       : %lu ns\n", elapsed_ns);
    }
};

class PerfCounters {
public:
    PerfCounters() {
        open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,        fd_cycles_);
        open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,      fd_insns_);
        open_counter(PERF_TYPE_HW_CACHE,
                     (PERF_COUNT_HW_CACHE_L1D) |
                     (PERF_COUNT_HW_CACHE_OP_READ    << 8) |
                     (PERF_COUNT_HW_CACHE_RESULT_MISS << 16), fd_l1_miss_);
        open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES,      fd_llc_miss_);
        open_counter(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES,     fd_br_miss_);
    }

    ~PerfCounters() {
        for (int fd : {fd_cycles_, fd_insns_, fd_l1_miss_, fd_llc_miss_, fd_br_miss_})
            if (fd >= 0) ::close(fd);
    }

    PerfCounters(const PerfCounters&) = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    [[nodiscard]] bool available() const noexcept { return fd_cycles_ >= 0; }

    void start() noexcept {
        for (int fd : {fd_cycles_, fd_insns_, fd_l1_miss_, fd_llc_miss_, fd_br_miss_})
            if (fd >= 0) { ioctl(fd, PERF_EVENT_IOC_RESET, 0); ioctl(fd, PERF_EVENT_IOC_ENABLE, 0); }
        clock_gettime(CLOCK_MONOTONIC, &t0_);
    }

    [[nodiscard]] PerfResult stop() noexcept {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);

        for (int fd : {fd_cycles_, fd_insns_, fd_l1_miss_, fd_llc_miss_, fd_br_miss_})
            if (fd >= 0) ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

        PerfResult r{};
        read_counter(fd_cycles_,   r.cycles);
        read_counter(fd_insns_,    r.instructions);
        read_counter(fd_l1_miss_,  r.l1_misses);
        read_counter(fd_llc_miss_, r.llc_misses);
        read_counter(fd_br_miss_,  r.branch_misses);
        r.elapsed_ns = static_cast<uint64_t>(t1.tv_sec  - t0_.tv_sec)  * 1'000'000'000ULL
                     + static_cast<uint64_t>(t1.tv_nsec - t0_.tv_nsec);
        return r;
    }

private:
    static void open_counter(uint32_t type, uint64_t config, int& fd) noexcept {
        perf_event_attr attr{};
        attr.type           = type;
        attr.size           = sizeof(attr);
        attr.config         = config;
        attr.disabled       = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv     = 1;

        fd = static_cast<int>(syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
        if (fd < 0) {
            // perf_event_open fails if kernel.perf_event_paranoid > 1.
            // Silently degrade — available() returns false.
        }
    }

    static void read_counter(int fd, uint64_t& out) noexcept {
        if (fd >= 0) ::read(fd, &out, sizeof(out));
    }

    int fd_cycles_  = -1;
    int fd_insns_   = -1;
    int fd_l1_miss_ = -1;
    int fd_llc_miss_= -1;
    int fd_br_miss_ = -1;
    struct timespec t0_{};
};

// ── RAII scope guard ──────────────────────────────────────────────────────────
// Measures and prints hardware counters for a lexical scope.
//
//   {
//     PerfScope scope("order_book_lookup");
//     book.add_order(o);
//   }  // prints counters on destruction

class PerfScope {
public:
    explicit PerfScope(const char* label) : label_(label) {
        if (perf_.available()) perf_.start();
    }

    ~PerfScope() {
        if (perf_.available()) perf_.stop().print(label_);
    }

private:
    PerfCounters perf_;
    const char*  label_;
};

} // namespace engine
