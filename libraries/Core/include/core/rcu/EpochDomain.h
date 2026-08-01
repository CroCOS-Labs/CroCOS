//
// Created by Spencer Martin on 8/1/26.
//
// Core::rcu::EpochDomain — the CroCOS epoch-based reclamation engine.
//
// Phase 1 of the RCU design (specs/rcu.md + specs/rcu-phase-1.md). This header
// is the WHOLE algorithm: the global epoch, the per-slot activation word, the
// activation/scan fence pairing, and the four-state limbo-bag machine that
// makes bags stealable. It has ZERO kernel dependencies (RCU-DEC-002) — the
// include set is capped at what TreiberStack.h allows itself — so it can be
// unit-tested as a pure algorithm with plain std::thread under TSan (R7).
//
// What the engine deliberately does NOT do (all Phase 2's job):
//   - own storage: it is constructed over a caller-supplied ReaderSlot array
//   - decide slot identity: the caller passes a slot index
//   - enforce forbidden-context rules: those need InterruptContextDepths
//   - know about SafePtr / TLB freshness: injected via the Hooks policy
//   - detect stalls: that needs a clock
//
// ─── The safety argument in one paragraph (RCU-DEC-004, as rewritten) ───────
//
// Three SEQ_CST fences carry the whole thing: the reader's activation fence
// (kReaderActivationFence), the scan's fence (kScanFence), and kRetireFence.
// Governing rule is C++20 [atomics.order]/4 bullet 4. Writing U for the
// caller's unlink, F_W for kRetireFence, L_W for retire's epoch load returning
// r, Sc2 for the scan whose CAS writes r+2 (epoch load L_S2 then fence F_Sc2),
// and R for a reader with epoch load L_R = s, activation store A_R, fence F_R,
// protected load D_R:
//   Lemma A: if D_R misses U then F_R <s F_W.
//   Lemma B: F_W <s F_Sc2   (L_W --fr--> CAS(r+1) --rf--> L_S2; needs I10).
//   Lemma C: if D_R misses U then s <= r.
//   Theorem: A+B give F_R <s F_Sc2, so Sc2 observes A_R or a co-later value.
//            If A_R, then s <= r != r+1 and I3 blocks Sc2 — contradiction.
//            If co-later, R already exited and the RELEASE/ACQUIRE pairing puts
//            its whole section happens-before the drain.
// The argument is C++20-specific and never requires the writer to be pinned.
//
// ─── Three lines that look removable and are not ───────────────────────────
//
//  1. kRetireFence. It guards a load that is "only" ACQUIRE, so it reads as
//     redundant. It is the entire fix for ITEM-007. Do not elide, move, or make
//     it conditional (in particular: do NOT skip it when the freshly-read epoch
//     already equals the open bag's tag — see ITEM-015).
//  2. I10 — tryAdvance's epoch load is sequenced BEFORE kScanFence. Reversing
//     the two collapses Lemma B while changing no ordering constant, and x86-TSO
//     would not fail. It is a correctness change disguised as a cleanup.
//  3. P1-DEC-004 — the scan never skips the calling CPU's own slot. A writer may
//     retire from inside its own section, so its own slot can legitimately be
//     active and must block its own advance.
//
// ─── Contract narrowing a future consumer will not expect ──────────────────
//
// RCU read-side sections are FORBIDDEN in NMI and #MC context (RCU-DEC-024).
// The original requirement promised legality "in any context up to and
// including NMI"; that promise is withdrawn, because the Phase-2 ReadGuard masks
// maskable interrupts across the outermost transition. Enforcement is a
// debug-only assert in the kernel veneer, so a profiler or watchdog author
// reaching for RCU from an NMI handler gets silence in a release build.
//
// ─── Caller obligations ────────────────────────────────────────────────────
//
//   - slot < slotCount, always.
//   - readLock/readUnlock balanced, on the same slot, and NOT reentered on the
//     same slot mid-transition. The engine is arch-free and cannot enforce this;
//     the Phase-2 ReadGuard supplies it with arch::InterruptDisabler, and the
//     userspace harness gets it for free by having no signal handlers.
//   - Every store that makes an object unreachable is sequenced before retire —
//     not merely "the unlink", since a splice has several.
//   - A writer installing a new node initializes it and publishes the link with
//     RELEASE. That is the other half of what makes the reader's ACQUIRE link
//     load meaningful.
//   - Deleters may run on ANY CPU, at any time after the grace period, and
//     since RCU-DEC-038 outside any read-side section. A deleter may read,
//     modify, or retire only state reachable SOLELY from the retired object; it
//     must not load a pointer out of the live structure (RCU-DEC-039). No
//     assert can catch a violation — cross-structure cleanup belongs at unlink
//     time, inside the writer's section.
//   - A protected pointer must not outlive its section. Nothing detects this.
//
// ─── References ────────────────────────────────────────────────────────────
//
//   specs/rcu.md            — RCU-DEC-001..040, invariants I1-I14
//   specs/rcu-phase-1.md    — P1-DEC-001..009, this component
//   core/atomic/TreiberStack.h — house style, policy-struct layering
//   K. Fraser, Practical Lock-Freedom, ch. 5; crossbeam-epoch
//

#ifndef CROCOS_RCU_EPOCHDOMAIN_H
#define CROCOS_RCU_EPOCHDOMAIN_H

#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <core/atomic.h>
#include <core/utility.h>
#include <core/TypeTraits.h>

// Under the Core test harness, assert(...) THROWS (libraries/Core/include/assert.h
// maps it onto CroCOSTest::AssertionFailure). Marking the entry points noexcept
// there would turn every EXPECT_ASSERT_FAILURE negative test — which the Phase-1
// verification targets require for the !inDrain and quiescence checks — into
// std::terminate. Kernel and release builds get the noexcept the Phase-1 contract
// specifies. The destructor is unconditionally noexcept regardless; that is why
// P1-DEC-007 routes the quiescence negative test through
// DebugIntrospection::assertQuiescent() rather than through ~EpochDomain.
#if defined(CORE_LIBRARY_TESTING)
#define CROCOS_RCU_NOEXCEPT
#else
#define CROCOS_RCU_NOEXCEPT noexcept
#endif

namespace Core::rcu {

    // ─── Retire node ─────────────────────────────────────────────────────────
    //
    // Intrusive tracking header, 16 bytes, embedded in the retired object
    // (I8: retire never allocates). The deleter is a plain function pointer
    // taking RetireHead* — type recovery is the caller's job (P1-DEC-003), which
    // is what keeps the engine free of templates over the retired type.
    //
    // `next` doubles as the already-linked marker for the double-retire debug
    // assert: it is nullptr exactly when the node is not in a bag. The drain
    // clears it before handing the node to its deleter.
    struct RetireHead {
        RetireHead* next    = nullptr;
        void      (*deleter)(RetireHead*) = nullptr;
    };

    // ─── Tunables ────────────────────────────────────────────────────────────
    //
    // kBagCount is a pure BATCHING tunable, not a correctness floor. It used to
    // be one — retired invariant I7 required >= 4 under the old
    // `epoch mod kBagCount` bag selection — but RCU-DEC-027 replaced that with
    // openBagIndex, which deleted I7 outright. The old framing may persist in
    // readers' memory; do not reintroduce it. The real structural minimum is 2
    // (rotation needs a second bag), asserted below as P1-I1.
    inline constexpr size_t kBagCount = 4;                  // P1-DEC-006, provisional

    // Retires per slot between automatic advance attempts. Provisional; real
    // tuning is parent ITEM-005 and waits for a RadixVM-shaped workload (the
    // vmsmalloc Phase 10 lesson: do not tune before a representative workload).
    inline constexpr size_t kRetireAdvanceThreshold = 64;

