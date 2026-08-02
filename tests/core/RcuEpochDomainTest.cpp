//
// Single-threaded unit tests for Core::rcu::EpochDomain (RCU Phase 1).
// Created by Spencer Martin on 8/1/26.
//
// Coverage map — see specs/rcu-phase-1.md "Verification Targets" for the
// authoritative list. Concurrent coverage (HAZARD-1 / I6, the re-seal hand-off,
// and the TSan release gate) lives in RcuConcurrentTest.cpp.
//
//   - encoding: state word + bagTagState round trips; zero-init decodes as
//     "inactive / (tag 0, Free)"  (P1-I3)
//   - I1  advance blocked by an active slot whose snapshot is stale
//   - I3  a stale snapshot DELAYS advancement and never permits it, driven by an
//         injected stall between the epoch load and the activation store
//   - I2  no bag drained before globalEpoch >= tag + 2, read out of the bag
//         algebra through DebugIntrospection
//   - P1-DEC-004  the scan never skips the caller's own slot, so a writer
//         retiring from inside its own section blocks its own advance
//   - bag machine: rotation seals the old bag at ITS tag and opens a new one,
//         leaving exactly one Open bag per slot at all times
//   - synchronize: advances exactly two epochs, and does NOT imply destruction
//   - barrier: destroys the caller's own pre-call retirees including its
//         still-Open bag; excludes remote Open-bag residents; drains remote
//         Sealed bags; does NOT cover objects retired DURING the call
//   - drainBatchBound: a bound hit mid-bag re-seals with the tag unchanged and
//         leaves the remainder re-claimable; the default is unbounded
//   - drainAllQuiescent: empties the domain including remote Open bags and
//         deleter-retires targeting a slot other than the drainer's
//   - negative tests: quiescence check fires; retire outside a section asserts;
//         synchronize/barrier from deleter context assert rather than hang
//   - deleters that themselves retire, from a barrier sweep with nesting == 0
//         (RCU-DEC-038)
//   - quiet-system residue via stealing: a slot's SEALED bags drain on another
//         slot's calls; its OPEN bag is the exact residue  (I13, ITEM-014)
//   - onPreTouch fires once per node  (RCU-DEC-017)
//
// TWO HARNESS INTERACTIONS WORTH KNOWING BEFORE EDITING THIS FILE.
//
//  1. tryAdvance ALWAYS sweeps, including when its own CAS fails or it never
//     attempts one. That is load-bearing (without it, one blocked CPU stops every
//     CPU from sweeping), but it means a test cannot advance the epoch "quietly":
//     any helper that drives the epoch also drains every expired bag in the
//     domain. Several tests below are ordered around that.
//
//  2. ~EpochDomain debug-asserts full quiescence (RCU-DEC-034), and under this
//     harness assert(...) THROWS — out of a noexcept destructor, which is
//     std::terminate, not a reported failure. So domains are heap-allocated via
//     Owned<> and destroyed only on the success path; a test that fails part-way
//     deliberately leaks its domain so the harness reports the failure instead of
//     taking the whole runner down with it. Same reasoning as
//     specs/rcu-phase-3.md P3-DEC-007.
//

#include "../test.h"
#include <harness/TestHarness.h>

#define CROCOS_RCU_TEST_HARNESS 1
#include <core/rcu/EpochDomain.h>
#include <core/rcu/DebugIntrospection.h>

#include <stddef.h>

using Core::rcu::EpochDomain;
using Core::rcu::ReaderSlot;
using Core::rcu::RetireHead;
using Core::rcu::NoopRcuHooks;
using Core::rcu::DebugIntrospection;
using Core::rcu::BagState;
using Core::rcu::kBagCount;
using Core::rcu::kUnboundedDrainBatch;

// ============================================================
// Test node, deleter, and destruction tracking
// ============================================================

namespace {

    struct TestNode {
        RetireHead head;
        int        id = 0;
    };

    TestNode* nodeOf(RetireHead* h) {
        return reinterpret_cast<TestNode*>(reinterpret_cast<char*>(h) - offsetof(TestNode, head));
    }

    constexpr int kMaxTrackedNodes = 256;

    int  gDestroyCount = 0;
    bool gDestroyed[kMaxTrackedNodes];

    void resetTracking() {
        gDestroyCount = 0;
        for (int i = 0; i < kMaxTrackedNodes; ++i) gDestroyed[i] = false;
    }

    void trackingDeleter(RetireHead* h) {
        TestNode* n = nodeOf(h);
        gDestroyed[n->id] = true;
        ++gDestroyCount;
        delete n;
    }

    TestNode* makeNode(int id) {
        auto* n = new TestNode();
        n->id = id;
        return n;
    }

    // Heap-owned domain; see note 2 in the file header.
    template <typename D>
    struct Owned {
        D* p;
        template <typename... A>
        explicit Owned(A&&... a) : p(new D(static_cast<A&&>(a)...)) {}
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
        D& operator*() const { return *p; }

