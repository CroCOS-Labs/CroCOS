//
// RCU endurance + performance soak. NOT part of the correctness gate.
//
// This is deliberately NOT in run_all_tests and NOT armed by default: it runs
// for minutes, and a minutes-long test in the default gate is a gate people stop
// running. It is registered as an ordinary test that SKIPS unless armed, so it
// stays visible and cannot rot.
//
//     cmake --build tests/build --target run_rcu_soak        # ASan, 60s, 8 threads
//     cmake --build tests/build --target run_rcu_soak_tsan   # TSan, 60s, 8 threads
//
//     # or drive it directly
//     CROCOS_RCU_SOAK_SECONDS=120 CROCOS_RCU_SOAK_THREADS=8 \
//         tests/build/kernel/rcu/KernelRcuIntegrationTestRunnerTSan rcuSoak
//
// Env knobs: CROCOS_RCU_SOAK_SECONDS (0 = disarmed), CROCOS_RCU_SOAK_THREADS
// (4..8), CROCOS_RCU_SOAK_QUIET_PCT (chance a thread naps between bursts).
//
// ─── What it measures, and what those numbers are worth ───────────────────
//
// 1. PEAK LIMBO OCCUPANCY — the memory overhead question. Reported as a live
//    object count and bytes: how much memory the framework is sitting on
//    because grace periods have not yet expired.
//
//    It is measured at the TEST's accounting layer (a counter incremented before
//    retire, decremented in the deleter), NOT by sampling the engine. Sampling
//    DebugIntrospection::totalResidue on a live domain is not merely imprecise,
//    it is unsound: walking a bag's node list races the drainer holding it
//    Claimed, so under ASan it can read freed nodes and under TSan it is a true
//    positive. Both headers say snapshots assume a quiescent domain. The
//    test-side counter measures the same quantity — retired-but-not-yet-destroyed
//    — and is race-free.
//
// 2. READ / UPDATE THROUGHPUT AND LATENCY. Throughput is uninstrumented
//    (total ops / wall clock). Latency comes from a 1-in-kLatencySample
//    sampled pair of clock reads, bucketed by log2(ns).
//
//    UNDER A SANITIZER THESE ARE NOT PERFORMANCE NUMBERS. ASan instruments every
//    load and store; TSan is worse and also serialises through its shadow memory.
//    Treat them as a relative signal and a regression tripwire. Real numbers need
//    an uninstrumented -O2 build — see the note at the bottom of this file.
//
// ─── Why the threads nap ──────────────────────────────────────────────────
//
// A thread that stops touching the domain entirely is the RCU-DEC-006 quiet-CPU
// case: an idle kernel CPU makes zero RCU calls, so its slot is invisible to
// grace periods but its sealed bags still need stealing and its Open bag becomes
// residue no one else can reach (I13 / ITEM-014). Naps are taken OUTSIDE any
// section — a thread stalling INSIDE one would just block every advance, which is
// the forced-stall scenario in TortureTest.cpp and a different question.
//
// The soak is self-contained rather than sharing TortureTest.cpp's helpers: that
// file's per-object run counters are exactly what does not scale here (at soak
// volumes the counter arrays cost more than the limbo they measure), so the
// accounting is aggregate and double-frees are ASan's job.
//

#include "../../test.h"
#include <harness/TestHarness.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include <arch.h>
#include <core/atomic.h>
#include <mem/NUMA.h>          // mock
#include <mem/VMSubstrate.h>   // mock
#include <rcu/RCU.h>           // real
#include "MockCpuLocal.h"
#include "MockRcuEnv.h"
#include "DebugIntrospection.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace CroCOSTest;
namespace VS   = kernel::mm::VMSubstrate;
namespace numa = kernel::numa;
namespace rcu  = kernel::rcu;

