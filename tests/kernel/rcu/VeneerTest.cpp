//
// RCU Phase 2 — single-threaded veneer scenarios.
//
// Exercises kernel/rcu/RCU.cpp against the shared vmsmalloc mocks, so
// retireDestroy round-trips through the REAL allocator rather than a stand-in.
// Negative (assertion) coverage is in AssertionsTest.cpp; the multi-CPU / TSan
// gate is in ConcurrentTest.cpp.
//
// Coverage map — see specs/rcu-phase-2.md "Verification Targets":
//
//   - P2-I1  slot index tracks kernel::getLogicalProcessorID()
//   - P2-I2  slotCount == arch::processorCount() at init()
//   - RCU-DEC-025  the interrupt mask covers the outermost TRANSITION only,
//                  never the section body
//   - protect returns the published pointer, wrapped for freshness
//   - the member-pointer thunk recovers T* when Head is NOT at offset 0
//   - a deleter runs exactly once per retired object
//   - retireDestroy round-trips a make<T> object back to its allocator slot
//   - synchronize advances the epoch and does NOT imply destruction (the
//     caller's own Open bag is the ITEM-014 residue); barrier does destroy it
//   - drain / tryAdvance pass through to the right engine entry points
//
// ONE HARNESS INTERACTION WORTH KNOWING: tryAdvance always sweeps, so any
// helper that drives the epoch also drains every expired bag in the domain. A
// test cannot advance the epoch "quietly". Several tests below are ordered
// around that; the same footgun bit Phase 1 three times.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include <stddef.h>
#include <stdint.h>

#include <arch.h>
#include <core/atomic.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock
#include <rcu/RCU.h>           // real
#include "MockCpuLocal.h"      // vmsmalloc mocks
#include "MockInterruptContext.h"
#include "MockRcuEnv.h"
#include "DebugIntrospection.h"

using namespace CroCOSTest;
namespace VS   = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;
namespace rcu  = kernel::rcu;

namespace {

numa::DomainID mapAllToZero(arch::ProcessorID) { return numa::DomainID{0}; }

// RAII harness: mmap region + topology + CPU count + thread binding, torn down
// (munmap) even when an assertion throws.
struct Harness {
    explicit Harness(size_t cpus = 1, size_t domains = 1) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, &mapAllToZero);
        arch::test::setProcessorCount(cpus);
        kernel::test::bindThreadToCpu(0);
        kernel::timing::test::resetMonoTime();
        kernel::interrupts::test::resetInterruptDepths();
    }
    ~Harness() { VS::test::shutdown(); }
};

// The retirable type. `head` is deliberately NOT the first member: the whole
// point of the member-pointer NTTP is that it recovers T* from an interior
// RetireHead, and a type with the head at offset 0 would pass even if the
// container_of arithmetic were dropped entirely.
struct Node {
    uint64_t              magic;
    Core::rcu::RetireHead head;
    uint32_t              payload;
};
static_assert(offsetof(Node, head) != 0,
              "the recovery thunk must be exercised at a non-zero offset");

constexpr uint64_t kMagic = 0x5AFEC0DE5AFEC0DEull;

Atomic<size_t>   gDeleted{0};
Atomic<uint64_t> gLastMagic{0};
Atomic<uint32_t> gLastPayload{0};

// Records, never frees — these nodes are array-resident, so the deleter's job
// here is only to prove it ran and that it was handed the right object.
void countingDeleter(Node* n) {
    gLastMagic.store(n->magic, SEQ_CST);
    gLastPayload.store(n->payload, SEQ_CST);
    gDeleted.fetch_add(1, SEQ_CST);
}

void resetCounters() {
    gDeleted.store(0, SEQ_CST);
    gLastMagic.store(0, SEQ_CST);
    gLastPayload.store(0, SEQ_CST);
}

Node makeNode(uint32_t payload) {
    Node n{};
    n.magic   = kMagic;
    n.payload = payload;
    return n;
}

// A Domain ready to use. Note Domain has no destructor at all (the engine lives
// in opaque storage and is never destroyed), which is correct for a type the
// kernel only ever declares at namespace scope — and convenient here, because a
// scenario that ends non-quiescent cannot take the runner down through a
// throwing destructor the way Phase 1's EpochDomain could.

