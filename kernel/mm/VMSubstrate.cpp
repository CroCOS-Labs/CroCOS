//
// Created by Spencer Martin on 4/26/26.
//

#include <mem/VMSubstrate.h>
#include <mem/mm.h>
#include <arch.h>

#include <kmemlayout.h>

#include "arch.h"
#include "mem/TempWindow.h"

namespace VMSubstrateHelper {

    // ────────────────────────────────────────────────────────────────────────
    // Radix-tree sizing (independent of the hardware page-tree depth).
    //
    // The radix tree is a software 64-ary tree whose only purpose is to find
    // free arena VAs. Each leaf bitmap covers kBranchFactor small pages; each
    // interior bitmap's bit i indicates whether the i-th child has any free
    // descendants. Modeled after libraries/Core/include/AtomicBitPool.cpp,
    // minus the per-level auxiliary counters (single allocator, possibly many
    // concurrent freers).
    //
    // The hardware page tree is an implementation detail used only when
    // backing a found VA with a physical page; it does not constrain radix
    // depth or branching factor.
    // ────────────────────────────────────────────────────────────────────────

    constexpr size_t leafLevel     = arch::pageTableDescriptor.LEVEL_COUNT - 1;
    constexpr size_t kBranchFactor = 64;

    // Total small pages an arena holds.
    constexpr size_t kArenaPageCount =
        (size_t{1} << arch::pageTableDescriptor.getVirtualAddressBitCount(
            kernel::mm::pageTableLevelForKMemRegion()))
        / arch::smallPageSize;

    // Number of leaf bitmaps. Each covers kBranchFactor small pages.
    constexpr size_t kLeafBitmapCount =
        (kArenaPageCount + kBranchFactor - 1) / kBranchFactor;

    // Radix-tree depth (levels including leaf).
    constexpr size_t kRadixDepth = []() {
        size_t depth = 1;
        size_t count = kLeafBitmapCount;
        while (count > 1) {
            count = (count + kBranchFactor - 1) / kBranchFactor;
            depth++;
        }
        return depth;
    }();

    // Per-level bitmap counts: v[0] = leaf, v[kRadixDepth-1] = root (always 1).
    struct LevelCounts { size_t v[kRadixDepth]; };
    constexpr LevelCounts levelBitmapCount = []() {
        LevelCounts r{};
        r.v[0] = kLeafBitmapCount;
        for (size_t i = 1; i < kRadixDepth; i++)
            r.v[i] = (r.v[i - 1] + kBranchFactor - 1) / kBranchFactor;
        return r;
    }();

    // BFS offset (within interiorFreeBitmap[]) of the first bitmap at each
    // level. Root is stored first. v[0] is unused (leaf level isn't stored
    // in interiorFreeBitmap).
    struct BFSOffsets { size_t v[kRadixDepth]; };
    constexpr BFSOffsets bfsOffset = []() {
        BFSOffsets r{};
        r.v[kRadixDepth - 1] = 0;
        for (size_t i = kRadixDepth - 1; i > 0; i--)
            r.v[i - 1] = r.v[i] + levelBitmapCount.v[i];
        return r;
    }();

    // Total interior bitmap count (sum over levels above leaf).
    constexpr size_t kInteriorBitmapCount = (kRadixDepth >= 2) ? bfsOffset.v[0] : 0;

    // ────────────────────────────────────────────────────────────────────────
    // Occupancy metadata buffer layout — packed contiguously at the buffer
    // base (= occupancyBufferBase(arenaBase)):
    //
    //   leafAllocBitmap     uint64_t          [kLeafBitmapCount]
    //   leafFreeBitmap      Atomic<uint64_t>  [kLeafBitmapCount]
    //   leafFreeWordCount   Atomic<uint8_t>   [kLeafBitmapCount]    values 0/1/2
    //   interiorFreeBitmap  Atomic<uint64_t>  [kInteriorBitmapCount] BFS, root first
    //
    // freeWordCount is per-leaf-bitmap and counts non-empty words across
    // {leafAllocBitmap, leafFreeBitmap}. It changes only at:
    //   • alloc-CPU when allocBitmap nonzero→0: fetch_sub
    //   • freeing CPU when freeBitmap 0→nonzero: fetch_add
    // The exchange-and-drain (freeBitmap[w] → allocBitmap[w]) leaves it alone.
    // ────────────────────────────────────────────────────────────────────────

    constexpr size_t kLeafAllocBitmapBytes    = kLeafBitmapCount     * sizeof(uint64_t);
    constexpr size_t kLeafFreeBitmapBytes     = kLeafBitmapCount     * sizeof(uint64_t);
    constexpr size_t kLeafFreeWordCountBytes  = kLeafBitmapCount     * sizeof(uint8_t);
    constexpr size_t kInteriorFreeBitmapBytes = kInteriorBitmapCount * sizeof(uint64_t);