        // Success-path teardown: the sanctioned sequence is quiesce ->
        // drainAllQuiescent() -> destroy (RCU-DEC-034/035).
        void finish() { p->drainAllQuiescent(); delete p; p = nullptr; }
    };

    // The common "writer retires one object" shape. A writer must itself be
    // inside a section (RCU-DEC-019) — it traverses the shared structure to
    // perform the unlink, so an unpinned writer can have the parent reclaimed
    // under it. Note that is writer-traversal safety, NOT reclamation safety:
    // reclamation safety comes from kRetireFence.
    template <typename D>
    void retireInSection(D& d, size_t slot, TestNode* n,
                         void (*deleter)(RetireHead*) = &trackingDeleter) {
        d.readLock(slot);
        d.retire(slot, &n->head, deleter);
        d.readUnlock(slot);
    }

    // ── Instrumented hooks ───────────────────────────────────────────────────
    //
    // Plain function pointers rather than std::function: the harness tracks
    // allocations per test, and a capturing callable would show up as an
    // allocation by the code under test.
    struct HookControl {
        void (*afterEpochLoad)(uint64_t) = nullptr;
        size_t preTouchCount = 0;
        size_t sealCount     = 0;
        size_t claimCount    = 0;
    };

    HookControl gHooks;

    struct TestHooks {
        // WINDOW-INTERIOR points (RCU-DEC-024). Instrumented here because the
        // userspace harness has no masked window; a kernel Hooks must leave these
        // empty, or the audited no-instruction-can-fault claim breaks.
        void onAfterEpochLoad(uint64_t e) const noexcept {
            if (gHooks.afterEpochLoad) gHooks.afterEpochLoad(e);
        }
        void onAfterActivation(uint64_t) const noexcept {}
        void onBeforeDeactivation() const noexcept {}

        void onAfterScanEpochLoad(uint64_t) const noexcept {}
        void onBeforeEpochAdvance(uint64_t) const noexcept {}
        void onAfterRetireEpochLoad(uint64_t) const noexcept {}
        void onBeforeSeal(size_t, size_t) const noexcept { ++gHooks.sealCount; }
        void onAfterClaim(size_t, size_t) const noexcept { ++gHooks.claimCount; }
        void onPreTouch(RetireHead*) const noexcept { ++gHooks.preTouchCount; }
    };

    void resetHooks() {
        gHooks.afterEpochLoad = nullptr;
        gHooks.preTouchCount  = 0;
        gHooks.sealCount      = 0;
        gHooks.claimCount     = 0;
    }

    using PlainDomain = EpochDomain<NoopRcuHooks>;
    using HookDomain  = EpochDomain<TestHooks>;
    using PlainDI     = DebugIntrospection<NoopRcuHooks>;
    using HookDI      = DebugIntrospection<TestHooks>;

    // Drives the epoch forward by `n`. NOTE: this also sweeps every expired bag
    // in the domain (see note 1 in the file header) — that is not incidental, it
    // is what tryAdvance is specified to do.
    template <typename D>
    void advanceEpochs(D& d, size_t slot, uint64_t n) {
        const uint64_t target = d.currentEpoch() + n;
        while (d.currentEpoch() < target) d.tryAdvance(slot);
    }

}

// ============================================================
// Encoding (P1-I3, I9 / ITEM-013)
// ============================================================

TEST(rcuStateWordAndBagEncoding) {
    using namespace Core::rcu;

    // Inactive is the all-zero encoding, which is what makes a zero-initialized
    // slot array mean "no readers" (P1-I3).
    ASSERT_FALSE(isActive(kInactive));
    ASSERT_EQ(uint64_t{0}, epochOf(kInactive));

    for (uint64_t e : { uint64_t{0}, uint64_t{1}, uint64_t{2}, uint64_t{12345}, kMaxEpoch }) {
        const uint64_t w = makeActive(e);
        ASSERT_TRUE(isActive(w));
        ASSERT_EQ(e, epochOf(w));
    }

    // (tag : 62, state : 2), state in the low bits so a zeroed word decodes as
    // (tag 0, Free).
    ASSERT_EQ(uint64_t{0}, bagTagOf(uint64_t{0}));
    ASSERT_TRUE(bagStateOf(uint64_t{0}) == BagState::Free);

    for (uint64_t t : { uint64_t{0}, uint64_t{1}, uint64_t{999}, kMaxEpoch }) {
        for (auto st : { BagState::Free, BagState::Open, BagState::Sealed, BagState::Claimed }) {
            const uint64_t v = packBag(t, st);
            ASSERT_EQ(t, bagTagOf(v));
            ASSERT_TRUE(bagStateOf(v) == st);
        }
    }

    // P1-I1 as corrected: 2 is the structural floor (rotation needs a second
    // bag); 4 is tuning slack, not a correctness floor — retired invariant I7's
    // ">= 4" went away with RCU-DEC-027.
    static_assert(kBagCount >= 2);
    static_assert(kEffectiveEpochBits == 62);
}

