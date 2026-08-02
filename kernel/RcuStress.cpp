//
// RCU Phase 4 (spike) — in-kernel stress test.
//
// Replaces `VmsmallocStress` as the smp_bringup per-CPU stress routine. It
// exists to close parent-spec ITEM-021: after Phases 1-3 the framework has 1261
// green userspace tests and yet SEVERAL of its kernel-side mechanisms have never
// executed on a real target even once. Specifically:
//
//   1. the real arch::InterruptDisabler inside the RCU-DEC-024 masked window
//   2. KernelRcuHooks::onPreTouch -> VMSubstrate::ensureTLBEntryFresh
//   3. SafePtr dereference and its freshness cost
//   4. the real assert -> PANIC on every debug check
//   5. .icd ordering against a live AP
//
// (2) IS THE POINT. onPreTouch exists solely to guard the vmsmalloc DEC-047
// stale-TLB bug class, and DEC-047 was found by the IN-KERNEL VmsmallocStress —
// the userspace harness did not and structurally could not find it, because its
// ensureTLBEntryFresh is a no-op and userspace has no page tables. RCU's exposure
// is the same shape: under RCU-DEC-006 stealing, a drainer dereferences intrusive
// RetireHead links living in ANOTHER CPU's retired slab memory, whose pages are
// subject to reclaimSlabPage. So the one Phase-2 mechanism most likely to be
// wrong is precisely the one every existing test is blind to.
//
// A COROLLARY THAT SHAPES THE WORKLOAD: the bug class is only reachable if slab
// pages actually get reclaimed and re-backed while a retired node still sits in
// a limbo bag. A workload that never frees enough for reclaimSlabPage to fire
// leaves the hazard unexercised while looking perfectly healthy. Whether this
// workload achieves that is the open question of the spike, not an assumption of
// it — see the liveness counters.
//
// ─── Why it also carries vmsmalloc breadth ─────────────────────────────────
//
// VmsmallocStress swept all 8 slab size classes plus a DEC-029 whole-page bucket.
// Replacing it would silently drop that coverage, so the node types here span a
// small slab class, the largest slab class, and a whole-page allocation. RCU sees
// one uniform retire/drain protocol; vmsmalloc sees three allocation paths.
// Cross-domain frees come for free: under stealing, the CPU that runs a deleter
// is usually NOT the CPU that allocated the node, so vmsfree's cross-domain gate
// (DEC-019) is exercised without any explicit hand-off machinery.
//
// DEC-028 discipline: vmsmalloc is never used raw. Every allocation is
// VMSubstrate::make<T>, and every read of allocator-returned memory goes through
// SafePtr<T>, whose operator*/operator-> call ensureTLBEntryFresh first.
//
// The stress is its own success signal: it runs until the shutdown timer fires.
// A panic, a hang, or a corruption report is the failure.
//

#include <kernel.h>
#include <arch.h>
#include <kassert.h>
#include <CpuLocal.h>
#include <mem/VMSubstrate.h>
#include <mem/NUMA.h>
#include <core/atomic.h>
#include <rcu/RCU.h>

namespace VMS  = kernel::mm::VMSubstrate;
namespace slab = kernel::mm::vmsmalloc;

namespace kernel {

namespace rcu_stress {

    constexpr uint64_t kMagic       = 0x5243555354524553ull;   // "RCUSTRES"
    constexpr size_t   kCells       = 64;    // shared cells per class
    constexpr size_t   kMaxCpus     = 16;    // matches the run / qmon -smp counts
    constexpr uint64_t kLivenessMask = 0xFFFFull;   // klog every 64K iterations

    // ── Node classes ───────────────────────────────────────────────────────
    //
    // Sized to land on: a mid slab class, the LARGEST slab class, and a
    // whole-page allocation that bypasses the slab entirely (DEC-029). The
    // static_asserts below are load-bearing — if a future size-class table edit
    // moves these off their intended paths the coverage silently narrows.
    template <size_t Bytes>
    struct Node {
        Core::rcu::RetireHead head;        // 16 — must be first-class, not aliased
        uint64_t              magic;
        uint64_t              version;
        uint64_t              checksum;
        uint32_t              cpu;
        uint32_t              cls;
        unsigned char         filler[Bytes - 48];
    };

