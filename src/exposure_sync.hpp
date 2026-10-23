#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

namespace dvxrec {

// Tracks active-low Daheng ExposureActive edges in the DVXplorer clock domain.
// AEDAT4 retains the original triggers; this only checks the edge sequence.
struct ExposureSync {
    uint64_t starts = 0;
    uint64_t ends = 0;
    uint64_t completeWindows = 0;
    uint64_t unmatchedEnds = 0;
    uint64_t repeatedStarts = 0;
    uint64_t invalidDurations = 0;
    uint64_t zeroDurationWindows = 0;
    std::optional<int64_t> pendingStartUs;
    std::optional<int64_t> shortestWindowUs;
    std::optional<int64_t> longestWindowUs;

    std::optional<std::pair<int64_t, int64_t>> observe(bool falling, int64_t timestampUs) {
        if (falling) {
            ++starts;
            if (pendingStartUs) ++repeatedStarts;
            pendingStartUs = timestampUs;
            return std::nullopt;
        }
        ++ends;
        if (!pendingStartUs) {
            ++unmatchedEnds;
            return std::nullopt;
        }
        const int64_t start = *pendingStartUs;
        pendingStartUs.reset();
        if (timestampUs < start) {
            ++invalidDurations;
            return std::nullopt;
        }
        const int64_t duration = timestampUs - start;
        if (duration == 0) ++zeroDurationWindows; // Both edges can quantize to one camera tick.
        shortestWindowUs = shortestWindowUs ? std::min(*shortestWindowUs, duration) : duration;
        longestWindowUs = longestWindowUs ? std::max(*longestWindowUs, duration) : duration;
        ++completeWindows;
        return std::pair{start, timestampUs};
    }
};

} // namespace dvxrec