TEST(rcuZeroInitialisedSlotArrayIsAValidEmptyDomain) {
    ReaderSlot slots[3]{};
    Owned<PlainDomain> owned(slots, size_t{3});
    PlainDomain& d = *owned;

    ASSERT_EQ(uint64_t{0}, d.currentEpoch());
    for (size_t i = 0; i < 3; ++i) {
        const auto s = PlainDI::slot(d, i);
        ASSERT_FALSE(s.active);
        ASSERT_EQ(uint64_t{0}, s.nesting);
        ASSERT_FALSE(s.inDrain);
        for (size_t b = 0; b < kBagCount; ++b) {
            const auto bag = PlainDI::bag(d, i, b);
            ASSERT_TRUE(bag.state == BagState::Free);
            ASSERT_EQ(size_t{0}, bag.nodeCount);
        }
    }
    // No readers anywhere, so the epoch moves freely.
    ASSERT_TRUE(d.tryAdvance(0));
    ASSERT_EQ(uint64_t{1}, d.currentEpoch());

    owned.finish();
}

// ============================================================
// Read side
// ============================================================

TEST(rcuReadLockNestingPublishesOnceAndRetiresOnce) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;

    d.readLock(0);
    auto s = PlainDI::slot(d, 0);
    ASSERT_TRUE(s.active);
    ASSERT_EQ(uint64_t{0}, s.epochSnapshot);
    ASSERT_EQ(uint64_t{1}, s.nesting);

    // A nested section — including one entered from an interrupt handler that
    // interrupted a section on the same CPU (R2) — must short-circuit on
    // nesting != 0 and leave the published snapshot untouched. Advance the epoch
    // first, so a (wrong) re-publish would show up as a fresh snapshot.
    d.tryAdvance(1);
    ASSERT_EQ(uint64_t{1}, d.currentEpoch());

    d.readLock(0);
    s = PlainDI::slot(d, 0);
    ASSERT_EQ(uint64_t{2}, s.nesting);
    ASSERT_EQ(uint64_t{0}, s.epochSnapshot);   // still the OUTERMOST entry's snapshot

    d.readUnlock(0);
    s = PlainDI::slot(d, 0);
    ASSERT_TRUE(s.active);                     // an inner exit must not deactivate
    ASSERT_EQ(uint64_t{1}, s.nesting);

    d.readUnlock(0);
    s = PlainDI::slot(d, 0);
    ASSERT_FALSE(s.active);
    ASSERT_EQ(uint64_t{0}, s.nesting);

    owned.finish();
}

TEST(rcuUnbalancedReadUnlockAsserts) {
    ReaderSlot slots[1]{};
    Owned<PlainDomain> owned(slots, size_t{1});
    EXPECT_ASSERT_FAILURE((*owned).readUnlock(0));
    owned.finish();
}

// ============================================================
// I1 / I3 — the advance gate
// ============================================================

TEST(rcuActiveSlotWithStaleSnapshotBlocksAdvance) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;

    // Slot 1 pins at epoch 0. The FIRST advance is not blocked: I3 is an EXACT
    // match, and a slot whose snapshot equals the current epoch has caught up.
    d.readLock(1);
    ASSERT_TRUE(d.tryAdvance(0));
    ASSERT_EQ(uint64_t{1}, d.currentEpoch());

    // Now slot 1's snapshot (0) differs from the epoch (1), so it blocks — I1.
    ASSERT_FALSE(d.tryAdvance(0));
    ASSERT_FALSE(d.tryAdvance(0));
    ASSERT_EQ(uint64_t{1}, d.currentEpoch());

    // Releasing it unblocks the domain immediately — only CPUs INSIDE sections
    // may block a grace period, and only until they exit (R3).
    d.readUnlock(1);
    ASSERT_TRUE(d.tryAdvance(0));
    ASSERT_EQ(uint64_t{2}, d.currentEpoch());

    owned.finish();
}

namespace {
    HookDomain* gStallDomain = nullptr;
    bool        gStallArmed  = false;

    // Fires between the epoch load and the activation store — exactly the window
    // I3 exists to license. Advancing the epoch here makes the reader publish a
    // snapshot that is already stale by the time it lands.
    void advanceOnceDuringReadLock(uint64_t) {
        if (!gStallArmed) return;
        gStallArmed = false;
        gStallDomain->tryAdvance(0);
    }
}

TEST(rcuStaleSnapshotDelaysAdvancementButNeverPermitsIt) {
    ReaderSlot slots[2]{};
    Owned<HookDomain> owned(slots, size_t{2});
    HookDomain& d = *owned;
    resetHooks();

    gStallDomain = &d;
    gStallArmed  = true;
    gHooks.afterEpochLoad = &advanceOnceDuringReadLock;

    // Slot 1 reads epoch 0, is stalled while the epoch moves to 1, and only then
    // publishes active(0).
    d.readLock(1);
    gHooks.afterEpochLoad = nullptr;

    ASSERT_EQ(uint64_t{1}, d.currentEpoch());
    const auto s = HookDI::slot(d, 1);
    ASSERT_TRUE(s.active);
    ASSERT_EQ(uint64_t{0}, s.epochSnapshot);   // stale, as arranged

    // The whole point of I3: the stale snapshot BLOCKS. It must never be treated
    // as "close enough" and allowed through — staleness may only delay
    // advancement, never permit it.
    ASSERT_FALSE(d.tryAdvance(0));
    ASSERT_FALSE(d.tryAdvance(1));
    ASSERT_EQ(uint64_t{1}, d.currentEpoch());

    d.readUnlock(1);
    ASSERT_TRUE(d.tryAdvance(0));

    gStallDomain = nullptr;
    owned.finish();
}