    // The supported "effectively unbounded" drain batch bound (RCU-DEC-033).
    inline constexpr size_t kUnboundedDrainBatch = static_cast<size_t>(-1);

    // ─── Effective epoch width (I9, ITEM-013 / P1-ITEM-008) ──────────────────
    //
    // The global epoch is a full uint64_t, but neither place it gets encoded
    // carries 64 bits: the slot state word spends bit 0 on the active flag, and
    // bagTagState spends 2 bits on the bag state. Every comparison — including
    // the `globalEpoch >= tag + 2` expiry gate — must be performed on a
    // consistent width, so the effective width is the narrower of the two.
    // Intermediate constexpr variables rather than literals in the assertion:
    // -Werror + -Wtautological-compare trips on constant-vs-constant comparisons
    // written inline (vmsmalloc Phase 2 hit exactly that).
    inline constexpr unsigned kStateEpochBits     = 63;
    inline constexpr unsigned kBagTagBits         = 62;
    inline constexpr unsigned kEffectiveEpochBits = kBagTagBits;
    inline constexpr uint64_t kMaxEpoch           = (uint64_t{1} << kEffectiveEpochBits) - 1;

    static_assert(kEffectiveEpochBits <= kStateEpochBits,
                  "the slot state word must be able to hold every representable epoch");
    static_assert(kEffectiveEpochBits <= kBagTagBits,
                  "bagTagState must be able to hold every representable epoch");
    static_assert(kBagCount >= 2,
                  "P1-I1: rotation needs a second bag (maybeRotate's acquire-before-release). "
                  "The old >= 4 floor was retired invariant I7's; 4 is now tuning slack only.");

    // Atomic<uint64_t> must be lock-free: RCU-DEC-024's fault-free-window
    // argument for the interrupt-masked readLock transition does not survive a
    // libatomic call (which is a lock acquisition) inside the window. Always true
    // on AMD64 and ARMv8; stated so a future port trips over it loudly.
    static_assert(__atomic_is_lock_free(8, nullptr),
                  "EpochDomain requires lock-free 64-bit atomics (RCU-DEC-024)");

    // ─── Slot state word encoding ────────────────────────────────────────────
    //
    //   bit  0     : active flag
    //   bits 63..1 : epoch snapshot taken when the outermost section was entered
    //
    // Inactive is the all-zero encoding, so a zero-initialized slot array is
    // correctly "no readers" (P1-I3) — which is what reservePerDomainStaticBuffer
    // hands Phase 2. Note the trap in that: an *uninitialized* domain also scans
    // clean and reclaims freely. Only the veneer's init assert guards it.
    inline constexpr uint64_t kInactive = 0;

    [[nodiscard]] inline constexpr uint64_t makeActive(uint64_t epoch) noexcept {
        return (epoch << 1) | uint64_t{1};
    }
    [[nodiscard]] inline constexpr bool isActive(uint64_t stateWord) noexcept {
        return (stateWord & uint64_t{1}) != 0;
    }
    [[nodiscard]] inline constexpr uint64_t epochOf(uint64_t stateWord) noexcept {
        return stateWord >> 1;
    }

    // ─── Bag state machine (RCU-DEC-008 — this closes HAZARD-1) ──────────────
    //
    //   Transition                              | By              | Precondition
    //   ----------------------------------------+-----------------+---------------------------
    //   Free -> Open (tag := e)                 | owner only      | bag empty
    //   push onto head                          | owner only      | state is Open
    //   Open -> Sealed                          | owner only      | owner will never push here again
    //   Sealed -> Claimed                       | owner or ANY CPU| globalEpoch >= tag + 2  (I2)
    //   Claimed -> Free   (bag emptied)         | the claim winner| -
    //   Claimed -> Sealed (batch bound hit)     | the claim winner| remainder intact, tag unchanged
    //
    // Sealed -> Claimed is the ONLY CAS in the machine, and it is what serializes
    // drainers: exactly one CPU wins and only the winner touches head. Because
    // the owner pushes only into Open bags and drainers take only Claimed bags,
    // the two never target the same bag (I6). That is HAZARD-1 closed
    // structurally rather than by timing or by discipline.
    //
    // Every owner-side transition has no concurrent writer and is a STORE, not a
    // CAS (I13) — "no CAS" is the claim, but the ORDERING of each store is still
    // whatever the table below assigns. In particular kBagSeal is RELEASE: a
    // relaxed seal would sever the claimer's happens-before to the owner's pushes.
    //
    // Free is terminal until the owner acts: thieves enter only at Sealed and
    // leave only at Claimed -> Free or Claimed -> Sealed. A re-sealed bag never
    // passes back through Free, which is what keeps the owner's plain-store
    // privileges sound.
    //
    // Encoding: (tag : 62, state : 2), state in the low bits so that a
    // zero-initialized word decodes as (tag 0, Free) — P1-I3.
    enum class BagState : uint64_t {
        Free    = 0,
        Open    = 1,
        Sealed  = 2,
        Claimed = 3,
    };

    [[nodiscard]] inline constexpr uint64_t packBag(uint64_t tag, BagState state) noexcept {
        return (tag << 2) | static_cast<uint64_t>(state);
    }
    [[nodiscard]] inline constexpr uint64_t bagTagOf(uint64_t tagState) noexcept {
        return tagState >> 2;
    }
    [[nodiscard]] inline constexpr BagState bagStateOf(uint64_t tagState) noexcept {
        return static_cast<BagState>(tagState & uint64_t{3});
    }

    // ─── Reader slot ─────────────────────────────────────────────────────────
    //
    // One per possible CPU per domain. `state` is written only by the owning CPU
    // (I4); every other CPU only ever loads it. nesting / openBagIndex /
    // retireCount / inDrain are owner-only and deliberately NON-ATOMIC (I5):
    // correct because interrupt nesting on one CPU is serialized, but they will
    // look like a bug to a reviewer and to TSan if a test ever drives one slot
    // from two threads. Harnesses must bind one thread per slot.
    //
    // alignas is a bare 64 rather than arch::CACHE_LINE_SIZE because Core cannot
    // include arch.h without breaking RCU-DEC-002 — the same convention
    // TreiberStack.h and SplitBitmap.h already use (P1-DEC-002). P1-I2 needs
    // exactly this: a slot's state word must share no cache line with another
    // slot's, so the scan's per-slot loads do not false-share with a reader's
    // activation store.
    struct alignas(64) ReaderSlot {
        Atomic<uint64_t>    state{kInactive};      // I4: owner writes, everyone reads
        uint64_t            nesting      = 0;      // I5: owner-only, plain
        uint64_t            openBagIndex = 0;      // I5: owner-only bookkeeping
        uint64_t            retireCount  = 0;      // I5: advance threshold counter
        bool                inDrain      = false;  // I5: I14 + the RCU-DEC-038 assert
        Atomic<RetireHead*> bagHead[kBagCount]     = {};
        Atomic<uint64_t>    bagTagState[kBagCount] = {};   // (tag : 62, state : 2)
    };

