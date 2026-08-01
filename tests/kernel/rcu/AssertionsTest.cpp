//
// RCU Phase 2 — negative tests for the veneer's debug-only assertions.
//
// Every check here is compiled out of a release kernel (RCU-DEC-013 /
// P2-DEC-005); the harness opts them back in via CROCOS_RCU_TEST_HARNESS. Under
// the harness `assert` THROWS rather than panicking, which is what
// EXPECT_ASSERT_FAILURE catches — and which is why the whole veneer is
// CROCOS_RCU_NOEXCEPT rather than plain noexcept: a noexcept entry point would
// turn each of these into std::terminate.
//
// The centrepiece is the two-tier context rule (RCU-DEC-012 / RCU-DEC-031),
// which needs BOTH directions tested. Showing that synchronize rejects #PF is
// only half of it; the other half is that retire ACCEPTS #PF, because that
// carve-out is load-bearing — RadixVM unlinks and retires from the fault path,
// and a mask that quietly tightened to "any interrupt context" would break that
// consumer with no test to catch it. The existing vmsmalloc mock returned
// permanently-zero depths, so this whole class of assertion was untestable until
// MockKernelEnv gained a settable one.
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
#include "MockCpuLocal.h"
#include "MockInterruptContext.h"
#include "MockRcuEnv.h"
#include "DebugIntrospection.h"

using namespace CroCOSTest;
namespace VS   = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;
namespace rcu  = kernel::rcu;
namespace ictx = kernel::interrupts;

namespace {

numa::DomainID mapAllToZero(arch::ProcessorID) { return numa::DomainID{0}; }

struct Harness {
    explicit Harness(size_t cpus = 2, size_t domains = 1) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, &mapAllToZero);
        arch::test::setProcessorCount(cpus);
        kernel::test::bindThreadToCpu(0);
        kernel::timing::test::resetMonoTime();
        ictx::test::resetInterruptDepths();
    }
    ~Harness() {
        ictx::test::resetInterruptDepths();
        VS::test::shutdown();
    }
};

struct Node {
    uint64_t              magic;
    Core::rcu::RetireHead head;
    uint32_t              payload;
};

void nullDeleter(Node*) {}

using Counter = uint8_t ictx::InterruptContextDepths::*;
constexpr Counter kIrq = &ictx::InterruptContextDepths::irq;
constexpr Counter kNmi = &ictx::InterruptContextDepths::nmi;
constexpr Counter kMc  = &ictx::InterruptContextDepths::mc;
constexpr Counter kPf  = &ictx::InterruptContextDepths::pf;

}   // namespace

// ─── Used before init ──────────────────────────────────────────────────────

// The failure table's worst case. With P2-DEC-009's opaque storage there is no
// engine pointer to null-check, so this flag is the ONLY thing standing between
// an uninitialized domain and silent reclamation with no reader protection:
// zeroed slot storage scans perfectly clean.
TEST(rcu_uninitialized_domain_asserts_on_every_entry_point) {
    Harness h;
    rcu::Domain never;
    ASSERT_FALSE(never.initialized());

    Node n{};
    EXPECT_ASSERT_FAILURE(rcu::ReadGuard{never});
    EXPECT_ASSERT_FAILURE((rcu::retire<Node, &Node::head, nullDeleter>(never, &n)));
    EXPECT_ASSERT_FAILURE(rcu::synchronize(never));
    EXPECT_ASSERT_FAILURE(rcu::barrier(never));
    EXPECT_ASSERT_FAILURE(rcu::tryAdvance(never));
    EXPECT_ASSERT_FAILURE(rcu::drain(never));
}

TEST(rcu_double_init_asserts) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("once"));
    EXPECT_ASSERT_FAILURE(d.init("twice"));
}

// ─── protect ───────────────────────────────────────────────────────────────

TEST(rcu_protect_outside_a_section_asserts) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("protect"));
    Node n{};
    Atomic<Node*> link{&n};

    EXPECT_ASSERT_FAILURE(rcu::protect(d, link));

    // ...and is fine inside one, so the assert is discriminating rather than
    // unconditional.
    {
        rcu::ReadGuard g(d);
        ASSERT_EQ(static_cast<void*>(&n), rcu::protect(d, link).raw());
    }
}

// ─── ReadGuard CPU binding (RCU-DEC-009) ───────────────────────────────────

// The destructor unlocks against the slot the lock was taken on and only THEN
// diagnoses, so the domain is left quiescent even though the assert fires —
// which is what lets this be a clean reported failure rather than a cascade.
TEST(rcu_readguard_destroyed_on_another_cpu_asserts) {
    Harness h(4);
    rcu::Domain d;
    ASSERT_TRUE(d.init("migrate"));

    EXPECT_ASSERT_FAILURE(([&] {
        rcu::ReadGuard g(d);
        kernel::test::bindThreadToCpu(1);
    }()));

    kernel::test::bindThreadToCpu(0);
    // The unlock still happened, against slot 0 — no slot was left stuck active.
    ASSERT_EQ(0u, rcu::test::nesting(d, 0));
    ASSERT_EQ(0u, rcu::test::nesting(d, 1));
    rcu::test::assertQuiescent(d);
}

// ─── Two-tier context masks ────────────────────────────────────────────────

