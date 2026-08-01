//
// Created by Spencer Martin on 8/1/26.
//
// Core::rcu::DebugIntrospection — test-only window into an EpochDomain's
// internals (P1-DEC-007), following the vmsmalloc Phase-8 pattern
// (kernel/mm/vmsmalloc.cpp:532-559).
//
// It exists for two reasons:
//
//   1. The bag algebra is otherwise untestable. I2 ("a bag tagged t is drained
//      only when the global epoch is >= t+2"), the Claimed -> Sealed re-seal on a
//      batch-bound hit, and the quiet-system residue bound are all statements
//      about state no public entry point reports.
//
//   2. assertQuiescent() is how RCU-DEC-034's check gets a negative test. It
//      cannot be driven through ~EpochDomain: under the Core test harness
//      assert(...) throws, and a throwing assert cannot escape a noexcept
//      destructor — it is std::terminate, not a catchable failure. The
//      destructor and this function call the same internal check, so testing
//      this one really does test that one.
//
// Gated behind CROCOS_RCU_TEST_HARNESS; the kernel build never sees the
// definitions. (EpochDomain's friend declaration is deliberately NOT gated, so
// that the two headers have no include-order dependency. Befriending a type
// that is never defined costs nothing.)
//
// SNAPSHOTS ASSUME A QUIESCENT DOMAIN — single-threaded, or post-join. They
// perform ordinary loads with no protocol participation whatsoever, so a
// snapshot taken while other CPUs are live is a torn view, not a measurement.
// In particular, walking a bag's node list races any drainer holding it Claimed.
//

#ifndef CROCOS_RCU_DEBUGINTROSPECTION_H
#define CROCOS_RCU_DEBUGINTROSPECTION_H

#include <core/rcu/EpochDomain.h>

#ifdef CROCOS_RCU_TEST_HARNESS

namespace Core::rcu {

    struct SlotSnapshot {
        bool     active;          // decoded from the state word's bit 0
        uint64_t epochSnapshot;   // decoded from bits 63..1; meaningless if !active
        uint64_t nesting;         // owner-only (I5)
        uint64_t openBagIndex;    // owner-only (I5)
        uint64_t retireCount;     // owner-only (I5)
        bool     inDrain;         // owner-only (I5); I14 + the RCU-DEC-038 assert
    };

    struct BagSnapshot {
        uint64_t    tag;
        BagState    state;
        RetireHead* head;
        size_t      nodeCount;    // walked; see the racing caveat in the file header
        bool        expired;      // globalEpoch >= tag + 2 — I2's reclaim gate
    };

    template <typename Hooks>
    struct DebugIntrospection {
        using Domain = EpochDomain<Hooks>;

        [[nodiscard]] static uint64_t epoch(const Domain& d) {
            return d.globalEpoch.load(kEpochObserve);
        }

        [[nodiscard]] static size_t slotCount(const Domain& d) { return d.slotCount; }

        [[nodiscard]] static bool teardownActive(const Domain& d) { return d.teardownActive; }

        [[nodiscard]] static size_t drainBatchBound(const Domain& d) { return d.drainBatchBound; }

        [[nodiscard]] static SlotSnapshot slot(const Domain& d, size_t i) {
            const ReaderSlot& s = d.slots[i];
            const uint64_t w = s.state.load(kScanLoad);
            return { isActive(w), epochOf(w), s.nesting, s.openBagIndex, s.retireCount, s.inDrain };
        }

        [[nodiscard]] static BagSnapshot bag(const Domain& d, size_t i, size_t b) {
            const ReaderSlot& s = d.slots[i];
            const uint64_t v = s.bagTagState[b].load(kBagTagLoad);
            const uint64_t tag = bagTagOf(v);
            RetireHead* head = s.bagHead[b].load(kBagDrainAccess);

            size_t n = 0;
            for (RetireHead* p = head; p != nullptr; p = p->next) ++n;

            return { tag, bagStateOf(v), head, n, epoch(d) >= tag + 2 };
        }

        // Total nodes still in limbo across every slot and every bag — the
        // "residue" the quiet-system tests measure. Note the correct expectation
        // there is NOT zero: open-bag contents are recoverable only by their
        // owner (I13), so a slot that retires and then goes permanently idle
        // leaves its own Open bag behind by design (ITEM-014). A test asserting
        // residue reaches zero fails on a CORRECT build.
        [[nodiscard]] static size_t totalResidue(const Domain& d) {
            size_t total = 0;
            for (size_t i = 0; i < d.slotCount; ++i) {
                for (size_t b = 0; b < kBagCount; ++b) total += bag(d, i, b).nodeCount;
            }
            return total;
        }

        // Nodes in slot i's currently-Open bag: the exact residue floor for a slot
        // whose owner has gone quiet.
        [[nodiscard]] static size_t openBagResidue(const Domain& d, size_t i) {
            const size_t b = static_cast<size_t>(d.slots[i].openBagIndex);
            const BagSnapshot snap = bag(d, i, b);
            return snap.state == BagState::Open ? snap.nodeCount : 0;
        }

        // RCU-DEC-034's check, invoked directly. Throws (under the test harness)
        // exactly where ~EpochDomain would panic in a debug kernel.
        static void assertQuiescent(const Domain& d) { d.checkQuiescent(); }
    };

}

#endif // CROCOS_RCU_TEST_HARNESS

#endif //CROCOS_RCU_DEBUGINTROSPECTION_H