    // ─── Hooks policy (RCU-DEC-017, P1-DEC-009) ──────────────────────────────
    //
    // Two unrelated jobs, one mechanism:
    //
    //   onPreTouch — the load-bearing one. Stealing (RCU-DEC-006) means a drainer
    //     dereferences intrusive links living in ANOTHER CPU's retired slab
    //     memory, whose pages are subject to reclaimSlabPage — the vmsmalloc
    //     DEC-047 stale-TLB bug class. The Phase-2 veneer supplies a hook calling
    //     VMSubstrate::ensureTLBEntryFresh. It fires once per node, before ANY
    //     read of that node's RetireHead fields, and covers ONLY those fields —
    //     not the object body and not anything the deleter reaches through.
    //     A Hooks policy that omits the freshness call is a Phase-2 bug.
    //
    //   the named protocol points — test instrumentation. The torture harness
    //     supplies a Hooks whose points spin on atomic gates, inducing exactly the
    //     delays I3 licenses; kernel and release builds compile them away.
    //     [[no_unique_address]] plus inlined empty bodies is a *provable* zero,
    //     which is why this beats an #ifdef.
    //
    // WINDOW-INTERIOR points are marked below. They sit inside the RCU-DEC-024
    // interrupt-masked, fault-free readLock/readUnlock transition window in the
    // kernel veneer, so a kernel Hooks MUST leave them empty: a point that
    // touches anything — a counter in vmsmalloc memory, a log, MMIO — breaks the
    // audited no-instruction-can-fault claim exactly as the evicted monoTimens
    // stall-stamp did, and a spinning point would spin with interrupts masked.
    // A non-inlined but EMPTY call is fine; the kernel stack is in RCU-DEC-024's
    // audited region list.
    struct NoopRcuHooks {
        // WINDOW-INTERIOR. readLock: between the epoch load and the activation
        // store. This is the I3 stall-injection point — an arbitrarily long delay
        // here must only delay advancement, never permit it.
        void onAfterEpochLoad(uint64_t) const noexcept {}
        // WINDOW-INTERIOR. readLock: after kReaderActivationFence.
        void onAfterActivation(uint64_t) const noexcept {}
        // WINDOW-INTERIOR. readUnlock: before the deactivation store.
        void onBeforeDeactivation() const noexcept {}

        // tryAdvance: after kScanEpochLoad, before kScanFence (the I10 seam).
        void onAfterScanEpochLoad(uint64_t) const noexcept {}
        // tryAdvance: scan came back clean, immediately before the epoch CAS.
        void onBeforeEpochAdvance(uint64_t) const noexcept {}
        // retire: after kRetireFence and the retire-path epoch load.
        void onAfterRetireEpochLoad(uint64_t) const noexcept {}
        // (slot, bag) immediately before an Open -> Sealed store.
        void onBeforeSeal(size_t, size_t) const noexcept {}
        // (slot, bag) immediately after winning a Sealed -> Claimed CAS, before
        // the winner touches head. The HAZARD-1 race window.
        void onAfterClaim(size_t, size_t) const noexcept {}

        // Per node, before any read of its RetireHead fields (RCU-DEC-017).
        void onPreTouch(RetireHead*) const noexcept {}
    };

    template <typename H>
    concept RcuHooks = requires(H& h, RetireHead* n, uint64_t e, size_t i) {
        { h.onAfterEpochLoad(e) };
        { h.onAfterActivation(e) };
        { h.onBeforeDeactivation() };
        { h.onAfterScanEpochLoad(e) };
        { h.onBeforeEpochAdvance(e) };
        { h.onAfterRetireEpochLoad(e) };
        { h.onBeforeSeal(i, i) };
        { h.onAfterClaim(i, i) };
        { h.onPreTouch(n) };
    };

    // ─── Memory-ordering constants (RCU-DEC-010) ─────────────────────────────
    //
    // Verbatim adoption of vmsmalloc's DEC-042 policy: every atomic site
    // references one of these by name AND re-cites the ordering inline at the
    // call site, so any downgrade is a one-point edit and an audit trigger.
    // ARMv8 is the release gate and x86-TSO would silently tolerate downgrades
    // that break on weak memory — which is what makes this load-bearing rather
    // than cosmetic.
    //
    // Three of these are correct for reasons that have nothing to do with their
    // obvious purpose, and each will look over-strong to someone optimizing later:
    //   kStatePublish is RELEASE not because the activation needs publishing but
    //     because C++20 P0982 removed same-thread relaxed stores from release
    //     sequences (RCU-DEC-020);
    //   kEpochLoadOnRetire is ACQUIRE not for ITEM-007 but because it feeds the
    //     drain path (RCU-DEC-021);
    //   kEpochAdvance's RELEASE half exists for the drainer, not the scanner.
    inline constexpr MemoryOrder kEpochLoadOnEnter      = RELAXED;  // section-entry snapshot; the fence does the work
    inline constexpr MemoryOrder kStatePublish          = RELEASE;  // activation store — NOT relaxed, RCU-DEC-020
    inline constexpr MemoryOrder kReaderActivationFence = SEQ_CST;  // DO NOT DOWNGRADE
    inline constexpr MemoryOrder kStateRetire           = RELEASE;  // deactivation store at section exit
    inline constexpr MemoryOrder kScanEpochLoad         = ACQUIRE;  // MUST be sequenced BEFORE kScanFence — I10
    inline constexpr MemoryOrder kScanFence             = SEQ_CST;  // DO NOT DOWNGRADE
    inline constexpr MemoryOrder kScanLoad              = ACQUIRE;  // per-slot state load during the scan
    inline constexpr MemoryOrder kEpochAdvance          = ACQ_REL;  // epoch CAS success; RELEASE half is RCU-DEC-021's
    inline constexpr MemoryOrder kEpochAdvanceFailure   = RELAXED;  // benign — someone else advanced
    inline constexpr MemoryOrder kRetireFence           = SEQ_CST;  // between the caller's unlink and the epoch load
    inline constexpr MemoryOrder kEpochLoadOnRetire     = ACQUIRE;  // RCU-DEC-021, not RCU-DEC-018
    inline constexpr MemoryOrder kSweepEpochLoad        = ACQUIRE;  // drives the expiry decision — RCU-DEC-021
    inline constexpr MemoryOrder kSynchronizeFence      = SEQ_CST;  // before synchronize's epoch read — RCU-DEC-023
    inline constexpr MemoryOrder kBarrierEntryFence     = SEQ_CST;  // after barrier's seal-and-rotate — RCU-DEC-036
    inline constexpr MemoryOrder kBagTagLoad            = ACQUIRE;  // bag tagState load
    inline constexpr MemoryOrder kBagSeal               = RELEASE;  // Open -> Sealed
    inline constexpr MemoryOrder kBagClaim              = ACQ_REL;  // Sealed -> Claimed CAS success
    inline constexpr MemoryOrder kBagClaimFailure       = RELAXED;  // lost the claim to another CPU
    inline constexpr MemoryOrder kBagRelease            = RELEASE;  // Claimed -> Free (bag emptied)
    inline constexpr MemoryOrder kBagReseal             = RELEASE;  // Claimed -> Sealed (batch bound hit)
    inline constexpr MemoryOrder kBagPush               = RELEASE;  // push onto a bag head
    inline constexpr MemoryOrder kBagPushLoad           = RELAXED;  // push CAS initial load
    inline constexpr MemoryOrder kBagPushFailure        = RELAXED;  // push CAS retry
    //
    // Three constants below are NOT in the parent spec's ordering table, which
    // covers the sites the design's proofs turn on. They are named here anyway so
    // that every atomic access in this file carries a named ordering, per the
    // spirit of RCU-DEC-010:
    //
    //   kBagOpen     — Free -> Open and the I11 tag bump. Both are owner-side
    //                  transitions on a bag no other CPU may write, and their
    //                  visibility to a future thief rides the subsequent
    //                  kBagSeal RELEASE. RELAXED is sufficient and correct.
    //   kBagDrainAccess — head reads/writes by the CPU holding a bag Claimed.
    //                  I6 grants that CPU exclusive ownership of head, so these
    //                  are the "plain ops" of RCU-DEC-033; visibility rides
    //                  kBagClaim / kBagReseal / kBagRelease on tagState. RELAXED
    //                  rather than a non-atomic access only because head is an
    //                  Atomic<> that concurrent observers (barrier's termination
    //                  predicate, DebugIntrospection) may legally load.
    //   kEpochObserve — diagnostic / termination-predicate epoch reads.
    inline constexpr MemoryOrder kBagOpen               = RELAXED;
    inline constexpr MemoryOrder kBagDrainAccess        = RELAXED;
    inline constexpr MemoryOrder kEpochObserve          = ACQUIRE;

