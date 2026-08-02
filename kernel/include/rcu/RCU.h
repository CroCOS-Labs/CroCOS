//
// RCU Phase 2 — the kernel veneer over Core::rcu::EpochDomain.
// Created by Spencer Martin on 8/1/26.
//
// specs/rcu-phase-2.md. This layer contains NO algorithm: every grace-period
// decision lives in Phase 1. What lives here is the binding of that engine to
// CPU identity, NUMA-placed slot storage, SafePtr freshness, and the DEC-014
// context rules.
//
// ─── Context rules, two tiers (RCU-DEC-012 / RCU-DEC-031) ──────────────────
//
//   retire / drain / tryAdvance   inherit vmsmalloc DEC-014 verbatim: forbidden
//                                 in IRQ / NMI / #UD / #DF / #GP / #MC,
//                                 conditionally legal in #PF. The #PF carve-out
//                                 is load-bearing — RadixVM retires from the
//                                 fault path.
//
//   synchronize / barrier         STRICT: forbidden in *any* interrupt context,
//                                 #PF included. A grace-period wait inside a
//                                 fault handler spins on other CPUs' progress
//                                 from a context that may itself be blocking
//                                 them.
//
//   ReadGuard                     legal in any context EXCEPT NMI and #MC
//                                 (RCU-DEC-024). Note this is NARROWER than the
//                                 original promise of "legal up to and including
//                                 NMI": both NMI and #MC are unmaskable, so the
//                                 outermost readLock/readUnlock transition
//                                 cannot be made atomic against them. A profiler
//                                 or watchdog author reaching for RCU from an NMI
//                                 handler is reading the right paragraph.
//
// Every check above is DEBUG-ONLY (RCU-DEC-013 / P2-DEC-005). Release builds
// trust the caller; the read side stays one store plus one fence.
//
// ─── Why the engine is not visible here ────────────────────────────────────
//
// P2-DEC-009: the EpochDomain instance is placement-new'd into opaque byte
// storage inside Domain and recovered with launder in RCU.cpp — the established
// vmsmalloc PartialStackStorage pattern (VMSubstrateSlab.h:289-290). So the
// instantiated engine type never appears in this header, and every veneer entry
// point is an out-of-line call into RCU.cpp. That call is the unavoidable price
// of the opaque-storage form; what it buys is that no consumer TU instantiates
// the Core template, and that Domain's constructor can stay constexpr-inert.
//
// <core/rcu/EpochDomain.h> is still included, for Core::rcu::RetireHead and
// kUnboundedDrainBatch only — RetireHead is named in retire's signature and its
// layout is needed by the container_of thunk, so an incomplete type will not do.
// Including the header does not instantiate the template.
//

#ifndef CROCOS_KERNEL_RCU_H
#define CROCOS_KERNEL_RCU_H

#include <stddef.h>
#include <stdint.h>

#include <arch.h>
#include <core/atomic.h>
#include <core/rcu/EpochDomain.h>
#include <mem/VMSubstrate.h>

// Debug-only checks (RCU-DEC-013). The userspace harness compiles without
// DEBUG_BUILD but its <kassert.h> mock throws unconditionally, so the harness
// opts the same bodies in explicitly — otherwise the negative tests in
// tests/kernel/rcu would be testing code that was compiled out.
//
// CROCOS_RCU_RELEASE_CHECKS overrides that opt-in, and exists for exactly one
// consumer: the uninstrumented soak runner. Tying the checks to
// CROCOS_RCU_TEST_HARNESS meant every harness build measured the DEBUG read
// path — the stall stamp, assertInSection, the context and pinning checks — so
// the "one store plus one fence" release path this file advertises had never
// actually been measured. Profiling on 2026-08-01 put those checks at ~8% of
// on-CPU work with the stall stamp alone once at 34%.
//
// It does NOT gate DebugIntrospection, which keys off CROCOS_RCU_TEST_HARNESS
// independently — so a release-checks build still gets drainAllQuiescent and
// totalResidue, and the engine's own asserts are untouched. Defining this in
// anything but a benchmark target silently removes the checks that catch
// caller-contract violations.
#if defined(CROCOS_RCU_RELEASE_CHECKS)
#define CROCOS_RCU_DEBUG_CHECKS 0
#elif defined(DEBUG_BUILD) || defined(CROCOS_RCU_TEST_HARNESS)
#define CROCOS_RCU_DEBUG_CHECKS 1
#else
#define CROCOS_RCU_DEBUG_CHECKS 0
#endif