TEST(rcuRetireFromOwnSectionBlocksOwnAdvance) {
    // P1-DEC-004: the scan never skips the calling CPU's own slot. Skipping it is
    // a plausible-looking optimization and a genuine safety bug, because a writer
    // may legitimately retire from inside a read-side section.
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    d.readLock(0);
    d.retire(0, &makeNode(0)->head, &trackingDeleter);

    // Snapshot == epoch, so the first advance goes through.
    ASSERT_TRUE(d.tryAdvance(0));
    ASSERT_EQ(uint64_t{1}, d.currentEpoch());

    // Our OWN slot is now stale, and it must block our OWN advance.
    ASSERT_FALSE(d.tryAdvance(0));
    ASSERT_EQ(uint64_t{1}, d.currentEpoch());

    d.readUnlock(0);
    ASSERT_TRUE(d.tryAdvance(0));

    owned.finish();
    ASSERT_EQ(1, gDestroyCount);
}

// ============================================================
// I2 and the bag state machine
// ============================================================

TEST(rcuNoBagDrainedBeforeTagPlusTwo) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    // Three nodes into slot 0's bag at epoch 0.
    for (int i = 0; i < 3; ++i) retireInSection(d, 0, makeNode(i));

    auto bag0 = PlainDI::bag(d, 0, 0);
    ASSERT_TRUE(bag0.state == BagState::Open);
    ASSERT_EQ(uint64_t{0}, bag0.tag);
    ASSERT_EQ(size_t{3}, bag0.nodeCount);

    // Seal it by rotating: a retire at a later epoch opens a fresh bag and seals
    // the old one AT ITS OWN TAG (not at the current epoch), so it expires as
    // early as it legitimately can.
    advanceEpochs(d, 1, 1);
    retireInSection(d, 0, makeNode(3));

    bag0 = PlainDI::bag(d, 0, 0);
    ASSERT_TRUE(bag0.state == BagState::Sealed);
    ASSERT_EQ(uint64_t{0}, bag0.tag);
    ASSERT_EQ(size_t{3}, bag0.nodeCount);
    ASSERT_FALSE(bag0.expired);              // epoch 1 < tag 0 + 2

    const auto bag1 = PlainDI::bag(d, 0, 1);
    ASSERT_TRUE(bag1.state == BagState::Open);
    ASSERT_EQ(uint64_t{1}, bag1.tag);
    ASSERT_EQ(size_t{1}, bag1.nodeCount);

    // Sweeping at epoch 1 must not touch it — I2's reclaim gate. This is the
    // assertion that catches an "it's sealed, therefore it's safe" shortcut.
    ASSERT_EQ(size_t{0}, d.sweepExpired(1));
    ASSERT_EQ(0, gDestroyCount);
    ASSERT_TRUE(PlainDI::bag(d, 0, 0).state == BagState::Sealed);

    // At epoch 2 the gate opens, and tryAdvance's unconditional sweep takes it.
    advanceEpochs(d, 1, 1);
    ASSERT_EQ(3, gDestroyCount);
    ASSERT_TRUE(PlainDI::bag(d, 0, 0).state == BagState::Free);
    ASSERT_EQ(size_t{0}, PlainDI::bag(d, 0, 0).nodeCount);

    owned.finish();
    ASSERT_EQ(4, gDestroyCount);
}

TEST(rcuRotationKeepsExactlyOneOpenBagPerSlot) {
    ReaderSlot slots[1]{};
    Owned<PlainDomain> owned(slots, size_t{1});
    PlainDomain& d = *owned;
    resetTracking();

    int id = 0;
    for (size_t round = 0; round < kBagCount + 2; ++round) {
        retireInSection(d, 0, makeNode(id++));

        // At most one Open bag per slot, always — that is what makes I6's
        // owner-push / drainer-take exclusion structural rather than a matter of
        // timing or discipline.
        size_t openBags = 0;
        for (size_t b = 0; b < kBagCount; ++b) {
            if (PlainDI::bag(d, 0, b).state == BagState::Open) ++openBags;
        }
        ASSERT_EQ(size_t{1}, openBags);
        ASSERT_TRUE(PlainDI::bag(d, 0, PlainDI::slot(d, 0).openBagIndex).state == BagState::Open);

        d.tryAdvance(0);
    }

    owned.finish();
    ASSERT_EQ(id, gDestroyCount);
}