    template <typename Hooks>
    struct DebugIntrospection;   // core/rcu/DebugIntrospection.h

    // ─── EpochDomain ─────────────────────────────────────────────────────────
    //
    // One independent grace-period universe. Holds nothing but the epoch, the
    // slot array pointer/count, the drain batch bound, the domain-global
    // teardownActive flag, and the hooks (P1-I4). Construction is trivial; the
    // destructor is a debug-only quiescence assert that compiles to nothing in
    // release.
    template <typename Hooks = NoopRcuHooks>
    requires RcuHooks<Hooks>
    class EpochDomain {
    public:
        // The constraint goes through Core's variable template rather than
        // naming __is_constructible directly: GCC 15 — the kernel compiler —
        // rejects a built-in trait in a function signature outright, while Clang
        // (the harness compiler) accepts it, so the spec's literal spelling
        // builds green in tests and fails the cross build.
        EpochDomain(ReaderSlot* slotArray, size_t count) noexcept
            requires is_default_constructible_v<Hooks>
            : slots(slotArray), slotCount(count) {}

        EpochDomain(ReaderSlot* slotArray, size_t count, Hooks h) noexcept
            : slots(slotArray), slotCount(count), hooks(h) {}

        EpochDomain(const EpochDomain&)            = delete;
        EpochDomain& operator=(const EpochDomain&) = delete;

        // RCU-DEC-034: performs NO draining and NO waiting. It debug-asserts full
        // quiescence — every bag Free and empty, no active section, no in-flight
        // drain. The sanctioned teardown is: externally guarantee no CPU will
        // touch the domain again, call drainAllQuiescent(), then destroy.
        //
        // Force-draining here was rejected on four grounds, the sharpest being
        // that it converts an unambiguous caller bug into an infinite spin inside
        // a destructor, where the assertion makes it a loud, attributable panic.
        // In release the assert compiles out and the caller is trusted; the
        // consequence (leaked objects whose deleters never run) is not defended.
        ~EpochDomain() { checkQuiescent(); }

        // ─── Read side ───────────────────────────────────────────────────────
        //
        // Loop-free, allocation-free, and legal in any context AT THE ENGINE
        // LEVEL — the kernel veneer narrows that to "any context except NMI/#MC"
        // (RCU-DEC-024), a narrowing an engine-level reader must not miss.
        //
        // The RELAXED-load / RELEASE-store / SEQ_CST-fence shape is deliberate: it
        // is the cheaper half of the Dekker-style pairing with the scan's fence,
        // and it is what makes I3 sufficient. An arbitrarily long delay between
        // the epoch load and the activation store is safe precisely because I3
        // makes a stale snapshot BLOCK rather than permit.
        //
        // NOT reentrancy-safe on its own: the caller must guarantee the outermost
        // transition is not reentered on the same slot.
        void readLock(size_t slot) CROCOS_RCU_NOEXCEPT {
            assert(slot < slotCount, "rcu: slot index out of range");
            ReaderSlot& s = slots[slot];
            if (s.nesting++ != 0) return;                          // I5: plain, owner-only

            const uint64_t e = globalEpoch.load(kEpochLoadOnEnter); // RELAXED
            hooks.onAfterEpochLoad(e);                             // WINDOW-INTERIOR (I3 stall point)
            s.state.store(makeActive(e), kStatePublish);            // RELEASE — RCU-DEC-020
            thread_fence(kReaderActivationFence);                   // SEQ_CST — the entire hot-path cost
            hooks.onAfterActivation(e);                            // WINDOW-INTERIOR
        }

        void readUnlock(size_t slot) CROCOS_RCU_NOEXCEPT {
            assert(slot < slotCount, "rcu: slot index out of range");
            ReaderSlot& s = slots[slot];
            assert(s.nesting > 0, "rcu: readUnlock without a matching readLock");
            if (--s.nesting != 0) return;

            hooks.onBeforeDeactivation();                          // WINDOW-INTERIOR
            s.state.store(kInactive, kStateRetire);                 // RELEASE — orders section reads before
        }

        // ─── Retire ──────────────────────────────────────────────────────────
        //
        // O(1) amortized, CPU-local, never allocates (I8). May drain expired bags
        // and may attempt an epoch advance, so it is NOT legal in every context —
        // Phase 2 inherits vmsmalloc's DEC-014 rule (forbidden in IRQ/NMI/#UD/
        // #DF/#GP/#MC; conditionally legal in #PF).
        //
        // Forward progress for the entire framework is pulled from this path
        // (RCU-DEC-005). No daemon, no tick, no IPI.
        void retire(size_t slot, RetireHead* node) CROCOS_RCU_NOEXCEPT {
            assert(slot < slotCount, "rcu: slot index out of range");
            assert(node != nullptr, "rcu: retire of a null node");
            assert(node->next == nullptr,
                   "rcu: retire of an already-linked RetireHead (double retire)");
            ReaderSlot& s = slots[slot];
            // RCU-DEC-019 as amended by RCU-DEC-038. A WRITER unlinking from the
            // live structure still requires a section unconditionally — it
            // traverses the shared structure, so an unpinned writer can have the
            // parent reclaimed under it. The carve-outs exist because deleters
            // legitimately retire children from sweep paths whose callers are
            // *required* to be outside any section, and because the teardown drain
            // has no bound slot at all. The assert can no longer distinguish a
            // legal child-retire from a deleter illegally acting as a writer;
            // that discipline now rests on RCU-DEC-039's contract clause.
            assert(s.nesting > 0 || s.inDrain || teardownActive,
                   "rcu: retire outside a read-side section, a drain, or teardown");

            // RCU-DEC-018. THE fix for ITEM-007, and it must not be moved, elided,
            // or made conditional. A SEQ_CST *load* is explicitly not a
            // substitute: Lemma B needs an SC fence at this end, and on AArch64 a
            // seq_cst load is LDAR, which does not order the preceding unlink
            // against itself in the store-buffer direction. DMB ISH is required.
            thread_fence(kRetireFence);                             // SEQ_CST
            const uint64_t e = globalEpoch.load(kEpochLoadOnRetire); // ACQUIRE — RCU-DEC-021
            hooks.onAfterRetireEpochLoad(e);
            assert(e <= kMaxEpoch, "rcu: epoch exceeded its effective width (I9)");

            const OpenBag target = prepareOpenBag(s, slot, e, /*allowManufacture=*/true);

            pushNode(s.bagHead[target.index], node);

            // Hazards note: the owner-push / drainer-take exclusion (I6) rests on
            // three things, and only two of them are structural — the third is
            // "the owner is not reentrant into its own bag", which no invariant
            // covers. RCU-DEC-027 removed the deleter-retires case and
            // RCU-DEC-030 the sweeper-recursion case, but #PF reentrancy remains a
            // pure contract obligation. One load catches exactly that class.
            const uint64_t postPush = s.bagTagState[target.index].load(kBagTagLoad);
            assert(postPush == target.tagState,
                   "rcu: open bag changed identity across a push (owner reentrancy)");
            (void)postPush;

            if (++s.retireCount >= kRetireAdvanceThreshold) {
                s.retireCount = 0;
                tryAdvance(slot);
            }
        }

