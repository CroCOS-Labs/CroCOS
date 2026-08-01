//
// RCU Phase 2 harness — mock <timing/timing.h>.
//
// Shadows the real kernel header via include-path ordering, and uses the real
// header's include guard so it fully replaces it. The real one pulls in
// timing/Clock.h and the whole ClockSource / EventSource hierarchy, none of
// which the harness has any way to satisfy; the only symbol RCU.cpp actually
// needs is monoTimens, for RCU-DEC-013's section-duration stall detector.
//
// No userspace mock for this existed anywhere under tests/ before Phase 2.
//

#ifndef CROCOS_CLOCKMANAGER_H
#define CROCOS_CLOCKMANAGER_H

#include <stdint.h>

namespace kernel::timing {

    // Monotonic nanoseconds. The harness backend advances a shared counter on
    // every call (see MockRcuEnv.cpp) rather than reading a real clock, so
    // elapsed times are deterministic and no test can be flaky on a loaded
    // machine.
    uint64_t monoTimens();
    uint64_t monoTimems();

}   // namespace kernel::timing

#endif // CROCOS_CLOCKMANAGER_H
