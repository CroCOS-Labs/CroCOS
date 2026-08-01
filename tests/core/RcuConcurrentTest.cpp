//
// Concurrent tests for Core::rcu::EpochDomain (RCU Phase 1).
// Created by Spencer Martin on 8/1/26.
//
// The engine has no kernel dependencies, so these are plain Core tests: one
// std::thread per slot index, no mocks, no bindThreadToCpu, no shadowed headers.
// ONE THREAD PER SLOT is not a convenience — `nesting`, `openBagIndex`,
// `retireCount` and `inDrain` are deliberately non-atomic owner-only state (I5),
// so driving a single slot from two threads is a genuine data race that TSan
// would (correctly) report.
//
// Coverage:
//
//   - rcuConcurrentReadersAndWriters — mini-rcutorture. Readers dereference a
//     protected pointer inside a section while writers swap and retire; the
//     deleter really calls `delete`, so a use-after-grace-period is a hard ASan
//     trap rather than a silent pass. Phase 3 builds the real torture suite on
//     this shape.
//
//   - rcuConcurrentOwnerPushWhileThievesDrain — HAZARD-1 / I6, the single most
//     dangerous spot in the design: if the owner-push / drainer-take exclusion is
//     wrong, objects are freed a grace period early, and the window is narrow
//     enough that it would not reproduce under casual testing. A stall injected
//     at onAfterClaim — after the claim CAS is won but before the winner touches
//     head — widens it deliberately.
//
//   - rcuConcurrentResealHandoff — the kBagReseal / kBagClaim pairing. With
//     drainBatchBound = 1, every node is a separate claim / drain / re-seal cycle
//     handed between threads. A plain (non-RELEASE) re-seal store lets the next
//     claimer read a stale remainder head and re-run freed deleters; the tagState
//     value is bit-identical before and after the drain, so coherence rescues
//     nothing. That is a weak-memory bug: expect it to pass on x86-TSO and fail
//     on ARMv8, which is exactly why ARMv8 TSan is the release gate.
//
// These use TEST_WITH_TIMEOUT_NO_TRACKING so the leak tracker does not attribute
// std::thread machinery to the test; the harness already scales per-test timeouts
// x10 under TSan. Domains are heap-allocated for the same reason as in
// RcuEpochDomainTest.cpp: ~EpochDomain asserts quiescence, and under this harness
// that assert throws out of a noexcept destructor.
//

#include "../test.h"
#include <harness/TestHarness.h>

#define CROCOS_RCU_TEST_HARNESS 1
#include <core/rcu/EpochDomain.h>
#include <core/rcu/DebugIntrospection.h>

#include <stddef.h>
#include <atomic>
#include <thread>
#include <vector>

using Core::rcu::EpochDomain;
using Core::rcu::ReaderSlot;
using Core::rcu::RetireHead;
using Core::rcu::NoopRcuHooks;
using Core::rcu::DebugIntrospection;
using Core::rcu::kUnboundedDrainBatch;

namespace {

    constexpr uint64_t kLiveMagic = 0x5243554C4956455Full;   // "RCULIVE"

    struct CNode {
        RetireHead head;
        uint64_t   magic = kLiveMagic;
        int        id    = -1;
    };

    CNode* cnodeOf(RetireHead* h) {
        return reinterpret_cast<CNode*>(reinterpret_cast<char*>(h) - offsetof(CNode, head));
    }

    // Exactly-once accounting. A second destruction of the same id is the
    // signature of a broken bag interlock; ASan would also trap on the double
    // delete, but the counter names the failure instead of just aborting.
    std::atomic<int>* gSeen = nullptr;
    std::atomic<int>  gDoubleFrees{0};
    std::atomic<int>  gDestroyed{0};
    std::atomic<int>  gReadErrors{0};

    void resetAccounting(int capacity) {
        delete[] gSeen;
        gSeen = new std::atomic<int>[static_cast<size_t>(capacity)];
        for (int i = 0; i < capacity; ++i) gSeen[i].store(0, std::memory_order_relaxed);
        gDoubleFrees.store(0);
        gDestroyed.store(0);
        gReadErrors.store(0);
    }

    void teardownAccounting() {
        delete[] gSeen;
        gSeen = nullptr;
    }

    void countingDeleter(RetireHead* h) {
        CNode* n = cnodeOf(h);
        if (gSeen[n->id].fetch_add(1, std::memory_order_relaxed) != 0) {
            gDoubleFrees.fetch_add(1, std::memory_order_relaxed);
        }
        gDestroyed.fetch_add(1, std::memory_order_relaxed);
        n->magic = 0;
        delete n;
    }

