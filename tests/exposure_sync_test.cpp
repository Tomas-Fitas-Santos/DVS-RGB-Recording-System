#include "exposure_sync.hpp"

#include <cassert>

int main() {
    dvxrec::ExposureSync sync;
    assert(!sync.observe(false, 100)); // A rising edge without a start.
    assert(!sync.observe(true, 200));
    assert(!sync.observe(true, 210)); // Repeated start replaces the incomplete one.
    const auto first = sync.observe(false, 1210);
    assert(first && first->first == 210 && first->second == 1210);
    assert(!sync.observe(true, 1300));
    const auto shortPulse = sync.observe(false, 1300); // Two edges on one DVX tick.
    assert(shortPulse && shortPulse->first == shortPulse->second);
    assert(!sync.observe(true, 2000));
    assert(!sync.observe(false, 1999)); // Reversed timestamps cannot form a valid window.
    assert(!sync.observe(true, 3000)); // A final unmatched start is reported at stop.
    assert(sync.pendingStartUs == 3000);
    assert(sync.starts == 5 && sync.ends == 4);
    assert(sync.completeWindows == 2);
    assert(sync.repeatedStarts == 1 && sync.unmatchedEnds == 1);
    assert(sync.invalidDurations == 1 && sync.zeroDurationWindows == 1);
    assert(sync.shortestWindowUs == 0 && sync.longestWindowUs == 1000);
}