    constexpr size_t kLeafAllocBitmapOffset    = 0;
    constexpr size_t kLeafFreeBitmapOffset     = kLeafAllocBitmapOffset    + kLeafAllocBitmapBytes;
    constexpr size_t kLeafFreeWordCountOffset  = kLeafFreeBitmapOffset     + kLeafFreeBitmapBytes;
    constexpr size_t kInteriorFreeBitmapOffset = kLeafFreeWordCountOffset  + kLeafFreeWordCountBytes;

    constexpr size_t kOccupancyBufferRawSize =
        kInteriorFreeBitmapOffset + kInteriorFreeBitmapBytes;

    constexpr size_t kOccupancyBufferPages = divideAndRoundUp(kOccupancyBufferRawSize, arch::smallPageSize);
    constexpr size_t kOccupancyBufferSize =
        kOccupancyBufferPages * arch::smallPageSize;

    // VA span of one entry at the arena root level (= 2 MiB on AMD64; the
    // start of root[1]'s data range).
    constexpr size_t kSelfRefSize =
        size_t{1} << arch::pageTableDescriptor.getVirtualAddressBitCount(
            kernel::mm::pageTableLevelForKMemRegion() + 1);

    // ────────────────────────────────────────────────────────────────────────
    // Page-table wrappers.
    // ────────────────────────────────────────────────────────────────────────

    using LeafPTE = arch::PTE<leafLevel>;

    struct LeafPageTableWrapper {
        arch::PageTable<leafLevel> table;

        static size_t  dirtyWordCount() {
            return divideAndRoundUp(arch::processorCount(), static_cast<size_t>(64));
        }
        static size_t  dirtyCPUWord(size_t cpu) { return cpu / 64; }
        static uint64_t dirtyCPUBit (size_t cpu) { return uint64_t{1} << (cpu % 64); }

        LeafPageTableWrapper() { memset(&table, 0, sizeof(table)); }
    };

    template <size_t level>
    struct PageTableWrapper {
        static_assert(level < leafLevel, "Use PageTableWrapper<leafLevel> for leaf tables");
        arch::PageTable<level> table;
        PageTableWrapper() { memset(&table, 0, sizeof(table)); }
    };

    template <>
    struct PageTableWrapper<leafLevel> : LeafPageTableWrapper {
        using LeafPageTableWrapper::LeafPageTableWrapper;
    };

    static_assert([]() {
        for (size_t i = 1; i < arch::pageTableDescriptor.LEVEL_COUNT; i++)
            if (arch::pageTableDescriptor.entryCount[i] != arch::pageTableDescriptor.entryCount[0])
                return false;
        return true;
    }(), "VMSubstrate requires uniform entry count across all page table levels");

    // The arena page count must be a multiple of kBranchFactor so leaf bitmaps
    // each cover exactly kBranchFactor pages with no fractional last leaf.
    static_assert(kArenaPageCount % kBranchFactor == 0,
        "kArenaPageCount must be a multiple of kBranchFactor");

    // Worst-case dirty + buffer reservation must fit in one leaf bitmap (the
    // first leaf covering root[1]'s data range receives all of them).
    static_assert(((arch::MAX_PROCESSOR_COUNT + 63) / 64) + kOccupancyBufferPages
                  <= kBranchFactor,
        "dirty bitmap + occupancy buffer overflow leaf bitmap's bit count");

    // ────────────────────────────────────────────────────────────────────────
    // Buffer accessors. Require the arena's VA to be live.
    // ────────────────────────────────────────────────────────────────────────

    [[nodiscard]] inline kernel::mm::virt_addr occupancyBufferBase(kernel::mm::virt_addr arenaBase) {
        return arenaBase + kSelfRefSize
                         + LeafPageTableWrapper::dirtyWordCount() * arch::smallPageSize;
    }

    [[nodiscard]] inline kernel::mm::virt_addr allocatableBase(kernel::mm::virt_addr arenaBase) {
        return occupancyBufferBase(arenaBase) + kOccupancyBufferSize;
    }

