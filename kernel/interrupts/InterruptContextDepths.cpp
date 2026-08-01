//
// vmsmalloc Phase 7 — interrupt-context tracking, out-of-line bodies.
//
// The InterruptContextGuard ctor/dtor and the currentCpuInterruptDepths
// accessor live here rather than in the header because they read/write the
// per-CPU kernel::CpuLocal struct, whose header (<CpuLocal.h>) includes
// <interrupts/InterruptContextDepths.h>. Defining them out-of-line keeps the
// include graph acyclic.
//
// All accesses are CPU-local with interrupts disabled at the dispatch site
// (P7-DEC-008), so no atomics or barriers are needed.
//

#include <interrupts/InterruptContextDepths.h>
#include <CpuLocal.h>
#include <assert.h>

namespace kernel::interrupts {

namespace {
    // Pointer to the per-CPU counter for a tracked kind, or nullptr for the
    // un-tracked InterruptKind::Other (the guard treats Other as a no-op).
    inline uint8_t* counterFor(InterruptContextDepths& d, arch::InterruptKind k) noexcept {
        switch (k) {
            case arch::InterruptKind::IRQ: return &d.irq;
            case arch::InterruptKind::NMI: return &d.nmi;
            case arch::InterruptKind::UD:  return &d.ud;
            case arch::InterruptKind::DF:  return &d.df;
            case arch::InterruptKind::GP:  return &d.gp;
            case arch::InterruptKind::MC:  return &d.mc;
            case arch::InterruptKind::PF:  return &d.pf;
            case arch::InterruptKind::Other: return nullptr;
        }
        return nullptr;
    }
}

const InterruptContextDepths& currentCpuInterruptDepths() noexcept {
    return kernel::cpuLocal().interruptDepths;
}

bool inForbiddenContext(uint64_t forbiddenMask) noexcept {
    return (currentCpuInterruptDepths().packed() & forbiddenMask) != 0;
}

InterruptContextGuard::InterruptContextGuard(arch::InterruptKind k) noexcept : kind_(k) {
    if (uint8_t* c = counterFor(kernel::cpuLocal().interruptDepths, kind_)) {
        // The counters are uint8_t (RCU-DEC-026). Nesting this deep is not
        // reachable — IRQ nesting is 0 or 1 given IF-clear dispatch, and
        // exception nesting hits #DF long first — but a wrapped counter would
        // silently disable every forbidden-context check, so it is worth an
        // assert rather than a comment.
        assert(*c != 0xFF, "InterruptContextDepths counter would overflow");
        ++*c;
    }
}

InterruptContextGuard::~InterruptContextGuard() noexcept {
    if (uint8_t* c = counterFor(kernel::cpuLocal().interruptDepths, kind_)) {
        assert(*c != 0, "InterruptContextDepths counter underflow — unbalanced guard");
        --*c;
    }
}

} // namespace kernel::interrupts
