//
// RCU Phase 3 — the torture suite. specs/rcu-phase-3.md.
//
// This is the release gate: the framework is not considered usable by any
// consumer until this file is green under BOTH runners on ARMv8 (P3-DEC-003).
//
// ─── What makes this different from the Phase 1/2 tests ────────────────────
//
// tests/core/RcuConcurrentTest.cpp already tortures the ENGINE — mini-rcutorture,
// the HAZARD-1 claim window via StallOnClaimHooks, and the reseal hand-off. Those
// are deliberately not repeated here. What this file adds is sustained churn
// through the VENEER: ReadGuard's masked transition, `protect`'s SafePtr wrap,
// the retire thunks, `barrier`, and the mock CpuLocal slot derivation — the layer
// a consumer actually touches, and the layer where a slot-identity or
// context-rule bug would live.
//
// ─── The oracle (P3-DEC-001) ──────────────────────────────────────────────
//
// Every deleter here really calls `delete`. Not a pool, not a poison-only mode,
// not `retireDestroy` — that path bottoms out in the mock VMSubstrate, which
// RECYCLES, and a recycled read is silent. Phase 2 covers `retireDestroy`; this
// phase deliberately gives it up so that a use-after-grace-period is a hard ASan
// trap with allocation and free stacks attached. That is the strongest UAGP
// detector available and the only reason heap-backed nodes matter.
//
// ─── The exception discipline (P3-DEC-004) ────────────────────────────────
//
// Under this harness `assert` THROWS. An exception escaping a std::thread lambda
// is uncaught and calls std::terminate, which kills the process and destroys the
// test summary — vmsmalloc Phase 8 hit exactly this. So every worker body goes
// through `worker()`, which catches and records. No exception may cross a thread
// boundary in this file.
//
// ─── Teardown (P3-I5) ─────────────────────────────────────────────────────
//
// A PASSING scenario tears down through the contract: join (the happens-before
// edge RCU-DEC-035 requires — a relaxed done-flag would NOT qualify), then
// drainAllQuiescent, then the quiescence assert. A FAILED scenario skips teardown
// entirely and leaks its domain on purpose: running the teardown drain on a
// domain wrecked mid-scenario throws from the epilogue, and a throwing assert
// during unwinding is std::terminate — the exact failure P3-DEC-004 exists to
// prevent. Leaked domains are parked in gLeakedDomains so they stay REACHABLE:
// the ASan runner also has -fsanitize=leak, and LSan reports unreachable blocks,
// so a bare leak would turn a deliberate design property into a spurious failure.
//
// ─── Structure under torture ──────────────────────────────────────────────
//
// An array of cells, each an Atomic<Payload*>. A published value is a CHAIN of
// kChainDepth payloads rather than a single node (P3-ITEM-003): an array of
// single cells only ever holds one pointer per section, which does not exercise
// the "hold a pointer across several dereferences" shape a radix walk has. The
// chain is immutable once published and retired as a unit, so its links need no
// atomics; readers walk all of it inside ONE section, which is precisely the
// no-ABA-within-a-section guarantee (R5) under test.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <arch.h>
#include <core/atomic.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock
#include <rcu/RCU.h>           // real
#include "MockCpuLocal.h"
#include "MockInterruptContext.h"
#include "MockKlog.h"
#include "MockRcuEnv.h"
#include "DebugIntrospection.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace CroCOSTest;
namespace VS   = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;
namespace rcu  = kernel::rcu;

