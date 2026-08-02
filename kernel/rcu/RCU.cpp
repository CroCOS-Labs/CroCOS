//
// RCU Phase 2 — the kernel veneer over Core::rcu::EpochDomain.
// Created by Spencer Martin on 8/1/26.
//
// Implements kernel/include/rcu/RCU.h. See specs/rcu-phase-2.md; the header
// carries the consumer-facing contract and this file carries the binding.
//
// Three things in here are worth reading before editing:
//
//   1. The engine type appears NOWHERE outside this TU (P2-DEC-009). Domain
//      holds opaque bytes; detail::Access launders them back into an
//      EpochDomain<KernelRcuHooks>. That is what keeps the Core template out of
//      every consumer TU and lets Domain's constructor stay constexpr-inert.
//
//   2. Slot storage geometry. EpochDomain indexes slots[i] at natural stride, so
//      the slot array must be DENSE and contiguous, while
//      reservePerDomainStaticBuffer places at PAGE granularity. Those two facts
//      together make "CPU i's slot on CPU i's NUMA domain" (P2-I4 as originally
//      written) unachievable; see the P2-I4 note on reserveSlots below for what
//      is achieved instead.
//
//   3. Every check is debug-only (RCU-DEC-013 / P2-DEC-005), gated on
//      CROCOS_RCU_DEBUG_CHECKS. In release the read side is one store plus one
//      fence, and the veneer is a call into the engine with no preamble.
//

#include <stddef.h>
#include <stdint.h>

#include <arch.h>
#include <kassert.h>
#include <kernel.h>
#include <CpuLocal.h>
#include <interrupts/InterruptContextDepths.h>
#include <mem/NUMA.h>
#include <mem/VMSubstrate.h>
#include <timing/timing.h>
#include <core/math.h>
#include <core/utility.h>
#include <core/rcu/EpochDomain.h>

#include <rcu/RCU.h>

#ifdef CROCOS_RCU_TEST_HARNESS
#include <core/rcu/DebugIntrospection.h>   // Core's, for the forwarding layer
#include <DebugIntrospection.h>            // the veneer's (test include path)
#endif

namespace kernel::rcu {

namespace {

    // ─── Hooks (RCU-DEC-017, P1-DEC-009) ───────────────────────────────────
    //
    // onPreTouch is the load-bearing one and the ONLY one the kernel may fill
    // in. Stealing (RCU-DEC-006) means a drainer dereferences intrusive links
    // living in another CPU's retired slab memory, whose pages are subject to
    // reclaimSlabPage — the vmsmalloc DEC-047 stale-TLB bug class, which is a
    // bug this project has already shipped once. A Hooks policy that omits this
    // call is a Phase-2 bug, not a missed optimization.
    //
    // Every other point is WINDOW-INTERIOR: it sits inside the RCU-DEC-024
    // interrupt-masked, fault-free readLock/readUnlock transition. P1-DEC-009
    // requires them to stay EMPTY in the kernel — a point that touched anything
    // (a counter in vmsmalloc memory, a log line, MMIO) would break the audited
    // no-instruction-can-fault claim exactly as the evicted monoTimens stall
    // stamp did, and a spinning point would spin with interrupts masked.
    struct KernelRcuHooks {
        void onAfterEpochLoad(uint64_t) const noexcept {}       // WINDOW-INTERIOR
        void onAfterActivation(uint64_t) const noexcept {}      // WINDOW-INTERIOR
        void onBeforeDeactivation() const noexcept {}           // WINDOW-INTERIOR

        void onAfterScanEpochLoad(uint64_t) const noexcept {}
        void onBeforeEpochAdvance(uint64_t) const noexcept {}
        void onAfterRetireEpochLoad(uint64_t) const noexcept {}
        void onBeforeSeal(size_t, size_t) const noexcept {}
        void onAfterClaim(size_t, size_t) const noexcept {}

        void onPreTouch(Core::rcu::RetireHead* n) const noexcept {
            mm::VMSubstrate::ensureTLBEntryFresh(n);
        }
    };

    using Engine = Core::rcu::EpochDomain<KernelRcuHooks>;