TEST(rcuOnPreTouchFiresOncePerRetireAndPerDrain) {
    // RCU-DEC-017. A drainer may be a different CPU than the retirer, walking
    // intrusive links in slab memory whose page may have been reclaimed and
    // re-backed — the vmsmalloc DEC-047 bug class — so the hook must fire per
    // node, before any ACCESS of that node's RetireHead fields.
    //
    // BOTH SIDES, and that is the point of this test's shape: `retire` WRITES
    // node->deleter and node->next, and those writes need freshness for exactly
    // the reason the drain's reads do. The retiring CPU is routinely not the
    // allocating one. This originally asserted drain-side firings only, and the
    // Phase 4 in-kernel stress found the write side missing — a stale mapping
    // swallowed the deleter store and the drainer then read a null deleter off
    // the real page (a panic in debug; a null call in release).
    ReaderSlot slots[2]{};
    Owned<HookDomain> owned(slots, size_t{2});
    HookDomain& d = *owned;
    resetTracking();
    resetHooks();

    for (int i = 0; i < 5; ++i) retireInSection(d, 0, makeNode(i));
    ASSERT_EQ(size_t{5}, gHooks.preTouchCount);   // retire side: one per node

    advanceEpochs(d, 1, 1);                  // epoch 1: bag tagged 0 is not yet expired
    retireInSection(d, 0, makeNode(5));      // rotate + seal at tag 0
    ASSERT_EQ(size_t{6}, gHooks.preTouchCount);   // ...including this one
    ASSERT_EQ(0, gDestroyCount);
    ASSERT_TRUE(gHooks.sealCount >= 1);

    advanceEpochs(d, 1, 1);                  // epoch 2: the sweep inside tryAdvance drains it
    ASSERT_EQ(5, gDestroyCount);
    ASSERT_EQ(size_t{11}, gHooks.preTouchCount);  // 6 retires + 5 drained
    ASSERT_TRUE(gHooks.claimCount >= 1);

    owned.finish();
    ASSERT_EQ(size_t{12}, gHooks.preTouchCount);  // the teardown drain touches the last one
}

// ============================================================
// drainBatchBound (RCU-DEC-033)
// ============================================================

TEST(rcuDrainBatchBoundResealsRemainderWithTagUnchanged) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    ASSERT_EQ(kUnboundedDrainBatch, d.getDrainBatchBound());   // the default

    // Set before anything can expire: tryAdvance sweeps unconditionally, so an
    // unbounded advance would empty the bag before the bound ever applied.
    d.setDrainBatchBound(1);

    for (int i = 0; i < 3; ++i) retireInSection(d, 0, makeNode(i));
    advanceEpochs(d, 1, 1);
    retireInSection(d, 0, makeNode(3));      // seals bag 0 at tag 0
    advanceEpochs(d, 1, 1);                  // epoch 2: expired, and swept with budget 1

    ASSERT_EQ(1, gDestroyCount);

    // Bound hit mid-bag: the remainder is stored back and the bag re-sealed with
    // the tag UNCHANGED, so it stays visible, still expired, and immediately
    // re-claimable by any CPU. A drainer-private remainder list was rejected
    // precisely because it would be invisible to barrier's accounting.
    auto bag0 = PlainDI::bag(d, 0, 0);
    ASSERT_TRUE(bag0.state == BagState::Sealed);
    ASSERT_EQ(uint64_t{0}, bag0.tag);
    ASSERT_EQ(size_t{2}, bag0.nodeCount);
    ASSERT_TRUE(bag0.expired);

    ASSERT_EQ(size_t{1}, d.sweepExpired(1));
    ASSERT_EQ(2, gDestroyCount);
    ASSERT_EQ(size_t{1}, PlainDI::bag(d, 0, 0).nodeCount);

    // Unbounded finishes the job in one call.
    d.setDrainBatchBound(kUnboundedDrainBatch);
    ASSERT_EQ(size_t{1}, d.sweepExpired(1));
    ASSERT_EQ(3, gDestroyCount);
    ASSERT_TRUE(PlainDI::bag(d, 0, 0).state == BagState::Free);

    owned.finish();
    ASSERT_EQ(4, gDestroyCount);
}

// ============================================================
// Completion primitives
// ============================================================

TEST(rcuSynchronizeAdvancesExactlyTwoEpochs) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;

    const uint64_t before = d.currentEpoch();
    d.synchronize(0);
    ASSERT_EQ(before + 2, d.currentEpoch());

    d.synchronize(1);
    ASSERT_EQ(before + 4, d.currentEpoch());

    owned.finish();
}

TEST(rcuSynchronizeDoesNotImplyDestruction) {
    // RCU-DEC-031, and the weaker guarantee is specified and intended, not an
    // implementation shortfall: two epoch advances can complete while a bag is
    // sealed-but-unswept, and a REMOTE Open bag can only ever be sealed by its
    // own owner (I13). A caller wanting destruction uses barrier.
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    retireInSection(d, 1, makeNode(0));      // lands in slot 1's OPEN bag

    d.synchronize(0);
    ASSERT_TRUE(d.currentEpoch() >= 2);
    ASSERT_FALSE(gDestroyed[0]);             // grace period elapsed; object alive
    ASSERT_EQ(0, gDestroyCount);

    owned.finish();
    ASSERT_TRUE(gDestroyed[0]);
}

