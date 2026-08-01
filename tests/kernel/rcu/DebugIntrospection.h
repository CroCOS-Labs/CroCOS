//
// RCU Phase 2 — test-only window into a kernel::rcu::Domain.
//
// P2-DEC-009 puts the engine in opaque storage inside Domain, which is exactly
// the property the phase wants (no consumer TU instantiates the Core template)
// and exactly the property that makes the domain untestable from outside: a
// test cannot name EpochDomain<KernelRcuHooks>, so it cannot reach
// Core::rcu::DebugIntrospection either. This header is the veneer's own thin
// forwarding layer to that one, following vmsmalloc's Phase-8 pattern
// (kernel/mm/vmsmalloc.cpp:537-564): declared here, defined inside RCU.cpp where
// the engine is visible, and gated so the kernel build never sees it.
//
// drainAllQuiescent is deliberately exposed HERE and nowhere else. RCU-DEC-035
// gives it a precondition — an external guarantee that no CPU will touch the
// domain again — that Phase 2 has no kernel path to supply (dynamic domain
// teardown is a Non-Goal), so it gets no veneer entry point. The tests DO
// satisfy that precondition, at the end of a joined single- or multi-threaded
// scenario, and they need it: without a teardown drain every test would leave
// its retirees in limbo and the residue assertions would be measuring the
// previous test's garbage.
//
// SNAPSHOTS ASSUME A QUIESCENT DOMAIN, exactly as the Core one does — ordinary
// loads with no protocol participation. Taken while other threads are live they
// are a torn view, not a measurement.
//

#ifndef CROCOS_KERNEL_RCU_DEBUGINTROSPECTION_H
#define CROCOS_KERNEL_RCU_DEBUGINTROSPECTION_H

#include <stddef.h>
#include <stdint.h>
#include <rcu/RCU.h>

#ifdef CROCOS_RCU_TEST_HARNESS

namespace kernel::rcu::test {

    // Is the owner of `slot` inside a read-side section? Meaningful only when
    // asked by that slot's own owner (the field is owner-only plain state).
    bool inSection(const Domain& d, size_t slot);

    // Nesting depth of the calling CPU's section on this domain.
    uint64_t nesting(const Domain& d, size_t slot);

    uint64_t epoch(const Domain& d);
    size_t   slotCount(const Domain& d);
    size_t   drainBatchBound(const Domain& d);

    // Nodes still in limbo across every slot and every bag. NOTE the correct
    // expectation is usually NOT zero: open-bag contents are recoverable only by
    // their owner (I13), so a slot that retires and then goes idle leaves its own
    // Open bag behind by design (ITEM-014). A test asserting this reaches zero
    // without first forcing a seal fails on a CORRECT build.
    size_t totalResidue(const Domain& d);

    // Nodes in slot i's currently-Open bag — the residue floor for an idle slot.
    size_t openBagResidue(const Domain& d, size_t slot);

    // RCU-DEC-034's quiescence check, invoked directly. Throws under the test
    // harness exactly where a debug kernel would panic.
    void assertQuiescent(const Domain& d);

    // RCU-DEC-035. Teardown drain under the caller's no-new-users guarantee;
    // force-seals remote Open bags. Engine-only in Phase 2 — see the file header.
    size_t drainAllQuiescent(Domain& d);

}   // namespace kernel::rcu::test

#endif // CROCOS_RCU_TEST_HARNESS

#endif // CROCOS_KERNEL_RCU_DEBUGINTROSPECTION_H
