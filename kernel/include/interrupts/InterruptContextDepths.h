//
// InterruptContextDepths — per-CPU counters for nested interrupt contexts
// (vmsmalloc Phase 4.5 ships the struct only; Phase 7 adds the
// InterruptKind enum, kindForVector mapping, and InterruptContextGuard RAII
// type to this same header).
//
// Each counter is decremented and incremented by Phase 7's
// InterruptContextGuard around dispatchInterrupt. Phase 7's entry-point
// asserts on vmsmalloc / vmsfree consult these counters to enforce the
// forbidden-context invariant (DEC-014: vmsmalloc must not run from IRQ,
// NMI, #UD, #DF, #GP, #MC; #PF is conditionally legal per the amendment).
//
// The struct is a field of kernel::CpuLocal. Cache-line aligned so the
// surrounding CpuLocal fields don't false-share with the depth counters
// under contended access patterns.
//

#ifndef CROCOS_INTERRUPT_CONTEXT_DEPTHS_H
#define CROCOS_INTERRUPT_CONTEXT_DEPTHS_H

#include <stddef.h>
#include <stdint.h>
#include <arch.h>   // arch::InterruptKind (portable interrupt classification)

namespace kernel::interrupts {

// arch::cacheLineSize is not exposed as a kernel-wide constant; hardcoded
// to 64 per CLAUDE.md (matches Phase 3's kVmsmallocCacheLine usage).
inline constexpr unsigned int kInterruptContextDepthsAlignment = 64;

// The seven counters are uint8_t and packed into the struct's first eight
// bytes so that "am I in a forbidden context?" is one aligned 64-bit load, a
// mask and a test, rather than a load-and-branch per kind. See RCU spec
// RCU-DEC-026. Depth headroom is ample: dispatchInterrupt runs with IF clear
// for interrupt-gate vectors, so IRQ nesting is 0 or 1, and exception nesting
// hits #DF long before 255.
//
// `reserved` is explicit rather than implicit padding: packed() reads all
// eight bytes, so the eighth must be a real, zero-valued object rather than
// padding whose value is unspecified.
struct alignas(kInterruptContextDepthsAlignment) InterruptContextDepths {
    uint8_t irq;   // external IRQ (vector >= 32)
    uint8_t nmi;   // vector 2
    uint8_t ud;    // vector 6  (#UD undefined opcode)
    uint8_t df;    // vector 8  (#DF double fault)
    uint8_t gp;    // vector 13 (#GP general protection)
    uint8_t mc;    // vector 18 (#MC machine check)
    uint8_t pf;    // vector 14 (#PF page fault)
    uint8_t reserved;  // must remain zero — part of the packed word

    // The seven counters plus `reserved` as one 64-bit word. memcpy rather
    // than a union or a reinterpret_cast: union type-punning is UB in standard
    // C++, and this compiles to a single load.
    [[nodiscard]] uint64_t packed() const noexcept {
        uint64_t w;
        __builtin_memcpy(&w, this, sizeof(w));
        return w;
    }

    // True if the CPU is inside ANY tracked interrupt context.
    [[nodiscard]] bool inAnyInterruptContext() const noexcept { return packed() != 0; }
};

static_assert(alignof(InterruptContextDepths) == kInterruptContextDepthsAlignment);

// Byte offsets are pinned so the masks below cannot silently desync if the
// fields are ever reordered.
static_assert(offsetof(InterruptContextDepths, irq)      == 0);
static_assert(offsetof(InterruptContextDepths, nmi)      == 1);
static_assert(offsetof(InterruptContextDepths, ud)       == 2);
static_assert(offsetof(InterruptContextDepths, df)       == 3);
static_assert(offsetof(InterruptContextDepths, gp)       == 4);
static_assert(offsetof(InterruptContextDepths, mc)       == 5);
static_assert(offsetof(InterruptContextDepths, pf)       == 6);
static_assert(offsetof(InterruptContextDepths, reserved) == 7);

// packed() maps the byte at offset k to bits [8k, 8k+8), which is a
// little-endian assumption. CroCOS targets AMD64 and ARMv8 (little-endian);
// a big-endian port must reverse the shift in depthMask().
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "InterruptContextDepths mask layout assumes a little-endian target");