        // ─── Epoch advance ───────────────────────────────────────────────────
        //
        // Safe to call from any number of CPUs simultaneously; a losing CAS is not
        // an error, it means the work was already done. Takes the calling slot so
        // it can honour I14 and so the sweep can start locally (P1-DEC-008).
        bool tryAdvance(size_t slot) CROCOS_RCU_NOEXCEPT {
            assert(slot < slotCount, "rcu: slot index out of range");
            ReaderSlot& self = slots[slot];

            // I14 — no reentry from a deleter. RCU-DEC-037 — no entry into the
            // fenced protocol while a universal-owner teardown drain is mixing
            // plain stores into the same state. tryAdvance only CHECKS inDrain; it
            // must never SET it, or its own unconditional call into sweepExpired
            // below would hit that function's top-of-body check and the mandatory
            // sweep would silently never run.
            if (self.inDrain || teardownActive) return false;

            bool advanced = false;

            // I10 — THE EPOCH LOAD IS SEQUENCED BEFORE THE FENCE. Lemma B fires
            // C++20 [atomics.order]/4 with this load on the
            // happens-before-the-fence side. Swapping these two lines as a
            // cosmetic cleanup collapses the entire safety argument while changing
            // no ordering constant, and x86-TSO would not fail. This is a
            // correctness change, not a style change.
            const uint64_t e = globalEpoch.load(kScanEpochLoad);    // ACQUIRE
            hooks.onAfterScanEpochLoad(e);

            // Own-slot early-out. If OUR slot is active with a stale snapshot the
            // scan below is guaranteed to fail, so skip the fence and the O(P)
            // remote-line walk. NOTE: this is NOT P1-DEC-004's forbidden
            // optimization — that one omits the own slot FROM the scan; this one
            // uses the own slot to decide not to scan at all. Skipping an advance
            // is always safe; skipping a slot is not.
            const uint64_t own = self.state.load(kScanLoad);         // ACQUIRE
            if (!(isActive(own) && epochOf(own) != e)) {
                thread_fence(kScanFence);                            // SEQ_CST — pairs with kReaderActivationFence

                bool blocked = false;
                for (size_t i = 0; i < slotCount; ++i) {             // never skips self — P1-DEC-004
                    const uint64_t w = slots[i].state.load(kScanLoad);  // ACQUIRE
                    if (isActive(w) && epochOf(w) != e) {            // I3: exact match
                        blocked = true;
                        break;
                    }
                }

                if (!blocked) {
                    hooks.onBeforeEpochAdvance(e);
                    // I12 — exactly +1, via an RMW, never skipping a value.
                    // Lemma B relays through L_W --fr--> CAS(r+1) --rf--> L_S2,
                    // which requires the write of r+1 to exist and to be the one
                    // the scan read. A batched advance or a plain store(e+2) would
                    // let L_W and L_S2 read the same write, breaking the coherence
                    // chain with no ordering constant changed — the same
                    // invisible-to-RCU-DEC-010 failure class as I10.
                    uint64_t expected = e;
                    globalEpoch.compare_exchange(expected, e + 1,
                                                 kEpochAdvance,          // ACQ_REL
                                                 kEpochAdvanceFailure);  // RELAXED
                    advanced = true;   // a lost CAS means someone else advanced
                }
            }

            // ALWAYS, and that is load-bearing. An earlier draft returned from
            // inside the scan loop, above the sweep. That silently defeated
            // RCU-DEC-006: while any one CPU blocked advancement, NO CPU swept, so
            // bags that had expired before the stall — provably safe to drain —
            // were held anyway, and the only remaining drain path was the owner's
            // own rotation. Expiry is gated per bag on globalEpoch >= tag + 2 and
            // has no dependency whatsoever on this call's advance succeeding.
            sweepExpired(slot);
            return advanced;
        }

        // ─── Sweep ───────────────────────────────────────────────────────────
        //
        // Drains every slot's claimable bags, not just the caller's own
        // (RCU-DEC-006) — this is what makes memory bounded with zero scheduler or
        // IPI input, because a CPU that retires a pile of objects into bags that
        // have sealed and then goes permanently idle does not strand them.
        //
        // Visit order is own-slot-first then rotated (P1-DEC-008). Safety is
        // visit-order-free — I2's expiry check guards each claim CAS individually
        // — so the order is pure policy: rotation spreads claim-CAS contention
        // that all-CPUs-start-at-slot-0 would concentrate, and own-slot-first adds
        // locality. It permutes order, never membership, so P1-DEC-004 is
        // unaffected.
        //
        // Returns the number of deleters run.
        size_t sweepExpired(size_t slot) CROCOS_RCU_NOEXCEPT {
            assert(slot < slotCount, "rcu: slot index out of range");
            ReaderSlot& self = slots[slot];
            if (self.inDrain || teardownActive) return 0;   // I14 / RCU-DEC-037

            // I14 discipline: the drain body runs only on a clear->set transition,
            // and only the setter clears. Without that, a nested site's exit would
            // clear the outer drain's flag and turn a later legal deleter-retire
            // into a spurious RCU-DEC-038 panic.
            self.inDrain = true;

            // RCU-DEC-033: the bound counts ALL deleter execution triggered by one
            // engine entry, across every bag this call claims — not per bag.
            size_t budget = drainBatchBound;
            size_t ran    = 0;
            const uint64_t e = globalEpoch.load(kSweepEpochLoad);   // ACQUIRE — RCU-DEC-021

            for (size_t k = 0; k < slotCount && budget > 0; ++k) {
                const size_t idx = (slot + k) % slotCount;          // own slot first — P1-DEC-008
                for (size_t b = 0; b < kBagCount && budget > 0; ++b) {
                    ran += claimAndDrain(slots[idx], idx, b, e, budget);
                }
            }

            self.inDrain = false;
            return ran;
        }

        // ─── Completion primitives (RCU-DEC-031, graded by guarantee) ────────
        //
        // synchronize: a grace period has elapsed. NOTHING MORE. It does not
        // promise the caller's retired objects have been destroyed — two epoch
        // advances can complete while the caller's bag is sealed-but-unswept.
        // That weaker guarantee is specified and intended; a caller wanting
        // destruction uses barrier (own retirees) or drainAllQuiescent (teardown).
        //
        // Forbidden inside a section on this domain (it would spin waiting for
        // itself), inside a drain (I14), and — unlike retire — under the STRICT
        // forbidden-context mask with no #PF carve-out: a grace-period wait inside
        // a fault handler spins on other CPUs' progress from a context that may
        // itself be blocking them. Interrupt readers arriving while it spins are
        // explicitly fine: they delay the advance for the duration of the handler
        // and nothing more (I3).
        void synchronize(size_t slot) CROCOS_RCU_NOEXCEPT {
            assert(slot < slotCount, "rcu: slot index out of range");
            ReaderSlot& s = slots[slot];
            assert(s.nesting == 0,
                   "rcu: synchronize from inside a read-side section on this domain");
            // Without this, a deleter-context call HANGS undiagnosed even in a
            // debug build: every inner tryAdvance hits the I14 early-out and the
            // epoch never moves.
            assert(!s.inDrain, "rcu: synchronize from deleter context");
            (void)s;   // assert compiles to nothing in release — TreiberStack.h:388-390

            // RCU-DEC-023. `unlink(); synchronize(); free();` is a teardown idiom a
            // consumer will write, and it has structurally the same shape as
            // retire — so it needs the same fence for the same reason.
            thread_fence(kSynchronizeFence);                        // SEQ_CST
            const uint64_t e0 = globalEpoch.load(kEpochObserve);    // ACQUIRE

            while (globalEpoch.load(kEpochObserve) < e0 + 2) {
                tryAdvance(slot);
            }
        }