    CNode* makeCNode(int id) {
        auto* n = new CNode();
        n->id = id;
        n->head.deleter = &countingDeleter;
        return n;
    }

    template <typename D>
    struct Owned {
        D* p;
        template <typename... A>
        explicit Owned(A&&... a) : p(new D(static_cast<A&&>(a)...)) {}
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
        D& operator*() const { return *p; }
        void finish() { p->drainAllQuiescent(); delete p; p = nullptr; }
    };

    // ── Hooks that widen the HAZARD-1 window ─────────────────────────────────
    //
    // A no-op or spinning hook perturbs no atomic ordering: delay is exactly what
    // I3 licenses and what these tests induce.
    std::atomic<bool> gStallOnClaim{false};

    struct StallOnClaimHooks {
        void onAfterEpochLoad(uint64_t) const noexcept {}
        void onAfterActivation(uint64_t) const noexcept {}
        void onBeforeDeactivation() const noexcept {}
        void onAfterScanEpochLoad(uint64_t) const noexcept {}
        void onBeforeEpochAdvance(uint64_t) const noexcept {}
        void onAfterRetireEpochLoad(uint64_t) const noexcept {}
        void onBeforeSeal(size_t, size_t) const noexcept {}

        // The exact race window: the claim CAS has been won, but the winner has
        // not yet touched head — so the owner is free to be pushing into its Open
        // bag right now. If those two bags could ever be the same, this is where
        // it shows.
        void onAfterClaim(size_t, size_t) const noexcept {
            if (gStallOnClaim.load(std::memory_order_relaxed)) std::this_thread::yield();
        }

        void onPreTouch(RetireHead*) const noexcept {}
    };

}

// ============================================================
// Mini-rcutorture: readers dereference while writers retire
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(rcuConcurrentReadersAndWriters, 30000) {
    constexpr size_t kSlots      = 4;
    constexpr size_t kWriters    = 2;      // slots 0..1; slots 2..3 read
    constexpr int    kWritesEach = 3000;
    constexpr int    kReadRounds = 40000;

    // ids: 0 is the seed, then one per write.
    resetAccounting(static_cast<int>(kWriters) * kWritesEach + 4);

    ReaderSlot slots[kSlots]{};
    Owned<EpochDomain<NoopRcuHooks>> owned(slots, kSlots);
    auto& d = *owned;

    // The protected structure: one link, swapped repeatedly. A pointer loaded
    // through it inside a section must stay dereferenceable for the whole section
    // (R5) with no revalidation and no version counters — that property is the
    // one this test exists to break if it is not real.
    std::atomic<int>    nextId{1};
    std::atomic<CNode*> link{makeCNode(0)};
    std::atomic<bool>   writersDone{false};
    std::atomic<int>    writersLeft{static_cast<int>(kWriters)};

    std::vector<std::thread> threads;
    threads.reserve(kSlots);

    for (size_t w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w]() {
            for (int i = 0; i < kWritesEach; ++i) {
                CNode* fresh = makeCNode(nextId.fetch_add(1, std::memory_order_relaxed));
                // A writer must itself be inside a section (RCU-DEC-019): it
                // traverses the shared structure to perform the unlink, so an
                // unpinned writer can have the node reclaimed under it.
                d.readLock(w);
                CNode* old = link.exchange(fresh, std::memory_order_acq_rel);
                d.retire(w, &old->head);
                d.readUnlock(w);

                if ((i & 15) == 0) d.tryAdvance(w);
            }
            if (writersLeft.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                writersDone.store(true, std::memory_order_release);
            }
        });
    }

    for (size_t r = kWriters; r < kSlots; ++r) {
        threads.emplace_back([&, r]() {
            for (int i = 0; i < kReadRounds; ++i) {
                d.readLock(r);
                CNode* c = link.load(std::memory_order_acquire);
                // If RCU's no-ABA-within-a-section guarantee is false, this read
                // is a use-after-free and ASan traps right here.
                if (c->magic != kLiveMagic) gReadErrors.fetch_add(1, std::memory_order_relaxed);
                d.readUnlock(r);

                if ((i & 63) == 0) d.tryAdvance(r);
                if (writersDone.load(std::memory_order_acquire)) break;
            }
        });
    }

    for (auto& t : threads) t.join();

    // Post-join. A thread join is a happens-before edge from every CPU's last
    // operation on the domain to this point, which is exactly what
    // drainAllQuiescent's precondition requires (RCU-DEC-035 as corrected — a
    // RELAXED done-flag would NOT qualify).
    const int retired = nextId.load() - 1;
    d.drainAllQuiescent();

    ASSERT_EQ(0, gReadErrors.load());
    ASSERT_EQ(0, gDoubleFrees.load());
    ASSERT_EQ(retired, gDestroyed.load());
    ASSERT_EQ(size_t{0}, DebugIntrospection<NoopRcuHooks>::totalResidue(d));

    // Whatever is left in the link was never retired; the domain never owned it.
    delete link.load();
    owned.finish();
    teardownAccounting();
}