inline constexpr uint64_t depthMask(size_t byteOffset) noexcept {
    return static_cast<uint64_t>(0xFF) << (byteOffset * 8);
}

inline constexpr uint64_t kIrqDepthMask = depthMask(0);
inline constexpr uint64_t kNmiDepthMask = depthMask(1);
inline constexpr uint64_t kUdDepthMask  = depthMask(2);
inline constexpr uint64_t kDfDepthMask  = depthMask(3);
inline constexpr uint64_t kGpDepthMask  = depthMask(4);
inline constexpr uint64_t kMcDepthMask  = depthMask(5);
inline constexpr uint64_t kPfDepthMask  = depthMask(6);

// ─── Context policies ──────────────────────────────────────────────────────
//
// vmsmalloc DEC-014: vmsmalloc / vmsfree (and therefore RCU's retire and drain
// paths, whose deleters bottom out in vmsfree) must not run from IRQ, NMI,
// #UD, #DF, #GP or #MC context. #PF is deliberately EXCLUDED — it is
// conditionally legal, subject to the caller-side obligation that VMSubstrate
// and its consumers do not fault during operation.
inline constexpr uint64_t kAllocForbiddenDepthMask =
    kIrqDepthMask | kNmiDepthMask | kUdDepthMask |
    kDfDepthMask  | kGpDepthMask  | kMcDepthMask;

// RCU-DEC-024: RCU read-side critical sections are forbidden in NMI and #MC
// context only. Both are unmaskable, so the outermost readLock/readUnlock
// transition cannot be made atomic against them.
inline constexpr uint64_t kRcuReadSideForbiddenDepthMask = kNmiDepthMask | kMcDepthMask;

// ─── Phase 7 — interrupt-context tracking consumer logic ───────────────────
//
// The RAII guard that maintains the per-CPU depth counters around
// dispatchInterrupt, keyed by the portable arch::InterruptKind. The
// vector→kind classification itself is architecture-specific and lives behind
// arch::interruptKind (see arch.h); this layer only tracks depths per kind.
// The guard's bodies and the currentCpuInterruptDepths accessor live in
// InterruptContextDepths.cpp (they touch kernel::cpuLocal(), whose header
// includes THIS one — defining them out-of-line keeps the include graph
// acyclic).

// RAII guard constructed at the top of dispatchInterrupt: the constructor
// increments the matching per-CPU depth counter, the destructor decrements it.
// arch::InterruptKind::Other is a no-op (no counter). CPU-local, no atomics —
// the dispatch path runs with interrupts disabled.
class InterruptContextGuard {
public:
    explicit InterruptContextGuard(arch::InterruptKind k) noexcept;
    ~InterruptContextGuard() noexcept;
    InterruptContextGuard(const InterruptContextGuard&) = delete;
    InterruptContextGuard& operator=(const InterruptContextGuard&) = delete;
private:
    arch::InterruptKind kind_;
};

// True if the calling CPU is inside any interrupt context named by
// `forbiddenMask`. Pass one of the k*ForbiddenDepthMask constants above.
// One aligned 64-bit load, one AND, one test.
//
// This is the single shared implementation of the DEC-014 context rules.
// It was previously duplicated as a file-local predicate in vmsmalloc.cpp;
// RCU consumes the same policy, so it lives here rather than being copied.
[[nodiscard]] bool inForbiddenContext(uint64_t forbiddenMask) noexcept;

// Read-only view of the calling CPU's depth counters (defined in the .cpp,
// where kernel::cpuLocal() is reachable).
const InterruptContextDepths& currentCpuInterruptDepths() noexcept;

} // namespace kernel::interrupts

#endif // CROCOS_INTERRUPT_CONTEXT_DEPTHS_H