TEST(rcuSynchronizeAndBarrierFromInsideASectionAssert) {
    ReaderSlot slots[1]{};
    Owned<PlainDomain> owned(slots, size_t{1});
    PlainDomain& d = *owned;

    d.readLock(0);
    EXPECT_ASSERT_FAILURE(d.synchronize(0));
    EXPECT_ASSERT_FAILURE(d.barrier(0));
    d.readUnlock(0);

    owned.finish();
}

TEST(rcuBarrierDestroysTheCallersOwnRetireesIncludingItsOpenBag) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    // Spread the caller's retirees across a sealed bag and its still-Open bag.
    for (int i = 0; i < 3; ++i) retireInSection(d, 0, makeNode(i));
    advanceEpochs(d, 1, 1);
    for (int i = 3; i < 5; ++i) retireInSection(d, 0, makeNode(i));   // rotate; 3,4 land Open

    ASSERT_EQ(0, gDestroyCount);
    d.barrier(0);

    // Complete for the caller's own slot: the seal-AND-rotate at entry is what
    // brings the still-Open bag inside the promise. The rotate is also what stops
    // a deleter-retire during barrier's own sweeps from pushing into a Sealed
    // bag — the I11-fatal case, and one of the two ways barrier is easy to get
    // wrong while still passing quiet unit tests.
    ASSERT_EQ(5, gDestroyCount);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(gDestroyed[i]);

    owned.finish();
}

TEST(rcuBarrierExcludesRemoteOpenBagsButDrainsRemoteSealedOnes) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    // Slot 1 fills a bag, rotates it out (sealing it), adds one more to its fresh
    // Open bag, and then goes quiet forever.
    for (int i = 0; i < 3; ++i) retireInSection(d, 1, makeNode(i));
    advanceEpochs(d, 0, 1);
    retireInSection(d, 1, makeNode(3));      // seals slot 1's bag 0; node 3 stays Open

    d.barrier(0);

    // Slot 1's SEALED bag is drained by slot 0 — stealing is exactly what makes
    // memory bounded with no scheduler and no IPI (RCU-DEC-006).
    ASSERT_TRUE(gDestroyed[0]);
    ASSERT_TRUE(gDestroyed[1]);
    ASSERT_TRUE(gDestroyed[2]);
    // Its OPEN bag is not, and that exclusion is by contract, not by omission:
    // Open -> Sealed is an owner-only plain store (I13), so no other CPU can reach
    // these. A barrier that waited on them would livelock against an idle owner.
    ASSERT_FALSE(gDestroyed[3]);
    ASSERT_EQ(3, gDestroyCount);
    ASSERT_EQ(size_t{1}, PlainDI::openBagResidue(d, 1));

    owned.finish();
    ASSERT_EQ(4, gDestroyCount);
}

TEST(rcuBarrierTerminatesUnderContinuedRemoteRetireTraffic) {
    // RCU-DEC-036's entry-snapshot bound. "Sweep until globally empty" chases a
    // moving target under sustained remote retire traffic and livelocks; bounding
    // by e0 makes the target set strictly shrink once the epoch passes e0.
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    int id = 0;
    for (int i = 0; i < 4; ++i) retireInSection(d, 0, makeNode(id++));

    d.barrier(0);
    ASSERT_EQ(4, gDestroyCount);

    // More traffic on both slots, then barrier again. Slot 0's retirees are ids
    // 4, 6, 8, 10.
    for (int i = 0; i < 4; ++i) {
        retireInSection(d, 0, makeNode(id++));
        retireInSection(d, 1, makeNode(id++));
        d.tryAdvance(1);
    }
    d.barrier(0);
    for (int i = 4; i < id; i += 2) ASSERT_TRUE(gDestroyed[i]);

    owned.finish();
    ASSERT_EQ(id, gDestroyCount);
}

// ============================================================
// Deleters that retire (RCU-DEC-030, RCU-DEC-038)
// ============================================================

namespace {
    PlainDomain* gChildDomain = nullptr;
    size_t       gChildSlot   = 0;
    int          gChildIdBase = 100;
    int          gChildrenPerParent = 0;

    // A destructor that retires child nodes reenters retire during a drain. Safe
    // because the drained bag is Claimed (exclusively owned, I6) and the
    // deleter's pushes go to its own slot's OPEN bag — two different bags by
    // construction — with recursion into the drain itself blocked by inDrain.
    //
    // Driven from a barrier/synchronize sweep this runs with nesting == 0 and
    // outside any section, which is exactly why RCU-DEC-038 had to widen retire's
    // assert. The deleter touches nothing but state reachable from the retired
    // object, per RCU-DEC-039 — no assert can check that clause.
    void parentDeleter(RetireHead* h) {
        TestNode* n = nodeOf(h);
        gDestroyed[n->id] = true;
        ++gDestroyCount;
        for (int i = 0; i < gChildrenPerParent; ++i) {
            auto* child = new TestNode();
            child->id = gChildIdBase++;
            gChildDomain->retire(gChildSlot, &child->head, &trackingDeleter);
        }
        delete n;
    }