    // P2-DEC-009. The header knows only "some bytes"; the real constraint is
    // checked here, where the template is visible. A growth in the engine's
    // members fails the build rather than silently overrunning Domain.
    static_assert(sizeof(Engine) <= kEngineStorageBytes,
                  "kernel::rcu::Domain's opaque storage is too small for the engine — "
                  "raise kEngineStorageBytes in rcu/RCU.h");
    static_assert(alignof(Engine) <= kEngineStorageAlign,
                  "kernel::rcu::Domain's opaque storage is under-aligned for the engine");

    // ─── P2-DEC-006 caller-side contract stubs ─────────────────────────────
    //
    // Byte-for-byte the DEC-030 pattern from vmsmalloc.cpp:315-321, and
    // deliberately duplicated rather than shared: they are vacuous today, and
    // the point of asserting them anyway is that when a scheduler lands the
    // assertions become real without anyone having to remember to add them. Two
    // predicates because they name two distinct obligations.
    inline bool preemptionDisabled() {
        // TODO(DEC-030, future scheduler): per-CPU preempt-count check.
        return true;
    }
    inline bool cpuPinned() {
        // TODO(DEC-030, future scheduler): per-thread migrate-disable check.
        return true;
    }

    // Slots per page, and the reason it must divide evenly: the array is dense,
    // so a slot straddling a page boundary would be split across two NUMA
    // placements. ReaderSlot is alignas(64) and a page is a multiple of 64, so
    // this holds structurally; the assertion is here to make a future ReaderSlot
    // that breaks it fail loudly.
    constexpr size_t kSlotsPerPage = arch::smallPageSize / sizeof(Core::rcu::ReaderSlot);
    static_assert(arch::smallPageSize % sizeof(Core::rcu::ReaderSlot) == 0,
                  "ReaderSlot must tile a page exactly — otherwise a slot straddles "
                  "two per-page NUMA placements");
    static_assert(kSlotsPerPage > 0, "ReaderSlot must fit in a page");