    using NodeS = Node<64>;    // slab class 64
    using NodeM = Node<512>;   // largest slab class
    using NodeL = Node<1024>;  // > 512 -> DEC-029 whole-page bypass

    static_assert(sizeof(NodeS) == 64,   "NodeS must land exactly on a slab class");
    static_assert(sizeof(NodeM) == 512,  "NodeM must land on the largest slab class");
    static_assert(sizeof(NodeL) == 1024, "NodeL must exceed the largest slab class");
    static_assert(slab::kSlabSizeClasses[slab::kNumSizeClasses - 1] == 512,
                  "NodeM/NodeL assume 512 is the largest slab class — retune them");

    // ── The shared structure ───────────────────────────────────────────────
    //
    // Deliberately SHARED across CPUs rather than per-CPU: a per-CPU structure
    // would never make one CPU read a node another CPU retired, which is the
    // entire hazard under test. BSS, zero-initialised, so every cell starts null
    // and readers must tolerate that until a writer fills it.
    Atomic<NodeS*> gCellsS[kCells];
    Atomic<NodeM*> gCellsM[kCells];
    Atomic<NodeL*> gCellsL[kCells];

    // Liveness / corruption counters. Relaxed throughout — diagnostics, not
    // protocol.
    Atomic<uint64_t> gReads{0};
    Atomic<uint64_t> gWrites{0};
    Atomic<uint64_t> gRetires{0};
    Atomic<uint64_t> gCorrupt{0};
    Atomic<uint64_t> gNullReads{0};

    [[nodiscard]] constexpr uint64_t checksumOf(uint64_t version, uint32_t cpu, uint32_t cls) {
        return kMagic ^ (version * 0x9E3779B97F4A7C15ull) ^ (uint64_t{cpu} << 32) ^ cls;
    }

    // Publish a fresh node into `cell` and retire whatever it displaced.
    //
    // The retire MUST happen inside a section (RCU-DEC-019): the writer traverses
    // the shared structure to perform the unlink, so an unpinned writer can have
    // the node reclaimed under it. Every field write goes through the SafePtr, so
    // ensureTLBEntryFresh runs before this CPU first touches freshly allocated
    // slab memory that may have been someone else's a moment ago.
    template <typename N>
    void publishAndRetire(Atomic<N*>& cell, uint64_t version, uint32_t cpu, uint32_t cls) {
        VMS::SafePtr<N> fresh = VMS::make<N>();
        fresh->head     = Core::rcu::RetireHead{};
        fresh->magic    = kMagic;
        fresh->version  = version;
        fresh->cpu      = cpu;
        fresh->cls      = cls;
        fresh->checksum = checksumOf(version, cpu, cls);

        rcu::ReadGuard g(rcu::kernelDomain);
        N* old = cell.exchange(static_cast<N*>(fresh.raw()), ACQ_REL);
        gWrites.fetch_add(1, RELAXED);
        if (old != nullptr) {
            rcu::retireDestroy<N, &N::head>(rcu::kernelDomain, VMS::SafePtr<N>(old));
            gRetires.fetch_add(1, RELAXED);
        }
    }

