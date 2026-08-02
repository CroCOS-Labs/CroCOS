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
    constexpr uint64_t kDefaultStepNs = 1000;
    Atomic<uint64_t> gStepNs{kDefaultStepNs};
}

uint64_t monoTimens() {
    const uint64_t step = gStepNs.load(RELAXED);
    // Frozen: a plain load, NOT a fetch_add of zero. The distinction is worth
    // 34% of on-CPU time in the 8-thread soak — profiled 2026-08-01, where this
    // function was the single largest cost in the whole process. ReadGuard calls
    // it twice per section for RCU-DEC-013's stall stamp, so a SEQ_CST RMW on one
    // shared word had every modelled CPU ping-ponging the same cache line on the
    // read path. A shared *read* keeps the line in Shared state in every cache
    // and costs nothing.
    //
    // The global timeline is deliberately preserved rather than switching to a
    // thread_local counter: in the kernel monoTimens IS a single clock all CPUs
    // agree on, and a per-thread mock would diverge from that for no gain.
    if (step == 0) return gMonoNs.load(RELAXED);
    return gMonoNs.fetch_add(step, SEQ_CST) + step;
}
uint64_t monoTimems() { return monoTimens() / 1'000'000; }

namespace test {
    void resetMonoTime() noexcept {
        gMonoNs.store(0, SEQ_CST);
        gStepNs.store(kDefaultStepNs, RELAXED);
    }
    void advanceMonoTime(uint64_t ns) noexcept { gMonoNs.fetch_add(ns, SEQ_CST); }

    // The counter is SHARED across the threads modelling CPUs, so the per-call
    // step makes a section's measured "elapsed" a function of how many calls
    // every OTHER thread made meanwhile, not of that section's own duration.
    // Harmless at unit-test volumes; fatal for a soak. With N threads looping
    // sections, elapsed crosses RCU-DEC-013's 100 ms threshold constantly, and
    // each warning takes AtomicPrintStream's PROCESS-WIDE spinlock — so the
    // diagnostic would serialise every thread it is meant to be observing.
    //
    // Setting the step to 0 removes the artifact, not a real signal: in the
    // kernel monoTimens is a real clock and a short section genuinely does not
    // warn. Tests that drive the detector on purpose use advanceMonoTime.
    void setMonoStep(uint64_t ns) noexcept { gStepNs.store(ns, RELAXED); }
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