    // ─── Slot reservation ──────────────────────────────────────────────────
    //
    // P2-I4, honestly stated. The original invariant ("CPU i's slot resides in
    // the NUMA domain local to CPU i") cannot hold: EpochDomain indexes
    // slots[i] at natural stride so the array must be dense, and
    // reservePerDomainStaticBuffer places whole pages, so the finest NUMA
    // granularity available is one page — 32 slots at the current ReaderSlot
    // size. What is implemented instead is per-PAGE placement: each page is
    // reserved on the home domain of the first CPU whose slot lands on it. On
    // every configuration CroCOS actually boots (<= 32 CPUs) that is one page on
    // one domain, i.e. all slots local to CPU 0's domain; it starts paying off
    // on a machine large enough for slots to span pages, where CPU numbering is
    // conventionally socket-clustered. User-confirmed 2026-08-01.
    //
    // The alternatives were: a single whole-array reservation (identical below
    // 32 CPUs, no better above it), or one page per CPU plus a stride parameter
    // threaded through Phase 1's hot path (exact, but 4 KiB/CPU and it reopens a
    // shipped, TSan-green engine).
    //
    // CONTIGUITY DEPENDENCY: consecutive reservations returning consecutive VAs
    // is a property of VMSubstrate's bump allocator, not a documented contract
    // of reservePerDomainStaticBuffer. It is checked rather than assumed —
    // silently non-contiguous pages would give a slot array whose upper slots
    // alias unrelated storage, which no later assertion would catch.
    Core::rcu::ReaderSlot* reserveSlots(size_t cpuCount, const char* name) {
        const size_t pages = divideAndRoundUp(cpuCount, kSlotsPerPage);
        unsigned char* base = nullptr;

        // arch::ProcessorID is uint8_t, so the cast below is a truncation waiting
        // to happen. It is safe today only by arithmetic: pages == ceil(cpuCount
        // / 32), so the largest firstCpu is 32*(pages-1) < cpuCount <= 256. That
        // is not obvious enough to leave implicit — a future ReaderSlot or CPU
        // ceiling that breaks it would silently place upper pages on CPU 0's
        // domain rather than fail.
        assert(cpuCount <= arch::MAX_PROCESSOR_COUNT,
               "rcu: processorCount() exceeds arch::MAX_PROCESSOR_COUNT");

        for (size_t p = 0; p < pages; p++) {
            const size_t firstCpuIndex = p * kSlotsPerPage;
            assert(firstCpuIndex < cpuCount,
                   "rcu: slot-page CPU index out of range (ProcessorID truncation)");
            const auto firstCpu = static_cast<arch::ProcessorID>(firstCpuIndex);
            const numa::DomainID d = numa::numaPolicy().homeDomain(firstCpu);

            void* got = mm::VMSubstrate::reservePerDomainStaticBuffer(arch::smallPageSize, d);
            auto* page = static_cast<unsigned char*>(got);

            if (p == 0) {
                base = page;
            } else if (page != base + p * arch::smallPageSize) {
                klog() << "rcu: [" << name << "] slot reservation was not contiguous at page "
                       << static_cast<uint64_t>(p) << " — aborting init\n";
                return nullptr;
            }
        }

        // The storage is zero-filled by reservePerDomainStaticBuffer and P1-I3
        // guarantees the all-zero pattern decodes as "inactive / (tag 0, Free)",
        // so this loop changes no bytes. It runs anyway to start the objects'
        // lifetimes formally, which is the same reason vmsmallocLateInit
        // placement-news over its own zeroed buffer.
        auto* slots = reinterpret_cast<Core::rcu::ReaderSlot*>(base);
        for (size_t i = 0; i < cpuCount; i++) {
            new (&slots[i]) Core::rcu::ReaderSlot();
        }
        return slots;
    }

}   // namespace

// ─── Access ────────────────────────────────────────────────────────────────
//
// Domain's only friend. Laundering is not decoration: the bytes are storage,
// not an object, until init() constructs into them, and launder is what tells
// the compiler the pointer now designates the new object.
namespace detail {
    struct Access {
        [[nodiscard]] static Engine& engine(Domain& d) noexcept {
            return *launder(reinterpret_cast<Engine*>(d.engineStorage));
        }
        [[nodiscard]] static const Engine& engine(const Domain& d) noexcept {
            return *launder(reinterpret_cast<const Engine*>(d.engineStorage));
        }
        [[nodiscard]] static void* storage(Domain& d) noexcept { return d.engineStorage; }
        static void markInitialized(Domain& d, const char* name) noexcept {
            d.domainName    = name;
            d.isInitialized = true;
        }
    };
}

// ─── Domain::init ──────────────────────────────────────────────────────────

bool Domain::init(const char* name, size_t drainBatchBound) CROCOS_RCU_NOEXCEPT {
    assert(!isInitialized, "rcu: Domain::init called twice");
    assert(name != nullptr, "rcu: Domain::init requires a name");

    // P2-I2. Sized off processorCount() rather than arch::MAX_PROCESSOR_COUNT:
    // the constant is 256 but the real AP ceiling is 16 (SMPStack stacks[16],
    // kernel/arch/amd64/smp/smp.cpp:80, itself marked a temporary hack), so the
    // constant would reserve eight pages of slots for CPUs that cannot exist,
    // and would depend on a number that is currently fictional.
    const size_t cpuCount = arch::processorCount();
    assert(cpuCount > 0, "rcu: processorCount() is zero at Domain::init");

    Core::rcu::ReaderSlot* slots = reserveSlots(cpuCount, name);
    if (slots == nullptr) return false;

    Engine* engine = new (detail::Access::storage(*this)) Engine(slots, cpuCount);
    engine->setDrainBatchBound(drainBatchBound);

    detail::Access::markInitialized(*this, name);

    // P2-DEC-004's verification target. The absence of this line is the
    // silent-failure mode the phase's failure table names as the worst one: a
    // domain that was never initialized scans clean and reclaims freely.
    if (drainBatchBound == Core::rcu::kUnboundedDrainBatch) {
        klog() << "rcu: domain [" << name << "] ready — " << static_cast<uint64_t>(cpuCount)
               << " slots across "
               << static_cast<uint64_t>(divideAndRoundUp(cpuCount, kSlotsPerPage))
               << " page(s), drain batch unbounded\n";
    } else {
        klog() << "rcu: domain [" << name << "] ready — " << static_cast<uint64_t>(cpuCount)
               << " slots across "
               << static_cast<uint64_t>(divideAndRoundUp(cpuCount, kSlotsPerPage))
               << " page(s), drain batch " << static_cast<uint64_t>(drainBatchBound) << "\n";
    }
    return true;
}

// ─── Context predicates ────────────────────────────────────────────────────

namespace {
    // DEC-014 as inherited by retire / drain / tryAdvance: #PF is deliberately
    // NOT in the mask. The carve-out is load-bearing — RadixVM unlinks and
    // retires from the fault path — and the caller-side obligation that
    // VMSubstrate does not fault during operation is what makes it sound.
    inline bool inAllocForbiddenContext() {
        return interrupts::inForbiddenContext(interrupts::kAllocForbiddenDepthMask);
    }

