//
// RCU Phase 2 harness — the two environment pieces vmsmalloc's mocks do not
// supply, plus their test control surfaces.
//
//   1. kernel::timing::monoTimens — no userspace mock existed anywhere under
//      tests/ before this phase. Its only consumer is RCU-DEC-013's
//      section-duration stall detector.
//
//   2. arch::InterruptDisabler — RCU is its first consumer in a harness build.
//      ReadGuard depends on it for RCU-DEC-024, and its real body lives in
//      kernel/arch/arch.cpp, which no userspace harness compiles.
//
// Everything else (CpuLocal binding, klog, panic, NUMA policy, interrupt
// depths, the mmap-backed VMSubstrate) is reused from tests/kernel/vmsmalloc/
// mocks/ rather than duplicated — see this directory's CMakeLists.
//

#ifndef CROCOS_MOCK_RCU_ENV_H
#define CROCOS_MOCK_RCU_ENV_H

#include <stdint.h>
#include <stddef.h>

namespace kernel::timing::test {
    // Reset the shared monotonic counter. Call between tests so elapsed-time
    // expectations do not depend on how many calls preceding tests made.
    void resetMonoTime() noexcept;
    // Jump the counter forward, for driving the stall detector past its
    // threshold without executing 100 ms of anything.
    void advanceMonoTime(uint64_t ns) noexcept;
}

namespace arch::test {
    // How many arch::InterruptDisabler instances the calling thread currently
    // holds. RCU-DEC-025 says the mask covers the outermost readLock/readUnlock
    // TRANSITION and never the section body; this is what lets a test check the
    // second half of that claim, which is the half that would otherwise fail
    // silently and only show up as latency.
    unsigned interruptMaskDepth() noexcept;
}

#endif // CROCOS_MOCK_RCU_ENV_H
