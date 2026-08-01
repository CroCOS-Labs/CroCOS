//
// Control surface for the harness's per-thread InterruptContextDepths.
//
// The depths themselves live in MockKernelEnv.cpp (thread_local, zero by
// default). vmsmalloc's own tests never touch them — userspace has no
// interrupts, so its DEC-014 forbidden-context check is vacuously satisfied.
// RCU Phase 2 needs them settable: its context rules are TWO-TIER (retire
// tolerates #PF, synchronize/barrier do not), and with permanently-zero depths
// the discrimination between the two masks cannot be tested at all.
//
// Set a depth by writing the matching counter, e.g.
//
//     kernel::interrupts::test::mutableInterruptDepths().pf = 1;
//
// and use the RAII guard below rather than hand-restoring — a test that throws
// mid-scope (which every EXPECT_ASSERT_FAILURE does) would otherwise leave the
// thread permanently "in interrupt context" for every later test on it.
//

#ifndef CROCOS_MOCK_INTERRUPT_CONTEXT_H
#define CROCOS_MOCK_INTERRUPT_CONTEXT_H

#include <interrupts/InterruptContextDepths.h>   // real kernel header

namespace kernel::interrupts::test {

    InterruptContextDepths& mutableInterruptDepths() noexcept;
    void resetInterruptDepths() noexcept;

    // Enters a mock interrupt context for the calling thread and leaves it on
    // scope exit, whatever the exit path.
    class ScopedContext {
    public:
        explicit ScopedContext(uint8_t InterruptContextDepths::* counter) noexcept
            : counter_(counter) {
            mutableInterruptDepths().*counter_ += 1;
        }
        ~ScopedContext() noexcept { mutableInterruptDepths().*counter_ -= 1; }

        ScopedContext(const ScopedContext&)            = delete;
        ScopedContext& operator=(const ScopedContext&) = delete;

    private:
        uint8_t InterruptContextDepths::* counter_;
    };

}   // namespace kernel::interrupts::test

#endif // CROCOS_MOCK_INTERRUPT_CONTEXT_H