// Same trap Phase 1 hit, same fix (see EpochDomain.h's CROCOS_RCU_NOEXCEPT,
// which this header reuses for the ordinary entry points): the Phase-2 contract
// says every entry point is noexcept, but under CORE_LIBRARY_TESTING `assert`
// THROWS — so a noexcept veneer would turn each of this phase's
// EXPECT_ASSERT_FAILURE targets into std::terminate rather than a reported
// failure. Kernel and release builds get the noexcept the contract specifies.
//
// ~ReadGuard needs its own spelling: a destructor is implicitly noexcept(true)
// unless it says otherwise, so leaving CROCOS_RCU_NOEXCEPT empty on it would not
// help. The cross-CPU-destruction assert (RCU-DEC-009) is a verification target,
// so it has to be catchable.
#if defined(CORE_LIBRARY_TESTING)
#define CROCOS_RCU_DTOR_NOEXCEPT noexcept(false)
#else
#define CROCOS_RCU_DTOR_NOEXCEPT noexcept
#endif

namespace kernel::rcu {

    // RCU-DEC-022. The load of a published link, performed by protect<T> before
    // the SafePtr wrap. ACQUIRE pairs with the writer's release-publish of the
    // object's initialized contents; a relaxed load here would permit the
    // consumer to observe the pointer before the fields it points at.
    inline constexpr MemoryOrder kProtectedLinkLoad = ACQUIRE;

    // RCU-DEC-013's section-duration stall threshold. Debug-only diagnostic; a
    // section held this long is a caller bug (it blocks every advance on the
    // domain), but it is reported, never enforced.
    inline constexpr uint64_t kSectionStallWarnNs = 100'000'000;   // 100 ms

    // P2-DEC-009 opaque engine storage. The size/alignment static_asserts
    // against the real sizeof(EpochDomain<KernelRcuHooks>) live in RCU.cpp,
    // where the template is visible; this header deliberately knows only that
    // the engine is "some bytes".
    inline constexpr size_t kEngineStorageBytes = 64;
    inline constexpr size_t kEngineStorageAlign = 64;

    class Domain;
    class ReadGuard;

    namespace detail {
        // The single friend of Domain. Defined in RCU.cpp, where the engine type
        // is visible; every veneer entry point recovers the engine through it,
        // so Domain needs exactly one friend declaration rather than one per
        // entry point.
        struct Access;

        // The bridges into RCU.cpp. Everything the templates below need from the
        // engine goes through one of these, so the engine type stays private.
        void retireNode(Domain& d, Core::rcu::RetireHead* node,
                        void (*deleter)(Core::rcu::RetireHead*)) CROCOS_RCU_NOEXCEPT;
        void assertInSection(const Domain& d) CROCOS_RCU_NOEXCEPT;
    }

    // ─── Domain ────────────────────────────────────────────────────────────
    //
    // One independent grace-period universe, declared at namespace scope and
    // init()'d from an .icd-registered routine at the memory_management phase.
    //
    // RCU-DEC-015 / P2-I5: the constructor is constexpr and touches nothing.
    // This is not stylistic. Global constructors genuinely run as of the
    // 2026-07-31 toolchain change, and they run during cpp_init — before
    // VMSubstrate exists. A Domain constructor that reserved storage would panic
    // exactly as PITEventSource's did. Any member gaining a non-trivial default
    // constructor silently reintroduces that bug, which is what the constexpr
    // keyword is here to prevent mechanically.
    class Domain {
    public:
        constexpr Domain() noexcept = default;
        Domain(const Domain&)            = delete;
        Domain& operator=(const Domain&) = delete;

        // Reserves one ReaderSlot per CPU, NUMA-places the backing pages, and
        // constructs the engine into this object's opaque storage. Returns false
        // only if the slot reservation could not be satisfied contiguously; a
        // VMSubstrate exhaustion panics inside VMSubstrate itself.
        //
        // drainBatchBound is RCU-DEC-033's knob, plumbed through here because
        // without it no kernel path could ever set it — and the #PF-latency
        // motivation for having a bound at all applies only in the environment
        // that has #PF. kUnboundedDrainBatch means "drain the bag to the end".
        bool init(const char* name,
                  size_t drainBatchBound = Core::rcu::kUnboundedDrainBatch) CROCOS_RCU_NOEXCEPT;

        [[nodiscard]] bool initialized() const noexcept { return isInitialized; }
        [[nodiscard]] const char* name() const noexcept { return domainName; }

    private:
        friend struct detail::Access;

        alignas(kEngineStorageAlign) unsigned char engineStorage[kEngineStorageBytes] = {};
        const char* domainName  = nullptr;
        // P2-DEC-009 / failure table: with opaque storage there is NO engine
        // pointer to null-check, so used-before-init is caught by this flag and
        // nothing else. Zeroed storage scans clean — an uninitialized domain
        // would reclaim freely with no reader protection.
        bool        isInitialized = false;
    };