    void retireParent(PlainDomain& d, size_t slot, int id) {
        auto* n = new TestNode();
        n->id = id;
        d.readLock(slot);
        d.retire(slot, &n->head, &parentDeleter);
        d.readUnlock(slot);
    }
}

TEST(rcuDeleterThatItselfRetires) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    gChildDomain       = &d;
    gChildSlot         = 0;
    gChildIdBase       = 100;
    gChildrenPerParent = 2;

    for (int i = 0; i < 3; ++i) retireParent(d, 0, i);

    d.barrier(0);

    // The three parents are destroyed, and each retired two children from inside
    // the drain. Those children are NOT covered by this barrier: RCU-DEC-031's
    // coverage boundary is objects retired BEFORE the call, and it says so
    // explicitly about deleter-retires. They sit in slot 0's fresh Open bag.
    ASSERT_EQ(3, gDestroyCount);
    ASSERT_EQ(size_t{6}, PlainDI::totalResidue(d));

    owned.finish();
    ASSERT_EQ(3 + 6, gDestroyCount);
    gChildDomain = nullptr;
}

namespace {
    PlainDomain* gAssertDomain = nullptr;

    // Both blocking primitives assert !inDrain at entry. Without that assert a
    // deleter-context call HANGS undiagnosed even in a debug build: every inner
    // tryAdvance hits the I14 early-out, so the epoch never moves.
    //
    // The expectation is checked INSIDE the deleter so the thrown AssertionFailure
    // never unwinds through the engine — an escaping throw would skip
    // sweepExpired's inDrain clear and strand the slot in a drain forever.
    void deleterThatCallsBlockingPrimitives(RetireHead* h) {
        TestNode* n = nodeOf(h);
        EXPECT_ASSERT_FAILURE(gAssertDomain->synchronize(0));
        EXPECT_ASSERT_FAILURE(gAssertDomain->barrier(0));
        gDestroyed[n->id] = true;
        ++gDestroyCount;
        delete n;
    }
}

TEST(rcuBlockingPrimitivesAssertFromDeleterContextInsteadOfHanging) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();
    gAssertDomain = &d;

    auto* n = new TestNode();
    n->id = 0;
    d.readLock(0);
    d.retire(0, &n->head, &deleterThatCallsBlockingPrimitives);
    d.readUnlock(0);

    d.barrier(0);
    ASSERT_EQ(1, gDestroyCount);
    ASSERT_TRUE(gDestroyed[0]);
    ASSERT_FALSE(PlainDI::slot(d, 0).inDrain);   // the flag was properly cleared

    gAssertDomain = nullptr;
    owned.finish();
}

// ============================================================
// Contract violations
// ============================================================

TEST(rcuRetireOutsideASectionAsserts) {
    // RCU-DEC-019 as amended by RCU-DEC-038: a WRITER unlinking from the live
    // structure requires a section unconditionally. The carve-outs are inDrain
    // and teardownActive, neither of which is set here.
    ReaderSlot slots[1]{};
    Owned<PlainDomain> owned(slots, size_t{1});
    resetTracking();

    auto* n = makeNode(0);
    EXPECT_ASSERT_FAILURE((*owned).retire(0, &n->head, &trackingDeleter));
    ASSERT_EQ(0, gDestroyCount);
    delete n;                                 // it never entered a bag

    owned.finish();
}

TEST(rcuOutOfRangeSlotAsserts) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;

    EXPECT_ASSERT_FAILURE(d.readLock(2));
    EXPECT_ASSERT_FAILURE(d.tryAdvance(9));
    EXPECT_ASSERT_FAILURE(d.sweepExpired(2));
    EXPECT_ASSERT_FAILURE(d.synchronize(2));
    EXPECT_ASSERT_FAILURE(d.barrier(2));

    owned.finish();
}

TEST(rcuQuiescenceCheckFiresOnANonQuiescentDomain) {
    // P1-DEC-007: driven through DebugIntrospection::assertQuiescent() and NOT
    // through ~EpochDomain, which calls the same internal check — a throwing
    // assert cannot escape a noexcept destructor.
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    // (a) objects still in limbo
    retireInSection(d, 0, makeNode(0));
    EXPECT_ASSERT_FAILURE(PlainDI::assertQuiescent(d));

    d.drainAllQuiescent();
    PlainDI::assertQuiescent(d);              // now clean — must NOT throw

    // (b) an active read-side section
    d.readLock(1);
    EXPECT_ASSERT_FAILURE(PlainDI::assertQuiescent(d));
    d.readUnlock(1);
    PlainDI::assertQuiescent(d);

    ASSERT_EQ(1, gDestroyCount);
    owned.finish();
}