    [[nodiscard]] inline uint64_t& leafAllocBitmap(kernel::mm::virt_addr arenaBase, size_t T) {
        return reinterpret_cast<uint64_t*>(
            occupancyBufferBase(arenaBase).value + kLeafAllocBitmapOffset)[T];
    }
    [[nodiscard]] inline Atomic<uint64_t>& leafFreeBitmap(kernel::mm::virt_addr arenaBase, size_t T) {
        return reinterpret_cast<Atomic<uint64_t>*>(
            occupancyBufferBase(arenaBase).value + kLeafFreeBitmapOffset)[T];
    }
    [[nodiscard]] inline Atomic<uint8_t>& leafFreeWordCount(kernel::mm::virt_addr arenaBase, size_t T) {
        return reinterpret_cast<Atomic<uint8_t>*>(
            occupancyBufferBase(arenaBase).value + kLeafFreeWordCountOffset)[T];
    }
    [[nodiscard]] inline Atomic<uint64_t>& interiorBitmapByBFS(kernel::mm::virt_addr arenaBase, size_t bfs) {
        return reinterpret_cast<Atomic<uint64_t>*>(
            occupancyBufferBase(arenaBase).value + kInteriorFreeBitmapOffset)[bfs];
    }

    // ────────────────────────────────────────────────────────────────────────
    // Leaf-bit operations.
    // ────────────────────────────────────────────────────────────────────────

    struct LeafClaimResult { int bit; bool becameFull; };  // bit == -1 when leaf is full