    // ─── ReadGuard ─────────────────────────────────────────────────────────
    //
    // The only supported way to enter a section. Binds to
    // kernel::getLogicalProcessorID() at construction and unlocks against that
    // same slot, debug-asserting the CPU has not changed underneath it
    // (RCU-DEC-009).
    //
    // The outermost transition — and ONLY the outermost transition, never the
    // section body — runs with maskable interrupts disabled via
    // arch::InterruptDisabler (RCU-DEC-024 / RCU-DEC-025). An IRQ arriving
    // during the protected traversal is harmless: the publish is complete, so a
    // nested readLock correctly short-circuits on nesting != 0.
    class ReadGuard {
    public:
        explicit ReadGuard(Domain& d) CROCOS_RCU_NOEXCEPT;
        ~ReadGuard() CROCOS_RCU_DTOR_NOEXCEPT;

        ReadGuard(const ReadGuard&)            = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
        ReadGuard(ReadGuard&&)                 = delete;
        ReadGuard& operator=(ReadGuard&&)      = delete;

    private:
        Domain&           domain;
        // Load-bearing in every build, not just debug: unlocking against the
        // slot the lock was taken on is what makes the pair well-formed even if
        // the CPU identity is misused. The debug assert reports the misuse; this
        // member keeps it from corrupting a second slot.
        arch::ProcessorID boundCpu;
        // RCU-DEC-024 correction (iii): monoTimens reads ClockManager state and
        // may touch an HPET MMIO page, so the stamp is taken OUTSIDE the masked,
        // fault-free window. Dead and eliminated in release.
        uint64_t          enteredAtNs;
    };

    // ─── protect ───────────────────────────────────────────────────────────
    //
    // Acquire-load an RCU-published pointer and wrap it for freshness
    // (RCU-DEC-011 / P2-DEC-003). RCU says the object has not been recycled;
    // ensureTLBEntryFresh says this CPU's view of its bytes is current. They are
    // orthogonal, and a radix walk needs both at every level — folding them
    // together gives one call site instead of two.
    template <typename T>
    [[nodiscard]] mm::VMSubstrate::SafePtr<T> protect(Domain& d,
                                                      const Atomic<T*>& src) CROCOS_RCU_NOEXCEPT {
        detail::assertInSection(d);
        return mm::VMSubstrate::SafePtr<T>(src.load(kProtectedLinkLoad));
    }

    namespace detail {
        // container_of for the RetireHead member designated by a member-pointer
        // NTTP. No RTTI, no offsetof, and no storage: the offset of a non-virtual
        // data member is a compile-time constant, so the arithmetic can be done
        // against a fabricated base that is never dereferenced. alignof(T) is
        // used as that base rather than nullptr specifically so no sanitizer
        // sees a null member access — taking a member's address is not a load,
        // and both GCC and Clang fold the whole expression to one constant
        // subtraction.
        //
        // REQUIREMENT ON T: Head must sit at a constant offset, i.e. T must not
        // reach it through a virtual base. Every plausible retirable type
        // satisfies this; a type that does not will silently miscompute.
        template <typename T, Core::rcu::RetireHead T::* Head>
        [[nodiscard]] inline ptrdiff_t headOffset() noexcept {
            auto* const fakeBase = reinterpret_cast<T*>(alignof(T));
            return reinterpret_cast<unsigned char*>(&(fakeBase->*Head)) -
                   reinterpret_cast<unsigned char*>(fakeBase);
        }

        template <typename T, Core::rcu::RetireHead T::* Head>
        [[nodiscard]] inline T* containerOf(Core::rcu::RetireHead* n) noexcept {
            return reinterpret_cast<T*>(
                reinterpret_cast<unsigned char*>(n) - headOffset<T, Head>());
        }

        // P1-DEC-003: the engine's deleter takes RetireHead* and knows nothing
        // about T. This is the thunk Phase 2 synthesizes to recover the object.
        //
        // Deleter is a template parameter rather than a runtime argument because
        // RetireHead has exactly one function-pointer slot and it must hold this
        // thunk — there is nowhere to also store a runtime deleter without
        // widening RetireHead past RCU-DEC-016's 16 bytes. A consumer needing a
        // runtime choice writes one dispatching deleter. (Spec deviation from
        // rcu-phase-2.md's written signature, user-confirmed 2026-08-01.)
        template <typename T, Core::rcu::RetireHead T::* Head, void (*Deleter)(T*)>
        void retireThunk(Core::rcu::RetireHead* n) {
            Deleter(containerOf<T, Head>(n));
        }