namespace {

constexpr size_t kCpus       = 4;
constexpr size_t kChainDepth = 3;
constexpr size_t kCells      = 8;

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

// ─── Payload ───────────────────────────────────────────────────────────────

constexpr uint64_t kLiveMagic = 0x544F52545552455Aull;   // "TORTUREZ"

struct Payload {
    uint64_t              magic     = kLiveMagic;
    Core::rcu::RetireHead head      = {};
    Payload*              chainNext = nullptr;   // plain: immutable once published
    uint64_t              version   = 0;
    uint32_t              id        = 0;
    uint32_t              depth     = 0;
    uint64_t              filler[3] = {};
    uint64_t              checksum  = 0;         // torn-write oracle
};

uint64_t checksumOf(const Payload& p) {
    return kLiveMagic ^ p.version ^ (uint64_t{p.id} << 8) ^ (uint64_t{p.depth} << 40);
}

// ─── Accounting ────────────────────────────────────────────────────────────
//
// Per-id run counters rather than a bare total: the batch-bound scenarios need
// "nothing lost, nothing double-run", and a total alone cannot distinguish one
// object run twice from two objects run once each.

constexpr uint32_t kNoExecutor = 0xFFFFFFFFu;

std::atomic<uint32_t>* gRunCount = nullptr;   // per-id deleter invocations
std::atomic<uint32_t>* gExecutor = nullptr;   // per-id thread tag that ran it
size_t                 gCapacity = 0;

std::atomic<size_t> gDestroyed{0};
std::atomic<size_t> gDoubleRun{0};
std::atomic<size_t> gCorrupt{0};      // bad magic or bad checksum at delete time
std::atomic<size_t> gReadErrors{0};   // reader saw a torn or dead payload
std::atomic<size_t> gIdOverflow{0};
std::atomic<uint32_t> gNextId{0};

// Identifies which modelled CPU ran a given deleter — the quiet-system residue
// scenario's whole assertion is about WHOSE thread the work happened on.
thread_local uint32_t tlThreadTag = kNoExecutor;

void resetAccounting(size_t capacity) {
    delete[] gRunCount;
    delete[] gExecutor;
    gRunCount = new std::atomic<uint32_t>[capacity];
    gExecutor = new std::atomic<uint32_t>[capacity];
    for (size_t i = 0; i < capacity; ++i) {
        gRunCount[i].store(0, std::memory_order_relaxed);
        gExecutor[i].store(kNoExecutor, std::memory_order_relaxed);
    }
    gCapacity = capacity;
    gDestroyed.store(0);
    gDoubleRun.store(0);
    gCorrupt.store(0);
    gReadErrors.store(0);
    gIdOverflow.store(0);
    gNextId.store(0);
}

void teardownAccounting() {
    delete[] gRunCount;
    delete[] gExecutor;
    gRunCount = nullptr;
    gExecutor = nullptr;
    gCapacity = 0;
}

// THE oracle. `delete` is real (P3-DEC-001) — do not soften this to a pool.
void tortureDeleter(Payload* p) {
    if (p->magic != kLiveMagic || p->checksum != checksumOf(*p)) {
        gCorrupt.fetch_add(1, std::memory_order_relaxed);
    }
    if (p->id < gCapacity) {
        if (gRunCount[p->id].fetch_add(1, std::memory_order_relaxed) != 0) {
            gDoubleRun.fetch_add(1, std::memory_order_relaxed);
        }
        gExecutor[p->id].store(tlThreadTag, std::memory_order_relaxed);
    }
    gDestroyed.fetch_add(1, std::memory_order_relaxed);
    p->magic = 0;
    delete p;
}

// A deleter that itself retires a second object (RCU-DEC-039 reentrancy). The
// domain is reachable through a global because the engine's deleter signature
// carries no context word — the same shape a real consumer would use.
std::atomic<rcu::Domain*> gDeleterDomain{nullptr};
std::atomic<int>          gDeleterRetireBudget{0};

void retiringDeleter(Payload* p);   // defined below tortureDeleter's users

Payload* makePayload(uint32_t depth) {
    const uint32_t id = gNextId.fetch_add(1, std::memory_order_relaxed);
    if (id >= gCapacity) {
        gIdOverflow.fetch_add(1, std::memory_order_relaxed);
    }
    auto* p = new Payload();
    p->id       = id;
    p->depth    = depth;
    p->version  = uint64_t{id} * 1000u + depth;
    p->checksum = checksumOf(*p);
    return p;
}

// A whole chain: kChainDepth payloads a reader walks inside one section.
Payload* makeChain() {
    Payload* headNode = nullptr;
    for (uint32_t d = kChainDepth; d-- > 0;) {
        Payload* n = makePayload(d);
        n->chainNext = headNode;
        headNode = n;
    }
    return headNode;
}

// Retire every node of a chain. Each carries its own RetireHead, so the chain
// links and the limbo links never alias.
void retireChain(rcu::Domain& d, Payload* chain) {
    while (chain != nullptr) {
        Payload* const next = chain->chainNext;
        rcu::retire<Payload, &Payload::head, tortureDeleter>(d, chain);
        chain = next;
    }
}

void deleteChainDirect(Payload* chain) {
    while (chain != nullptr) {
        Payload* const next = chain->chainNext;
        delete chain;
        chain = next;
    }
}

void retiringDeleter(Payload* p) {
    rcu::Domain* d = gDeleterDomain.load(std::memory_order_acquire);
    // Bounded, or the chain never terminates and the teardown drain spins.
    if (d != nullptr && gDeleterRetireBudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        Payload* fresh = makePayload(0);
        // Legal precisely because we are inside a drain (RCU-DEC-019 as amended
        // by RCU-DEC-038); the engine asserts that carve-out.
        rcu::retire<Payload, &Payload::head, tortureDeleter>(*d, fresh);
    }
    tortureDeleter(p);
}

// ─── Reader body ───────────────────────────────────────────────────────────
//
// One section, a `protect` load, then a walk of the whole chain. Every step
// dereferences a pointer whose only protection is that the section is open —
// which is exactly what the ASan runner is watching.
void readOneCell(rcu::Domain& d, Atomic<Payload*>& cell) {
    rcu::ReadGuard g(d);
    VS::SafePtr<Payload> sp = rcu::protect<Payload>(d, cell);
    Payload* p = &(*sp);
    for (size_t step = 0; step < kChainDepth && p != nullptr; ++step) {
        if (p->magic != kLiveMagic || p->checksum != checksumOf(*p)) {
            gReadErrors.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        p = p->chainNext;
    }
}

// ─── Worker-thread exception discipline (P3-DEC-004) ───────────────────────

std::atomic<size_t> gWorkerFailures{0};
std::mutex          gFailureMutex;
std::string         gFirstFailure;

void resetFailures() {
    gWorkerFailures.store(0);
    std::lock_guard<std::mutex> lock(gFailureMutex);
    gFirstFailure.clear();
}

void recordFailure(const char* what) {
    {
        std::lock_guard<std::mutex> lock(gFailureMutex);
        if (gFirstFailure.empty()) gFirstFailure = what;
    }
    gWorkerFailures.fetch_add(1, std::memory_order_relaxed);
}

bool firstFailureContains(const char* needle) {
    std::lock_guard<std::mutex> lock(gFailureMutex);
    return gFirstFailure.find(needle) != std::string::npos;
}

// Every std::thread body in this file goes through here. Nothing may throw past
// it — see the file header.
template <typename F>
void worker(uint32_t tag, F&& body) noexcept {
    tlThreadTag = tag;
    kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(tag));
    try {
        body();
    } catch (const AssertionFailure& e) {
        recordFailure(e.what());
    } catch (const std::exception& e) {
        recordFailure(e.what());
    } catch (...) {
        recordFailure("unknown exception escaped a worker body");
    }
}

// ─── Scenario domain holder (P3-I5) ────────────────────────────────────────

std::vector<rcu::Domain*> gLeakedDomains;   // keeps failed scenarios' domains REACHABLE

struct TortureDomain {
    rcu::Domain* d = nullptr;

    explicit TortureDomain(const char* name,
                           size_t bound = Core::rcu::kUnboundedDrainBatch)
        : d(new rcu::Domain()) {
        if (!d->init(name, bound)) {
            abandon();
            throw AssertionFailure("rcu torture: Domain::init failed");
        }
    }

    TortureDomain(const TortureDomain&)            = delete;
    TortureDomain& operator=(const TortureDomain&) = delete;

    rcu::Domain& operator*() const { return *d; }

    // The success path, and the ONLY path that touches the domain again.
    size_t finish() {
        const size_t ran = rcu::test::drainAllQuiescent(*d);
        rcu::test::assertQuiescent(*d);
        delete d;
        d = nullptr;
        return ran;
    }

    // The failure path: park the pointer and never touch the domain again.
    void abandon() {
        if (d != nullptr) gLeakedDomains.push_back(d);
        d = nullptr;
    }

    // NOT a cleanup. If a scenario threw before finish(), the domain is wrecked
    // and must be abandoned rather than drained — see the file header.
    ~TortureDomain() { abandon(); }
};

// Convenience for the many scenarios that assert the same closing invariants.
void assertNothingLostOrDoubled(size_t expectedDestroyed) {
    ASSERT_EQ(size_t{0}, gIdOverflow.load());
    ASSERT_EQ(size_t{0}, gReadErrors.load());
    ASSERT_EQ(size_t{0}, gCorrupt.load());
    ASSERT_EQ(size_t{0}, gDoubleRun.load());
    ASSERT_EQ(size_t{0}, gWorkerFailures.load());
    ASSERT_EQ(expectedDestroyed, gDestroyed.load());
}

}   // namespace