// RCU-DEC-034's quiescence check demands every bag be FREE, not merely empty,
// and a bag that has ever been opened stays Open until a drain releases it. So
// asserting quiescence straight after a barrier fails on a CORRECT build:
// barrier seals AND rotates (RCU-DEC-036), and the freshly rotated bag is
// Open-and-empty. The sanctioned teardown is drainAllQuiescent THEN destroy
// (RCU-DEC-035), and this helper is exactly that sequence.
void quiesce(rcu::Domain& d) {
    rcu::test::drainAllQuiescent(d);
    rcu::test::assertQuiescent(d);
}

struct Fixture {
    rcu::Domain d;
    explicit Fixture(const char* name = "test",
                     size_t bound = Core::rcu::kUnboundedDrainBatch) {
        resetCounters();
        ASSERT_TRUE(d.init(name, bound));
    }
};

}   // namespace

// ─── init ──────────────────────────────────────────────────────────────────

// P2-I2: the slot array is sized off processorCount(), not off
// arch::MAX_PROCESSOR_COUNT (which is 256 while the real AP ceiling is 16).
TEST(rcu_init_sizes_slots_from_processor_count) {
    Harness h(4);
    Fixture f;
    ASSERT_EQ(4u, rcu::test::slotCount(f.d));
    ASSERT_EQ(0u, rcu::test::epoch(f.d));
    ASSERT_TRUE(f.d.initialized());
    rcu::test::assertQuiescent(f.d);
}

// drainBatchBound is RCU-DEC-033's knob, and it exists on init() precisely so a
// kernel path can set it — a default-only parameter would be unreachable.
TEST(rcu_init_plumbs_drain_batch_bound) {
    Harness h(2);
    Fixture unbounded("unbounded");
    ASSERT_EQ(Core::rcu::kUnboundedDrainBatch, rcu::test::drainBatchBound(unbounded.d));

    rcu::Domain bounded;
    ASSERT_TRUE(bounded.init("bounded", 3));
    ASSERT_EQ(3u, rcu::test::drainBatchBound(bounded));
}

// ─── ReadGuard ─────────────────────────────────────────────────────────────

// P2-I1. The slot a section lands in is the calling CPU's logical ID and
// nothing else; there is no other mapping.
TEST(rcu_readguard_binds_to_logical_processor_id) {
    Harness h(4);
    Fixture f;
    kernel::test::bindThreadToCpu(3);

    {
        rcu::ReadGuard g(f.d);
        ASSERT_TRUE(rcu::test::inSection(f.d, 3));
        for (size_t i = 0; i < 3; i++) {
            ASSERT_FALSE(rcu::test::inSection(f.d, i));
        }
    }
    ASSERT_FALSE(rcu::test::inSection(f.d, 3));
    rcu::test::assertQuiescent(f.d);
}

TEST(rcu_readguard_nests) {
    Harness h(2);
    Fixture f;
    {
        rcu::ReadGuard outer(f.d);
        ASSERT_EQ(1u, rcu::test::nesting(f.d, 0));
        {
            rcu::ReadGuard inner(f.d);
            ASSERT_EQ(2u, rcu::test::nesting(f.d, 0));
        }
        ASSERT_EQ(1u, rcu::test::nesting(f.d, 0));
    }
    ASSERT_EQ(0u, rcu::test::nesting(f.d, 0));
}

// RCU-DEC-024/025: masking covers the outermost transition ONLY. This is the
// half of the decision that would fail silently — a mask accidentally held
// across the section body still produces correct results, just with interrupts
// off for an unbounded traversal.
TEST(rcu_readguard_does_not_hold_the_mask_across_the_section_body) {
    Harness h(2);
    Fixture f;
    ASSERT_EQ(0u, arch::test::interruptMaskDepth());
    {
        rcu::ReadGuard g(f.d);
        ASSERT_EQ(0u, arch::test::interruptMaskDepth());
        {
            rcu::ReadGuard nested(f.d);
            ASSERT_EQ(0u, arch::test::interruptMaskDepth());
        }
    }
    ASSERT_EQ(0u, arch::test::interruptMaskDepth());
}