    // RCU-DEC-031's strict mask for the blocking pair. #PF included: a
    // grace-period wait inside a fault handler spins on other CPUs' progress
    // from a context that may itself be blocking them. With RCU-DEC-026's packed
    // word this is one load and one test.
    inline bool inAnyInterruptContext() {
        return interrupts::currentCpuInterruptDepths().inAnyInterruptContext();
    }

    // RCU-DEC-024. Both unmaskable, so the outermost transition cannot be made
    // atomic against them.
    inline bool inReadSideForbiddenContext() {
        return interrupts::inForbiddenContext(interrupts::kRcuReadSideForbiddenDepthMask);
    }
}

// ─── ReadGuard ─────────────────────────────────────────────────────────────

ReadGuard::ReadGuard(Domain& d) CROCOS_RCU_NOEXCEPT
    : domain(d),
      // RCU-DEC-024 correction (ii): slot identity is derived from CpuLocal
      // through GS — a SECOND memory region, and one that migrates mid-boot
      // when VMSubstrate re-points GSBase — so it is read BEFORE the mask, not
      // inside the window whose fault-freedom is justified by "touches only
      // pinned slot storage".
      boundCpu(kernel::getLogicalProcessorID()),
      enteredAtNs(0) {
#if CROCOS_RCU_DEBUG_CHECKS
    assert(d.initialized(), "rcu: ReadGuard on a Domain that was never init()'d");
    assert(!inReadSideForbiddenContext(),
           "rcu: read-side section entered from NMI or #MC context (RCU-DEC-024)");
    assert(preemptionDisabled(), "rcu: read-side section entered with preemption enabled");
    assert(cpuPinned(), "rcu: read-side section entered without CPU pinning");
    // RCU-DEC-024 correction (iii): monoTimens reads ClockManager state and may
    // touch an HPET MMIO page — a third region. It must not be inside the
    // masked window, and it is not.
    enteredAtNs = timing::monoTimens();
#endif

    // RCU-DEC-025. Masks the outermost transition ONLY, never the section body.
    // An IRQ arriving during the protected traversal is harmless: the publish is
    // complete, so a nested readLock correctly short-circuits on nesting != 0.
    arch::InterruptDisabler mask;
    detail::Access::engine(domain).readLock(boundCpu);
}

ReadGuard::~ReadGuard() CROCOS_RCU_DTOR_NOEXCEPT {
    // The unlock runs FIRST, before any diagnostic. That ordering is deliberate
    // in both build flavours. In the kernel it does not matter (assert panics);
    // under the test harness assert THROWS, and a diagnostic placed ahead of the
    // unlock would leave the slot permanently nested — so every negative test
    // would then trip the engine's quiescence assert on teardown and report the
    // wrong failure. Diagnosing after the protocol completes costs nothing and
    // keeps the domain consistent no matter which check fires.
    //
    // Unlocking against boundCpu rather than re-deriving the ID is the other
    // half of that: even when the caller HAS migrated, the lock/unlock pair
    // stays well-formed and the damage is confined to the caller's bug rather
    // than corrupting a second slot (one left stuck active, one under-nested).
    {
        arch::InterruptDisabler mask;
        detail::Access::engine(domain).readUnlock(boundCpu);
    }

#if CROCOS_RCU_DEBUG_CHECKS
    // RCU-DEC-009.
    assert(kernel::getLogicalProcessorID() == boundCpu,
           "rcu: ReadGuard destroyed on a different CPU than it was constructed on");
    assert(!inReadSideForbiddenContext(),
           "rcu: read-side section left from NMI or #MC context (RCU-DEC-024)");

    // RCU-DEC-013's stall detector, in its ReadGuard form: per-section, and it
    // catches sections nobody happens to be waiting on. P2-ITEM-004 remains open
    // on whether tryAdvance should also carry one — that variant knows WHICH
    // slot is blocking an advance, which this one cannot, and the two are
    // complementary rather than alternatives.
    const uint64_t elapsed = timing::monoTimens() - enteredAtNs;
    if (elapsed > kSectionStallWarnNs) {
        klog() << "rcu: [" << domain.name() << "] section on CPU "
               << static_cast<uint64_t>(boundCpu)
               << " held for " << static_cast<uint64_t>(elapsed) << " ns\n";
    }
#endif
}

// ─── detail bridges ────────────────────────────────────────────────────────

namespace detail {

void assertInSection([[maybe_unused]] const Domain& d) CROCOS_RCU_NOEXCEPT {
#if CROCOS_RCU_DEBUG_CHECKS
    assert(d.initialized(), "rcu: protect on a Domain that was never init()'d");
    assert(Access::engine(d).inSection(kernel::getLogicalProcessorID()),
           "rcu: protect called outside a read-side section on this domain");
#endif
}

void retireNode(Domain& d, Core::rcu::RetireHead* node,
                void (*deleter)(Core::rcu::RetireHead*)) CROCOS_RCU_NOEXCEPT {
#if CROCOS_RCU_DEBUG_CHECKS
    assert(d.initialized(), "rcu: retire on a Domain that was never init()'d");
    // Deleters bottom out in vmsfree, so retire cannot be more permissive than
    // vmsmalloc's own DEC-014 rule.
    assert(!inAllocForbiddenContext(),
           "rcu: retire from a forbidden interrupt context "
           "(IRQ/NMI/#UD/#DF/#GP/#MC — #PF is legal)");
    assert(preemptionDisabled(), "rcu: retire with preemption enabled");
    assert(cpuPinned(), "rcu: retire without CPU pinning");
#endif
    // The deleter store used to live HERE, ahead of the engine call and with no
    // freshness of its own — which meant it landed on a stale mapping whenever
    // the retiring CPU was not the allocating one. It now happens inside the
    // engine, after onPreTouch. See EpochDomain::retire.
    //
    // The section / drain / teardown precondition (RCU-DEC-038) is asserted by
    // the engine, which is the only layer that can see slot.inDrain and
    // teardownActive. Not duplicated here.
    Access::engine(d).retire(kernel::getLogicalProcessorID(), node, deleter);
}

}   // namespace detail

// ─── Grace-period drivers ──────────────────────────────────────────────────

void synchronize(Domain& d) CROCOS_RCU_NOEXCEPT {
#if CROCOS_RCU_DEBUG_CHECKS
    assert(d.initialized(), "rcu: synchronize on a Domain that was never init()'d");
    assert(!inAnyInterruptContext(),
           "rcu: synchronize from interrupt context — the strict mask has no #PF "
           "carve-out (RCU-DEC-031)");
    assert(preemptionDisabled(), "rcu: synchronize with preemption enabled");
    assert(cpuPinned(), "rcu: synchronize without CPU pinning");
#endif
    // The in-section and !inDrain preconditions are the engine's asserts: from a
    // deleter this call HANGS rather than fails, so the check has to sit where
    // inDrain is visible.
    detail::Access::engine(d).synchronize(kernel::getLogicalProcessorID());
}

void barrier(Domain& d) CROCOS_RCU_NOEXCEPT {
#if CROCOS_RCU_DEBUG_CHECKS
    assert(d.initialized(), "rcu: barrier on a Domain that was never init()'d");
    assert(!inAnyInterruptContext(),
           "rcu: barrier from interrupt context — the strict mask has no #PF "
           "carve-out (RCU-DEC-031)");
    assert(preemptionDisabled(), "rcu: barrier with preemption enabled");
    assert(cpuPinned(), "rcu: barrier without CPU pinning");
#endif
    detail::Access::engine(d).barrier(kernel::getLogicalProcessorID());
}

bool tryAdvance(Domain& d) CROCOS_RCU_NOEXCEPT {
#if CROCOS_RCU_DEBUG_CHECKS
    assert(d.initialized(), "rcu: tryAdvance on a Domain that was never init()'d");
    assert(!inAllocForbiddenContext(),
           "rcu: tryAdvance from a forbidden interrupt context "
           "(IRQ/NMI/#UD/#DF/#GP/#MC — #PF is legal)");
    assert(preemptionDisabled(), "rcu: tryAdvance with preemption enabled");
    assert(cpuPinned(), "rcu: tryAdvance without CPU pinning");
#endif
    return detail::Access::engine(d).tryAdvance(kernel::getLogicalProcessorID());
}

size_t drain(Domain& d) CROCOS_RCU_NOEXCEPT {
#if CROCOS_RCU_DEBUG_CHECKS
    assert(d.initialized(), "rcu: drain on a Domain that was never init()'d");
    assert(!inAllocForbiddenContext(),
           "rcu: drain from a forbidden interrupt context "
           "(IRQ/NMI/#UD/#DF/#GP/#MC — #PF is legal)");
    assert(preemptionDisabled(), "rcu: drain with preemption enabled");
    assert(cpuPinned(), "rcu: drain without CPU pinning");
#endif
    return detail::Access::engine(d).sweepExpired(kernel::getLogicalProcessorID());
}

// ─── The general-purpose kernel domain + the .icd routine ──────────────────

// RCU-DEC-015 / P2-I5. constexpr-inert, so this is constant-initialized into
// .bss and no global constructor runs for it during cpp_init — which is before
// VMSubstrate exists.
Domain kernelDomain;

#ifdef CROCOS_RCU_TEST_HARNESS
// ─── Test-only introspection (tests/kernel/rcu/DebugIntrospection.h) ───────
//
// Defined here because this is the only TU in which the engine type exists.
// Compiled out of the kernel build entirely.
namespace test {
    using Intro = Core::rcu::DebugIntrospection<KernelRcuHooks>;

