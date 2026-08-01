//
// RCU Phase 2 harness — bodies for MockRcuEnv.h.
//

#include <arch.h>
#include <timing/timing.h>   // mock (include-path ordering)
#include <core/atomic.h>

#include "MockRcuEnv.h"

// ─── kernel::timing::monoTimens ─────────────────────────────────────────────
//
// A shared counter, not a real clock. Two reasons. Determinism: a wall-clock
// mock would make the stall-detector threshold sensitive to how loaded the
// machine is, which is exactly how a suite acquires a flaky test. And
// controllability: advanceMonoTime lets a test cross a 100 ms threshold without
// waiting 100 ms.
//
// Atomic rather than plain, and shared rather than thread_local: harness threads
// model CPUs and several of them call this concurrently, so a plain global would
// be a genuine data race and the TSan runner would (correctly) say so.
namespace kernel::timing {
namespace {
    Atomic<uint64_t> gMonoNs{0};
    // Per-call step. Small enough that no ordinary section trips the 100 ms
    // stall threshold by accident, large enough to be visibly monotonic.
    constexpr uint64_t kStepNs = 1000;
}

uint64_t monoTimens() { return gMonoNs.fetch_add(kStepNs, SEQ_CST) + kStepNs; }
uint64_t monoTimems() { return monoTimens() / 1'000'000; }

namespace test {
    void resetMonoTime() noexcept { gMonoNs.store(0, SEQ_CST); }
    void advanceMonoTime(uint64_t ns) noexcept { gMonoNs.fetch_add(ns, SEQ_CST); }
}
}

// ─── arch::InterruptDisabler ────────────────────────────────────────────────
//
// Userspace has no interrupt flag, so the mock models the one property the
// tests can actually check: nesting depth. `wasEnabled` is kept faithful to the
// real implementation's meaning (were interrupts enabled on entry) so the
// save/restore shape is preserved rather than stubbed away — that shape is what
// the 2026-08-01 fix to the real InterruptDisabler restored, and a mock that
// dropped it would stop modelling the thing being relied on.
namespace arch {
namespace { thread_local unsigned tlMaskDepth = 0; }

InterruptDisabler::InterruptDisabler() {
    wasEnabled = (tlMaskDepth == 0);
    active     = true;
    tlMaskDepth++;
}

void InterruptDisabler::release() {
    if (active) {
        active = false;
        tlMaskDepth--;
    }
}

InterruptDisabler::~InterruptDisabler() { release(); }

namespace test {
    unsigned interruptMaskDepth() noexcept { return tlMaskDepth; }
}
}