// ============================================================
// drainAllQuiescent (RCU-DEC-035, RCU-DEC-037)
// ============================================================

TEST(rcuDrainAllQuiescentEmptiesEverythingIncludingRemoteOpenBags) {
    ReaderSlot slots[3]{};
    Owned<PlainDomain> owned(slots, size_t{3});
    PlainDomain& d = *owned;
    resetTracking();

    int id = 0;
    for (size_t s = 0; s < 3; ++s) {
        for (int i = 0; i < 3; ++i) retireInSection(d, s, makeNode(id++));
        advanceEpochs(d, s, 1);
        retireInSection(d, s, makeNode(id++));   // rotate: one sealed bag, one open
    }

    // Some of these have already been swept by the advances above — tryAdvance
    // sweeps unconditionally — so account for whatever is left rather than
    // assuming nothing drained.
    const int destroyedBefore = gDestroyCount;
    ASSERT_TRUE(PlainDI::totalResidue(d) > 0);

    // The universal-owner drain: force-seals every Open bag. It is the only entry
    // point allowed to perform another slot's owner-side transitions, legal
    // precisely because the caller guarantees no CPU will touch the domain again
    // — and the only one whose caller needs no bound slot at all, which is how a
    // harness main thread tears a domain down.
    const size_t ran = d.drainAllQuiescent();
    ASSERT_EQ(static_cast<size_t>(id - destroyedBefore), ran);
    ASSERT_EQ(id, gDestroyCount);
    ASSERT_EQ(size_t{0}, PlainDI::totalResidue(d));
    ASSERT_FALSE(PlainDI::teardownActive(d));
    PlainDI::assertQuiescent(d);

    owned.finish();
}

TEST(rcuDrainAllQuiescentWithAnActiveSectionAsserts) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;

    d.readLock(1);
    EXPECT_ASSERT_FAILURE(d.drainAllQuiescent());
    d.readUnlock(1);

    owned.finish();
}

TEST(rcuDrainAllQuiescentCollectsDeleterRetiresTargetingAnySlot) {
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    gChildDomain       = &d;
    gChildSlot         = 1;      // deliberately a DIFFERENT slot than the retirer
    gChildIdBase       = 100;
    gChildrenPerParent = 2;

    for (int i = 0; i < 2; ++i) retireParent(d, 0, i);

    // Deleter-retires during the teardown drain may target ANY slot — the caller
    // is universal owner and there is no concurrency left to discipline — and are
    // collected by the continuing loop. They are legal under teardownActive even
    // though the caller holds no section and no slot.
    const size_t ran = d.drainAllQuiescent();
    ASSERT_EQ(size_t{2 + 4}, ran);
    ASSERT_EQ(2 + 4, gDestroyCount);
    ASSERT_EQ(size_t{0}, PlainDI::totalResidue(d));

    gChildDomain = nullptr;
    owned.finish();
}

// ============================================================
// Quiet-system residue via stealing (RCU-DEC-006, I13, ITEM-014)
// ============================================================

TEST(rcuQuietSlotSealedBagsDrainElsewhereAndOpenBagIsTheResidue) {
    // The choreography matters. "Assert all of A's bags drain" FAILS on a correct
    // build: open-bag contents are recoverable only by their owner (I13), so a
    // slot that retires and then goes permanently quiet leaves its Open bag
    // behind by design. What stealing buys is that its SEALED bags do not
    // strand — the difference between a fixed high-water mark and a single idle
    // CPU holding memory hostage while the rest of the system runs.
    ReaderSlot slots[2]{};
    Owned<PlainDomain> owned(slots, size_t{2});
    PlainDomain& d = *owned;
    resetTracking();

    constexpr size_t kA = 0, kB = 1;

    // A retires three objects at epoch 0.
    for (int i = 0; i < 3; ++i) retireInSection(d, kA, makeNode(i));

    // B drives one advance; A then retires once more, which rotates — sealing A's
    // first bag at tag 0 and opening a fresh one at tag 1.
    ASSERT_TRUE(d.tryAdvance(kB));
    retireInSection(d, kA, makeNode(3));

    ASSERT_TRUE(PlainDI::bag(d, kA, 0).state == BagState::Sealed);
    ASSERT_EQ(size_t{3}, PlainDI::bag(d, kA, 0).nodeCount);

    // A now goes permanently quiet. ONLY B ever calls into the domain again — no
    // tick, no scheduler, no IPI, nothing to nudge A.
    for (int i = 0; i < 8; ++i) d.tryAdvance(kB);

    ASSERT_EQ(3, gDestroyCount);                       // A's sealed bag drained on B's calls
    ASSERT_TRUE(PlainDI::bag(d, kA, 0).state == BagState::Free);
    ASSERT_FALSE(gDestroyed[3]);
    ASSERT_EQ(size_t{1}, PlainDI::openBagResidue(d, kA));
    ASSERT_EQ(size_t{1}, PlainDI::totalResidue(d));    // the exact residue floor

    owned.finish();
    ASSERT_EQ(4, gDestroyCount);
}