    bool     inSection(const Domain& d, size_t slot) { return Intro::slot(detail::Access::engine(d), slot).nesting > 0; }
    uint64_t nesting(const Domain& d, size_t slot)   { return Intro::slot(detail::Access::engine(d), slot).nesting; }
    uint64_t epoch(const Domain& d)                  { return Intro::epoch(detail::Access::engine(d)); }
    size_t   slotCount(const Domain& d)              { return Intro::slotCount(detail::Access::engine(d)); }
    size_t   drainBatchBound(const Domain& d)        { return Intro::drainBatchBound(detail::Access::engine(d)); }
    size_t   totalResidue(const Domain& d)           { return Intro::totalResidue(detail::Access::engine(d)); }
    size_t   openBagResidue(const Domain& d, size_t i) { return Intro::openBagResidue(detail::Access::engine(d), i); }
    void     assertQuiescent(const Domain& d)        { Intro::assertQuiescent(detail::Access::engine(d)); }
    size_t   drainAllQuiescent(Domain& d)            { return detail::Access::engine(d).drainAllQuiescent(); }
}
#endif // CROCOS_RCU_TEST_HARNESS

bool initialize() CROCOS_RCU_NOEXCEPT {
    // drainBatchBound is left unbounded here deliberately. RCU-DEC-033's bound
    // exists to cap #PF latency, and picking a number before there is a
    // RadixVM-shaped workload to measure would be exactly the vmsmalloc Phase 10
    // mistake (parent ITEM-005). A consumer that needs a bound passes one.
    return kernelDomain.init("kernel");
}

}   // namespace kernel::rcu