        // barrier: every object retired to this domain BEFORE the call has been
        // destroyed — except objects still, at barrier entry, in OTHER slots' Open
        // bags. That exclusion is not a wart: Open -> Sealed is an owner-only plain
        // store (I13) and an owner seals only during its own subsequent retires, so
        // a promise covering remote open bags would contradict the spec's own
        // memory model. Full-domain destruction is drainAllQuiescent, not barrier.
        //
        // The guarantee for the caller's OWN retirees is complete, and it is
        // per-SLOT, not per-thread (RCU-DEC-040): under a future scheduler the
        // retires a barrier is meant to cover must have been issued from the same
        // CPU, i.e. CPU affinity across the retire->barrier span. Preemption and
        // blocking stay permitted; only migration is constrained.
        //
        // Drive-to-completion, not a passive wait — Linux's rcu_barrier can wait
        // passively because kthreads guarantee callbacks eventually run, whereas
        // here nothing runs deleters except a CPU that sweeps, so on a quiet system
        // a passive wait would never terminate. The teardown caller pays for its
        // own teardown.
        void barrier(size_t slot) CROCOS_RCU_NOEXCEPT {
            assert(slot < slotCount, "rcu: slot index out of range");
            ReaderSlot& s = slots[slot];
            assert(s.nesting == 0,
                   "rcu: barrier from inside a read-side section on this domain");
            assert(!s.inDrain, "rcu: barrier from deleter context");

            // (1) Seal AND rotate. The rotate is mandatory, not tidiness: after a
            // bare seal, openBagIndex designates a Sealed bag, and a deleter-retire
            // during barrier's own sweeps would push into it — the I11-fatal case.
            sealAndRotateOwnBag(s, slot);

            // (2) RCU-DEC-036. The RCU-DEC-023-class guarantee for the
            // `unlink(); barrier(); reuse();` idiom, and it must sit after the seal.
            thread_fence(kBarrierEntryFence);                       // SEQ_CST
            const uint64_t e0 = globalEpoch.load(kEpochObserve);    // ACQUIRE

            // (3) Drive until the epoch has passed e0 by two AND nothing
            // Sealed-or-Claimed tagged <= e0 is still non-empty.
            //
            // Both obvious formulations of this loop livelock. "Until globally
            // empty" chases a moving target under sustained remote retire traffic.
            // And leaving Open bags inside the predicate waits on bags that are
            // unclaimable and unsealable by anyone but their owner — it even
            // SELF-livelocks, since the caller's own freshly rotated bag opens
            // tagged e0 and collects deleter-retires.
            //
            // Termination, stated precisely: the Sealed/Claimed-tagged-<=-e0 set is
            // NOT monotone — a remote owner that rotates converts its Open <= e0
            // bag into a new member arbitrarily late, and may do so more than once,
            // since its epoch load can lawfully return values <= e0 for a while.
            // But each slot's successive open-bag tags strictly increase, so per
            // slot only finitely many <= e0 conversions exist; each claimed bag
            // drains finitely many nodes; so the loop terminates.
            //
            // A bag sealed just after the final check stays undrained. That is
            // permissible because the "still in Open bags" exclusion is evaluated
            // AT ENTRY — worth stating, since it is what makes the exit race benign.
            while (globalEpoch.load(kEpochObserve) < e0 + 2 || anyPendingBagAtOrBefore(e0)) {
                tryAdvance(slot);
            }
        }

        // drainAllQuiescent: the teardown drain. Not a completion primitive in the
        // same sense as the other two — it is only legal once the domain is
        // finished with.
        //
        // PRECONDITION, and it is an obligation the engine cannot fully check: a
        // HAPPENS-BEFORE EDGE must exist from every CPU's last operation on this
        // domain to this call. A thread join qualifies. A relaxed done-flag does
        // NOT. The hb edge — not any fence in this entry path — is what makes the
        // plain reads below sound: an entry fence with no prior observing read
        // pairs with nothing, and the drained data (RetireHead fields, object
        // bodies) is non-atomic, so only genuine happens-before covers it.
        //
        // Under that precondition the caller acts as UNIVERSAL OWNER: it performs
        // other slots' owner-side transitions with plain operations, force-seals
        // every Open bag, and drains to empty ignoring drainBatchBound. It is the
        // only entry point allowed to do any of that, and the only one whose caller
        // needs no bound slot at all — which is also how a harness main thread
        // tears a domain down.
        //
        // Returns the number of deleters run.
        size_t drainAllQuiescent() CROCOS_RCU_NOEXCEPT {
            // The assert must cover no-bag-Claimed, not merely no-active-section:
            // a Claimed bag IS concurrent use — an in-flight drain on another CPU —
            // and universal-owner plain ops on it would race the thief.
            assertNoActiveSectionsOrClaims();

            // Required because a deleter-retire crossing the advance threshold
            // would otherwise re-enter tryAdvance mid-universal-owner-mode,
            // interleaving the fenced concurrent protocol with the plain-store
            // quiescent one on the same state. Domain-global rather than a slot's
            // inDrain (RCU-DEC-037 as corrected), precisely because the retire may
            // target a slot other than the drainer's.
            teardownActive = true;

            size_t ran = 0;
            for (;;) {
                bool didWork = false;
                for (size_t i = 0; i < slotCount; ++i) {
                    for (size_t b = 0; b < kBagCount; ++b) {
                        Atomic<uint64_t>& ts = slots[i].bagTagState[b];
                        uint64_t v = ts.load(kBagDrainAccess);       // RELAXED — no concurrency
                        BagState st = bagStateOf(v);
                        if (st == BagState::Free) continue;
                        assert(st != BagState::Claimed,
                               "rcu: bag became Claimed during a teardown drain");

                        const uint64_t tag = bagTagOf(v);
                        if (st == BagState::Open) {
                            ts.store(packBag(tag, BagState::Sealed), kBagDrainAccess);
                        }
                        // Universal owner: claim without a CAS. Legal only because
                        // the precondition removed all concurrency.
                        ts.store(packBag(tag, BagState::Claimed), kBagDrainAccess);

                        size_t budget = kUnboundedDrainBatch;       // exempt from RCU-DEC-033
                        const size_t here = drainClaimedBag(slots[i], b, budget);
                        ran += here;
                        didWork = didWork || here > 0;

                        assert(slots[i].bagHead[b].load(kBagDrainAccess) == nullptr,
                               "rcu: teardown drain left a bag non-empty");
                        ts.store(packBag(tag, BagState::Free), kBagDrainAccess);
                    }
                }
                // Deleters may have retired into bags already visited this pass —
                // legal, and collected by the next one. Terminates because with no
                // new users the total retire volume is finite.
                if (!didWork) break;
            }

            teardownActive = false;
            return ran;
        }

        // RCU-DEC-033. Plain per-domain value read only by the CPU inside a drain,
        // so it costs no atomicity. Must be set before concurrent use begins.
        // kUnboundedDrainBatch is the supported way to run effectively unbounded,
        // and is the default until parent ITEM-005's tuning pass.
        void setDrainBatchBound(size_t bound) CROCOS_RCU_NOEXCEPT { drainBatchBound = bound; }

        [[nodiscard]] size_t getDrainBatchBound() const CROCOS_RCU_NOEXCEPT { return drainBatchBound; }

        [[nodiscard]] uint64_t currentEpoch() const CROCOS_RCU_NOEXCEPT {
            return globalEpoch.load(kEpochObserve);
        }

        [[nodiscard]] size_t getSlotCount() const CROCOS_RCU_NOEXCEPT { return slotCount; }