    // Claim a free bit from leafAllocBitmap[T].
    // Pass 0: scan allocBitmap (no atomics).
    // Pass 1: drain leafFreeBitmap → leafAllocBitmap (ACQ_REL exchange), then scan again.
    // freeWordCount is not touched during drain.
    // When allocBitmap goes nonzero→0: fetch_sub freeWordCount; becameFull iff it was 1.
    [[nodiscard]] inline LeafClaimResult claimLeafBit(kernel::mm::virt_addr arenaBase, size_t T) {
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                const uint64_t freed = leafFreeBitmap(arenaBase, T).exchange(0, ACQ_REL);
                if (!freed) return {-1, false};
                leafAllocBitmap(arenaBase, T) |= freed;
            }
            uint64_t& alloc = leafAllocBitmap(arenaBase, T);
            if (!alloc) continue;
            const int bit = __builtin_ctzll(alloc);
            alloc &= alloc - 1;
            bool becameFull = false;
            if (!alloc) {
                const uint8_t prev = leafFreeWordCount(arenaBase, T).fetch_sub(1, ACQ_REL);
                becameFull = (prev == 1);
            }
            return {bit, becameFull};
        }
        return {-1, false};
    }

    inline void propagateEdge(kernel::mm::virt_addr arenaBase, size_t leafIdx, bool isAvailableEdge) {
        size_t childIdx   = leafIdx;
        size_t childLevel = 0;

        while (childLevel + 1 < kRadixDepth) {
            const size_t parentLevel = childLevel + 1;
            const size_t parentIdx   = childIdx / kBranchFactor;
            const size_t bitInParent = childIdx % kBranchFactor;
            const size_t bfs         = bfsOffset.v[parentLevel] + parentIdx;

            const uint64_t mask = uint64_t{1} << bitInParent;
            const uint64_t prev = interiorBitmapByBFS(arenaBase, bfs).fetch_xor(mask, ACQ_REL);

            // If making available, continue ONLY if we transitioned the parent from 0 -> >0
            if (isAvailableEdge && prev != 0) return;

            // If making full, continue ONLY if we transitioned the parent from >0 -> 0
            if (!isAvailableEdge && (prev ^ mask) != 0) return;

            childIdx   = parentIdx;
            childLevel = parentLevel;
        }
    }

    // Permanently mark a slot as occupied. Used during arena init to reserve
    // VAs that are not allocatable (root[0] self-ref shadow region; dirty
    // pages and occupancy-buffer pages). Single-threaded init context;
    // RELAXED memory order. Propagates "leaf full" up the radix tree if the
    // leaf transitions to fully reserved.
    inline void reserveLeafBit(kernel::mm::virt_addr arenaBase, size_t T, size_t bit) {
        const uint64_t mask = uint64_t{1} << bit;
        uint64_t& alloc = leafAllocBitmap(arenaBase, T);
        const uint64_t prevAlloc = alloc;
        alloc &= ~mask;
        if (prevAlloc && !alloc) {
            const uint8_t prevCount = leafFreeWordCount(arenaBase, T).fetch_sub(1, RELAXED);
            if (prevCount == 1)
                propagateEdge(arenaBase, T, false);
        }
    }

    // ────────────────────────────────────────────────────────────────────────
    // Top-down descent through interior bitmaps to find a free leaf bitmap T.
    // Returns SIZE_MAX if the arena has no free pages.
    // Single-allocator-per-arena: RELAXED reads suffice. A concurrent freer
    // can only add availability bits, so observing a stale "no free" value at
    // an interior level is benign — the alloc loop above will retry.
    // ────────────────────────────────────────────────────────────────────────

    [[nodiscard]] inline size_t descendToFreeLeaf(kernel::mm::virt_addr arenaBase) {
        size_t parentIdx = 0;
        for (size_t level = kRadixDepth - 1; level >= 1; level--) {
            const size_t bfs = bfsOffset.v[level] + parentIdx;
            const uint64_t bitmap = interiorBitmapByBFS(arenaBase, bfs).load(RELAXED);
            if (bitmap == 0) return SIZE_MAX;
            const int bit = __builtin_ctzll(bitmap);
            parentIdx = parentIdx * kBranchFactor + static_cast<size_t>(bit);
        }
        return parentIdx;
    }

    // ────────────────────────────────────────────────────────────────────────
    // Self-reference HW-walk arithmetic. The arena root has slot 0 pointing
    // at itself, which lets the leaf PTE that maps `va` be reached by simple
    // division on the arena VA: leafPTEAddr = (va - base) / entryCount + base.
    // Recursing the same formula one more time gives the parent entry that
    // points at the leaf PT, and so on up the chain.
    // ────────────────────────────────────────────────────────────────────────

    constexpr size_t kEntryCount = arch::pageTableDescriptor.entryCount[0];

    [[nodiscard]] inline kernel::mm::virt_addr leafPTEAddrFor(kernel::mm::virt_addr arenaBase,
                                                              kernel::mm::virt_addr va) {
        return kernel::mm::virt_addr{
            (va.value - arenaBase.value) / kEntryCount + arenaBase.value
        };
    }

    [[nodiscard]] inline kernel::mm::virt_addr parentEntryAddrOf(kernel::mm::virt_addr arenaBase,
                                                                 kernel::mm::virt_addr childAddr) {
        return kernel::mm::virt_addr{
            (((childAddr.value - arenaBase.value) / kEntryCount) & ~7ul) + arenaBase.value
        };
    }

    [[nodiscard]] inline kernel::mm::virt_addr leafPTBaseFor(kernel::mm::virt_addr arenaBase,
                                                              kernel::mm::virt_addr va) {
        return kernel::mm::virt_addr{
            roundDownToNearestMultiple(leafPTEAddrFor(arenaBase, va).value, arch::smallPageSize)
        };
    }

    // ────────────────────────────────────────────────────────────────────────
    // Set the dirty bit for every CPU other than the current one for the
    // small page at `va`. Subsequent SafePtr<T> dereferences on those CPUs
    // call ensureTLBEntryFresh, which sees the bit, invlpgs the VA, and
    // clears the bit. This is how PTE changes propagate without IPIs.
    // ────────────────────────────────────────────────────────────────────────

    inline void setDirtyForOtherCPUs(kernel::mm::virt_addr va) {
        const auto vaValue = va.value;
        const auto tableBase = roundDownToNearestMultiple(vaValue, arch::bigPageSize);
        const size_t k_abs = (vaValue - tableBase) / arch::smallPageSize;
        const size_t myCPU = static_cast<size_t>(arch::getCurrentProcessorID());
        const size_t myWord = LeafPageTableWrapper::dirtyCPUWord(myCPU);
        const uint64_t myBit = LeafPageTableWrapper::dirtyCPUBit(myCPU);
        const size_t D = LeafPageTableWrapper::dirtyWordCount();
        for (size_t dw = 0; dw < D; dw++) {
            const uint64_t mask = (dw == myWord) ? (UINT64_MAX & ~myBit) : UINT64_MAX;
            if (mask == 0) continue;
            auto& dirtyEntry = *reinterpret_cast<Atomic<uint64_t>*>(
                tableBase + dw * arch::smallPageSize + k_abs * sizeof(uint64_t));
            dirtyEntry.fetch_or(mask, RELEASE);
        }
    }

    // ────────────────────────────────────────────────────────────────────────
    // Boot-time radix-tree initialization (called after the arena VA is live).
    // ────────────────────────────────────────────────────────────────────────

    // Sets every leaf bitmap to "all bits available" and every interior
    // bitmap to "all valid bits available". For levels whose count below is
    // not a multiple of kBranchFactor, the last bitmap at that level uses
    // only its low (count_below mod 64) bits; invalid bits are left at 0.
    inline void seedAvailableState(kernel::mm::virt_addr arenaBase) {
        const auto base = occupancyBufferBase(arenaBase).value;

        memset(reinterpret_cast<void*>(base + kLeafAllocBitmapOffset),
               0xFF, kLeafAllocBitmapBytes);
        memset(reinterpret_cast<void*>(base + kLeafFreeBitmapOffset),
               0x00, kLeafFreeBitmapBytes);
        memset(reinterpret_cast<void*>(base + kLeafFreeWordCountOffset),
               0x01, kLeafFreeWordCountBytes);

        for (size_t level = 1; level < kRadixDepth; level++) {
            const size_t bitmapsAtLevel = levelBitmapCount.v[level];
            const size_t childCount     = levelBitmapCount.v[level - 1];
            for (size_t j = 0; j < bitmapsAtLevel; j++) {
                const size_t childrenHere =
                    (j == bitmapsAtLevel - 1)
                        ? (childCount - j * kBranchFactor)
                        : kBranchFactor;
                const uint64_t init = (childrenHere >= kBranchFactor)
                                        ? UINT64_MAX
                                        : (uint64_t{1} << childrenHere) - 1;
                interiorBitmapByBFS(arenaBase, bfsOffset.v[level] + j).store(init, RELAXED);
            }
        }
    }

    // Reserves the bits corresponding to non-allocatable VAs:
    //   (a) the entire root[0] self-ref shadow region (page-tree pages and
    //       the dead zone past them); and
    //   (b) the dirty-bitmap pages and occupancy-buffer pages at the head of
    //       root[1]'s data range.
    inline void reserveBootBits(kernel::mm::virt_addr arenaBase) {
        constexpr size_t selfRefBitmaps =
            kSelfRefSize / (kBranchFactor * arch::smallPageSize);

        // (a) Self-ref shadow region.
        for (size_t T = 0; T < selfRefBitmaps; T++)
            for (size_t bit = 0; bit < kBranchFactor; bit++)
                reserveLeafBit(arenaBase, T, bit);

        // (b) Dirty + occupancy-buffer pages at the head of root[1]'s data range.
        constexpr size_t T = selfRefBitmaps;
        const size_t D = LeafPageTableWrapper::dirtyWordCount();
        for (size_t bit = 0; bit < D + kOccupancyBufferPages; bit++)
            reserveLeafBit(arenaBase, T, bit);
    }

    // ────────────────────────────────────────────────────────────────────────
    // Lazy HW-subtable installation. Walks from `level` down to leafLevel - 1,
    // installing any missing subtable along the way. When the freshly-installed
    // child is the leaf PT, also allocates D dirty-bitmap pages, maps them at
    // slots [0, D-1] of the new PT, and reserves the matching radix-tree bits.
    //
    // Single-allocator-per-arena: the caller is the sole allocator for this
    // arena, so racing installs are impossible. `entry.isPresent()` reads are
    // safe without atomics.
    // ────────────────────────────────────────────────────────────────────────

    template <size_t level>
    inline void ensureSubtableInstalled(kernel::mm::virt_addr arenaBase,
                                        kernel::mm::virt_addr va,
                                        arch::ProcessorID cpu)
        requires (level >= kernel::mm::pageTableLevelForKMemRegion())
              && (level <  leafLevel)
    {
        using Flag = arch::PageEntryFlag;
        constexpr auto kFlags = Flag::Write | Flag::Global | Flag::NoExecute;

        // The entry-at-level-L for `va` is reachable from the arena's self-ref:
        //   addr = (va - base) / entryCount^(leafLevel - L + 1) + base.
        constexpr size_t depthFromLeaf = leafLevel - level;
        constexpr size_t parentDivisor = []() {
            size_t d = 1;
            for (size_t i = 0; i <= depthFromLeaf; i++) d *= kEntryCount;
            return d;
        }();
        constexpr size_t childDivisor = parentDivisor / kEntryCount;

        const uint64_t parentEntryAddr =
            ((va.value - arenaBase.value) / parentDivisor & ~7ul) + arenaBase.value;
        auto& parentEntry = *reinterpret_cast<arch::PTE<level>*>(parentEntryAddr);

        if (!parentEntry.isPresent()) {
            const kernel::mm::phys_addr physAddr =
                kernel::mm::PageAllocator::allocateSmallPage(cpu);
            parentEntry = arch::PTE<level>::subtableEntry(physAddr, kFlags);

            const uint64_t childEntryAddr =
                (va.value - arenaBase.value) / childDivisor + arenaBase.value;
            const auto childTableAddr = kernel::mm::virt_addr{
                roundDownToNearestMultiple(childEntryAddr, arch::smallPageSize)
            };

            // Drop any negative-TLB entry the local CPU may have for the new
            // subtable's VA before placement-new walks through the self-ref.
            arch::invlpg(childTableAddr);

            new (reinterpret_cast<PageTableWrapper<level + 1>*>(childTableAddr.value))
                PageTableWrapper<level + 1>();

            if constexpr (level + 1 == leafLevel) {
                // Fresh leaf PT: install its per-CPU dirty-bitmap pages at
                // slots [0, D-1] and reserve the matching radix bits so the
                // allocator never hands those slots to a caller.
                const size_t D = LeafPageTableWrapper::dirtyWordCount();
                auto* leafPT =
                    reinterpret_cast<arch::PageTable<leafLevel>*>(childTableAddr.value);
                for (size_t dw = 0; dw < D; dw++) {
                    const kernel::mm::phys_addr dirtyPhys =
                        kernel::mm::PageAllocator::allocateSmallPage(cpu);
                    (*leafPT)[dw] = arch::PTE<leafLevel>::leafEntry(dirtyPhys, kFlags);
                    arch::invlpg(kernel::mm::virt_addr{
                        roundDownToNearestMultiple(va.value, arch::bigPageSize)}
                        + dw * arch::smallPageSize);
                }
                // The first radix bitmap covering this leaf PT's pages.
                const size_t T_first =
                    ((va.value - arenaBase.value) / arch::bigPageSize)
                    * (arch::bigPageSize / (kBranchFactor * arch::smallPageSize));
                for (size_t bit = 0; bit < D; bit++)
                    reserveLeafBit(arenaBase, T_first, bit);
            }
        }

        if constexpr (level + 1 < leafLevel) {
            ensureSubtableInstalled<level + 1>(arenaBase, va, cpu);
        }
    }

} // VMSubstrateHelper