// ============================================================
// HAZARD-1 / I6 — owner pushes while thieves drain
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(rcuConcurrentOwnerPushWhileThievesDrain, 30000) {
    constexpr size_t kSlots       = 4;
    constexpr size_t kOwner       = 0;
    constexpr int    kRetireCount = 8000;

    resetAccounting(kRetireCount);

    ReaderSlot slots[kSlots]{};
    Owned<EpochDomain<StallOnClaimHooks>> owned(slots, kSlots);
    auto& d = *owned;
    gStallOnClaim.store(true);

    std::atomic<bool> ownerDone{false};
    std::vector<std::thread> threads;
    threads.reserve(kSlots);

    // The owner: pushes into its Open bag as fast as it can, rotating and sealing
    // as the epoch moves under it.
    threads.emplace_back([&]() {
        for (int i = 0; i < kRetireCount; ++i) {
            d.readLock(kOwner);
            d.retire(kOwner, &makeCNode(i)->head);
            d.readUnlock(kOwner);
            if ((i & 7) == 0) d.tryAdvance(kOwner);
        }
        ownerDone.store(true, std::memory_order_release);
    });

    // Thieves: nothing but advance-and-sweep. Every sealed, expired bag they find
    // belongs to another CPU (RCU-DEC-006), and they stall between winning the
    // claim CAS and touching head.
    for (size_t t = 1; t < kSlots; ++t) {
        threads.emplace_back([&, t]() {
            while (!ownerDone.load(std::memory_order_acquire)) {
                d.tryAdvance(t);
                d.sweepExpired(t);
            }
            for (int i = 0; i < 64; ++i) { d.tryAdvance(t); d.sweepExpired(t); }
        });
    }

    for (auto& t : threads) t.join();
    gStallOnClaim.store(false);
    d.drainAllQuiescent();

    ASSERT_EQ(0, gDoubleFrees.load());
    ASSERT_EQ(kRetireCount, gDestroyed.load());
    ASSERT_EQ(size_t{0}, DebugIntrospection<StallOnClaimHooks>::totalResidue(d));

    owned.finish();
    teardownAccounting();
}

// ============================================================
// kBagReseal / kBagClaim hand-off (RCU-DEC-033)
// ============================================================

TEST_WITH_TIMEOUT_NO_TRACKING(rcuConcurrentResealHandoff, 30000) {
    constexpr size_t kSlots    = 4;
    constexpr int    kRounds   = 12;
    constexpr int    kPerRound = 60;

    resetAccounting(kRounds * (kPerRound + 1) + 4);

    ReaderSlot slots[kSlots]{};
    Owned<EpochDomain<NoopRcuHooks>> owned(slots, kSlots);
    auto& d = *owned;

    // One node per claim / drain / re-seal cycle: every single drain hits the
    // bound mid-bag and hands the remainder to whoever claims next.
    d.setDrainBatchBound(1);

    int id = 0;
    for (int round = 0; round < kRounds; ++round) {
        for (int i = 0; i < kPerRound; ++i) {
            d.readLock(0);
            d.retire(0, &makeCNode(id++)->head);
            d.readUnlock(0);
        }
        // Rotate the filled bag out (sealing it at its own tag), then age it past
        // the I2 gate so the thieves below have something to fight over.
        d.tryAdvance(1);
        d.readLock(0);
        d.retire(0, &makeCNode(id++)->head);
        d.readUnlock(0);
        d.tryAdvance(1);
        d.tryAdvance(1);

        // Three CPUs race to drain, one node at a time.
        std::vector<std::thread> threads;
        threads.reserve(kSlots - 1);
        for (size_t t = 1; t < kSlots; ++t) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < kPerRound * 4; ++i) d.sweepExpired(t);
            });
        }
        for (auto& t : threads) t.join();

        ASSERT_EQ(0, gDoubleFrees.load());
    }

    d.setDrainBatchBound(kUnboundedDrainBatch);
    d.drainAllQuiescent();

    ASSERT_EQ(0, gDoubleFrees.load());
    ASSERT_EQ(id, gDestroyed.load());
    ASSERT_EQ(size_t{0}, DebugIntrospection<NoopRcuHooks>::totalResidue(d));

    owned.finish();
    teardownAccounting();
}