// ─── protect ───────────────────────────────────────────────────────────────

TEST(rcu_protect_returns_the_published_pointer) {
    Harness h(2);
    Fixture f;
    Node n = makeNode(7);
    Atomic<Node*> link{&n};

    rcu::ReadGuard g(f.d);
    VS::SafePtr<Node> p = rcu::protect(f.d, link);
    ASSERT_TRUE(static_cast<bool>(p));
    ASSERT_EQ(static_cast<void*>(&n), p.raw());
    ASSERT_EQ(kMagic, p->magic);
    ASSERT_EQ(7u, p->payload);
}

TEST(rcu_protect_carries_a_null_link_through) {
    Harness h(2);
    Fixture f;
    Atomic<Node*> link{nullptr};

    rcu::ReadGuard g(f.d);
    VS::SafePtr<Node> p = rcu::protect(f.d, link);
    ASSERT_FALSE(static_cast<bool>(p));
}

// ─── retire ────────────────────────────────────────────────────────────────

// The member-pointer thunk must recover the whole object from an interior
// RetireHead — see Node's static_assert. A wrong offset would hand the deleter
// a pointer 8 bytes into the object and `magic` would read as garbage.
TEST(rcu_retire_thunk_recovers_the_object_from_an_interior_head) {
    Harness h(2);
    Fixture f;
    Node n = makeNode(42);

    { rcu::ReadGuard g(f.d); rcu::retire<Node, &Node::head, countingDeleter>(f.d, &n); }
    rcu::barrier(f.d);

    ASSERT_EQ(1u, gDeleted.load(SEQ_CST));
    ASSERT_EQ(kMagic, gLastMagic.load(SEQ_CST));
    ASSERT_EQ(42u, gLastPayload.load(SEQ_CST));
}

TEST(rcu_deleter_runs_exactly_once_per_retired_object) {
    Harness h(2);
    Fixture f;
    constexpr size_t kCount = 16;
    Node nodes[kCount];
    for (size_t i = 0; i < kCount; i++) nodes[i] = makeNode(static_cast<uint32_t>(i));

    {
        rcu::ReadGuard g(f.d);
        for (auto& n : nodes) rcu::retire<Node, &Node::head, countingDeleter>(f.d, &n);
    }
    rcu::barrier(f.d);
    ASSERT_EQ(kCount, gDeleted.load(SEQ_CST));

    // Driving the machinery further must not re-run any deleter.
    rcu::barrier(f.d);
    rcu::synchronize(f.d);
    (void)rcu::tryAdvance(f.d);
    (void)rcu::drain(f.d);
    ASSERT_EQ(kCount, gDeleted.load(SEQ_CST));
    quiesce(f.d);
}

// RCU-DEC-031: synchronize promises a grace period and NOTHING about
// destruction. The retiree sits in the caller's own Open bag, and Open -> Sealed
// is an owner-only store made during the owner's own later retires (I13) — so
// this is the ITEM-014 residue, not a missed sweep. barrier is the primitive
// that seals first and therefore does destroy it.
TEST(rcu_synchronize_advances_the_epoch_without_destroying) {
    Harness h(2);
    Fixture f;
    Node n = makeNode(1);

    { rcu::ReadGuard g(f.d); rcu::retire<Node, &Node::head, countingDeleter>(f.d, &n); }

    const uint64_t before = rcu::test::epoch(f.d);
    rcu::synchronize(f.d);
    ASSERT_GE(rcu::test::epoch(f.d), before + 2);

    ASSERT_EQ(0u, gDeleted.load(SEQ_CST));
    ASSERT_EQ(1u, rcu::test::openBagResidue(f.d, 0));

    rcu::barrier(f.d);
    ASSERT_EQ(1u, gDeleted.load(SEQ_CST));
    ASSERT_EQ(0u, rcu::test::totalResidue(f.d));
}