    // One protected read. The pointer is only safe for the life of the section,
    // and its BYTES are only current because SafePtr re-checks freshness — the
    // two orthogonal protections this phase exists to exercise together.
    template <typename N>
    void protectedRead(Atomic<N*>& cell) {
        rcu::ReadGuard g(rcu::kernelDomain);
        VMS::SafePtr<N> n = rcu::protect<N>(rcu::kernelDomain, cell);
        if (!n) { gNullReads.fetch_add(1, RELAXED); return; }

        const uint64_t magic = n->magic;
        const uint64_t ver   = n->version;
        const uint64_t sum   = n->checksum;
        const uint32_t cpu   = n->cpu;
        const uint32_t cls   = n->cls;
        gReads.fetch_add(1, RELAXED);

        if (magic != kMagic || sum != checksumOf(ver, cpu, cls)) {
            gCorrupt.fetch_add(1, RELAXED);
            klog() << "rcuStress: CORRUPTION cpu=" << static_cast<uint64_t>(getLogicalProcessorID())
                   << " magic=" << magic << " ver=" << ver << " cls=" << static_cast<uint64_t>(cls)
                   << "\n";
            assert(false, "rcuStress: protected read saw a torn or recycled node");
        }
    }

}   // namespace rcu_stress

// Per-CPU stress driver. Runs until the shutdown timer fires; its own success
// signal is the absence of a panic.
[[noreturn]] bool rcuStress() {
    namespace st = rcu_stress;

    const arch::ProcessorID myCpu = getLogicalProcessorID();
    assert(static_cast<size_t>(myCpu) < st::kMaxCpus,
           "rcuStress: CPU id exceeds kMaxCpus — raise the bound");
    assert(rcu::kernelDomain.initialized(),
           "rcuStress: kernelDomain not initialized — check .icd phase ordering");

    klog() << "rcuStress: starting on CPU " << static_cast<uint64_t>(myCpu) << "\n";

    // Cheap per-CPU PRNG; distinct seed per CPU so the CPUs do not march in
    // lockstep over the same cells.
    uint64_t rng = 0x9E3779B97F4A7C15ull ^ (uint64_t{myCpu} * 0xBF58476D1CE4E5B9ull);
    auto next = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        return rng;
    };

    uint64_t iteration = 0;
    for (;;) {
        const uint64_t r    = next();
        const size_t   idx  = (r >> 3) % st::kCells;
        const uint32_t cls  = static_cast<uint32_t>(r % 3);

        // ~1 in 8 iterations mutates; the rest read. Read-mostly is the shape
        // RCU is for, and it keeps enough readers in flight that a drainer is
        // usually racing one.
        const bool write = ((r >> 20) % 8) == 0;

        if (write) {
            switch (cls) {
                case 0: st::publishAndRetire(st::gCellsS[idx], iteration, myCpu, cls); break;
                case 1: st::publishAndRetire(st::gCellsM[idx], iteration, myCpu, cls); break;
                default: st::publishAndRetire(st::gCellsL[idx], iteration, myCpu, cls); break;
            }
        } else {
            switch (cls) {
                case 0: st::protectedRead(st::gCellsS[idx]); break;
                case 1: st::protectedRead(st::gCellsM[idx]); break;
                default: st::protectedRead(st::gCellsL[idx]); break;
            }
        }

        // Pull-based progress (RCU-DEC-005): no daemon, no tick. Every CPU drives
        // advancement from its own path, which is also what makes it steal other
        // CPUs' expired bags.
        if ((r >> 40) % 64 == 0) (void)rcu::tryAdvance(rcu::kernelDomain);

        if ((iteration & st::kLivenessMask) == 0) {
            // P4-ITEM-001: `reclaims` and `stale` are the coverage claim, not
            // decoration. reclaims == 0 means the DEC-047 reclaim path never
            // ran; stale == 0 means no RCU node was ever touched through a
            // remapped mapping, so the hazard onPreTouch guards was never
            // actually presented to it — however healthy everything else looks.
            klog() << "rcuStress: cpu=" << static_cast<uint64_t>(myCpu)
                   << " iter=" << iteration
                   << " reads=" << st::gReads.load(RELAXED)
                   << " writes=" << st::gWrites.load(RELAXED)
                   << " retires=" << st::gRetires.load(RELAXED)
                   << " nullReads=" << st::gNullReads.load(RELAXED)
                   << " corrupt=" << st::gCorrupt.load(RELAXED)
#ifdef CROCOS_FRESHNESS_STATS
                   << " reclaims=" << mm::VMSubstrate::reclaimedSlabPageCount()
                   << " preTouch=" << rcu::freshnessStats().preTouches
                   << " stale=" << rcu::freshnessStats().staleHits
#else
                   << " [stats=off]"
#endif
                   << "\n";
        }
        ++iteration;
    }
}

}   // namespace kernel