namespace kernel::mm::VMSubstrate {

    arch::PageTable<pageTableLevelForKMemRegion() - 1> vmmArenaTable;

    Atomic<size_t> freeArenaIndex = 0;
    WITH_GLOBAL_CONSTRUCTOR(Spinlock, arenaCreationLock);

    // Construct one hardware page table page at the given level. For the
    // arena root: writes a self-reference at slot 0 and the chain pointer at
    // slot 1. For the leaf level: installs per-CPU dirty-bitmap pages at
    // slots [0, D-1] and the caller-supplied occupancy-buffer pages at slots
    // [D, D + kOccupancyBufferPages - 1]. For intermediate levels: installs a
    // single subtable pointer at slot 0.
    template <size_t level>
    phys_addr initializePageTable(arch::ProcessorID cpu,
                                  phys_addr subtable = phys_addr(nullptr),
                                  const phys_addr* occBufPhys = nullptr)
        requires (level >= pageTableLevelForKMemRegion())
              && (level <  arch::pageTableDescriptor.LEVEL_COUNT) {
        const auto ptaddr = PageAllocator::allocateSmallPage(cpu);
        TempWindow<VMSubstrateHelper::PageTableWrapper<level>> window(ptaddr);
        auto* pageTablePtr = new (&*window) VMSubstrateHelper::PageTableWrapper<level>();
        using Flag = arch::PageEntryFlag;
        constexpr auto kFlags = Flag::Write | Flag::Global | Flag::NoExecute;

        if constexpr (level == pageTableLevelForKMemRegion()) {
            pageTablePtr->table[0] = arch::PTE<level>::subtableEntry(ptaddr,   kFlags);
            pageTablePtr->table[1] = arch::PTE<level>::subtableEntry(subtable, kFlags);
        } else if constexpr (level == arch::pageTableDescriptor.LEVEL_COUNT - 1) {
            const size_t D = VMSubstrateHelper::LeafPageTableWrapper::dirtyWordCount();
            for (size_t dw = 0; dw < D; dw++) {
                const phys_addr dirtyPhys = PageAllocator::allocateSmallPage(cpu);
                pageTablePtr->table[dw] = arch::PTE<level>::leafEntry(dirtyPhys, kFlags);
            }
            for (size_t i = 0; i < VMSubstrateHelper::kOccupancyBufferPages; i++) {
                pageTablePtr->table[D + i] =
                    arch::PTE<level>::leafEntry(occBufPhys[i], kFlags);
            }
        } else {
            pageTablePtr->table[0] = arch::PTE<level>::subtableEntry(subtable, kFlags);
        }
        return ptaddr;
    }