// ============================================================================
// Stutter — all workers pause; every SEALED bag must drain
// ============================================================================
//
// The failure this catches: readers that pause OUTSIDE a section must not block
// anything, so once everyone is quiet a driver's advances must reclaim every
// sealed bag. If drain never completes, residue stays above the open-bag floor.
//
// The measurement is taken with every worker parked, not mid-flight. That is
// deliberate: introspection snapshots assume a quiescent domain, and P3-I3 says
// no scenario may depend on a race happening.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureStutter, 60000) {
    Harness h;
    TortureDomain td("torture-stutter");
    auto& d = *td;

    constexpr size_t kRounds     = 4;
    constexpr size_t kPerRound   = 300;
    constexpr size_t kReadRounds = 2000;

    resetAccounting(kRounds * kCpus * kPerRound * kChainDepth + 1024);
    resetFailures();

    std::vector<Atomic<Payload*>> cells(kCells);
    for (auto& c : cells) c.store(makeChain(), SEQ_CST);

    std::atomic<size_t> parked{0};
    std::atomic<size_t> round{0};
    std::atomic<bool>   stutterGate{false};
    std::atomic<bool>   done{false};

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t r = 0; r < kRounds; ++r) {
                    // Writers on even slots, readers on odd — every slot both
                    // retires and reads across the rounds.
                    for (size_t i = 0; i < kPerRound; ++i) {
                        Atomic<Payload*>& cell = cells[(c * 3 + i) % kCells];
                        if (((c + r) & 1) == 0) {
                            Payload* fresh = makeChain();
                            rcu::ReadGuard g(d);
                            Payload* old = cell.exchange(fresh, ACQ_REL);
                            retireChain(d, old);
                        } else {
                            for (size_t k = 0; k < kReadRounds / kPerRound; ++k) {
                                readOneCell(d, cell);
                            }
                        }
                        if ((i & 31) == 0) (void)rcu::tryAdvance(d);
                    }

                    // ── The stutter: park OUTSIDE any section and wait ──
                    parked.fetch_add(1, std::memory_order_acq_rel);
                    while (round.load(std::memory_order_acquire) == r &&
                           !stutterGate.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    while (round.load(std::memory_order_acquire) == r) {
                        std::this_thread::yield();
                    }
                }
                done.store(true, std::memory_order_release);
            });
        });
    }

    for (size_t r = 0; r < kRounds; ++r) {
        while (parked.load(std::memory_order_acquire) != kCpus * (r + 1)) {
            std::this_thread::yield();
        }
        stutterGate.store(true, std::memory_order_release);

        // Every worker is parked outside a section, so nothing can block an
        // advance and nothing can push into a bag. Drive to a fixed point.
        for (size_t i = 0; i < 32; ++i) (void)rcu::tryAdvance(d);

        // The exact expectation: every SEALED bag has drained, and what is left
        // is precisely the per-slot Open-bag floor (I13 / ITEM-014). Asserting
        // zero here would fail on a CORRECT build.
        size_t openFloor = 0;
        for (size_t s = 0; s < kCpus; ++s) openFloor += rcu::test::openBagResidue(d, s);
        ASSERT_EQ(openFloor, rcu::test::totalResidue(d));

        stutterGate.store(false, std::memory_order_release);
        round.store(r + 1, std::memory_order_release);
    }

    for (auto& t : threads) t.join();
    ASSERT_TRUE(done.load());

    kernel::test::bindThreadToCpu(0);
    const size_t retired = gNextId.load() - kCells * kChainDepth;
    rcu::test::drainAllQuiescent(d);

    // The chains still published were never retired; the domain never owned them.
    for (auto& c : cells) deleteChainDirect(c.load(SEQ_CST));

    assertNothingLostOrDoubled(retired);
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Ratio sweep — behaviour stable from write-heavy to read-heavy
// ============================================================================
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureRatioSweep, 60000) {
    Harness h;
    TortureDomain td("torture-ratios");
    auto& d = *td;

    constexpr size_t kIters       = 400;
    constexpr size_t kReadsPerIter = 6;
    const size_t     writerCounts[] = {1, 2, 3, 4};

    resetAccounting(4 * kCpus * kIters * kChainDepth + 1024);
    resetFailures();

    std::vector<Atomic<Payload*>> cells(kCells);
    for (auto& c : cells) c.store(makeChain(), SEQ_CST);

    for (const size_t writers : writerCounts) {
        std::vector<std::thread> threads;
        for (size_t c = 0; c < kCpus; ++c) {
            threads.emplace_back([&, c] {
                worker(static_cast<uint32_t>(c), [&] {
                    const bool isWriter = c < writers;
                    for (size_t i = 0; i < kIters; ++i) {
                        Atomic<Payload*>& cell = cells[(c * 5 + i) % kCells];
                        if (isWriter) {
                            Payload* fresh = makeChain();
                            rcu::ReadGuard g(d);
                            Payload* old = cell.exchange(fresh, ACQ_REL);
                            retireChain(d, old);
                        } else {
                            for (size_t k = 0; k < kReadsPerIter; ++k) readOneCell(d, cell);
                        }
                        if ((i & 15) == 0) (void)rcu::tryAdvance(d);
                    }
                    // Per-slot completion for the caller's own retirees.
                    if (isWriter) rcu::barrier(d);
                });
            });
        }
        for (auto& t : threads) t.join();
        kernel::test::bindThreadToCpu(0);

        // Cross-ratio invariant, checked while the domain is quiescent between
        // phases: no object may have been run twice at any ratio.
        ASSERT_EQ(size_t{0}, gDoubleRun.load());
        ASSERT_EQ(size_t{0}, gWorkerFailures.load());
    }

    const size_t retired = gNextId.load() - kCells * kChainDepth;
    rcu::test::drainAllQuiescent(d);
    for (auto& c : cells) deleteChainDirect(c.load(SEQ_CST));

    assertNothingLostOrDoubled(retired);
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Forced stall — reclamation MUST stall, and the diagnostic MUST fire
// ============================================================================
//
// The parent Hazards call this out by name: a torture suite that cannot fail is
// worse than none. Both halves are asserted here — that a held section really
// does block advancement past exactly one step (I3's exact-match check), and
// that RCU-DEC-013's stall diagnostic really does emit. The second half is why
// the klog capture in MockKlog.h exists; without it the diagnostic could only be
// trusted.
//
// The stall is INJECTED and released deterministically (P3-DEC-005) — nothing
// here waits hoping a race occurs.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureForcedStall, 60000) {
    Harness h;
    TortureDomain td("torture-stall");
    auto& d = *td;

    constexpr size_t kRetires = 40;
    resetAccounting(kRetires + 64);
    resetFailures();

    std::atomic<bool> readerIn{false};
    std::atomic<bool> release{false};
    std::atomic<bool> readerOut{false};

    kernel::test::beginKlogCapture();

    std::thread stalled([&] {
        worker(1, [&] {
            rcu::ReadGuard g(d);
            readerIn.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            // Cross RCU-DEC-013's threshold without waiting 100 ms of wall clock.
            kernel::timing::test::advanceMonoTime(rcu::kSectionStallWarnNs + 1);
        });
        readerOut.store(true, std::memory_order_release);
    });

    while (!readerIn.load(std::memory_order_acquire)) std::this_thread::yield();

    // The reader's snapshot is <= e0. From e0 the epoch may advance at most once
    // (to e0+1, if the snapshot happened to equal e0); after that the exact-match
    // check blocks every further advance until the section exits.
    kernel::test::bindThreadToCpu(0);
    const uint64_t e0 = rcu::test::epoch(d);

    std::vector<Payload*> pending;
    for (size_t i = 0; i < kRetires; ++i) {
        Payload* p = makePayload(0);
        pending.push_back(p);
        rcu::ReadGuard g(d);
        rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
    }
    for (size_t i = 0; i < 64; ++i) (void)rcu::tryAdvance(d);

    // Half one: reclamation stalled.
    const uint64_t stalledEpoch = rcu::test::epoch(d);
    ASSERT_TRUE(stalledEpoch <= e0 + 1);
    ASSERT_EQ(size_t{0}, gDestroyed.load());
    ASSERT_EQ(kRetires, rcu::test::totalResidue(d));

    release.store(true, std::memory_order_release);
    stalled.join();
    ASSERT_TRUE(readerOut.load());
    kernel::test::endKlogCapture();

    // Half two: the diagnostic fired. ~ReadGuard logs the domain name and the
    // slot it was bound to.
    ASSERT_TRUE(kernel::test::klogCaptureContains("torture-stall"));
    ASSERT_TRUE(kernel::test::klogCaptureContains("section on CPU 1"));

    // ...and reclamation resumes now that nothing blocks it.
    kernel::test::bindThreadToCpu(0);
    for (size_t i = 0; i < 64; ++i) (void)rcu::tryAdvance(d);
    ASSERT_TRUE(rcu::test::epoch(d) > stalledEpoch);

    rcu::test::drainAllQuiescent(d);
    assertNothingLostOrDoubled(kRetires);
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Quiet-system residue (RCU-DEC-006 / P3-DEC-007)
// ============================================================================
//
// The choreography is the whole test, and it is choreographed precisely because
// the obvious formulation fails on a CORRECT build (P3-DEC-007's same-day
// correction):
//
//   1. A retires a batch SMALLER than kRetireAdvanceThreshold, so A never
//      self-advances and never self-drains.
//   2. B advances the epoch past A's bag tag — between A's batch and A's final
//      retire.
//   3. A's final retire therefore rotates, SEALING the batch bag at its own
//      lower tag, which is already expired.
//   4. A goes quiet — it never touches the domain again, which is exactly what
//      an idle kernel CPU looks like to the domain (P3-ITEM-002).
//
// Assert: every sealed-bag retiree of A was destroyed ON B'S THREAD (stealing,
// RCU-DEC-006), and the terminal residue is EXACTLY A's open bag — the final
// retiree — because Open -> Sealed is an owner-only store (I13 / ITEM-014).
//
// This fails if stealing is removed, and it also fails if residue exceeds the
// documented bound. Both directions matter.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureQuietSystemResidue, 60000) {
    Harness h;
    TortureDomain td("torture-quiet");
    auto& d = *td;

    constexpr size_t kBatch  = 10;   // << kRetireAdvanceThreshold (64)
    constexpr uint32_t kTagA = 1;
    constexpr uint32_t kTagB = 2;

    resetAccounting(kBatch + 64);
    resetFailures();

    std::atomic<int> step{0};

    std::thread threadA([&] {
        worker(kTagA, [&] {
            for (size_t i = 0; i < kBatch; ++i) {
                Payload* p = makePayload(0);
                rcu::ReadGuard g(d);
                rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
            }
            step.store(1, std::memory_order_release);                      // batch in A's Open bag
            while (step.load(std::memory_order_acquire) != 2) std::this_thread::yield();

            // B has moved the epoch on. This retire rotates: it seals the batch
            // bag at its ORIGINAL tag (already expired) and opens a fresh one.
            Payload* last = makePayload(0);
            rcu::ReadGuard g(d);
            rcu::retire<Payload, &Payload::head, tortureDeleter>(d, last);
            step.store(3, std::memory_order_release);
            // ...and A never touches the domain again.
        });
    });

    std::thread threadB([&] {
        worker(kTagB, [&] {
            while (step.load(std::memory_order_acquire) != 1) std::this_thread::yield();
            // Two advances put the epoch at tag+2, so A's bag expires the instant
            // it is sealed.
            for (size_t i = 0; i < 8; ++i) (void)rcu::tryAdvance(d);
            step.store(2, std::memory_order_release);

            while (step.load(std::memory_order_acquire) != 3) std::this_thread::yield();
            // B alone drives the system from here. Every deleter that runs is
            // stolen work on A's sealed bag.
            for (size_t i = 0; i < 64; ++i) (void)rcu::tryAdvance(d);
        });
    });

    threadA.join();
    threadB.join();
    kernel::test::bindThreadToCpu(0);

    ASSERT_EQ(size_t{0}, gWorkerFailures.load());

    // A's slot is quiet and holds exactly its Open bag.
    ASSERT_FALSE(rcu::test::inSection(d, kTagA));
    ASSERT_EQ(size_t{1}, rcu::test::openBagResidue(d, kTagA));
    ASSERT_EQ(size_t{1}, rcu::test::totalResidue(d));

    // The sealed batch was destroyed, all of it, on B's thread.
    ASSERT_EQ(kBatch, gDestroyed.load());
    for (size_t i = 0; i < kBatch; ++i) {
        ASSERT_EQ(uint32_t{1}, gRunCount[i].load());
        ASSERT_EQ(kTagB, gExecutor[i].load());
    }
    // ...and the final retiree, in A's unsealed Open bag, was NOT — a promise the
    // design explicitly declines to make.
    ASSERT_EQ(uint32_t{0}, gRunCount[kBatch].load());

    // Only the teardown drain can force-seal a remote Open bag.
    ASSERT_EQ(size_t{1}, rcu::test::drainAllQuiescent(d));
    assertNothingLostOrDoubled(kBatch + 1);
    td.finish();
    teardownAccounting();
}

