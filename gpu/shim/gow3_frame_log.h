// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GOW3_FRAME_LOG=<file> (with GOW3_FRAME_STATS=1) writes one line per flip with cumulative
// counters, so a benchmark can cut any span of frames afterwards (tools/deck/bench-summary.py).
#pragma once

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace Gow3FrameLog {

/// The file's first line: the columns of every following line.
inline constexpr const char* Header = "# ms_of_day steady_us draws a_cpu_us b_cycles tsc\n";

struct Sample {
    std::uint32_t ms_of_day; ///< local wall clock, the clock of stamp.py and screenshot names
    std::uint64_t steady_us; ///< monotonic clock
    std::uint64_t draws;     ///< Gow3Stats::draws
    std::uint64_t a_cpu_us;  ///< GPU command thread (stage A) user + system CPU time
    std::uint64_t b_cycles;  ///< TSC cycles the draw recording thread (stage B) ran packets
    std::uint64_t tsc;       ///< __rdtsc() at the flip: converts b_cycles to time
};

[[nodiscard]] inline std::string Format(const Sample& s) {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "%" PRIu32 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
                  s.ms_of_day, s.steady_us, s.draws, s.a_cpu_us, s.b_cycles, s.tsc);
    return line;
}

/// Milliseconds since local midnight.
[[nodiscard]] inline std::uint32_t MsOfDay(std::chrono::system_clock::time_point now) {
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    return std::uint32_t(((local.tm_hour * 60 + local.tm_min) * 60 + local.tm_sec) * 1000 + ms);
}

} // namespace Gow3FrameLog