    // Recursively initializes the full page table chain from leaf up to root,
    // feeding each level's physical address as the subtable of the level
    // above. Threads occBufPhys to the leaf level so the leaf installs the
    // occupancy-buffer pages alongside the dirty-bitmap pages.
    template <size_t level>
    phys_addr initializeArenaChain(arch::ProcessorID cpu, const phys_addr* occBufPhys)
        requires (level >= pageTableLevelForKMemRegion())
              && (level <  arch::pageTableDescriptor.LEVEL_COUNT) {
        if constexpr (level == arch::pageTableDescriptor.LEVEL_COUNT - 1) {
            return initializePageTable<level>(cpu, phys_addr(nullptr), occBufPhys);
        } else {
            return initializePageTable<level>(
                cpu, initializeArenaChain<level + 1>(cpu, occBufPhys));
        }
    }

    virt_addr arenaVirtualBase(size_t index) {
        constexpr virt_addr substrateBase = arch::pageTableDescriptor.canonicalizeVirtualAddress(
            virt_addr{static_cast<uint64_t>(VMM_SUBSTRATE_ROOT_INDEX)
                      << arch::pageTableDescriptor.getVirtualAddressBitCount(pageTableLevelForKMemRegion() - 1)});
        return substrateBase + index * getKernelMemRegionSize();
    }