// ============================================================================
// A dead slot must not make limbo grow with churn volume
// ============================================================================
//
// The quiet-system scenario above proves the residue bound EXACTLY, but at 11
// objects and with no concurrent load — so it cannot distinguish "a dead slot
// contributes O(1)" from "a dead slot contributes O(retires)". This one can:
// one thread retires a batch and dies, then THOUSANDS of retires happen on the
// remaining slots, and the surviving residue must still be a few open bags.
//
// The property under test is R3. A thread quiet OUTSIDE a section has an
// inactive slot, so it is invisible to the scan and cannot block advancement;
// its sealed bags are stealable (RCU-DEC-006); its Open bag is a fixed one-time
// residue (I13) that cannot grow, because the thread is not retiring any more.
// Contribution is therefore O(1) per dead slot, not O(retires).
//
// If that is ever false — if a dead slot's state could block an advance — the
// epoch freezes, nothing expires, and residue tracks total churn. The two
// assertions that catch it are the epoch having moved at all, and residue being
// bounded by a constant while retires are three orders of magnitude larger.
//
// NOT the same as a thread quiet INSIDE a section, which blocks advancement by
// design and is the accepted EBR weakness — that is rcuTortureForcedStall.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureDeadSlotDoesNotUnboundLimbo, 90000) {
    Harness h;
    TortureDomain td("torture-dead-slot");
    auto& d = *td;

    constexpr size_t   kDeadSlot    = 3;
    constexpr size_t   kDeadBatch   = 50;
    constexpr size_t   kChurnPerCpu = 5000;
    constexpr size_t   kChurners    = kCpus - 1;          // slots 0..2
    constexpr size_t   kTotal       = kChurners * kChurnPerCpu + kDeadBatch;
    // O(slots), NOT O(retires) — each slot contributes at most its own Open bag.
    // Deliberately generous: the discriminating statement is that kTotal retires
    // leave under kResidueBound behind, a ratio of well under 1%. A build where a
    // dead slot blocked advancement would leave residue in the thousands.
    constexpr size_t   kResidueBound = 8 * Core::rcu::kRetireAdvanceThreshold;   // 512

    resetAccounting(kTotal + 1024);
    resetFailures();

    // ── Phase 1: one slot retires a batch, then dies ──
    std::thread dead([&] {
        worker(static_cast<uint32_t>(kDeadSlot), [&] {
            for (size_t i = 0; i < kDeadBatch; ++i) {
                Payload* p = makePayload(0);
                rcu::ReadGuard g(d);
                rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
            }
        });
    });
    dead.join();          // joined BEFORE any churn starts, so the slot is
                          // provably dead for the whole of phase 2

    kernel::test::bindThreadToCpu(0);
    const uint64_t epochBefore   = rcu::test::epoch(d);
    const size_t   deadResidue0  = rcu::test::openBagResidue(d, kDeadSlot);

    // ── Phase 2: sustained churn on the surviving slots only ──
    std::vector<std::thread> threads;
    for (size_t c = 0; c < kChurners; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t i = 0; i < kChurnPerCpu; ++i) {
                    Payload* p = makePayload(0);
                    {
                        rcu::ReadGuard g(d);
                        rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
                    }
                    if ((i & 15) == 0) (void)rcu::tryAdvance(d);
                }
            });
        });
    }
    for (auto& t : threads) t.join();
    kernel::test::bindThreadToCpu(0);

    ASSERT_EQ(size_t{0}, gWorkerFailures.load());

    // Everything is joined, so the domain is quiescent and introspection is a
    // measurement rather than a torn view. Drive to a fixed point so that what
    // remains is only what is structurally unreachable.
    for (size_t i = 0; i < 64; ++i) (void)rcu::tryAdvance(d);

    const uint64_t epochAfter = rcu::test::epoch(d);
    const size_t   residue    = rcu::test::totalResidue(d);

    // (1) The dead slot did not freeze the epoch.
    ASSERT_GT(epochAfter, epochBefore + 1);

    // (2) Residue is O(slots), not O(retires). THE assertion of this scenario.
    ASSERT_GT(kTotal, kResidueBound * 4);      // the test is only meaningful if
                                               // churn dwarfs the bound
    ASSERT_TRUE(residue <= kResidueBound);

    // (3) ...and it is exactly the per-slot Open bags — everything stealable was
    // stolen, including the dead slot's sealed bags (RCU-DEC-006).
    size_t openFloor = 0;
    for (size_t s = 0; s < kCpus; ++s) openFloor += rcu::test::openBagResidue(d, s);
    ASSERT_EQ(openFloor, residue);

    // (4) The dead slot's own contribution cannot have grown past what it
    // retired before dying, and it never re-entered.
    ASSERT_FALSE(rcu::test::inSection(d, kDeadSlot));
    ASSERT_TRUE(rcu::test::openBagResidue(d, kDeadSlot) <= deadResidue0);

    // (5) So nearly everything was reclaimed WITHOUT the teardown drain.
    ASSERT_EQ(kTotal - residue, gDestroyed.load());

    ASSERT_EQ(residue, rcu::test::drainAllQuiescent(d));
    assertNothingLostOrDoubled(kTotal);
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Deleter-retires — reentrancy into retire from inside a drain
// ============================================================================
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureDeleterRetires, 60000) {
    Harness h;
    TortureDomain td("torture-deleter-retires");
    auto& d = *td;

    constexpr size_t kPerCpu = 200;
    constexpr int    kBudget = 400;

    resetAccounting(kCpus * kPerCpu + kBudget + 256);
    resetFailures();
    gDeleterDomain.store(&d, std::memory_order_release);
    gDeleterRetireBudget.store(kBudget, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t i = 0; i < kPerCpu; ++i) {
                    Payload* p = makePayload(0);
                    {
                        rcu::ReadGuard g(d);
                        // The retiring deleter pushes a fresh object into THIS
                        // slot's Open bag from inside a drain — a different bag
                        // from the Claimed one by construction.
                        rcu::retire<Payload, &Payload::head, retiringDeleter>(d, p);
                    }
                    if ((i & 7) == 0) (void)rcu::tryAdvance(d);
                }
                rcu::barrier(d);
            });
        });
    }
    for (auto& t : threads) t.join();
    kernel::test::bindThreadToCpu(0);

    rcu::test::drainAllQuiescent(d);
    gDeleterDomain.store(nullptr, std::memory_order_release);

    // Every object created — originals plus everything the deleters spawned.
    assertNothingLostOrDoubled(gNextId.load());
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Nested sections, including a simulated interrupt-nested entry
// ============================================================================
//
// The engine's `nesting` is deliberately non-atomic owner-only state (I5). This
// checks the bookkeeping directly rather than inferring it, including the case
// the read side exists to support: an interrupt arriving mid-section and itself
// entering a section on the same CPU.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureNestedSections, 60000) {
    Harness h;
    TortureDomain td("torture-nesting");
    auto& d = *td;

    constexpr size_t kIters = 500;
    resetAccounting(kCpus * kIters + 256);
    resetFailures();

    std::vector<Atomic<Payload*>> cells(kCells);
    for (auto& c : cells) c.store(makeChain(), SEQ_CST);

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t i = 0; i < kIters; ++i) {
                    rcu::ReadGuard outer(d);
                    if (rcu::test::nesting(d, c) != 1) {
                        throw AssertionFailure("outer section did not set nesting to 1");
                    }
                    {
                        rcu::ReadGuard middle(d);
                        if (rcu::test::nesting(d, c) != 2) {
                            throw AssertionFailure("nested section did not set nesting to 2");
                        }
                        {
                            // The case R2 exists for: a #PF arrives mid-section
                            // and its handler enters a section of its own. Legal
                            // in every context except NMI/#MC (RCU-DEC-024).
                            kernel::interrupts::test::ScopedContext pf(
                                &kernel::interrupts::InterruptContextDepths::pf);
                            rcu::ReadGuard interrupt(d);
                            if (rcu::test::nesting(d, c) != 3) {
                                throw AssertionFailure("interrupt-nested section did not reach 3");
                            }
                            // retire is conditionally legal in #PF — the carve-out
                            // RadixVM's fault-path mutations depend on.
                            Payload* p = makePayload(0);
                            rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
                        }
                        if (rcu::test::nesting(d, c) != 2) {
                            throw AssertionFailure("nesting did not fall back to 2");
                        }
                        Payload* q = &(*rcu::protect<Payload>(d, cells[(c + i) % kCells]));
                        if (q->magic != kLiveMagic) {
                            gReadErrors.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (rcu::test::nesting(d, c) != 1) {
                        throw AssertionFailure("nesting did not fall back to 1");
                    }
                }
                // The slot must be fully inactive once every guard has unwound.
                if (rcu::test::inSection(d, c)) {
                    throw AssertionFailure("slot still in a section after all guards exited");
                }
            });
        });
    }
    for (auto& t : threads) t.join();
    kernel::test::bindThreadToCpu(0);

    const size_t retired = kCpus * kIters;
    rcu::test::drainAllQuiescent(d);
    for (auto& c : cells) deleteChainDirect(c.load(SEQ_CST));

    assertNothingLostOrDoubled(retired);
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Batch-bound churn — the Claimed -> Sealed exit under full contention
// ============================================================================
//
// drainBatchBound = 1, so EVERY drain hits the bound mid-bag and re-seals the
// remainder. RCU-DEC-033's re-seal must lose nothing, run nothing twice, and
// release nothing early. Per-id run counters are what make the middle claim
// checkable — a total alone cannot tell one object run twice from two run once.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureBatchBoundChurn, 90000) {
    Harness h;
    TortureDomain td("torture-bound", /*drainBatchBound=*/1);
    auto& d = *td;

    ASSERT_EQ(size_t{1}, rcu::test::drainBatchBound(d));

    constexpr size_t kPerCpu = 600;
    resetAccounting(kCpus * kPerCpu * kChainDepth + kCells * kChainDepth + 1024);
    resetFailures();

    std::vector<Atomic<Payload*>> cells(kCells);
    for (auto& c : cells) c.store(makeChain(), SEQ_CST);

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t i = 0; i < kPerCpu; ++i) {
                    Atomic<Payload*>& cell = cells[(c * 3 + i) % kCells];
                    if ((c & 1) == 0) {
                        Payload* fresh = makeChain();
                        rcu::ReadGuard g(d);
                        Payload* old = cell.exchange(fresh, ACQ_REL);
                        retireChain(d, old);
                    } else {
                        readOneCell(d, cell);
                    }
                    // Sweeping one node at a time is the point: the remainder is
                    // handed from claimer to claimer through kBagReseal.
                    (void)rcu::drain(d);
                    if ((i & 15) == 0) (void)rcu::tryAdvance(d);
                }
            });
        });
    }
    for (auto& t : threads) t.join();
    kernel::test::bindThreadToCpu(0);

    const size_t retired = gNextId.load() - kCells * kChainDepth;
    rcu::test::drainAllQuiescent(d);
    for (auto& c : cells) deleteChainDirect(c.load(SEQ_CST));

    assertNothingLostOrDoubled(retired);
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Barrier semantics (RCU-DEC-031, as amended)
// ============================================================================
//
// The amended contract has three clauses and this checks all three:
//   - every object the CALLER retired before the call is destroyed;
//   - a remote slot's OPEN bag is untouched (I13 — Open -> Sealed is owner-only);
//   - a remote slot's SEALED bags ARE drained (stealing, RCU-DEC-006).
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureBarrierSemantics, 60000) {
    Harness h;
    TortureDomain td("torture-barrier");
    auto& d = *td;

    constexpr size_t   kQuietBatch = 20;
    constexpr size_t   kOwnRetires = 50;
    constexpr uint32_t kTagA       = 0;
    constexpr uint32_t kTagB       = 1;

    resetAccounting(kQuietBatch + kOwnRetires + 128);
    resetFailures();

    // ── B retires, gets a bag sealed, then goes quiet with an Open bag ──
    std::atomic<int> step{0};
    std::thread threadB([&] {
        worker(kTagB, [&] {
            for (size_t i = 0; i < kQuietBatch; ++i) {
                Payload* p = makePayload(0);
                rcu::ReadGuard g(d);
                rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
            }
            step.store(1, std::memory_order_release);
            while (step.load(std::memory_order_acquire) != 2) std::this_thread::yield();
            // Rotate: seals the batch at its own tag, opens a fresh bag for this
            // one object, which is then unreachable to anyone but B.
            Payload* last = makePayload(0);
            rcu::ReadGuard g(d);
            rcu::retire<Payload, &Payload::head, tortureDeleter>(d, last);
        });
    });

    while (step.load(std::memory_order_acquire) != 1) std::this_thread::yield();
    kernel::test::bindThreadToCpu(kTagA);
    tlThreadTag = kTagA;
    for (size_t i = 0; i < 8; ++i) (void)rcu::tryAdvance(d);   // move the epoch past B's tag
    step.store(2, std::memory_order_release);
    threadB.join();

    const size_t bSealed = kQuietBatch;
    const size_t bOpen   = 1;

    // ── A retires its own batch and barriers ──
    kernel::test::bindThreadToCpu(kTagA);
    tlThreadTag = kTagA;
    const size_t firstOwnId = gNextId.load();
    for (size_t i = 0; i < kOwnRetires; ++i) {
        Payload* p = makePayload(0);
        rcu::ReadGuard g(d);
        rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
    }
    rcu::barrier(d);

    // Clause 1: every one of A's own retirees is gone.
    for (size_t i = 0; i < kOwnRetires; ++i) {
        ASSERT_EQ(uint32_t{1}, gRunCount[firstOwnId + i].load());
    }
    // Clause 3: B's sealed bag was drained on the way (barrier sweeps everything
    // claimable, not just its own).
    for (size_t i = 0; i < bSealed; ++i) ASSERT_EQ(uint32_t{1}, gRunCount[i].load());
    // Clause 2: B's Open bag is exactly as B left it.
    ASSERT_EQ(bOpen, rcu::test::openBagResidue(d, kTagB));
    ASSERT_EQ(bOpen, rcu::test::totalResidue(d));
    ASSERT_EQ(uint32_t{0}, gRunCount[bSealed].load());

    ASSERT_EQ(bOpen, rcu::test::drainAllQuiescent(d));
    assertNothingLostOrDoubled(bSealed + bOpen + kOwnRetires);
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Barrier x batch bound — the mid-barrier kBagReseal hand-off
// ============================================================================
//
// RCU-DEC-036's termination argument is bounded-conversion, not monotonicity:
// remote owners keep converting Open <= e0 bags into new members of the wait set
// while the barrier runs. With drainBatchBound = 1 every claim also hands a
// remainder back. If either the hand-off or the termination argument is wrong,
// this hangs and the harness timeout reports it.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureBarrierTimesBound, 90000) {
    Harness h;
    TortureDomain td("torture-barrier-bound", /*drainBatchBound=*/1);
    auto& d = *td;

    constexpr size_t kPerCpu   = 400;
    constexpr size_t kBarriers = 12;

    resetAccounting(kCpus * kPerCpu + kBarriers * 8 + 1024);
    resetFailures();

    std::atomic<bool> churnDone{false};

    std::vector<std::thread> threads;
    for (size_t c = 1; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t i = 0; i < kPerCpu; ++i) {
                    Payload* p = makePayload(0);
                    rcu::ReadGuard g(d);
                    rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
                }
            });
        });
    }

    // Slot 0 barriers repeatedly under that churn. Each call must return.
    std::thread barrierThread([&] {
        worker(0, [&] {
            for (size_t b = 0; b < kBarriers; ++b) {
                Payload* p = makePayload(0);
                // Snapshot the id BEFORE retiring. After the barrier `p` is
                // freed for real (P3-DEC-001), so reading p->id afterwards is
                // itself the use-after-grace-period bug this suite hunts.
                const uint32_t id = p->id;
                {
                    rcu::ReadGuard g(d);
                    rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
                }
                rcu::barrier(d);
                // Own retirees complete unconditionally, mid-churn.
                if (gRunCount[id].load() != 1) {
                    throw AssertionFailure("barrier returned with an own retiree undestroyed");
                }
            }
            churnDone.store(true, std::memory_order_release);
        });
    });

    for (auto& t : threads) t.join();
    barrierThread.join();
    ASSERT_TRUE(churnDone.load());
    kernel::test::bindThreadToCpu(0);

    rcu::test::drainAllQuiescent(d);
    assertNothingLostOrDoubled(gNextId.load());
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Dual barriers — two slots barrier concurrently
// ============================================================================
//
// Should be impossible to deadlock: claims are never held while waiting. This is
// the test that says so out loud. Both calls must return; the harness timeout is
// the detector if they do not.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureDualBarriers, 90000) {
    Harness h;
    TortureDomain td("torture-dual-barrier");
    auto& d = *td;

    constexpr size_t kRounds      = 20;
    constexpr size_t kPerRound    = 40;
    // The churners are BOUNDED as well as stop-flagged. A bare `while (!stop)`
    // allocates for as long as the barriers take, which is a fixed cost under
    // ASan and a ~10x one under TSan — so the id space a bare loop needs is not
    // a property of the test but of the sanitizer. Bounding it makes the
    // accounting capacity exact. Running out early only weakens the churn; it
    // cannot hang the barriers.
    constexpr size_t kMaxChurnRounds = 3000;

    resetAccounting(2 * kRounds * kPerRound + 2 * kMaxChurnRounds * kPerRound + 1024);
    resetFailures();

    std::atomic<size_t> returned{0};
    std::atomic<bool>   churnStop{false};

    // Two churners keep the wait set moving under both barriers.
    std::vector<std::thread> threads;
    for (size_t c = 2; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t r = 0; r < kMaxChurnRounds &&
                                   !churnStop.load(std::memory_order_acquire); ++r) {
                    for (size_t i = 0; i < kPerRound; ++i) {
                        Payload* p = makePayload(0);
                        rcu::ReadGuard g(d);
                        rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
                    }
                    (void)rcu::tryAdvance(d);
                }
            });
        });
    }

    for (size_t b = 0; b < 2; ++b) {
        threads.emplace_back([&, b] {
            worker(static_cast<uint32_t>(b), [&] {
                for (size_t r = 0; r < kRounds; ++r) {
                    for (size_t i = 0; i < kPerRound; ++i) {
                        Payload* p = makePayload(0);
                        rcu::ReadGuard g(d);
                        rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
                    }
                    rcu::barrier(d);
                    returned.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        });
    }

    // Wait for both barrier threads to finish all their rounds, then stop churn.
    while (returned.load(std::memory_order_acquire) < 2 * kRounds) {
        std::this_thread::yield();
    }
    churnStop.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    kernel::test::bindThreadToCpu(0);

    ASSERT_EQ(2 * kRounds, returned.load());
    rcu::test::drainAllQuiescent(d);
    assertNothingLostOrDoubled(gNextId.load());
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Deleter-retires DURING a barrier
// ============================================================================
//
// RCU-DEC-036's rotate is mandatory rather than tidiness: after a bare seal,
// openBagIndex would designate a Sealed bag and a deleter-retire during the
// barrier's own sweeps would push into it — the I11-fatal case. This drives
// exactly that: the barrier's sweeps run deleters that retire fresh objects.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureDeleterRetiresDuringBarrier, 90000) {
    Harness h;
    TortureDomain td("torture-barrier-reentry");
    auto& d = *td;

    constexpr size_t kRounds  = 10;
    constexpr size_t kPerCpu  = 120;
    constexpr int    kBudget  = 600;

    resetAccounting(kCpus * kRounds * kPerCpu + kBudget + 1024);
    resetFailures();
    gDeleterDomain.store(&d, std::memory_order_release);
    gDeleterRetireBudget.store(kBudget, std::memory_order_relaxed);

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t r = 0; r < kRounds; ++r) {
                    for (size_t i = 0; i < kPerCpu; ++i) {
                        Payload* p = makePayload(0);
                        rcu::ReadGuard g(d);
                        rcu::retire<Payload, &Payload::head, retiringDeleter>(d, p);
                    }
                    // Every slot barriers, so every slot's sweeps run retiring
                    // deleters while its own barrier is in flight.
                    rcu::barrier(d);
                }
            });
        });
    }
    for (auto& t : threads) t.join();
    kernel::test::bindThreadToCpu(0);

    rcu::test::drainAllQuiescent(d);
    gDeleterDomain.store(nullptr, std::memory_order_release);

    assertNothingLostOrDoubled(gNextId.load());
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Contended teardown — drainAllQuiescent over realistic debris
// ============================================================================
//
// Phase 1's teardown test drains a quiet domain. This one drains a domain left
// in the state a real workload leaves: remote Open bags on every slot, sealed
// bags at several tags, and deleters that retire more objects mid-drain.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureContendedTeardown, 90000) {
    Harness h;
    TortureDomain td("torture-teardown");
    auto& d = *td;

    constexpr size_t kPerCpu = 500;
    constexpr int    kBudget = 500;

    resetAccounting(kCpus * kPerCpu * kChainDepth + kBudget + kCells * kChainDepth + 1024);
    resetFailures();
    gDeleterDomain.store(&d, std::memory_order_release);
    gDeleterRetireBudget.store(kBudget, std::memory_order_relaxed);

    std::vector<Atomic<Payload*>> cells(kCells);
    for (auto& c : cells) c.store(makeChain(), SEQ_CST);

    std::vector<std::thread> threads;
    for (size_t c = 0; c < kCpus; ++c) {
        threads.emplace_back([&, c] {
            worker(static_cast<uint32_t>(c), [&] {
                for (size_t i = 0; i < kPerCpu; ++i) {
                    Atomic<Payload*>& cell = cells[(c * 7 + i) % kCells];
                    Payload* fresh = makeChain();
                    {
                        rcu::ReadGuard g(d);
                        Payload* old = cell.exchange(fresh, ACQ_REL);
                        retireChain(d, old);
                    }
                    readOneCell(d, cell);
                    if ((i & 63) == 0) (void)rcu::tryAdvance(d);
                }
                // Deliberately NO barrier and NO final advance: every slot is
                // left with a populated Open bag and whatever sealed bags the
                // rotation happened to leave. That is the debris under test.
                Payload* straggler = makePayload(0);
                rcu::ReadGuard g(d);
                rcu::retire<Payload, &Payload::head, retiringDeleter>(d, straggler);
            });
        });
    }
    for (auto& t : threads) t.join();
    kernel::test::bindThreadToCpu(0);

    // Every slot should be holding something the owner alone could seal.
    size_t openFloor = 0;
    for (size_t s = 0; s < kCpus; ++s) openFloor += rcu::test::openBagResidue(d, s);
    ASSERT_GT(openFloor, size_t{0});

    // The join above is the happens-before edge RCU-DEC-035's precondition needs.
    const size_t before = gDestroyed.load();
    const size_t ran    = rcu::test::drainAllQuiescent(d);
    gDeleterDomain.store(nullptr, std::memory_order_release);

    ASSERT_EQ(ran, gDestroyed.load() - before);
    for (auto& c : cells) deleteChainDirect(c.load(SEQ_CST));

    assertNothingLostOrDoubled(gNextId.load() - kCells * kChainDepth);
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    td.finish();
    teardownAccounting();
}