// Tier 1, the vmsmalloc DEC-014 mask. Deleters bottom out in vmsfree, so retire
// cannot be more permissive than the allocator it will reenter.
TEST(rcu_retire_asserts_in_irq_context) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("irq"));
    Node n{};

    rcu::ReadGuard g(d);
    ictx::test::ScopedContext irq(kIrq);
    EXPECT_ASSERT_FAILURE((rcu::retire<Node, &Node::head, nullDeleter>(d, &n)));
}

// THE load-bearing half of the two-tier rule. If this ever starts asserting,
// RadixVM's fault-path retire is broken.
TEST(rcu_retire_is_legal_in_page_fault_context) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("pf"));
    Node n{};

    {
        rcu::ReadGuard g(d);
        ictx::test::ScopedContext pf(kPf);
        rcu::retire<Node, &Node::head, nullDeleter>(d, &n);   // must NOT assert
    }
    {
        ictx::test::ScopedContext pf(kPf);
        (void)rcu::tryAdvance(d);                              // must NOT assert
        (void)rcu::drain(d);                                   // must NOT assert
    }
    rcu::barrier(d);
    // checkQuiescent wants every bag FREE, not merely empty; a rotated bag is
    // Open-and-empty, so the teardown drain has to run first (RCU-DEC-035).
    rcu::test::drainAllQuiescent(d);
    rcu::test::assertQuiescent(d);
}

TEST(rcu_drain_and_tryAdvance_assert_in_irq_context) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("irq2"));

    ictx::test::ScopedContext irq(kIrq);
    EXPECT_ASSERT_FAILURE(rcu::drain(d));
    EXPECT_ASSERT_FAILURE(rcu::tryAdvance(d));
}

// Tier 2, the strict mask (RCU-DEC-031). A grace-period wait inside a fault
// handler spins on other CPUs' progress from a context that may itself be
// blocking them — so unlike retire, the blocking pair has NO #PF carve-out.
// This is the pair of assertions that discriminates the two tiers.
TEST(rcu_synchronize_and_barrier_assert_in_page_fault_context) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("strict"));

    ictx::test::ScopedContext pf(kPf);
    EXPECT_ASSERT_FAILURE(rcu::synchronize(d));
    EXPECT_ASSERT_FAILURE(rcu::barrier(d));
}

TEST(rcu_synchronize_and_barrier_assert_in_irq_context) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("strict2"));

    ictx::test::ScopedContext irq(kIrq);
    EXPECT_ASSERT_FAILURE(rcu::synchronize(d));
    EXPECT_ASSERT_FAILURE(rcu::barrier(d));
}

// ─── Read-side context (RCU-DEC-024) ───────────────────────────────────────

// The narrowing that belongs in the header comment as much as in the spec: a
// profiler or watchdog author reaching for RCU in an NMI handler finds this.
TEST(rcu_readguard_asserts_in_nmi_and_machine_check_context) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("nmi"));

    {
        ictx::test::ScopedContext nmi(kNmi);
        EXPECT_ASSERT_FAILURE(rcu::ReadGuard{d});
    }
    {
        ictx::test::ScopedContext mc(kMc);
        EXPECT_ASSERT_FAILURE(rcu::ReadGuard{d});
    }
    rcu::test::assertQuiescent(d);
}

// ...but IRQ and #PF sections are explicitly permitted, and that permission is
// what makes I3 sufficient: an interrupt-context reader arriving mid-primitive
// delays an advance and never permits one.
TEST(rcu_readguard_is_legal_in_irq_and_page_fault_context) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("irqread"));

    {
        ictx::test::ScopedContext irq(kIrq);
        rcu::ReadGuard g(d);
        ASSERT_TRUE(rcu::test::inSection(d, 0));
    }
    {
        ictx::test::ScopedContext pf(kPf);
        rcu::ReadGuard g(d);
        ASSERT_TRUE(rcu::test::inSection(d, 0));
    }
    rcu::test::assertQuiescent(d);
}

// ─── Engine-level preconditions, reached through the veneer ────────────────

// RCU-DEC-019 as amended by RCU-DEC-038. A WRITER unlinking from the live
// structure still needs a section unconditionally: it traverses the shared
// structure, so an unpinned writer can have the parent reclaimed under it.
TEST(rcu_retire_outside_a_section_asserts) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("nosection"));
    Node n{};
    EXPECT_ASSERT_FAILURE((rcu::retire<Node, &Node::head, nullDeleter>(d, &n)));
}

TEST(rcu_synchronize_and_barrier_inside_a_section_assert) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("insection"));

    rcu::ReadGuard g(d);
    EXPECT_ASSERT_FAILURE(rcu::synchronize(d));
    EXPECT_ASSERT_FAILURE(rcu::barrier(d));
}

// Double retire. `next` is nullptr exactly when a node is not in a bag, which is
// what makes this catchable at all.
TEST(rcu_double_retire_asserts) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("double"));
    Node n{};

    Node first{};
    rcu::ReadGuard g(d);
    // The already-linked marker is `next != nullptr`, which cannot see the FIRST
    // node pushed into a bag — that one's next legitimately IS nullptr. Retiring
    // another node first puts a real link in n->next, which is what makes the
    // double retire detectable. Phase 1's RetireHead comment claims next is
    // nullptr "exactly when the node is not in a bag"; the bag-head case is the
    // standing exception, so the check is best-effort rather than complete.
    rcu::retire<Node, &Node::head, nullDeleter>(d, &first);
    rcu::retire<Node, &Node::head, nullDeleter>(d, &n);
    EXPECT_ASSERT_FAILURE((rcu::retire<Node, &Node::head, nullDeleter>(d, &n)));
}