    // Reserves a free VA from the current CPU's arena. Returns the VA along
    // with the arenaBase used (so the caller can install the leaf PTE through
    // the self-ref shadow). Lazy-installs HW subtables as needed.
    static virt_addr reserveFreeVA(virt_addr& outArenaBase, arch::ProcessorID& outCPU) {
        const arch::ProcessorID cpu = arch::getCurrentProcessorID();
        const virt_addr arenaBase = arenaVirtualBase(static_cast<size_t>(cpu));
        outArenaBase = arenaBase;
        outCPU = cpu;

        while (true) {
            const size_t T = VMSubstrateHelper::descendToFreeLeaf(arenaBase);
            assert(T != SIZE_MAX, "VMSubstrate arena exhausted");

            // Touch any va within T's coverage to drive lazy HW install. This
            // must happen before claimLeafBit so that, if T_first of a freshly
            // installed leaf PT is T, the dirty-page bits get reserved before
            // we try to claim a bit.
            const virt_addr probeVA = arenaBase
                + T * VMSubstrateHelper::kBranchFactor * arch::smallPageSize;
            VMSubstrateHelper::ensureSubtableInstalled<pageTableLevelForKMemRegion()>(
                arenaBase, probeVA, cpu);

            const auto [bit, becameFull] = VMSubstrateHelper::claimLeafBit(arenaBase, T);
            if (bit < 0) continue;
            if (becameFull) VMSubstrateHelper::propagateEdge(arenaBase, T, false);

            return arenaBase
                + T * VMSubstrateHelper::kBranchFactor * arch::smallPageSize
                + static_cast<size_t>(bit) * arch::smallPageSize;
        }
    }

    void* allocPage() {
        using Flag = arch::PageEntryFlag;
        constexpr auto kFlags = Flag::Write | Flag::Global | Flag::NoExecute;

        virt_addr arenaBase{uint64_t{0}};
        arch::ProcessorID cpu{};
        const virt_addr va = reserveFreeVA(arenaBase, cpu);

        const phys_addr phys = PageAllocator::allocateSmallPage(cpu);
        auto& leafPTE = *reinterpret_cast<arch::PTE<VMSubstrateHelper::leafLevel>*>(
            VMSubstrateHelper::leafPTEAddrFor(arenaBase, va).value);
        leafPTE = arch::PTE<VMSubstrateHelper::leafLevel>::leafEntry(phys, kFlags);

        VMSubstrateHelper::setDirtyForOtherCPUs(va);
        arch::invlpg(va);

        return reinterpret_cast<void*>(va.value);
    }

    void* mapMMIOPage(phys_addr paddr) {
        assert(paddr.value % arch::smallPageSize == 0, "Misaligned MMIO physical address");
        using Flag = arch::PageEntryFlag;
        constexpr auto kFlags = Flag::Write | Flag::Global | Flag::NoExecute | Flag::CacheDisable;

        virt_addr arenaBase{uint64_t{0}};
        arch::ProcessorID cpu{};
        const virt_addr va = reserveFreeVA(arenaBase, cpu);
        (void)cpu;

        auto& leafPTE = *reinterpret_cast<arch::PTE<VMSubstrateHelper::leafLevel>*>(
            VMSubstrateHelper::leafPTEAddrFor(arenaBase, va).value);
        leafPTE = arch::PTE<VMSubstrateHelper::leafLevel>::leafEntry(paddr, kFlags);

        VMSubstrateHelper::setDirtyForOtherCPUs(va);
        arch::invlpg(va);

        return reinterpret_cast<void*>(va.value);
    }