// ============================================================================
// Failed-scenario survival (P3-I5's failure path)
// ============================================================================
//
// A scenario is forced to fail inside a worker thread. What is under test is not
// RCU at all — it is the harness discipline this whole file depends on: that the
// assertion is caught rather than reaching std::terminate, that the summary
// survives, and that the wrecked domain is abandoned rather than drained.
//
// If this test is deleted or weakened, every other scenario in the file silently
// loses its ability to report a failure instead of killing the process.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureFailedScenarioSurvival, 60000) {
    Harness h;
    resetAccounting(256);
    resetFailures();

    const size_t leakedBefore = gLeakedDomains.size();

    {
        TortureDomain td("torture-failure-path");
        auto& d = *td;

        std::thread t([&] {
            worker(1, [&] {
                Payload* p = makePayload(0);
                {
                    rcu::ReadGuard g(d);
                    rcu::retire<Payload, &Payload::head, tortureDeleter>(d, p);
                }
                // The injected failure. In a real scenario this would be an
                // ASSERT_* from the engine or the test body.
                throw AssertionFailure("injected torture failure");
            });
        });
        t.join();

        // The mechanism worked: recorded, not terminated.
        ASSERT_EQ(size_t{1}, gWorkerFailures.load());
        ASSERT_TRUE(firstFailureContains("injected torture failure"));

        // ...and the scenario does NOT call finish(). ~TortureDomain abandons the
        // domain, leaving its retiree in limbo on purpose: draining a wrecked
        // domain throws from the epilogue, and that throw during unwinding is
        // std::terminate.
    }

    ASSERT_EQ(leakedBefore + 1, gLeakedDomains.size());
    ASSERT_EQ(size_t{0}, gDestroyed.load());   // teardown was skipped, as designed

    // The process is alive to run this line. That is the assertion.
    resetFailures();
    teardownAccounting();
}

