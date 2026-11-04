// SPDX-License-Identifier: GPL-2.0-or-later
// GOW3_FRAME_LOG line format and clock (gow3_frame_log.h). No device needed.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

#include "gow3_frame_log.h"

int main() {
    using namespace Gow3FrameLog;
    assert(std::string(Header) == "# ms_of_day steady_us draws a_cpu_us b_cycles tsc\n");
    assert(Format({37991690, 2, 3, 4, 5, 6}) == "37991690 2 3 4 5 6\n");
    // Large counters print in full, no exponent or truncation.
    assert(Format({0, 18446744073709551615ull, 1, 1, 1, 1}) ==
           "0 18446744073709551615 1 1 1 1\n");
    // 10:33:11.690 local time is 37 991 690 ms after midnight.
    std::tm local{};
    local.tm_year = 126;
    local.tm_mon = 10;
    local.tm_mday = 4;
    local.tm_hour = 10;
    local.tm_min = 33;
    local.tm_sec = 11;
    local.tm_isdst = -1;
    const auto at = std::chrono::system_clock::from_time_t(std::mktime(&local)) +
                    std::chrono::milliseconds(690);
    assert(MsOfDay(at) == 37991690u);
    std::puts("frame log: ok");
}