    void freePage(void* ptr) {
        const virt_addr va{reinterpret_cast<uint64_t>(ptr)};
        const virt_addr arenaBase{
            roundDownToNearestMultiple(va.value, getKernelMemRegionSize())
        };

        // Clear the leaf PTE and recover the underlying phys page.
        auto& leafPTE = *reinterpret_cast<arch::PTE<VMSubstrateHelper::leafLevel>*>(
            VMSubstrateHelper::leafPTEAddrFor(arenaBase, va).value);
        const phys_addr phys = leafPTE.getPhysicalAddress();
        leafPTE = arch::PTE<VMSubstrateHelper::leafLevel>{};

        // Local TLB clear; remote CPUs will lazy-invlpg via SafePtr/dirty-bitmap.
        VMSubstrateHelper::setDirtyForOtherCPUs(va);
        arch::invlpg(va);

        PageAllocator::freeSmallPage(phys);

        // Multi-freer-safe radix update.
        const size_t offsetPages = (va.value - arenaBase.value) / arch::smallPageSize;
        const size_t T   = offsetPages / VMSubstrateHelper::kBranchFactor;
        const size_t bit = offsetPages % VMSubstrateHelper::kBranchFactor;
        const uint64_t mask = uint64_t{1} << bit;

        const uint64_t prev =
            VMSubstrateHelper::leafFreeBitmap(arenaBase, T).fetch_or(mask, ACQ_REL);
        if (prev == 0) {
            const uint8_t prevCount =
                VMSubstrateHelper::leafFreeWordCount(arenaBase, T).fetch_add(1, ACQ_REL);
            if (prevCount == 0)
                VMSubstrateHelper::propagateEdge(arenaBase, T, true);
        }
    }

    void* createArena(arch::ProcessorID cpu) {
        LockGuard arenaGuard(arenaCreationLock);

        // 1. Allocate the occupancy-buffer's physical pages.
        phys_addr occBufPhys[VMSubstrateHelper::kOccupancyBufferPages];
        for (size_t i = 0; i < VMSubstrateHelper::kOccupancyBufferPages; i++)
            occBufPhys[i] = PageAllocator::allocateSmallPage(cpu);

        // 2. Build the HW chain. The leaf level installs both the per-CPU
        //    dirty-bitmap pages and the occupancy-buffer pages.
        const phys_addr topAddr =
            initializeArenaChain<pageTableLevelForKMemRegion()>(cpu, occBufPhys);

        // 3. Publish the arena.
        using Flag = arch::PageEntryFlag;
        constexpr auto kSubtableFlags = Flag::Write | Flag::Global | Flag::NoExecute;
        const size_t index = freeArenaIndex.fetch_add(1, RELAXED);
        vmmArenaTable[index] =
            arch::PTE<pageTableLevelForKMemRegion() - 1>::subtableEntry(topAddr, kSubtableFlags);
        const virt_addr arenaBase = arenaVirtualBase(index);

        // 4. The arena's VAs were unmapped before step 3; flush any negative
        //    TLB entries on this CPU for the buffer pages we're about to
        //    write through.
        for (size_t i = 0; i < VMSubstrateHelper::kOccupancyBufferPages; i++)
            arch::invlpg(VMSubstrateHelper::occupancyBufferBase(arenaBase)
                         + i * arch::smallPageSize);

        // 5. Seed the radix tree to "everything available".
        VMSubstrateHelper::seedAvailableState(arenaBase);

        // 6. Reserve the bits for non-allocatable VAs (self-ref shadow + dirty + buffer).
        VMSubstrateHelper::reserveBootBits(arenaBase);

        return reinterpret_cast<void*>(arenaBase.value);
    }

    void ensureTLBEntryFresh(void* ptr) {
        const auto ptrAddr = reinterpret_cast<uint64_t>(ptr);
        const auto tableBase = roundDownToNearestMultiple(ptrAddr, arch::bigPageSize);
        const size_t k_abs = (ptrAddr - tableBase) / arch::smallPageSize;
        const size_t myCPU = arch::getCurrentProcessorID();
        using LPT = VMSubstrateHelper::LeafPageTableWrapper;
        const size_t dw = LPT::dirtyCPUWord(myCPU);
        const uint64_t bit = LPT::dirtyCPUBit(myCPU);
        auto& dirtyEntry = *reinterpret_cast<Atomic<uint64_t>*>(
            tableBase + dw * arch::smallPageSize + k_abs * sizeof(uint64_t));
        if (dirtyEntry.load(ACQUIRE) & bit) {
            arch::invlpg(virt_addr{ptrAddr});
            dirtyEntry.fetch_and(~bit, RELAXED);
        }
    }

    bool init() {
        using Flag = arch::PageEntryFlag;
        constexpr auto kSubtableFlags = Flag::Write | Flag::Global | Flag::NoExecute;
        bootPageTable[VMM_SUBSTRATE_ROOT_INDEX] = arch::PTE<0>::subtableEntry(
            early_boot_virt_to_phys(virt_addr(&vmmArenaTable)),
            kSubtableFlags);
        for (size_t i = 0; i < arch::processorCount(); i++) {
            createArena(static_cast<arch::ProcessorID>(i));
        }
        arch::flushTLB();
        return true;
    }
}