    private:
        friend struct DebugIntrospection<Hooks>;

        ReaderSlot*      slots;
        size_t           slotCount;
        Atomic<uint64_t> globalEpoch{0};
        size_t           drainBatchBound = kUnboundedDrainBatch;
        bool             teardownActive  = false;
        [[no_unique_address]] Hooks hooks{};

        static constexpr size_t kNoBag = static_cast<size_t>(-1);

        // What prepareOpenBag settled on: the bag to push into, plus the exact
        // tagState value it left there, so retire's reentrancy re-check needs no
        // second load on the fast path.
        struct OpenBag {
            size_t   index;
            uint64_t tagState;
        };

        // ─── Bag head push ───────────────────────────────────────────────────
        //
        // A CAS loop rather than a plain store even though an Open bag has exactly
        // one legal writer. Push-only Treiber is ABA-immune on its own — a
        // successful CAS means head == old NOW and node->next == old, so the
        // resulting list is consistent regardless of the location's history — and
        // the loop costs one uncontended CAS on a path that has no contention by
        // construction. Bag heads carry no ABA tag at all (RCU-DEC-028): the take
        // path has no CAS on head, only plain pops under Claimed exclusivity, so
        // there is no ABA-sensitive comparison anywhere in the design. That matters
        // concretely, not aesthetically — the kernel target has no lock-free
        // 16-byte CAS, which is why vmsmalloc had to invent a packed 64-bit head
        // encoding.
        static void pushNode(Atomic<RetireHead*>& head, RetireHead* node) CROCOS_RCU_NOEXCEPT {
            RetireHead* old = head.load(kBagPushLoad);              // RELAXED
            do {
                node->next = old;
            } while (!head.compare_exchange(old, node,
                                            kBagPush,               // RELEASE
                                            kBagPushFailure));      // RELAXED
        }

        [[nodiscard]] static size_t findFreeBag(ReaderSlot& s) CROCOS_RCU_NOEXCEPT {
            for (size_t b = 0; b < kBagCount; ++b) {
                if (bagStateOf(s.bagTagState[b].load(kBagTagLoad)) == BagState::Free) return b;
            }
            return kNoBag;
        }

        // ─── prepareOpenBag — bag selection and rotation ─────────────────────
        //
        // Returns the index of the bag `retire` should push into, having ensured it
        // is Open and tagged >= the current epoch (I11: a tag LARGER than r(n) is
        // always safe, since it only delays the drain; a tag SMALLER is a UAGP).
        //
        // Bag selection is openBagIndex, NOT `epoch mod kBagCount` (RCU-DEC-027).
        // That framing was the error behind ITEM-001: all three candidate fixes
        // tried to HANDLE the claim-race collision rather than make it
        // non-load-bearing. Linear probing in particular is a UAGP generator — it
        // destroys retired I7's premise (tag == index mod kBagCount) while leaving
        // I7's conclusion licensing an unconditional drain.
        //
        // Rotation is opportunistic and NEVER blocks. If no Free bag exists and one
        // cannot be manufactured, the owner keeps its current open bag one epoch
        // longer via the tag bump — degradation is one bag growing with a rising
        // tag, i.e. reclamation LATENCY, never blocking and never unbounded beyond
        // what the workload retires during the stall.
        //
        // NOTE ON SHAPE: rcu.md's retire sketch bumps the tag before calling
        // maybeRotate, which makes maybeRotate's own trigger ("e exceeds the open
        // bag's tag") permanently false — rotation could never fire and every
        // retire would pile into one bag forever. Rotate-first with the bump as the
        // failure fallback is what RCU-DEC-027 and P1-ITEM-001's prose actually
        // describe, and it seals the old bag at its lower tag t so it expires at
        // t+2 rather than e+2. Confirmed with the spec author 2026-08-01.
        OpenBag prepareOpenBag(ReaderSlot& s, size_t slot, uint64_t e,
                               bool allowManufacture) CROCOS_RCU_NOEXCEPT {
            // At most two iterations: the manufacture attempt is one-shot, and it
            // re-enters only because a deleter it ran may itself have rotated the
            // slot under us.
            for (;;) {
                const size_t i = s.openBagIndex;
                const uint64_t v = s.bagTagState[i].load(kBagTagLoad);   // ACQUIRE
                const BagState st = bagStateOf(v);

                if (st == BagState::Free) {
                    const uint64_t opened = packBag(e, BagState::Open);
                    s.bagTagState[i].store(opened, kBagOpen);            // RELAXED
                    return { i, opened };
                }
                assert(st == BagState::Open,
                       "rcu: openBagIndex must designate a Free or Open bag");

                const uint64_t t = bagTagOf(v);
                if (t >= e) return { i, v };                 // already this epoch's bag

                const size_t j = findFreeBag(s);
                if (j != kNoBag) {
                    // Acquire before release: open the new bag FIRST, so the owner
                    // is never left without one.
                    const uint64_t opened = packBag(e, BagState::Open);
                    s.bagTagState[j].store(opened, kBagOpen);            // RELAXED
                    hooks.onBeforeSeal(slot, i);
                    // RELEASE, and load-bearing: it orders every push into bag i
                    // before the seal, which is what the claimer's kBagClaim
                    // ACQUIRE synchronizes with. A relaxed seal severs that edge.
                    s.bagTagState[i].store(packBag(t, BagState::Sealed), kBagSeal);
                    s.openBagIndex = j;
                    return { j, opened };
                }

                if (allowManufacture && !s.inDrain) {
                    allowManufacture = false;
                    manufactureFreeBag(s, slot);
                    continue;   // openBagIndex may have moved under a deleter-retire
                }

                // Fallback (RCU-DEC-027): keep the current bag one epoch longer.
                // Safe by I11 — bumping a tag upward only ever delays the drain.
                const uint64_t bumped = packBag(e, BagState::Open);
                s.bagTagState[i].store(bumped, kBagOpen);                // RELAXED
                return { i, bumped };
            }
        }

        // Try to make a Free bag by claiming and draining this slot's own expired
        // Sealed bags. Losing a claim CAS to a thief is a no-op — the owner simply
        // ends up keeping its current bag one epoch longer.
        //
        // I14: this sits on the retire / #PF fast path, which is the exact latency
        // spike RCU-DEC-033's bound targets, so it respects drainBatchBound. The
        // caller has already checked !inDrain, so this is a clear->set transition
        // and only this frame clears.
        void manufactureFreeBag(ReaderSlot& s, size_t slot) CROCOS_RCU_NOEXCEPT {
            s.inDrain = true;
            size_t budget = drainBatchBound;
            const uint64_t e = globalEpoch.load(kSweepEpochLoad);   // ACQUIRE
            for (size_t b = 0; b < kBagCount && budget > 0; ++b) {
                claimAndDrain(s, slot, b, e, budget);
            }
            s.inDrain = false;
        }