namespace {

using Clock = std::chrono::steady_clock;
using Ns    = std::chrono::nanoseconds;

// Which sanitizer we were built with, for the report header. Clang answers
// through __has_feature; the __SANITIZE_* macros are GCC's spelling and are
// kept as the fallback so this stays correct if the harness compiler changes.
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CROCOS_SOAK_SAN "TSan"
#  elif __has_feature(address_sanitizer)
#    define CROCOS_SOAK_SAN "ASan"
#  endif
#endif
#ifndef CROCOS_SOAK_SAN
#  if defined(__SANITIZE_THREAD__)
#    define CROCOS_SOAK_SAN "TSan"
#  elif defined(__SANITIZE_ADDRESS__)
#    define CROCOS_SOAK_SAN "ASan"
#  else
#    define CROCOS_SOAK_SAN "none (uninstrumented — these numbers are real)"
#  endif
#endif
constexpr const char* kSanitizerName = CROCOS_SOAK_SAN;

constexpr size_t   kCells          = 64;    // enough to spread cell contention
constexpr size_t   kChainDepth     = 3;     // multi-node traversal per section
constexpr uint64_t kLatencySample  = 256;   // 1-in-N ops carry a timed pair
constexpr uint64_t kSamplePeriodUs = 500;   // limbo sampler cadence

// Histogram: 4 sub-buckets per octave (~19% resolution) rather than plain log2.
// Plain octaves are useless at these magnitudes — a read section and its p90 land
// in the same power-of-two bin, so the distribution reads as a single spike.
constexpr size_t kSubBits     = 2;
constexpr size_t kSubs        = size_t{1} << kSubBits;
constexpr size_t kHistBuckets = 32 * kSubs;

// ─── Env knobs ─────────────────────────────────────────────────────────────

uint64_t envU64(const char* name, uint64_t fallback) {
    const char* v = getenv(name);
    if (v == nullptr || v[0] == '\0') return fallback;
    char* end = nullptr;
    const unsigned long long parsed = strtoull(v, &end, 10);
    if (end == v) return fallback;
    return static_cast<uint64_t>(parsed);
}

// ─── Payload ───────────────────────────────────────────────────────────────

constexpr uint64_t kLiveMagic = 0x534F414B4C495645ull;   // "SOAKLIVE"

struct Payload {
    uint64_t              magic     = kLiveMagic;
    Core::rcu::RetireHead head      = {};
    Payload*              chainNext = nullptr;
    uint64_t              version   = 0;
    uint64_t              filler[4] = {};
    uint64_t              checksum  = 0;
};

uint64_t checksumOf(const Payload& p) { return kLiveMagic ^ (p.version * 0x9E3779B97F4A7C15ull); }

// ─── Payload storage ───────────────────────────────────────────────────────
//
// Profiling put the test's own new/delete at ~13% of on-CPU work — noise that
// drowns the RCU signal it is supposed to be measuring. CROCOS_SOAK_POOLED
// swaps in a per-thread free list, and is set for the PERF RUNNER ONLY.
//
// It must never be set for the ASan or TSan runners: P3-DEC-001 turns on the
// deleter really calling free(), which is what makes a use-after-grace-period a
// hard trap instead of a silent read of recycled bytes. A pool defeats exactly
// that. The perf runner already has no sanitizer behind its assertions, so it
// gives up nothing it still had.
//
// Objects are allocated on one thread and destroyed on whichever thread steals
// the bag (RCU-DEC-006), so a per-thread list is a cache, not an owner —
// hence the cap and the fall-through to the real allocator.

#ifdef CROCOS_SOAK_POOLED
constexpr size_t kPoolCap = 8192;

// Recycling is by assignment, not destroy-and-placement-new: Payload is all
// scalars, so `*p = Payload{}` is a well-defined reset and the object's lifetime
// never ends. That avoids writing through a pointer to a destroyed object to
// thread the free list, which is the usual way this pattern acquires UB.
struct Pool {
    Payload* head = nullptr;
    size_t   n    = 0;
    ~Pool() { while (head) { Payload* nx = head->chainNext; delete head; head = nx; } }
};
thread_local Pool tlPool;

Payload* poolAlloc() {
    if (tlPool.head != nullptr) {
        Payload* p  = tlPool.head;
        tlPool.head = p->chainNext;
        --tlPool.n;
        *p = Payload{};
        return p;
    }
    return new Payload();
}

void poolFree(Payload* p) {
    if (tlPool.n >= kPoolCap) { delete p; return; }
    p->chainNext = tlPool.head;
    tlPool.head  = p;
    ++tlPool.n;
}
#else
inline Payload* poolAlloc()          { return new Payload(); }
inline void     poolFree(Payload* p) { delete p; }
#endif

// ─── Aggregate accounting ──────────────────────────────────────────────────
//
// No per-object arrays: at soak volumes they would dwarf the limbo they measure.
// Double-free detection is delegated to ASan, which traps a real double delete
// with both stacks attached — a strictly better report than a counter.

std::atomic<int64_t> gLive{0};        // retired but not yet destroyed
std::atomic<size_t>  gRetired{0};
std::atomic<size_t>  gDestroyed{0};
std::atomic<size_t>  gCorrupt{0};
std::atomic<size_t>  gReadErrors{0};

void soakDeleter(Payload* p) {
    if (p->magic != kLiveMagic || p->checksum != checksumOf(*p)) {
        gCorrupt.fetch_add(1, std::memory_order_relaxed);
    }
    p->magic = 0;
    gDestroyed.fetch_add(1, std::memory_order_relaxed);
    gLive.fetch_sub(1, std::memory_order_relaxed);
    poolFree(p);                     // real free() unless CROCOS_SOAK_POOLED
}

// ─── Per-thread stats, one cache line apart ────────────────────────────────

struct alignas(128) ThreadStats {
    uint64_t reads          = 0;
    uint64_t updates        = 0;
    uint64_t retires        = 0;
    uint64_t advances       = 0;
    uint64_t naps           = 0;
    uint64_t napNs          = 0;
    uint64_t readHist[kHistBuckets]   = {};
    uint64_t updateHist[kHistBuckets] = {};
    uint64_t readSamples    = 0;
    uint64_t updateSamples  = 0;
    // Batch timing: amortises the Clock::now() pair over kBatchOps sections so
    // the read path can be resolved BELOW the clock tick. The sampled histogram
    // above cannot — once the fix to the mock clock landed, a read section came
    // in at 48 ns against a 41 ns tick, i.e. at the instrument's floor.
    uint64_t guardBatchNs   = 0;   // ReadGuard ctor+dtor only
    uint64_t guardBatchOps  = 0;
    uint64_t guardBatchMin  = ~uint64_t{0};   // best observed ps/op (see kPicoScale)
    uint64_t fullBatchNs    = 0;   // guard + protect + chain walk
    uint64_t fullBatchOps   = 0;
    uint64_t fullBatchMin   = ~uint64_t{0};
};

// Batch means land in the tens of nanoseconds, so integer ns/op would quantise
// the answer to ~2 significant figures. Work in picoseconds.
constexpr uint64_t kPicoScale = 1000;
constexpr uint64_t kBatchOps  = 8192;
constexpr uint64_t kBatchEvery = 32;      // one timed batch per N bursts

// Consumes a value the optimiser would otherwise use to delete the work.
volatile uint64_t gSink = 0;

// Deterministic per-thread PRNG. std::random_device would make a soak failure
// unreproducible, which is the one thing a soak failure must not be.
struct Xorshift {
    uint64_t s;
    explicit Xorshift(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }
};

size_t bucketOf(uint64_t v) {
    if (v < kSubs) return static_cast<size_t>(v);
    const size_t e = 63 - static_cast<size_t>(__builtin_clzll(v));   // floor(log2 v)
    const size_t m = static_cast<size_t>(v >> (e - kSubBits)) & (kSubs - 1);
    const size_t b = ((e - kSubBits + 1) << kSubBits) + m;
    return b < kHistBuckets ? b : kHistBuckets - 1;
}

uint64_t bucketUpperBound(size_t b) {
    if (b < kSubs) return b;
    const size_t e = (b >> kSubBits) + kSubBits - 1;
    const size_t m = b & (kSubs - 1);
    return (static_cast<uint64_t>(kSubs + m + 1)) << (e - kSubBits);
}

// Percentile from the histogram, reported as the bucket's UPPER bound — so
// "at or below N", bucket granularity rather than false precision.
uint64_t percentile(const uint64_t* hist, uint64_t total, double q) {
    if (total == 0) return 0;
    const uint64_t target = static_cast<uint64_t>(static_cast<double>(total) * q);
    uint64_t seen = 0;
    for (size_t b = 0; b < kHistBuckets; ++b) {
        seen += hist[b];
        if (seen >= target) return bucketUpperBound(b);
    }
    return bucketUpperBound(kHistBuckets - 1);
}

// ─── Worker-thread exception discipline (P3-DEC-004) ───────────────────────

std::atomic<size_t> gFailures{0};
std::mutex          gFailureMutex;
std::string         gFirstFailure;

void recordFailure(const char* what) {
    {
        std::lock_guard<std::mutex> lock(gFailureMutex);
        if (gFirstFailure.empty()) gFirstFailure = what;
    }
    gFailures.fetch_add(1, std::memory_order_relaxed);
}

numa::DomainID mapAllToZero(arch::ProcessorID) { return numa::DomainID{0}; }

}   // namespace