// An active section blocks the advance — the read side really is doing its job
// through the veneer, not just through the engine's own unit tests.
TEST(rcu_active_section_blocks_the_epoch_advance) {
    Harness h(2);
    Fixture f;
    const uint64_t before = rcu::test::epoch(f.d);
    {
        rcu::ReadGuard g(f.d);
        // I3 is an EXACT-match rule, and the distinction matters: a reader
        // pinned at the CURRENT epoch is not stale, so it does not block the
        // first advance...
        ASSERT_TRUE(rcu::tryAdvance(f.d));
        ASSERT_EQ(before + 1, rcu::test::epoch(f.d));
        // ...but it is stale now, and it blocks the second. That pair is
        // precisely what a grace period is, so this is the read side genuinely
        // holding reclamation off through the veneer.
        ASSERT_FALSE(rcu::tryAdvance(f.d));
        ASSERT_EQ(before + 1, rcu::test::epoch(f.d));
    }
    ASSERT_TRUE(rcu::tryAdvance(f.d));
    ASSERT_EQ(before + 2, rcu::test::epoch(f.d));
}

// ─── retireDestroy ─────────────────────────────────────────────────────────

// P2-DEC-007. Grace-period expiry is exactly the point at which vmsfree's
// validation chain may safely run, so the round trip has to be through the REAL
// allocator: run the destructor, then genuinely release the storage.
//
// Storage release is checked by page count rather than by "the next allocation
// returns the same address". The latter holds only for a slab that was Full when
// the slot was freed (vmsmalloc's own becameAvailable test fills one first); from
// a Partial slab the reuse order is bookkeeper policy, and asserting it here
// would be testing the allocator's internals through RCU. Sizing the type past
// the largest slab class puts it on the whole-page bypass, where release is
// observable as a page returning to the pool and nothing about slot ordering
// matters.
namespace {
    Atomic<size_t> gDtorRuns{0};

    struct BigOwned {
        uint64_t              magic;
        Core::rcu::RetireHead head;
        unsigned char         pad[1024 - sizeof(uint64_t) - sizeof(Core::rcu::RetireHead)];
        ~BigOwned() { gDtorRuns.fetch_add(1, SEQ_CST); }
    };
    static_assert(sizeof(BigOwned) > 512,
                  "BigOwned must exceed the largest slab class so it takes the "
                  "whole-page bypass, which is what makes release observable");
}

TEST(rcu_retireDestroy_runs_the_destructor_then_releases_the_storage) {
    Harness h(2);
    Fixture f;
    gDtorRuns.store(0, SEQ_CST);

    const size_t pagesBefore = VS::test::activePageCount();

    VS::SafePtr<BigOwned> p = VS::make<BigOwned>();
    p->magic = kMagic;
    p->head  = Core::rcu::RetireHead{};
    ASSERT_EQ(pagesBefore + 1, VS::test::activePageCount());

    { rcu::ReadGuard g(f.d); rcu::retireDestroy<BigOwned, &BigOwned::head>(f.d, p); }

    // Deferred, not immediate — that is the entire point of retiring.
    ASSERT_EQ(0u, gDtorRuns.load(SEQ_CST));
    ASSERT_EQ(pagesBefore + 1, VS::test::activePageCount());

    rcu::barrier(f.d);

    ASSERT_EQ(1u, gDtorRuns.load(SEQ_CST));
    ASSERT_EQ(pagesBefore, VS::test::activePageCount());
    quiesce(f.d);
}

TEST(rcu_retireDestroy_ignores_a_null_pointer) {
    Harness h(2);
    Fixture f;
    VS::SafePtr<Node> nothing{nullptr};
    { rcu::ReadGuard g(f.d); rcu::retireDestroy<Node, &Node::head>(f.d, nothing); }
    rcu::barrier(f.d);
    quiesce(f.d);
}

// ─── drain / tryAdvance / drainAllQuiescent ────────────────────────────────