        // barrier's step (1). Seals the caller's own open bag and opens a
        // replacement, manufacturing a Free bag if none exists — draining our own
        // expired bags, and driving advances until one expires. It terminates
        // because barrier itself drives the advances.
        void sealAndRotateOwnBag(ReaderSlot& s, size_t slot) CROCOS_RCU_NOEXCEPT {
            for (;;) {
                const size_t i = s.openBagIndex;
                const uint64_t v = s.bagTagState[i].load(kBagTagLoad);   // ACQUIRE
                if (bagStateOf(v) == BagState::Free) return;             // nothing retired here yet

                assert(bagStateOf(v) == BagState::Open,
                       "rcu: openBagIndex must designate a Free or Open bag");

                // An empty open bag holds no pre-call retirees, so there is nothing
                // to seal and nothing for the barrier to wait on. Leaving it Open
                // also keeps deleter-retires during our own sweeps out of the
                // I11-fatal push-into-a-Sealed-bag case, which is the whole reason
                // the rotate is mandatory in the first place.
                if (s.bagHead[i].load(kBagDrainAccess) == nullptr) return;

                const size_t j = findFreeBag(s);
                if (j != kNoBag) {
                    const uint64_t e = globalEpoch.load(kEpochObserve);  // ACQUIRE
                    s.bagTagState[j].store(packBag(e, BagState::Open), kBagOpen);
                    hooks.onBeforeSeal(slot, i);
                    s.bagTagState[i].store(packBag(bagTagOf(v), BagState::Sealed), kBagSeal);
                    s.openBagIndex = j;
                    return;
                }

                // I14 discipline: the manufacture step sets inDrain for its own
                // duration and clears it before we call tryAdvance (which would
                // otherwise hit the early-out) or return to barrier's main loop.
                if (!s.inDrain) manufactureFreeBag(s, slot);
                if (findFreeBag(s) == kNoBag) tryAdvance(slot);   // drive expiry
            }
        }

        // ─── Claim + drain one bag ───────────────────────────────────────────
        //
        // Returns the number of deleters run, and decrements `budget` by the same.
        size_t claimAndDrain(ReaderSlot& owner, size_t ownerIndex, size_t bag,
                             uint64_t epoch, size_t& budget) CROCOS_RCU_NOEXCEPT {
            Atomic<uint64_t>& ts = owner.bagTagState[bag];
            const uint64_t v = ts.load(kBagTagLoad);                // ACQUIRE
            if (bagStateOf(v) != BagState::Sealed) return 0;

            const uint64_t tag = bagTagOf(v);
            if (epoch < tag + 2) return 0;                          // I2 — the reclaim gate

            uint64_t expected = v;
            if (!ts.compare_exchange(expected, packBag(tag, BagState::Claimed),
                                     kBagClaim,                     // ACQ_REL
                                     kBagClaimFailure)) {           // RELAXED
                return 0;   // another CPU won; benign
            }
            hooks.onAfterClaim(ownerIndex, bag);

            const size_t ran = drainClaimedBag(owner, bag, budget);

            if (owner.bagHead[bag].load(kBagDrainAccess) != nullptr) {
                // RCU-DEC-033: batch bound hit mid-bag. Re-seal with the tag
                // UNCHANGED, leaving the remainder in a visible, still-expired,
                // immediately re-claimable bag — which is what keeps barrier's
                // all-bags accounting sound. A drainer-private remainder list was
                // rejected for exactly that reason.
                //
                // RELEASE, not plain. The plain form is a data race: the next
                // claimer would read a stale remainder head and re-run freed
                // deleters, and the tagState value is bit-identical before and
                // after the drain, so coherence rescues nothing.
                ts.store(packBag(tag, BagState::Sealed), kBagReseal);   // RELEASE
            } else {
                ts.store(packBag(tag, BagState::Free), kBagRelease);    // RELEASE
            }
            return ran;
        }

        // Pop-run-pop, in place. NOT a wholesale head.exchange(nullptr) detach —
        // that form makes the batch-bound remainder invisible to barrier, so do not
        // reintroduce it. Exclusivity comes from the Claimed state itself (I6: only
        // the claim winner touches head), which is why the pops are plain ops.
        //
        // A deleter that itself retires is safe and expected: its pushes go to its
        // OWN slot's Open bag, which is a different bag from this Claimed one by
        // construction, and recursion back into the drain is blocked by inDrain.
        size_t drainClaimedBag(ReaderSlot& owner, size_t bag, size_t& budget) CROCOS_RCU_NOEXCEPT {
            Atomic<RetireHead*>& head = owner.bagHead[bag];
            size_t ran = 0;

            RetireHead* n = head.load(kBagDrainAccess);             // RELAXED — I6 exclusivity
            while (n != nullptr && budget > 0) {
                // RCU-DEC-017, and this is the line stealing makes necessary: `n`
                // may live in another CPU's retired slab memory whose page has been
                // reclaimed and re-backed, so THIS CPU's TLB entry for it may be
                // stale. Fires before any read of n's RetireHead fields — and
                // covers only those fields, not the object body.
                hooks.onPreTouch(n);

                RetireHead* const next = n->next;
                void (* const deleter)(RetireHead*) = n->deleter;
                assert(deleter != nullptr, "rcu: retired node has no deleter");

                head.store(next, kBagDrainAccess);                  // RELAXED — pop
                n->next = nullptr;                                  // clears the double-retire marker
                --budget;
                ++ran;

                deleter(n);                                         // may reenter retire
                n = next;
            }
            return ran;
        }

        // barrier's termination predicate. Ranges over Sealed/Claimed ONLY — Open
        // bags are outside the predicate exactly as they are outside the promise.
        //
        // The tagState and head reads are not atomic with respect to each other, so
        // a bag can transition out from under us between them. Benign: the worst
        // case is one extra loop iteration, and once the epoch passes e0 every
        // newly opened bag is tagged > e0, so the set this scans strictly shrinks.
        [[nodiscard]] bool anyPendingBagAtOrBefore(uint64_t e0) const CROCOS_RCU_NOEXCEPT {
            for (size_t i = 0; i < slotCount; ++i) {
                for (size_t b = 0; b < kBagCount; ++b) {
                    const uint64_t v = slots[i].bagTagState[b].load(kBagTagLoad);   // ACQUIRE
                    const BagState st = bagStateOf(v);
                    if (st != BagState::Sealed && st != BagState::Claimed) continue;
                    if (bagTagOf(v) > e0) continue;
                    if (slots[i].bagHead[b].load(kBagDrainAccess) != nullptr) return true;
                }
            }
            return false;
        }

        void assertNoActiveSectionsOrClaims() const CROCOS_RCU_NOEXCEPT {
            for (size_t i = 0; i < slotCount; ++i) {
                assert(!isActive(slots[i].state.load(kScanLoad)),
                       "rcu: drainAllQuiescent with an active read-side section");
                assert(slots[i].nesting == 0,
                       "rcu: drainAllQuiescent with an active read-side section");
                for (size_t b = 0; b < kBagCount; ++b) {
                    assert(bagStateOf(slots[i].bagTagState[b].load(kBagTagLoad)) != BagState::Claimed,
                           "rcu: drainAllQuiescent with a bag mid-drain on another CPU");
                }
            }
        }

        // RCU-DEC-034's check, factored out so DebugIntrospection::assertQuiescent()
        // can exercise it directly. Routing the negative test through the
        // destructor is impossible: under the test harness assert throws, and a
        // throwing assert cannot escape a noexcept destructor.
        void checkQuiescent() const CROCOS_RCU_NOEXCEPT {
            for (size_t i = 0; i < slotCount; ++i) {
                assert(!isActive(slots[i].state.load(kScanLoad)),
                       "rcu: domain destroyed with an active read-side section");
                assert(slots[i].nesting == 0,
                       "rcu: domain destroyed with an active read-side section");
                assert(!slots[i].inDrain,
                       "rcu: domain destroyed with a drain in flight");
                for (size_t b = 0; b < kBagCount; ++b) {
                    assert(bagStateOf(slots[i].bagTagState[b].load(kBagTagLoad)) == BagState::Free,
                           "rcu: domain destroyed with a non-Free limbo bag");
                    assert(slots[i].bagHead[b].load(kBagDrainAccess) == nullptr,
                           "rcu: domain destroyed with objects still in limbo");
                }
            }
        }
    };

}

#endif //CROCOS_RCU_EPOCHDOMAIN_H