// ============================================================================
// Escaped SafePtr — OPT-IN, because a pass means the process dies
// ============================================================================
//
// The parent Hazards ask for an oracle for the escape bug class: a reader that
// keeps a protected pointer past readUnlock and dereferences it after the grace
// period. Under the ASan runner that is a hard use-after-free trap — which is
// the detector WORKING, and also an aborted process with no test summary.
//
// So it cannot run by default; a suite that always aborts is not a release gate.
// It is compiled unconditionally (so it cannot rot) and armed only by
// CROCOS_RCU_ESCAPE_DEMO=1 in the environment:
//
//     CROCOS_RCU_ESCAPE_DEMO=1 tests/build/kernel/rcu/KernelRcuIntegrationTestRunner
//
// Expected armed behaviour: ASan reports a heap-use-after-free with both the
// allocation and the free stack, and the process aborts. Anything else — a clean
// read, a silent pass — means the oracle is not wired up and P3-DEC-001 has been
// defeated somewhere.
TEST_WITH_TIMEOUT_NO_TRACKING(rcuTortureEscapedSafePtr, 60000) {
    const char* arm = getenv("CROCOS_RCU_ESCAPE_DEMO");
    if (arm == nullptr || arm[0] != '1') {
        // Not a silent skip: assert the guard itself, so the test is visibly
        // present and visibly disarmed in every ordinary run.
        ASSERT_TRUE(getenv("CROCOS_RCU_ESCAPE_DEMO") == nullptr || arm[0] != '1');
        return;
    }

    Harness h;
    TortureDomain td("torture-escape");
    auto& d = *td;

    resetAccounting(256);
    resetFailures();

    Atomic<Payload*> cell{makePayload(0)};

    // The bug, deliberately written: the pointer outlives the section.
    Payload* escaped = nullptr;
    {
        rcu::ReadGuard g(d);
        escaped = &(*rcu::protect<Payload>(d, cell));
        Payload* old = cell.exchange(makePayload(0), ACQ_REL);
        rcu::retire<Payload, &Payload::head, tortureDeleter>(d, old);
    }

    // Force the grace period to complete, which really frees `escaped`.
    rcu::barrier(d);
    ASSERT_EQ(size_t{1}, gDestroyed.load());

    // ASan traps HERE. Reaching the line after it is the failure.
    volatile uint64_t observed = escaped->magic;
    (void)observed;
    ASSERT_TRUE(false && "escaped SafePtr dereference did not trap — the UAGP oracle is broken");

    deleteChainDirect(cell.load(SEQ_CST));
    td.finish();
    teardownAccounting();
}
