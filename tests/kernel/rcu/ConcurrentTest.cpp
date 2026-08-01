//
// RCU Phase 2 — multi-CPU scenarios and the TSan release gate.
//
// One std::thread per modelled CPU, each bound once via bindThreadToCpu — the
// engine's I5 fields are owner-only plain state, so driving one slot from two
// threads is a genuine race and would (correctly) be reported as one. That
// binding discipline is a harness obligation, not something the framework
// checks.
//
// The properties under test here are the ones a single-threaded run cannot
// reach: that the veneer's slot derivation really is per-CPU under concurrency,
// that stealing works through it (a slot's SEALED bags drain on ANOTHER CPU's
// calls), and that no object is destroyed twice or lost when every CPU is
// retiring and sweeping at once.
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

#include <thread>
#include <vector>

using namespace CroCOSTest;
namespace VS   = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;
namespace rcu  = kernel::rcu;

namespace {

constexpr size_t kCpus = 4;

numa::DomainID mapAllToZero(arch::ProcessorID) { return numa::DomainID{0}; }

struct Harness {
    explicit Harness(size_t cpus = kCpus, size_t domains = 1) {
        VS::test::initialize(cpus, domains);
        numa::test::configure(cpus, domains, &mapAllToZero);
        arch::test::setProcessorCount(cpus);
        kernel::test::bindThreadToCpu(0);
        kernel::timing::test::resetMonoTime();
        kernel::interrupts::test::resetInterruptDepths();
    }
    ~Harness() { VS::test::shutdown(); }
};

struct Node {
    uint64_t              magic;
    Core::rcu::RetireHead head;
    uint32_t              payload;
};

constexpr uint64_t kMagic = 0x5AFEC0DE5AFEC0DEull;

Atomic<size_t> gDeleted{0};
Atomic<size_t> gBadMagic{0};

void countingDeleter(Node* n) {
    // Recovering the wrong address would show up here rather than as a silent
    // memory smear.
    if (n->magic != kMagic) gBadMagic.fetch_add(1, SEQ_CST);
    n->magic = 0;
    gDeleted.fetch_add(1, SEQ_CST);
}

}   // namespace

// Every CPU retires, reads and sweeps concurrently. After the join, the
// teardown drain (RCU-DEC-035) is the only thing that can recover the idle
// slots' Open bags, and afterwards every object must have been destroyed
// exactly once.
TEST(rcu_concurrent_retire_and_sweep_destroys_everything_exactly_once) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("concurrent"));
    gDeleted.store(0, SEQ_CST);
    gBadMagic.store(0, SEQ_CST);

    constexpr size_t kPerCpu = 200;
    std::vector<std::vector<Node>> storage(kCpus);
    for (size_t c = 0; c < kCpus; c++) {
        storage[c].resize(kPerCpu);
        for (size_t i = 0; i < kPerCpu; i++) {
            storage[c][i] = Node{kMagic, {}, static_cast<uint32_t>(i)};
        }
    }

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; c++) {
        threads.emplace_back([&, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
            for (size_t i = 0; i < kPerCpu; i++) {
                {
                    rcu::ReadGuard g(d);
                    rcu::retire<Node, &Node::head, countingDeleter>(d, &storage[c][i]);
                }
                if ((i & 7) == 0) (void)rcu::tryAdvance(d);
                if ((i & 15) == 0) {
                    rcu::ReadGuard g(d);      // a reader that must delay, never permit
                    (void)rcu::test::epoch(d);
                }
            }
            rcu::barrier(d);
        });
    }
    for (auto& t : threads) t.join();

    kernel::test::bindThreadToCpu(0);
    rcu::test::drainAllQuiescent(d);

    ASSERT_EQ(kCpus * kPerCpu, gDeleted.load(SEQ_CST));
    ASSERT_EQ(0u, gBadMagic.load(SEQ_CST));
    ASSERT_EQ(0u, rcu::test::totalResidue(d));
    rcu::test::assertQuiescent(d);
}

// Stealing (RCU-DEC-006) through the veneer: a slot that retires, seals and then
// goes permanently idle still gets its SEALED bags drained by other CPUs. Its
// OPEN bag is the exact residue and is recoverable only by its owner (I13) — a
// test expecting residue to reach zero here would fail on a CORRECT build.
TEST(rcu_idle_slots_sealed_bags_are_drained_by_other_cpus) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("stealing"));
    gDeleted.store(0, SEQ_CST);
    gBadMagic.store(0, SEQ_CST);

    constexpr size_t kRetires = 40;
    std::vector<Node> quiet(kRetires);
    for (size_t i = 0; i < kRetires; i++) {
        quiet[i] = Node{kMagic, {}, static_cast<uint32_t>(i)};
    }

    // CPU 1 retires a batch and then never touches the domain again. The last
    // retire leaves an Open bag behind; everything before it has been sealed by
    // rotation.
    std::thread quietCpu([&] {
        kernel::test::bindThreadToCpu(1);
        for (auto& n : quiet) {
            rcu::ReadGuard g(d);
            rcu::retire<Node, &Node::head, countingDeleter>(d, &n);
        }
    });
    quietCpu.join();

    const size_t residue = rcu::test::openBagResidue(d, 1);
    ASSERT_GT(residue, 0u);
    ASSERT_EQ(0u, gDeleted.load(SEQ_CST));

    // CPU 0 drives the domain. It never retires anything of its own, so every
    // deleter that runs is stolen work on CPU 1's sealed bags.
    kernel::test::bindThreadToCpu(0);
    for (size_t i = 0; i < 8; i++) (void)rcu::tryAdvance(d);

    ASSERT_EQ(kRetires - residue, gDeleted.load(SEQ_CST));
    ASSERT_EQ(residue, rcu::test::totalResidue(d));

    // Only the teardown drain can force-seal a remote Open bag.
    ASSERT_EQ(residue, rcu::test::drainAllQuiescent(d));
    ASSERT_EQ(kRetires, gDeleted.load(SEQ_CST));
    ASSERT_EQ(0u, gBadMagic.load(SEQ_CST));
    rcu::test::assertQuiescent(d);
}

// P2-I1 under concurrency: each thread's section lands in its own slot and
// nowhere else. Single-threaded this is trivially true; the point here is that
// nothing in the veneer caches or shares the derived index.
TEST(rcu_each_cpu_lands_in_its_own_slot) {
    Harness h;
    rcu::Domain d;
    ASSERT_TRUE(d.init("slots"));

    Atomic<size_t> ready{0};
    Atomic<bool>   release{false};
    Atomic<size_t> mismatches{0};

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; c++) {
        threads.emplace_back([&, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
            rcu::ReadGuard g(d);
            ready.fetch_add(1, SEQ_CST);
            while (!release.load(SEQ_CST)) { std::this_thread::yield(); }
            if (!rcu::test::inSection(d, c)) mismatches.fetch_add(1, SEQ_CST);
        });
    }

    while (ready.load(SEQ_CST) != kCpus) { std::this_thread::yield(); }
    // All four sections are open at once, so every slot must read as active.
    for (size_t c = 0; c < kCpus; c++) ASSERT_TRUE(rcu::test::inSection(d, c));
    release.store(true, SEQ_CST);

    for (auto& t : threads) t.join();
    ASSERT_EQ(0u, mismatches.load(SEQ_CST));

    kernel::test::bindThreadToCpu(0);
    rcu::test::assertQuiescent(d);
}