// ============================================================================
// The soak
// ============================================================================

TEST_WITH_TIMEOUT_NO_TRACKING(rcuSoak, 1800000) {
    const uint64_t seconds  = envU64("CROCOS_RCU_SOAK_SECONDS", 0);
    if (seconds == 0) {
        // Disarmed: visibly present, visibly off. Not a silent skip.
        printf("  [rcuSoak] disarmed — set CROCOS_RCU_SOAK_SECONDS to run "
               "(or use the run_rcu_soak / run_rcu_soak_tsan targets)\n");
        ASSERT_EQ(uint64_t{0}, seconds);
        return;
    }

    size_t threads = static_cast<size_t>(envU64("CROCOS_RCU_SOAK_THREADS", 8));
    if (threads < 4) threads = 4;
    if (threads > 8) threads = 8;
    const uint64_t quietPct = envU64("CROCOS_RCU_SOAK_QUIET_PCT", 12);

    // ── Harness ──
    VS::test::initialize(threads, 1);
    numa::test::configure(threads, 1, &mapAllToZero);
    arch::test::setProcessorCount(threads);
    kernel::test::bindThreadToCpu(0);
    kernel::timing::test::resetMonoTime();
    // See MockRcuEnv.cpp: the shared auto-advancing counter would make every
    // section look like a 100 ms stall and funnel all threads through
    // AtomicPrintStream's process-wide spinlock.
    kernel::timing::test::setMonoStep(0);

    gLive.store(0); gRetired.store(0); gDestroyed.store(0);
    gCorrupt.store(0); gReadErrors.store(0); gFailures.store(0);
    { std::lock_guard<std::mutex> lock(gFailureMutex); gFirstFailure.clear(); }

    // Heap-allocated and destroyed only on the success path (P3-I5 / P1-DEC-012).
    auto* domain = new rcu::Domain();
    ASSERT_TRUE(domain->init("soak"));
    auto& d = *domain;

    std::vector<Atomic<Payload*>> cells(kCells);
    auto makeChain = [](uint64_t version) {
        Payload* headNode = nullptr;
        for (size_t i = 0; i < kChainDepth; ++i) {
            auto* n = poolAlloc();
            n->version  = version + i;
            n->checksum = checksumOf(*n);
            n->chainNext = headNode;
            headNode = n;
        }
        return headNode;
    };
    for (size_t i = 0; i < kCells; ++i) cells[i].store(makeChain(i), SEQ_CST);

    std::vector<ThreadStats> stats(threads);
    std::atomic<bool> stop{false};

    // ── Limbo sampler ──
    //
    // A separate thread that touches NO RCU state — it only reads the test's own
    // counter, so it needs no slot and cannot perturb the protocol.
    int64_t  peakLive = 0;
    int64_t  sumLive  = 0;
    uint64_t samples  = 0;
    // Peak alone is a single unlucky instant and the mean hides the tail; the
    // distribution is what says how much memory the framework actually wants
    // steady-state versus in a burst.
    std::vector<uint64_t> liveHist(kHistBuckets, 0);
    std::thread sampler([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const int64_t live = gLive.load(std::memory_order_relaxed);
            if (live > peakLive) peakLive = live;
            sumLive += live;
            ++samples;
            ++liveHist[bucketOf(static_cast<uint64_t>(live < 0 ? 0 : live))];
            std::this_thread::sleep_for(std::chrono::microseconds(kSamplePeriodUs));
        }
    });

    // Calibrate the sampling instrument before using it. Every sampled op is
    // bracketed by two Clock::now() calls, and on an uninstrumented build a read
    // section costs the same order as that pair — so without this number the
    // reader cannot tell how much of the reported latency is the measurement.
    // Reported, never subtracted: the raw bucket stays honest.
    uint64_t clockP50 = 0, clockP90 = 0, clockResNs = 0;
    {
        constexpr int kCal = 50000;
        std::vector<uint64_t> calHist(kHistBuckets, 0);
        uint64_t minNonZero = ~uint64_t{0};
        for (int i = 0; i < kCal; ++i) {
            const auto a = Clock::now();
            const auto b = Clock::now();
            const uint64_t dt = static_cast<uint64_t>(
                std::chrono::duration_cast<Ns>(b - a).count());
            ++calHist[bucketOf(dt)];
            if (dt > 0 && dt < minNonZero) minNonZero = dt;
        }
        clockP50 = percentile(calHist.data(), kCal, 0.50);
        clockP90 = percentile(calHist.data(), kCal, 0.90);
        // Smallest nonzero delta between back-to-back reads: the clock's
        // effective tick. On Apple Silicon this is ~41 ns, coarser than the call
        // pair itself — which is why a median of 0 does NOT mean "free", it means
        // both reads usually land in the same tick. Every latency figure below is
        // quantised to this, so it is the number that bounds their precision.
        clockResNs = (minNonZero == ~uint64_t{0}) ? 0 : minNonZero;
    }

    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);

    std::vector<std::thread> workers;
    for (size_t c = 0; c < threads; ++c) {
        workers.emplace_back([&, c] {
            kernel::test::bindThreadToCpu(static_cast<arch::ProcessorID>(c));
            ThreadStats& st = stats[c];
            Xorshift rng(0xA5A5'0000ull + c);
            uint64_t op = 0;
            try {
                while (Clock::now() < deadline && !stop.load(std::memory_order_acquire)) {
                    // A burst of work, then maybe a nap. The burst length is
                    // randomised so threads drift out of phase with each other.
                    const uint64_t burst = 256 + (rng.next() % 1024);
                    for (uint64_t i = 0; i < burst; ++i) {
                        const uint64_t r = rng.next();
                        Atomic<Payload*>& cell = cells[r % kCells];
                        const bool sampled = (++op % kLatencySample) == 0;

                        if ((r >> 8) % 8 == 0) {
                            // ── UPDATE: publish a fresh chain, retire the old ──
                            Payload* fresh = makeChain(r);
                            const auto t0 = sampled ? Clock::now() : Clock::time_point{};
                            {
                                rcu::ReadGuard g(d);
                                Payload* old = cell.exchange(fresh, ACQ_REL);
                                for (Payload* p = old; p != nullptr;) {
                                    Payload* const next = p->chainNext;
                                    gLive.fetch_add(1, std::memory_order_relaxed);
                                    gRetired.fetch_add(1, std::memory_order_relaxed);
                                    rcu::retire<Payload, &Payload::head, soakDeleter>(d, p);
                                    ++st.retires;
                                    p = next;
                                }
                            }
                            if (sampled) {
                                const uint64_t ns = static_cast<uint64_t>(
                                    std::chrono::duration_cast<Ns>(Clock::now() - t0).count());
                                ++st.updateHist[bucketOf(ns)];
                                ++st.updateSamples;
                            }
                            ++st.updates;
                        } else {
                            // ── READ: one section, walk the whole chain ──
                            const auto t0 = sampled ? Clock::now() : Clock::time_point{};
                            {
                                rcu::ReadGuard g(d);
                                VS::SafePtr<Payload> sp = rcu::protect<Payload>(d, cell);
                                Payload* p = &(*sp);
                                for (size_t step = 0; step < kChainDepth && p != nullptr; ++step) {
                                    if (p->magic != kLiveMagic ||
                                        p->checksum != checksumOf(*p)) {
                                        gReadErrors.fetch_add(1, std::memory_order_relaxed);
                                        break;
                                    }
                                    p = p->chainNext;
                                }
                            }
                            if (sampled) {
                                const uint64_t ns = static_cast<uint64_t>(
                                    std::chrono::duration_cast<Ns>(Clock::now() - t0).count());
                                ++st.readHist[bucketOf(ns)];
                                ++st.readSamples;
                            }
                            ++st.reads;
                        }

                        if ((r >> 16) % 64 == 0) { (void)rcu::tryAdvance(d); ++st.advances; }
                    }

                    // ── Batch-timed measurement of the read path ──
                    //
                    // Two batches, so the RCU section cost is separated from the
                    // traversal it protects. Both run UNDER LOAD — every other
                    // thread is still churning — which is the number that
                    // matters; an idle-machine microbenchmark would flatter the
                    // protocol by removing the cache-line traffic its scan and
                    // its activation store actually contend for.
                    //
                    // ReadGuard's ctor/dtor are out-of-line calls into RCU.cpp,
                    // so the empty-scope batch cannot be optimised away.
                    if ((rng.next() % kBatchEvery) == 0) {
                        Atomic<Payload*>& cell = cells[rng.next() % kCells];

                        const auto g0 = Clock::now();
                        for (uint64_t k = 0; k < kBatchOps; ++k) {
                            rcu::ReadGuard g(d);
                        }
                        const uint64_t gNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<Ns>(Clock::now() - g0).count());
                        st.guardBatchNs += gNs; st.guardBatchOps += kBatchOps;
                        const uint64_t gPer = gNs * kPicoScale / kBatchOps;
                        if (gPer < st.guardBatchMin) st.guardBatchMin = gPer;

                        uint64_t acc = 0;
                        const auto f0 = Clock::now();
                        for (uint64_t k = 0; k < kBatchOps; ++k) {
                            rcu::ReadGuard g(d);
                            VS::SafePtr<Payload> sp = rcu::protect<Payload>(d, cell);
                            Payload* p = &(*sp);
                            for (size_t s = 0; s < kChainDepth && p != nullptr; ++s) {
                                acc += p->version;
                                p = p->chainNext;
                            }
                        }
                        const uint64_t fNs = static_cast<uint64_t>(
                            std::chrono::duration_cast<Ns>(Clock::now() - f0).count());
                        gSink = acc;
                        st.fullBatchNs += fNs; st.fullBatchOps += kBatchOps;
                        const uint64_t fPer = fNs * kPicoScale / kBatchOps;
                        if (fPer < st.fullBatchMin) st.fullBatchMin = fPer;

                        // Deliberately NOT added to st.reads. These are a
                        // measurement phase, not workload, and folding them into
                        // the throughput counter would make reads/s incomparable
                        // across runs with different batch settings — it briefly
                        // did, and inflated the figure by ~2x.
                    }

                    // ── The nap: go completely quiet, OUTSIDE any section ──
                    //
                    // This is an idle CPU as the domain sees one: zero RCU calls,
                    // slot invisible to grace periods, sealed bags left for other
                    // CPUs to steal and an Open bag no one else can reach.
                    if (rng.next() % 100 < quietPct) {
                        const uint64_t napUs = 200 + (rng.next() % 3000);
                        const auto napStart = Clock::now();
                        std::this_thread::sleep_for(std::chrono::microseconds(napUs));
                        st.napNs += static_cast<uint64_t>(
                            std::chrono::duration_cast<Ns>(Clock::now() - napStart).count());
                        ++st.naps;
                    }
                }
            } catch (const AssertionFailure& e) {
                recordFailure(e.what());
            } catch (const std::exception& e) {
                recordFailure(e.what());
            } catch (...) {
                recordFailure("unknown exception escaped a soak worker");
            }
        });
    }

    for (auto& t : workers) t.join();
    const auto ended = Clock::now();
    stop.store(true, std::memory_order_release);
    sampler.join();

    // ── Teardown (P3-I5): join is the happens-before edge RCU-DEC-035 needs ──
    kernel::test::bindThreadToCpu(0);
    const int64_t liveAtDrain = gLive.load();
    rcu::test::drainAllQuiescent(d);
    for (size_t i = 0; i < kCells; ++i) {
        for (Payload* p = cells[i].load(SEQ_CST); p != nullptr;) {
            Payload* const next = p->chainNext;
            delete p;                       // never retired; the domain never owned it
            p = next;
        }
    }

    // ── Aggregate ──
    const double elapsedS = std::chrono::duration<double>(ended - started).count();
    uint64_t reads = 0, updates = 0, retires = 0, advances = 0, naps = 0, napNs = 0;
    uint64_t readHist[kHistBuckets] = {}, updateHist[kHistBuckets] = {};
    uint64_t readSamples = 0, updateSamples = 0;
    uint64_t gNs = 0, gOps = 0, gMin = ~uint64_t{0};
    uint64_t fNs = 0, fOps = 0, fMin = ~uint64_t{0};
    for (const auto& s : stats) {
        reads += s.reads; updates += s.updates; retires += s.retires;
        advances += s.advances; naps += s.naps; napNs += s.napNs;
        readSamples += s.readSamples; updateSamples += s.updateSamples;
        gNs += s.guardBatchNs; gOps += s.guardBatchOps;
        fNs += s.fullBatchNs;  fOps += s.fullBatchOps;
        if (s.guardBatchMin < gMin) gMin = s.guardBatchMin;
        if (s.fullBatchMin  < fMin) fMin = s.fullBatchMin;
        for (size_t b = 0; b < kHistBuckets; ++b) {
            readHist[b]   += s.readHist[b];
            updateHist[b] += s.updateHist[b];
        }
    }

    // Sampler locals are read only after join(), which is the happens-before
    // edge that makes them plain rather than atomic.
    const int64_t  peak     = peakLive;
    const uint64_t n        = samples;
    const double   meanLive = n ? static_cast<double>(sumLive) / static_cast<double>(n) : 0.0;
    const uint64_t liveP50  = percentile(liveHist.data(), n, 0.50);
    const uint64_t liveP90  = percentile(liveHist.data(), n, 0.90);
    const uint64_t liveP99  = percentile(liveHist.data(), n, 0.99);
    const double   objMiB   = static_cast<double>(sizeof(Payload)) / (1024.0 * 1024.0);

    const char* san = kSanitizerName;

    printf("\n"
           "  ┌─ RCU soak ────────────────────────────────────────────────────\n"
           "  │ %.1f s   %zu threads   sanitizer: %s   quiet chance: %llu%%\n"
           "  ├─ throughput ──────────────────────────────────────────────────\n"
           "  │ reads          %14llu   %12.0f /s\n"
           "  │ updates        %14llu   %12.0f /s\n"
           "  │ retires        %14llu   %12.0f /s\n"
           "  │ tryAdvance     %14llu\n"
           "  │ naps           %14llu   %8.1f%% of wall time quiet\n"
           "  ├─ latency (sampled 1-in-%llu; bucket UPPER bound, ns) ──────────\n"
           "  │ instrument: p50 %llu ns, p90 %llu ns, clock tick %llu ns — NOT subtracted\n"
           "  │ read      p50 %8llu   p90 %8llu   p99 %8llu   (%llu samples)\n"
           "  │ update    p50 %8llu   p90 %8llu   p99 %8llu   (%llu samples)\n"
           "  ├─ batch-timed read path (%llu ops/batch, under load; ns/section) ─\n"
           "  │ ReadGuard only          mean %7.2f   best %7.2f\n"
           "  │ + protect + %zu-node walk  mean %7.2f   best %7.2f\n"
           "  │ %s\n"
           "  ├─ limbo occupancy — objects retired but not yet destroyed ─────\n"
           "  │ (%llu samples @ %lluus; sizeof(Payload) = %zu B)\n"
           "  │ p50  %12llu   = %8.3f MiB\n"
           "  │ p90  %12llu   = %8.3f MiB\n"
           "  │ p99  %12llu   = %8.3f MiB\n"
           "  │ mean %12.0f   = %8.3f MiB\n"
           "  │ PEAK %12lld   = %8.3f MiB\n"
           "  │ at drain %8lld   = %8.3f MiB\n"
           "  └───────────────────────────────────────────────────────────────\n",
           elapsedS, threads, san, static_cast<unsigned long long>(quietPct),
           static_cast<unsigned long long>(reads),   static_cast<double>(reads) / elapsedS,
           static_cast<unsigned long long>(updates), static_cast<double>(updates) / elapsedS,
           static_cast<unsigned long long>(retires), static_cast<double>(retires) / elapsedS,
           static_cast<unsigned long long>(advances),
           static_cast<unsigned long long>(naps),
           100.0 * (static_cast<double>(napNs) / 1e9) /
               (elapsedS * static_cast<double>(threads)),
           static_cast<unsigned long long>(kLatencySample),
           static_cast<unsigned long long>(clockP50),
           static_cast<unsigned long long>(clockP90),
           static_cast<unsigned long long>(clockResNs),
           static_cast<unsigned long long>(percentile(readHist, readSamples, 0.50)),
           static_cast<unsigned long long>(percentile(readHist, readSamples, 0.90)),
           static_cast<unsigned long long>(percentile(readHist, readSamples, 0.99)),
           static_cast<unsigned long long>(readSamples),
           static_cast<unsigned long long>(percentile(updateHist, updateSamples, 0.50)),
           static_cast<unsigned long long>(percentile(updateHist, updateSamples, 0.90)),
           static_cast<unsigned long long>(percentile(updateHist, updateSamples, 0.99)),
           static_cast<unsigned long long>(updateSamples),
           static_cast<unsigned long long>(kBatchOps),
           gOps ? static_cast<double>(gNs) / static_cast<double>(gOps) : 0.0,
           gMin == ~uint64_t{0} ? 0.0 : static_cast<double>(gMin) / kPicoScale,
           kChainDepth,
           fOps ? static_cast<double>(fNs) / static_cast<double>(fOps) : 0.0,
           fMin == ~uint64_t{0} ? 0.0 : static_cast<double>(fMin) / kPicoScale,
           CROCOS_RCU_DEBUG_CHECKS
               ? "veneer debug checks ON (stall stamp, assertInSection, context) — NOT the release path"
               : "veneer debug checks OFF — this IS the release read path",
           static_cast<unsigned long long>(n),
           static_cast<unsigned long long>(kSamplePeriodUs),
           sizeof(Payload),
           static_cast<unsigned long long>(liveP50), static_cast<double>(liveP50) * objMiB,
           static_cast<unsigned long long>(liveP90), static_cast<double>(liveP90) * objMiB,
           static_cast<unsigned long long>(liveP99), static_cast<double>(liveP99) * objMiB,
           meanLive, meanLive * objMiB,
           static_cast<long long>(peak), static_cast<double>(peak) * objMiB,
           static_cast<long long>(liveAtDrain), static_cast<double>(liveAtDrain) * objMiB);
    fflush(stdout);

    // ── It is still a test ──
    if (gFailures.load() != 0) {
        std::lock_guard<std::mutex> lock(gFailureMutex);
        printf("  first worker failure: %s\n", gFirstFailure.c_str());
    }
    ASSERT_EQ(size_t{0}, gFailures.load());
    ASSERT_EQ(size_t{0}, gReadErrors.load());
    ASSERT_EQ(size_t{0}, gCorrupt.load());
    ASSERT_EQ(gRetired.load(), gDestroyed.load());
    ASSERT_EQ(int64_t{0}, gLive.load());
    ASSERT_EQ(size_t{0}, rcu::test::totalResidue(d));
    ASSERT_TRUE(reads > 0);
    ASSERT_TRUE(updates > 0);
    ASSERT_TRUE(naps > 0);          // the quiet-CPU case really was exercised

    rcu::test::assertQuiescent(d);
    delete domain;
    VS::test::shutdown();
}

// ─── Getting real performance numbers ──────────────────────────────────────
//
// The runners this test ships in are both instrumented, so the latency figures
// above are sanitizer-inflated by roughly an order of magnitude and the
// throughput figures by more. They are a regression tripwire, not a measurement.
//
// For profiling, build a third sibling of this directory's CMake block with
// `-O2 -g` and no `-fsanitize=` at all, and run the same test. Nothing in this
// file depends on a sanitizer being present — the correctness assertions still
// hold, they just stop having ASan as a backstop for double frees.