// drain sweeps without attempting an advance; the retiree only becomes
// reclaimable once the epoch has moved past its bag's tag by two (I2).
TEST(rcu_drain_sweeps_only_what_has_expired) {
    Harness h(2);
    Fixture f;
    Node n = makeNode(5);

    { rcu::ReadGuard g(f.d); rcu::retire<Node, &Node::head, countingDeleter>(f.d, &n); }

    // Still in the caller's Open bag: nothing is sweepable at any epoch.
    ASSERT_EQ(0u, rcu::drain(f.d));
    ASSERT_EQ(0u, gDeleted.load(SEQ_CST));

    rcu::barrier(f.d);
    ASSERT_EQ(1u, gDeleted.load(SEQ_CST));
    ASSERT_EQ(0u, rcu::drain(f.d));
}

// RCU-DEC-035, exercised through the test-only surface: it is the teardown
// drain, and unlike barrier it force-seals remote Open bags, so it is the only
// thing that can empty a domain whose other slots have gone idle.
TEST(rcu_drainAllQuiescent_empties_a_domain_with_idle_remote_slots) {
    Harness h(4);
    Fixture f;
    Node a = makeNode(1), b = makeNode(2);

    kernel::test::bindThreadToCpu(2);
    { rcu::ReadGuard g(f.d); rcu::retire<Node, &Node::head, countingDeleter>(f.d, &a); }

    kernel::test::bindThreadToCpu(0);
    { rcu::ReadGuard g(f.d); rcu::retire<Node, &Node::head, countingDeleter>(f.d, &b); }

    // CPU 2's retiree is in CPU 2's Open bag, which only CPU 2 can seal — so a
    // barrier on CPU 0 covers its own and leaves CPU 2's behind by design.
    rcu::barrier(f.d);
    ASSERT_EQ(1u, gDeleted.load(SEQ_CST));
    ASSERT_EQ(1u, rcu::test::openBagResidue(f.d, 2));

    ASSERT_EQ(1u, rcu::test::drainAllQuiescent(f.d));
    ASSERT_EQ(2u, gDeleted.load(SEQ_CST));
    ASSERT_EQ(0u, rcu::test::totalResidue(f.d));
}

// RCU-DEC-033: a bound hit mid-bag re-seals and leaves the remainder
// re-claimable, so repeated drives still destroy everything exactly once.
TEST(rcu_bounded_drain_batch_still_destroys_everything_exactly_once) {
    Harness h(2);
    Fixture f("bounded", 2);
    constexpr size_t kCount = 9;
    Node nodes[kCount];
    for (size_t i = 0; i < kCount; i++) nodes[i] = makeNode(static_cast<uint32_t>(i));

    {
        rcu::ReadGuard g(f.d);
        for (auto& n : nodes) rcu::retire<Node, &Node::head, countingDeleter>(f.d, &n);
    }
    rcu::barrier(f.d);

    ASSERT_EQ(kCount, gDeleted.load(SEQ_CST));
    ASSERT_EQ(0u, rcu::test::totalResidue(f.d));
}

// RCU-DEC-038: a deleter may retire, from a sweep whose caller is required to be
// OUTSIDE any section. That is the carve-out the amended retire assert exists
// for, and it has to hold through the veneer as well as the engine.
namespace {
    Node gChild;
    Atomic<rcu::Domain*> gChildDomain{nullptr};

    void parentDeleter(Node* n) {
        countingDeleter(n);
        rcu::Domain* d = gChildDomain.load(SEQ_CST);
        if (d != nullptr) {
            gChildDomain.store(nullptr, SEQ_CST);
            rcu::retire<Node, &Node::head, countingDeleter>(*d, &gChild);
        }
    }
}

TEST(rcu_deleter_may_retire_a_child_from_a_sweep) {
    Harness h(2);
    Fixture f;
    Node parent = makeNode(1);
    gChild = makeNode(2);
    gChildDomain.store(&f.d, SEQ_CST);

    { rcu::ReadGuard g(f.d); rcu::retire<Node, &Node::head, parentDeleter>(f.d, &parent); }

    rcu::barrier(f.d);
    ASSERT_EQ(1u, gDeleted.load(SEQ_CST));   // barrier does not cover mid-call retires

    rcu::barrier(f.d);
    ASSERT_EQ(2u, gDeleted.load(SEQ_CST));
    ASSERT_EQ(0u, rcu::test::totalResidue(f.d));
}
