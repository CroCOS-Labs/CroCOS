//
// Control surface for the harness's kernel::klog() sink.
//
// The sink itself lives in MockKernelEnv.cpp and discards by default, which is
// what vmsmalloc's tests want — they assert on state, never on log text.
//
// RCU Phase 3 needs it capturable for exactly one reason, and it is a
// load-bearing one: the parent Hazards say "a torture suite that cannot fail is
// worse than none", and name the forced-stall scenario specifically — it must
// assert that reclamation stalls AND that RCU-DEC-013's stall diagnostic fires.
// The diagnostic's only observable is a klog line from ~ReadGuard, so without a
// capture the second half of that assertion could only be trusted, not tested.
//
// No std::string in this header: the buffer is an implementation detail of
// MockKernelEnv.cpp, and every question a test needs to ask about it is a
// predicate.
//
// Capture is process-global and NOT nestable. Tests using it should be
// TEST_WITH_TIMEOUT_NO_TRACKING — the buffer allocates, and the leak tracker
// would otherwise attribute that to whichever test happened to log.
//

#ifndef CROCOS_MOCK_KLOG_H
#define CROCOS_MOCK_KLOG_H

#include <stddef.h>

namespace kernel::test {

    // Clear the buffer and start recording everything written to klog().
    void beginKlogCapture() noexcept;

    // Stop recording. The buffer survives, so predicates below stay answerable
    // after the scope that produced the output has exited.
    void endKlogCapture() noexcept;

    // Does the captured text contain `needle`? Substring match, no globbing.
    [[nodiscard]] bool klogCaptureContains(const char* needle) noexcept;

    [[nodiscard]] size_t klogCaptureLength() noexcept;

}   // namespace kernel::test

#endif // CROCOS_MOCK_KLOG_H