        // P2-DEC-007's terminal deleter: run the destructor, poison in debug,
        // then release the storage.
        //
        // NOTE the ordering against the spec's prose ("poisons in debug then
        // calls VMSubstrate::destroy<T>"). Poisoning first would run a
        // non-trivial destructor over scribbled members, so the poison is placed
        // between the destructor and the free — the only point at which it is
        // both meaningful and safe. This unavoidably open-codes destroy<T>'s two
        // steps rather than calling it.
        //
        // 0xA5 rather than vmsmalloc's DEC-024 0xCC so the two tells stay
        // distinguishable: 0xA5 says RCU released the object at grace-period
        // expiry, 0xCC says the allocator reclaimed the slot. For a slab-backed T
        // the second immediately overwrites the first; the RCU poison earns its
        // keep for whole-page objects and for anything a custom deleter leaves
        // un-freed.
        template <typename T>
        void destroyDeleter(T* p) {
            p->~T();
#if CROCOS_RCU_DEBUG_CHECKS
            __builtin_memset(static_cast<void*>(p), 0xA5, sizeof(T));
#endif
            mm::VMSubstrate::vmsfree(static_cast<void*>(p));
        }
    }

    // ─── retire ────────────────────────────────────────────────────────────
    //
    // Deferred destruction. T embeds a Core::rcu::RetireHead; the member-pointer
    // NTTP recovers T* from the head at drain time.
    //
    // The caller must already be inside a section on this domain, OR be running
    // as a deleter, OR be inside drainAllQuiescent (RCU-DEC-019 as amended by
    // RCU-DEC-038) — the engine asserts exactly that. Deleters are additionally
    // bound by RCU-DEC-039: they may touch only state reachable solely from the
    // retired object. No assert can check that clause.
    template <typename T, Core::rcu::RetireHead T::* Head, void (*Deleter)(T*)>
    void retire(Domain& d, T* obj) CROCOS_RCU_NOEXCEPT {
        detail::retireNode(d, &(obj->*Head), &detail::retireThunk<T, Head, Deleter>);
    }

    // Convenience wrapper for the overwhelmingly common case: the object is
    // vmsmalloc-backed and its grace period ends in destruction. Grace-period
    // expiry is exactly the point at which DEC-026's vmsfree validation chain
    // may safely run.
    template <typename T, Core::rcu::RetireHead T::* Head>
    void retireDestroy(Domain& d, mm::VMSubstrate::SafePtr<T> obj) CROCOS_RCU_NOEXCEPT {
        if (!obj) return;
        retire<T, Head, &detail::destroyDeleter<T>>(d, static_cast<T*>(obj.raw()));
    }

    // ─── Grace-period drivers ──────────────────────────────────────────────

    // A grace period has elapsed (RCU-DEC-031). Says NOTHING about destruction.
    // Blocking; strict context mask; forbidden inside a section on this domain
    // and inside a drain.
    void synchronize(Domain& d) CROCOS_RCU_NOEXCEPT;

    // Every object THIS CPU retired before the call has been destroyed
    // (RCU-DEC-031 / RCU-DEC-036). Objects still in *other* CPUs' Open bags at
    // entry are excluded — that is the ITEM-014 residue class, and it follows
    // from Open -> Sealed being an owner-only store (I13), not from an
    // implementation shortcut. Full-domain destruction is drainAllQuiescent,
    // which has no veneer entry point in this phase.
    //
    // The guarantee is per-SLOT, not per-thread (RCU-DEC-040): under a future
    // scheduler the caller must hold CPU affinity from the covered retire
    // through the barrier. Preemption stays fine; migration does not.
    void barrier(Domain& d) CROCOS_RCU_NOEXCEPT;

    // One attempt to advance the global epoch, then a sweep of every expired bag
    // reachable from this CPU. Returns whether the epoch moved. Note the sweep
    // happens either way — that is what keeps one blocked CPU from stopping
    // every other CPU from reclaiming.
    bool tryAdvance(Domain& d) CROCOS_RCU_NOEXCEPT;

    // Sweep expired bags only, no advance attempt. Returns the number of objects
    // destroyed.
    size_t drain(Domain& d) CROCOS_RCU_NOEXCEPT;

    // ─── The general-purpose kernel domain ─────────────────────────────────
    //
    // P2-DEC-008 makes one-domain-per-protected-structure the norm, and this is
    // not a counterexample to it — it is the domain a consumer adopts before it
    // is contended enough to deserve its own. It exists in this phase for a
    // second reason: with no consumer shipping, it is the only thing that
    // exercises init() in a real boot, and therefore the only thing that can
    // demonstrate the RCU-DEC-015 inert constructor and the P2-DEC-004 component
    // actually running. Its absence from the boot log IS the silent-failure mode
    // the phase's failure table calls the worst one.
    extern Domain kernelDomain;

    // The .icd routine (P2-DEC-004): memory_management phase, after
    // VMSubstrateSlab. Global, not per-CPU — which makes it an implicit all-CPU
    // barrier, so slots exist before any AP can enter a section.
    bool initialize() CROCOS_RCU_NOEXCEPT;

}   // namespace kernel::rcu

#endif // CROCOS_KERNEL_RCU_H
