//
// Created by Spencer Martin on 2/4/26.
//

#include <mem/PageAllocator.h>
#include <core/math.h>
#include <core/ds/Trees.h>
#include <core/algo/sort.h>
#include <mem/NUMA.h>

constexpr size_t PA_BITPOOL_RELAXED_RETRIES = 16;
constexpr size_t PA_BITPOOL_DETERMINED_RETRIES = 1000000;

constexpr size_t bigPagesInRange(const kernel::mm::phys_memory_range range) {
    const auto alignedTop = roundUpToNearestMultiple(range.end.value, static_cast<uint64_t>(arch::bigPageSize));
    const auto alignedBottom = roundDownToNearestMultiple(range.start.value, static_cast<uint64_t>(arch::bigPageSize));
    return (alignedTop - alignedBottom)/arch::bigPageSize;
}

// ================= Bootstrap Allocator ==================

using namespace kernel;

BootstrapAllocator::BootstrapAllocator() : current(nullptr), end(nullptr), measuring(true) {}

BootstrapAllocator::BootstrapAllocator(void* buffer, size_t size)
        : current(static_cast<uint8_t *>(buffer)),
          end(static_cast<uint8_t *>(buffer) + size),
          measuring(false) {}

template<typename T>
T* BootstrapAllocator::allocate() {
    return allocate<T>(1);
}

template<typename T>
T* BootstrapAllocator::allocate(const size_t count, size_t alignment) {
    const auto addr = reinterpret_cast<size_t>(current);
    const size_t aligned = roundUpToNearestMultiple(addr, alignment);
    const size_t paddedObjSize = roundUpToNearestMultiple(sizeof(T), alignof(T));
    const size_t size = paddedObjSize * (count - 1) + sizeof(T);

    if (measuring) {
        current = reinterpret_cast<uint8_t *>(aligned + size);
        return nullptr;
    }

    current = reinterpret_cast<uint8_t *>(aligned);
    T* result = static_cast<T *>(static_cast<void*>(current));
    current += size;
    memset(static_cast<void *>(result), 0, size);

    assert(current <= end, "Bootstrap allocator overflow");
    return result;
}

template<typename T>
T* BootstrapAllocator::allocate(FunctionRef<void(T&)> init, const size_t count, size_t alignment) {
    T* result = allocate<T>(count, alignment);
    if (!measuring) {
        for (size_t i = 0; i < count; i++) {
            init(result[i]);
        }
    }
    return result;
}

size_t BootstrapAllocator::bytesNeeded() const {
    return reinterpret_cast<size_t>(current);
}

size_t BootstrapAllocator::bytesRemaining() const {
    return measuring ? 0ul : static_cast<size_t>(end - current);
}

// ======================= PageRef ========================
mm::phys_addr PageRef::addr() const {
    return mm::phys_addr{value & ~(arch::smallPageSize - 1)};
}

mm::PageSize PageRef::size() const {
    return (value & 1) ? mm::PageSize::BIG : mm::PageSize::SMALL;
}

PageRef PageRef::small(mm::phys_addr addr) {
    assert(addr.value % arch::smallPageSize == 0, "Physical address is not small page aligned");
    return {addr.value};
}

PageRef PageRef::big(mm::phys_addr addr) {
    assert(addr.value % arch::bigPageSize == 0, "Physical address is not big page aligned");
    return {addr.value | 1};
}

static_assert(mm::PageAllocator::smallPagesPerBigPage * 2 <= arch::smallPageSize, "Can't smuggle run length in bottom of PageRef");

constexpr uint64_t pageRefRunMask = arch::smallPageSize - 2; //mask off all lower bits except for bottom

// ==================== SmallPageAllocator ====================

SmallPageAllocator::SmallPageAllocator(mm::phys_addr b) : baseAddr(b) {
    // All pages start available in the alloc bitmap.
    // freeBitmap and allocatedCount are zero-initialized by BootstrapAllocator's memset.
    for (auto& w : allocBitmap) w = ~0ull;
}

OccupancyState SmallPageAllocator::stateFromCount(const size_t count, const size_t maxAlloc) {
    if (count == 0) return OccupancyState::Empty;
    if (count >= maxAlloc) return OccupancyState::Full;
    return OccupancyState::Partial;
}

void SmallPageAllocator::allocAll() {
    assert(reservedCount == 0, "Can't allocate all from page with reserved subpages");
    for (auto& w : allocBitmap) w = 0;
    for (auto& w : freeBitmap)  w.store(0, RELEASE);
    allocatedCount.store(static_cast<SmallPageCount>(mm::PageAllocator::smallPagesPerBigPage), RELEASE);
}

void SmallPageAllocator::freeAll() {
    assert(reservedCount == 0, "Can't free all from page with reserved subpages");
    for (auto& w : allocBitmap) w = ~0ull;
    for (auto& w : freeBitmap)  w.store(0, RELEASE);
    allocatedCount.store(0, RELEASE);
}

mm::phys_addr SmallPageAllocator::fromPageIndex(SmallPageIndex index) const {
    return baseAddr + index * arch::smallPageSize;
}

void SmallPageAllocator::flushAllocBitmap() {
    // Move any unclaimed allocBitmap pages into freeBitmap so the next alloc CPU
    // can find them after picking this big page up from the pool.
    for (size_t w = 0; w < bitmapWordCount; w++) {
        if (allocBitmap[w]) {
            freeBitmap[w].fetch_or(allocBitmap[w], RELEASE);
            allocBitmap[w] = 0;
        }
    }
    allocHint = 0;
}

size_t SmallPageAllocator::alloc(PageAllocationCallback cb, size_t count, OccupancyTransition& transition) {
    size_t allocated = 0;
    const size_t maxAlloc = mm::PageAllocator::smallPagesPerBigPage - reservedCount;

    // Pass 0: scan allocBitmap starting at allocHint (zero atomics).
    // Pass 1: drain freeBitmap into allocBitmap, reset allocHint, scan again.
    for (int pass = 0; pass < 2 && allocated < count; pass++) {
        if (pass == 1) {
            size_t newHint = bitmapWordCount;
            for (size_t w = 0; w < bitmapWordCount; w++) {
                const uint64_t freed = freeBitmap[w].exchange(0ull, ACQ_REL);
                if (freed) {
                    allocBitmap[w] = freed;
                    if (newHint == bitmapWordCount) newHint = w;
                }
            }
            if (newHint == bitmapWordCount) break;  // truly nothing left
            allocHint = static_cast<uint8_t>(newHint);
        }

        for (size_t w = allocHint; w < bitmapWordCount && allocated < count; w++) {
            while (allocBitmap[w] && allocated < count) {
                const int bit = __builtin_ctzll(allocBitmap[w]);
                allocBitmap[w] &= allocBitmap[w] - 1;  // clear lowest set bit
                cb(PageRef::small(fromPageIndex(static_cast<SmallPageIndex>(w * 64u + static_cast<unsigned>(bit)))));
                allocated++;
            }
            if (!allocBitmap[w] && w == allocHint) allocHint = static_cast<uint8_t>(w + 1);
        }
    }

    const size_t prevAllocated = allocatedCount.fetch_add(static_cast<SmallPageCount>(allocated), ACQ_REL);
    transition.before = stateFromCount(prevAllocated, maxAlloc);
    transition.after  = stateFromCount(static_cast<size_t>(prevAllocated) + allocated, maxAlloc);
    return allocated;
}

void SmallPageAllocator::free(PageRef* pages, size_t count, OccupancyTransition& transition) {
    const size_t maxAlloc = mm::PageAllocator::smallPagesPerBigPage - reservedCount;

    uint64_t pending[bitmapWordCount] = {};
    for (size_t i = 0; i < count; i++) {
        const auto addrRaw = pages[i].addr().value;
        assert((addrRaw & ~(arch::bigPageSize - 1)) == baseAddr.value,
               "Tried to free small page in wrong small page allocator");
        const SmallPageIndex pageIndex =
            static_cast<SmallPageIndex>((addrRaw / arch::smallPageSize) % mm::PageAllocator::smallPagesPerBigPage);
        pending[pageIndex / 64] |= 1ull << (pageIndex % 64);
    }

    for (size_t w = 0; w < bitmapWordCount; w++) {
        if (!pending[w]) continue;
        assert(!(atomic_load(allocBitmap[w]) & pending[w]), "Double free: page is already in allocBitmap");
        const uint64_t old = freeBitmap[w].fetch_or(pending[w], RELEASE);
        assert(!(old & pending[w]), "Double free: page is already in freeBitmap");
    }

    const size_t prevAllocated = allocatedCount.fetch_sub(static_cast<SmallPageCount>(count), ACQ_REL);
    transition.before = stateFromCount(prevAllocated, maxAlloc);
    transition.after  = stateFromCount(static_cast<size_t>(prevAllocated) - count, maxAlloc);
}

bool SmallPageAllocator::isPageFree(PageRef page) const {
    const auto addrRaw = page.addr().value;
    const SmallPageIndex index =
        static_cast<SmallPageIndex>((addrRaw / arch::smallPageSize) % mm::PageAllocator::smallPagesPerBigPage);
    const size_t w     = index / 64;
    const uint64_t bit = 1ull << (index % 64);
    return (allocBitmap[w] & bit) || (freeBitmap[w].load(ACQUIRE) & bit);
}

bool SmallPageAllocator::isEmpty() const {
    return allocatedCount.load(ACQUIRE) == 0 && reservedCount == 0;
}

bool SmallPageAllocator::isFull() const {
    return allocatedCount.load(ACQUIRE) ==
           static_cast<SmallPageCount>(mm::PageAllocator::smallPagesPerBigPage - reservedCount);
}

size_t SmallPageAllocator::freePageCount() const {
    return (mm::PageAllocator::smallPagesPerBigPage - reservedCount)
           - static_cast<size_t>(allocatedCount.load(ACQUIRE));
}

void SmallPageAllocator::reservePage(kernel::mm::phys_addr addr) {
    assert(allocatedCount.load(RELAXED) == 0, "Can only reserve pages during memory allocator init");
    assert(addr.value % arch::smallPageSize == 0, "Address must be page aligned");
    const SmallPageIndex index =
        static_cast<SmallPageIndex>((addr.value / arch::smallPageSize) % mm::PageAllocator::smallPagesPerBigPage);
    const size_t w     = index / 64;
    const uint64_t bit = 1ull << (index % 64);
    if (allocBitmap[w] & bit) {
        allocBitmap[w] &= ~bit;
        reservedCount++;
    }
}

#ifdef CROCOS_TESTING
size_t SmallPageAllocator::getBitmapPopcount() const {
    size_t total = 0;
    for (size_t w = 0; w < bitmapWordCount; w++) {
        total += static_cast<size_t>(__builtin_popcountll(allocBitmap[w]));
        total += static_cast<size_t>(__builtin_popcountll(freeBitmap[w].load(RELAXED)));
    }
    return total;
}

bool SmallPageAllocator::checkInvariants() const {
    return getBitmapPopcount() + getAllocatedCount() + getReservedCount()
           == mm::PageAllocator::smallPagesPerBigPage;
}
#endif

// ==================== BigPageMetadata ====================

BigPageMetadata::BigPageMetadata(NUMAPool& pool, mm::phys_addr baseAddr)
    : subpageAllocator(baseAddr), ownerPool(&pool) {}

size_t BigPageMetadata::allocatePages(size_t smallPageCount, PageAllocationCallback cb, OccupancyTransition& transition) {
    assert(allocHolder.load() != SIZE_MAX, "Allocating from big page without setting alloc holder");
    assert(allocHolder.load() == static_cast<size_t>(arch::getCurrentProcessorID()), "Two CPUs allocating from same page simultaneously");
    size_t allocated = 0;
    while (allocated < smallPageCount) {
        allocated += subpageAllocator.alloc(cb, smallPageCount - allocated, transition);
        if (transition.becameFull()) {
            break;
        }
    }
    return allocated;
}

void BigPageMetadata::freePages(PageRef *pages, size_t count, OccupancyTransition& transition) {
    subpageAllocator.free(pages, count, transition);
}

bool BigPageMetadata::isEmpty() const {
    return subpageAllocator.isEmpty();
}

bool BigPageMetadata::isFull() const {
    return subpageAllocator.isFull();
}

// ==================== NUMAPool ====================

NUMAPool::NUMAPool(BigPageMetadata* metadataBuffer,
                   BigPageMetadata** freeBuffer,
                   Atomic<size_t>* wgc,
                   Atomic<size_t>* rgc,
                   AtomicBitPool&& paPagesBitPool,
                   SubrangeInfo* subrangeBuffer,
                   size_t numSubranges,
                   size_t totalBigPageCount,
                   kernel::numa::DomainID domain)
    : bigPageMetadataBuffer(metadataBuffer),
      freeBigPages(freeBuffer, totalBigPageCount, wgc, rgc),
      paPages(move(paPagesBitPool)),
      associatedDomain(domain),
      subrangeInfo(subrangeBuffer),
      subrangeCount(numSubranges),
      bigPageCount(totalBigPageCount)
{
    // Separate fully-free pages from partially-reserved pages.
    // Pages with reserved subpages cannot be handed out as whole big pages, so
    // they belong in the PA bitpool for small-page allocation rather than in the
    // free ring buffer.
    size_t freePagesCount = 0;
    for (size_t i = 0; i < totalBigPageCount; i++) {
        if (!bigPageMetadataBuffer[i].hasReservedSubpages()) {
            freePagesCount++;
        }
    }

    // Publish only fully-free big pages in the ring buffer.
    size_t freeIdx = 0;
    freeBigPages.bulkWrite(freePagesCount, [&](size_t, BigPageMetadata*& slot) {
        while (bigPageMetadataBuffer[freeIdx].hasReservedSubpages()) {
            freeIdx++;
        }
        slot = &bigPageMetadataBuffer[freeIdx++];
    });

    // Mark partially-reserved pages in the PA bitpool so they are available
    // for small-page allocation.
    for (size_t i = 0; i < totalBigPageCount; i++) {
        if (bigPageMetadataBuffer[i].hasReservedSubpages()) {
            paPages.add(i);
        }
    }
}

BigPageMetadata* NUMAPool::findMetadata(mm::phys_addr addr) {
    const uint64_t addrAligned = roundDownToNearestMultiple(addr.value, static_cast<uint64_t>(arch::bigPageSize));
    for (size_t i = 0; i < subrangeCount; i++) {
        const SubrangeInfo& sr = subrangeInfo[i];
        if (addrAligned >= sr.rangeStart.value && addrAligned < sr.rangeEnd.value) {
            const size_t bigPageIndex = static_cast<size_t>((addrAligned - sr.rangeStart.value) / arch::bigPageSize);
            return &sr.metadataBase[bigPageIndex];
        }
    }
    return nullptr;
}

void BigPageMetadata::returnPage(bool evictedAsFull) {
    subpageAllocator.flushAllocBitmap();
    releaseAllocHolder();
    getOwnerPool().returnPage(*this, evictedAsFull);
}

void NUMAPool::reserveRange(mm::phys_memory_range range) {
    for (size_t i = 0; i < bigPageCount; i++) {
        auto& meta = bigPageMetadataBuffer[i];
        const mm::phys_addr bigStart = meta.baseAddr();
        const mm::phys_addr bigEnd{bigStart.value + arch::bigPageSize};

        if (range.end.value <= bigStart.value || range.start.value >= bigEnd.value) continue;

        const uint64_t alignedStart = roundDownToNearestMultiple(
            max(range.start.value, bigStart.value), static_cast<uint64_t>(arch::smallPageSize));
        const uint64_t alignedEnd = roundUpToNearestMultiple(
            min(range.end.value, bigEnd.value), static_cast<uint64_t>(arch::smallPageSize));

        for (uint64_t addr = alignedStart; addr < alignedEnd; addr += arch::smallPageSize) {
            meta.reservePage(mm::phys_addr{addr});
        }
    }
    fixupAfterReserveRange();
}

void NUMAPool::fixupAfterReserveRange() {
    // Drain freeBigPages (discard — we rebuild from metadata below).
    freeBigPages.bulkReadBestEffort(bigPageCount, [](size_t, BigPageMetadata*) {});

    // Rebuild freeBigPages and paPages from the current metadata state.
    size_t newFreeCount = 0;
    for (size_t i = 0; i < bigPageCount; i++) {
        auto& meta = bigPageMetadataBuffer[i];
        if (!meta.hasReservedSubpages()) {
            newFreeCount++;
        } else if (!meta.isFull()) {
            (void)paPages.add(i);   // Idempotent: OK if already present.
        } else {
            (void)paPages.remove(i); // All subpages reserved; remove if present.
        }
    }

    size_t freeIdx = 0;
    freeBigPages.bulkWrite(newFreeCount, [&](size_t, BigPageMetadata*& slot) {
        while (bigPageMetadataBuffer[freeIdx].hasReservedSubpages()) freeIdx++;
        slot = &bigPageMetadataBuffer[freeIdx++];
    });
}

#ifdef CROCOS_TESTING
#include <kernel.h>
bool NUMAPool::checkInvariants() const {
    size_t emptyInPA = 0;
    for (size_t i = 0; i < bigPageCount; i++) {
        if (isInPAPages(i)) {
            const BigPageMetadata& meta = bigPageMetadataBuffer[i];
            if (meta.isFull()) return false;
            if (meta.isEmpty()) emptyInPA++;
        }
    }
    if (emptyInPA > 0)
        kernel::klog() << "  [NUMAPool::checkInvariants] warning: " << static_cast<uint64_t>(emptyInPA) << " empty page(s) in paPages\n";
    if (getFreeBigPageCount() + getPAPagesCount() > bigPageCount) return false;
    return true;
}

size_t NUMAPool::countTotalFreePages() const {
    size_t total = 0;
    for (size_t i = 0; i < bigPageCount; i++) {
        total += bigPageMetadataBuffer[i].freeSubpageCount();
    }
    return total;
}
#endif


// ==================== Page Allocator Factory ====================

namespace {

// Maximum ranges per NUMA domain we handle. In practice this is 1-3.
constexpr size_t MAX_RANGES_PER_DOMAIN = 64;

// A merged range: big-page-aligned bounds + which original ranges it covers.
struct TempMergedRange {
    mm::phys_addr start;      // bigPageAlignDown of merged range
    mm::phys_addr end;        // bigPageAlignUp of merged range
    size_t firstOrigIdx;      // inclusive index into sorted input
    size_t lastOrigIdx;       // inclusive index into sorted input
    size_t bigPageCount;
};

// Sort a copy of the input ranges by start address, then compute merged ranges.
// Returns the number of merged ranges written to `out`.
size_t computeMergedRanges(const Vector<mm::phys_memory_range>& ranges,
                            TempMergedRange* out, size_t maxOut,
                            mm::phys_memory_range* sortedOut) {
    const size_t n = ranges.size();
    assert(n <= MAX_RANGES_PER_DOMAIN, "Too many memory ranges for NUMA domain");
    assert(n <= maxOut, "Not enough space for merged ranges");

    if (n == 0) return 0;

    for (size_t i = 0; i < n; i++) sortedOut[i] = ranges[i];

    auto comp = [](const mm::phys_memory_range& a, const mm::phys_memory_range& b) {
        return a.start.value < b.start.value;
    };
    if (n > 1) algorithm::sort(sortedOut, n, comp);

    size_t count = 0;
    mm::phys_addr mergedStart{roundDownToNearestMultiple(sortedOut[0].start.value,
                                                          static_cast<uint64_t>(arch::bigPageSize))};
    mm::phys_addr mergedEnd  {roundUpToNearestMultiple  (sortedOut[0].end.value,
                                                          static_cast<uint64_t>(arch::bigPageSize))};
    size_t firstIdx = 0;

    for (size_t i = 1; i < n; i++) {
        const mm::phys_addr nextBigPageStart{
            roundDownToNearestMultiple(sortedOut[i].start.value,
                                       static_cast<uint64_t>(arch::bigPageSize))};

        if (nextBigPageStart.value < mergedEnd.value) {
            // This range starts within a big page already covered — extend the merge.
            const mm::phys_addr candidate{
                roundUpToNearestMultiple(sortedOut[i].end.value,
                                         static_cast<uint64_t>(arch::bigPageSize))};
            if (candidate.value > mergedEnd.value) mergedEnd = candidate;
        } else {
            // Emit current merged range.
            assert(count < maxOut, "Merged range overflow");
            out[count++] = { mergedStart, mergedEnd, firstIdx, i - 1,
                             (mergedEnd.value - mergedStart.value) / arch::bigPageSize };
            mergedStart = nextBigPageStart;
            mergedEnd   = mm::phys_addr{roundUpToNearestMultiple(sortedOut[i].end.value,
                                                                  static_cast<uint64_t>(arch::bigPageSize))};
            firstIdx    = i;
        }
    }
    // Emit final merged range.
    assert(count < maxOut, "Merged range overflow");
    out[count++] = { mergedStart, mergedEnd, firstIdx, n - 1,
                     (mergedEnd.value - mergedStart.value) / arch::bigPageSize };
    return count;
}

// Reserve all small pages in [reserveStart, reserveEnd) within the given BigPageMetadata.
// reserveStart and reserveEnd must lie within the big page's bounds.
void reserveSmallPageRange(BigPageMetadata& meta,
                            mm::phys_addr reserveStart, mm::phys_addr reserveEnd) {
    assert(reserveStart.value % arch::smallPageSize == 0, "reserveStart not small-page aligned");
    assert(reserveEnd.value   % arch::smallPageSize == 0, "reserveEnd not small-page aligned");
    for (mm::phys_addr p = reserveStart; p.value < reserveEnd.value; p += arch::smallPageSize) {
        meta.reservePage(p);
    }
}

} // namespace

NUMAPool* createNumaPool(BootstrapAllocator& alloc,
                          const Vector<mm::phys_memory_range>& ranges,
                          kernel::numa::DomainID domain) {
    // --- Phase 1: compute merged ranges (same in both measure and real mode) ---
    TempMergedRange merged[MAX_RANGES_PER_DOMAIN];
    mm::phys_memory_range sorted[MAX_RANGES_PER_DOMAIN];
    const size_t mergedCount = computeMergedRanges(ranges, merged, MAX_RANGES_PER_DOMAIN, sorted);

    size_t totalBigPageCount = 0;
    for (size_t i = 0; i < mergedCount; i++) totalBigPageCount += merged[i].bigPageCount;
    assert(totalBigPageCount > 0, "createNumaPool: domain has no big pages");

    // --- Phase 2: allocate all structures from the bootstrap allocator ---
    NUMAPool* poolPtr = alloc.allocate<NUMAPool>();

    BigPageMetadata* metadata = alloc.allocate<BigPageMetadata>(totalBigPageCount);

    BigPageMetadata** freeBuffer = alloc.allocate<BigPageMetadata*>(totalBigPageCount);

    // Gen counter arrays for the ring buffer (ScanOnComplete = true).
    Atomic<size_t>* wgc = alloc.allocate<Atomic<size_t>>(totalBigPageCount);
    Atomic<size_t>* rgc = alloc.allocate<Atomic<size_t>>(totalBigPageCount);

    // BitPool backing storage, cache-line aligned.
    const size_t bitPoolBytes = AtomicBitPool::requiredBufferSize(totalBigPageCount, arch::CACHE_LINE_SIZE);
    void* bitPoolStorage = alloc.allocate<uint8_t>(bitPoolBytes, arch::CACHE_LINE_SIZE);

    SubrangeInfo* subranges = alloc.allocate<SubrangeInfo>(mergedCount);

    // --- Phase 3: populate structures (real mode only) ---
    if (!alloc.isFake()) {
        size_t metaIdx = 0;

        for (size_t mi = 0; mi < mergedCount; mi++) {
            const TempMergedRange& mr = merged[mi];
            BigPageMetadata* rangeMetaBase = &metadata[metaIdx];

            // Construct BigPageMetadata for each big page in this merged range.
            for (size_t bpIdx = 0; bpIdx < mr.bigPageCount; bpIdx++) {
                mm::phys_addr bigPageBase{mr.start.value + bpIdx * arch::bigPageSize};
                new (&metadata[metaIdx + bpIdx]) BigPageMetadata(*poolPtr, bigPageBase);
            }

            // Reserve pages in each big page based on which portions are outside
            // any original range (alignment gaps) or in gaps between original ranges.
            for (size_t bpIdx = 0; bpIdx < mr.bigPageCount; bpIdx++) {
                mm::phys_addr bigPageStart{mr.start.value + bpIdx * arch::bigPageSize};
                mm::phys_addr bigPageEnd  {bigPageStart.value + arch::bigPageSize};
                BigPageMetadata& meta = metadata[metaIdx + bpIdx];

                // Walk original ranges within this merged range, tracking covered intervals.
                mm::phys_addr prevEnd = bigPageStart; // last covered address within this big page

                for (size_t ri = mr.firstOrigIdx; ri <= mr.lastOrigIdx; ri++) {
                    const mm::phys_memory_range& orig = sorted[ri];

                    // Clamp original range to this big page's extent.
                    const mm::phys_addr origClampedStart{
                        orig.start.value > bigPageStart.value ? orig.start.value : bigPageStart.value};
                    const mm::phys_addr origClampedEnd{
                        orig.end.value < bigPageEnd.value ? orig.end.value : bigPageEnd.value};

                    if (origClampedStart.value >= bigPageEnd.value) break;  // past this big page
                    if (origClampedEnd.value   <= bigPageStart.value) continue; // before this big page

                    // Reserve the gap between prevEnd and the start of this original range.
                    if (prevEnd.value < origClampedStart.value) {
                        reserveSmallPageRange(meta, prevEnd, origClampedStart);
                    }
                    if (origClampedEnd.value > prevEnd.value) {
                        prevEnd = origClampedEnd;
                    }
                }

                // Reserve tail gap (from last covered address to end of big page).
                if (prevEnd.value < bigPageEnd.value) {
                    reserveSmallPageRange(meta, prevEnd, bigPageEnd);
                }
            }

            // Populate SubrangeInfo for this merged range.
            subranges[mi].rangeStart   = mr.start;
            subranges[mi].rangeEnd     = mr.end;
            subranges[mi].metadataBase = rangeMetaBase;

            metaIdx += mr.bigPageCount;
        }

        // Construct the AtomicBitPool and move it into the NUMAPool constructor.
        AtomicBitPool paPages(totalBigPageCount, bitPoolStorage, arch::CACHE_LINE_SIZE);

        // Initialize Atomic gen counters to 0 (they're raw memory from BootstrapAllocator).
        for (size_t i = 0; i < totalBigPageCount; i++) {
            new (&wgc[i]) Atomic<size_t>(0);
            new (&rgc[i]) Atomic<size_t>(0);
        }

        new (poolPtr) NUMAPool(metadata, freeBuffer, wgc, rgc,
                               move(paPages), subranges, mergedCount,
                               totalBigPageCount, domain);
    }

    return poolPtr;
}

LocalPool* createLocalPool(BootstrapAllocator& alloc, const kernel::numa::NUMATopology* topology, NUMAPool* homePool, arch::ProcessorID pid) {
    return alloc.allocate<LocalPool>([&](LocalPool& lp) { new(&lp) LocalPool(topology, homePool, pid); }, 1);
}

// ==================== PageAllocatorImpl ====================

BigPageMetadata* PageAllocatorImpl::findMetadata(mm::phys_addr addr) {
    // Binary search the domain table for the subrange containing addr.
    size_t lo = 0, hi = domainTableSize;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (addr.value < domainTable[mid].rangeStart.value) {
            hi = mid;
        } else if (addr.value >= domainTable[mid].rangeEnd.value) {
            lo = mid + 1;
        } else {
            return domainTable[mid].pool->findMetadata(addr);
        }
    }
    return nullptr;
}

// Static storage for the domain lookup table. Built once at boot.
static NUMADomainEntry gDomainTable[512];
static NUMAPool*       gNumaPoolStorage[arch::MAX_PROCESSOR_COUNT];

PageAllocatorImpl createPageAllocator(Vector<NUMAPool*>&& numaPools, LocalPool** localPools,
                                       size_t processorCount,
                                       NUMAPool* unownedPool,
                                       const kernel::numa::NUMAPolicy* numaPolicy) {
    // Copy pool pointers into stable static storage.
    // numDomains == maxDomainID + 1; slots for empty domains hold nullptr.
    const size_t numDomains = numaPools.size();
    for (size_t i = 0; i < numDomains; i++) gNumaPoolStorage[i] = numaPools[i];

    // Build the global domain table: one entry per subrange (merged range) per pool.
    // Skip nullptr slots (domains with no physical memory).
    size_t tableSize = 0;
    for (size_t pi = 0; pi < numDomains; pi++) {
        NUMAPool* pool = gNumaPoolStorage[pi];
        if (pool == nullptr) continue;
        for (size_t si = 0; si < pool->getSubrangeCount(); si++) {
            assert(tableSize < 512, "Domain table overflow");
            gDomainTable[tableSize++] = {
                pool->getSubranges()[si].rangeStart,
                pool->getSubranges()[si].rangeEnd,
                pool
            };
        }
    }

    // Sort the domain table by rangeStart for binary search.
    if (tableSize > 1) {
        auto comp = [](const NUMADomainEntry& a, const NUMADomainEntry& b) {
            return a.rangeStart.value < b.rangeStart.value;
        };
        algorithm::sort(gDomainTable, tableSize, comp);
    }

    PageAllocatorImpl impl{gNumaPoolStorage, numDomains, localPools, gDomainTable, tableSize, unownedPool, numaPolicy, {}};

    // Precompute cpuNearestPool: for each CPU, record the nearest non-null pool.
    if (numaPolicy != nullptr) {
        // Walk the policy's domain fallback order for each CPU.
        for (size_t p = 0; p < processorCount; p++) {
            const auto home = numaPolicy->homeDomain(static_cast<arch::ProcessorID>(p));
            for (const auto domainID : numaPolicy->domainOrder(home)) {
                if (domainID.value < numDomains && gNumaPoolStorage[domainID.value] != nullptr) {
                    impl.cpuNearestPool[p] = domainID;
                    break;
                }
            }
        }
    } else {
        // Trivial single-domain topology: enforce exactly one pool with no unowned pool,
        // then point every CPU at it so that nearestPool() works without a policy.
        assert(unownedPool == nullptr,
               "createPageAllocator: unownedPool must be null when numaPolicy is null");
        size_t singleDomainIdx = numDomains; // sentinel — set below
        for (size_t i = 0; i < numDomains; i++) {
            if (gNumaPoolStorage[i] != nullptr) {
                assert(singleDomainIdx == numDomains,
                       "createPageAllocator: more than one NUMAPool provided with null numaPolicy");
                singleDomainIdx = i;
            }
        }
        assert(singleDomainIdx != numDomains,
               "createPageAllocator: no NUMAPool provided with null numaPolicy");
        const auto singleDomain = kernel::numa::DomainID{static_cast<uint16_t>(singleDomainIdx)};
        for (size_t p = 0; p < processorCount; p++) {
            impl.cpuNearestPool[p] = singleDomain;
        }
    }

    return impl;
}

void PageAllocatorImpl::reserveRange(mm::phys_memory_range range) {
    for (size_t i = 0; i < numDomains; i++) {
        if (numaPools[i] != nullptr) {
            numaPools[i]->reserveRange(range);
        }
    }
    if (unownedPool != nullptr) {
        unownedPool->reserveRange(range);
    }
}

size_t PageAllocatorImpl::allocatePages(size_t smallPageCount, PageAllocationCallback cb, kernel::numa::DomainID targetDomain, AllocFlags flags) {
    size_t allocatedPages = 0;

    const auto allocFromPool = [&](NUMAPool& pool) {
        if (smallPageCount == 0) return;
        BigPageMetadata* extras = nullptr;
        const auto allocCount = pool.allocatePages(smallPageCount, cb, extras, flags);
        smallPageCount -= min(allocCount, smallPageCount);
        allocatedPages += allocCount;
        // extras has its allocHolder already released by NUMAPool; returnPage flushes
        // allocBitmap → freeBitmap and routes it back to paPages or freeBigPages.
        if (extras != nullptr) {
            extras->returnPage();
        }
    };

    if (numaPolicy != nullptr) {
        if (flags.has(AllocBehavior::LOCAL_DOMAIN_ONLY)) {
            if (targetDomain.value < numDomains && numaPools[targetDomain.value] != nullptr) {
                allocFromPool(*numaPools[targetDomain.value]);
            }
        } else {
            for (const auto domain : numaPolicy->domainOrder(targetDomain)) {
                if (smallPageCount == 0) break;
                if (domain.value < numDomains && numaPools[domain.value] != nullptr) {
                    allocFromPool(*numaPools[domain.value]);
                }
            }
        }
    } else {
        allocFromPool(nearestPool(static_cast<arch::ProcessorID>(0)));
    }

    if (smallPageCount > 0 && unownedPool != nullptr) {
        allocFromPool(*unownedPool);
    }

    if (!flags.has(AllocBehavior::GRACEFUL_OOM) && smallPageCount != 0) {
        assertNotReached("Panic!!! Page allocator is out of memory");
    }

    return allocatedPages;
}

size_t PageAllocatorImpl::allocatePages(size_t smallPageCount, PageAllocationCallback cb, arch::ProcessorID targetProc, AllocFlags flags) {
    const kernel::numa::DomainID targetDomain = (numaPolicy != nullptr)
        ? numaPolicy->homeDomain(targetProc)
        : cpuNearestPool[targetProc];
    return allocatePages(smallPageCount, cb, targetDomain, flags);
}

size_t PageAllocatorImpl::allocateFast(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags) {
    if (!flags.has(AllocBehavior::BIG_PAGE_ONLY) && smallPageCount < mm::PageAllocator::smallPagesPerBigPage) {
        const auto pid = arch::getCurrentProcessorID();
        auto& localPool = *localPools[pid];
        return localPool.allocatePages(smallPageCount, cb, flags | AllocBehavior::LOCAL_DOMAIN_ONLY);
    }
    return 0;
}

size_t PageAllocatorImpl::allocatePages(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags) {
    const auto fastAllocs = allocateFast(smallPageCount, cb, flags);
    if (fastAllocs != smallPageCount) {
        return fastAllocs + allocateFallback(smallPageCount - fastAllocs, cb, flags);
    }
    return fastAllocs;
}

size_t PageAllocatorImpl::allocateFallback(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags) {
    size_t allocatedPages = 0;
    const auto pid = arch::getCurrentProcessorID();
    auto& localPool = *localPools[pid];

    const auto allocFromLocalPool = [&](const size_t count, const AllocFlags f) {
        const auto allocCount = localPool.allocatePages(count, cb, f);
        smallPageCount -= allocCount;
        allocatedPages += allocCount;
    };

    const auto allocFromNumaPool = [&](const size_t count, NUMAPool& pool, const AllocFlags f) {
        BigPageMetadata* extras = nullptr;
        const auto allocCount = pool.allocatePages(count, cb, extras, f);
        //Make sure we don't underflow if we're allocating big pages only and the requested page count is not divisible
        //by smallPagesPerBigPage
        smallPageCount -= min(allocCount, smallPageCount);
        allocatedPages += allocCount;
        if (extras != nullptr) {
            localPool.tryGivePAPage(*extras);
        }
    };

    //If we're allowed to use small pages, then the best strategy is to first try allocating big pages from the nearest
    //NUMA pool, then try to fill in the remaining small pages using locally cached PA pages from that same NUMA pool
    if (!flags.has(AllocBehavior::BIG_PAGE_ONLY)) {
        allocFromNumaPool(roundDownToNearestMultiple(smallPageCount, mm::PageAllocator::smallPagesPerBigPage), nearestPool(pid), flags | AllocBehavior::BIG_PAGE_ONLY);
        allocFromLocalPool(smallPageCount, flags | AllocBehavior::LOCAL_DOMAIN_ONLY);
    }

    //Check if we're done before iterating through the various NUMA pools.
    if (smallPageCount == 0) {
        return allocatedPages;
    }

    //If we still need more pages, we'll begin by iterating over the NUMA domains in appropriate order - closest to furthest
    if (numaPolicy != nullptr) {
        const auto homeID = numaPolicy->homeDomain(pid);
        if (flags.has(AllocBehavior::LOCAL_DOMAIN_ONLY)) {
            allocFromNumaPool(smallPageCount, nearestPool(pid), flags);
        }
        else {
            for (const auto domain : numaPolicy->domainOrder(homeID)) {
                if (auto* poolPtr = numaPools[domain.value]) {
                    allocFromNumaPool(smallPageCount, *poolPtr, flags);
                }
                if (smallPageCount == 0) {
                    return allocatedPages;
                }
            }
        }
    } else {
        // Trivial single-domain topology: nearestPool() returns the one pool.
        // Make a final pass without BIG_PAGE_ONLY to satisfy any remaining small-page demand.
        allocFromNumaPool(smallPageCount, nearestPool(pid), flags);
    }

    if (!flags.has(AllocBehavior::BIG_PAGE_ONLY)) {
        allocFromLocalPool(smallPageCount, flags);
    }

    //As a last resort, if any pages don't belong to any sort of NUMA domain (due to a firmware misconfiguration)
    //try to allocate from the unowned pool
    if (unownedPool != nullptr) {
        allocFromNumaPool(smallPageCount, *unownedPool, flags);
    }

    if (!flags.has(AllocBehavior::GRACEFUL_OOM)) {
        if (smallPageCount != 0) {
            assertNotReached("Panic!!! Page allocator is out of memory");
        }
    }

    return allocatedPages;
}

size_t LocalPool::allocatePages(size_t smallPageCount, PageAllocationCallback cb, AllocFlags flags) {
    if (flags.has(AllocBehavior::BIG_PAGE_ONLY)) {
        return 0;
    }
    size_t allocatedPages = 0;
    const auto allocFromPAPage = [&](BigPageMetadata& metadata) {
        if (metadata.isEmpty()) {
            // Hysteresis: if this is the only cached page and it belongs to the home pool,
            // hold onto it rather than returning it to freeBigPages. This avoids the
            // paPages↔freeBigPages round-trip when the same page is repeatedly alloc/freed.
            // It will be released in tryGivePAPage when a fresh home-pool page arrives,
            // or immediately if paPage2 is occupied (a real page is already available).
            if (!(paPage2 == nullptr && homePool != nullptr && &metadata.getOwnerPool() == homePool)) {
                metadata.returnPage();
                return true;
            }
        }
        OccupancyTransition transition{};
        const auto allocd = metadata.allocatePages(smallPageCount, cb, transition);
        smallPageCount -= allocd;
        allocatedPages += allocd;
        if (transition.becameFull()) {
            metadata.returnPage(true);
            return true;
        }
        return false;
    };

    if (paPage1 && allocFromPAPage(*paPage1)) {
        paPage1 = paPage2;
        paPage2 = nullptr;
    }
    if (smallPageCount == 0) {
        return allocatedPages;
    }
    if (paPage1 && allocFromPAPage(*paPage1)) {
        paPage1 = nullptr;
    }

    return allocatedPages;
}

void LocalPool::tryGivePAPage(BigPageMetadata& page) {
    // Never hold new empty pages — they have nothing to give and would only occupy a slot.
    if (page.isEmpty()) {
        page.returnPage();
        return;
    }

    // Hysteresis release: if paPage1 is an empty home-pool page being held as a reservation,
    // release it now that a fresh home-pool page has arrived to replace it.
    if (paPage1 != nullptr && paPage1->isEmpty() && paPage2 == nullptr &&
        homePool != nullptr && &paPage1->getOwnerPool() == homePool &&
        &page.getOwnerPool() == homePool) {
        paPage1->returnPage();
        page.markAllocHolder(pid);
        paPage1 = &page;
        return;
    }

    // Without NUMA topology, accept pages with simple FIFO priority.
    if (topology == nullptr) {
        if (paPage1 == nullptr) { page.markAllocHolder(pid); paPage1 = &page; return; }
        if (paPage2 == nullptr) { page.markAllocHolder(pid); paPage2 = &page; return; }
        page.returnPage();
        return;
    }

    const kernel::numa::DomainID cpuDomain = topology->domainForCpu(pid).id;

    // Read latency from the CPU's home domain to a page's NUMA domain, used as
    // the proximity metric.  Falls back to UINT64_MAX when no latency data is
    // available, so unknown domains are treated as maximally far away.
    const auto distanceTo = [&](const BigPageMetadata& pg) -> uint64_t {
        const auto lat = topology->latencyBetween(cpuDomain, pg.getOwnerPool().domain());
        return lat.occupied() ? *lat : UINT64_MAX;
    };

    const uint64_t dNew = distanceTo(page);

    // Pool is empty — paPage2 is always null when paPage1 is null.
    if (paPage1 == nullptr) {
        page.markAllocHolder(pid);
        paPage1 = &page;
        return;
    }

    // One slot occupied — insert in closest-first order, no eviction needed.
    if (paPage2 == nullptr) {
        page.markAllocHolder(pid);
        if (dNew <= distanceTo(*paPage1)) {
            paPage2 = paPage1;
            paPage1 = &page;
        } else {
            paPage2 = &page;
        }
        return;
    }

    // Both slots occupied. Only accept the incoming page if it is strictly closer
    // than the current furthest-away slot; otherwise return it to its NUMA pool.
    const uint64_t d2 = distanceTo(*paPage2);
    if (dNew >= d2) {
        page.returnPage();
        return;
    }

    // The incoming page displaces paPage2. Re-sort so paPage1 remains the closer one.
    BigPageMetadata* toReturn = paPage2;
    page.markAllocHolder(pid);
    if (dNew <= distanceTo(*paPage1)) {
        paPage2 = paPage1;
        paPage1 = &page;
    } else {
        paPage2 = &page;
    }

    toReturn->returnPage();
}

void NUMAPool::returnPage(BigPageMetadata &metadata, const bool evictedAsFull) {
    //Returning a full page is a noop
    if (evictedAsFull || metadata.isFull()) {
        return;
    }
    if (metadata.isEmpty()) {
        freeBigPages.write(&metadata);
        return;
    }
    // NOTE: there is a benign TOCTOU window between the isEmpty() check above and
    // paPages.add() below. A concurrent free on another CPU can decrement allocatedCount
    // to zero (Partial→Empty) and call paPages.remove() — which returns NotPresent since
    // we haven't added yet — leaving the page unrouted. We then add an empty page to
    // paPages. The page is still fully allocatable for small-page requests and will
    // self-correct through normal allocation activity; the only consequence is that
    // BIG_PAGE_ONLY allocations cannot claim it until it is reclaimed.
    const auto index = metadataIndex(&metadata);
    paPages.add(index);
}

size_t NUMAPool::allocatePages(size_t smallPageCount, const PageAllocationCallback cb, BigPageMetadata *&paPageRemaining, const AllocFlags flags) {
    //If we can only allocate from big pages, we're forced to only allocate from the freeBigPages buffer
    if (flags.has(AllocBehavior::BIG_PAGE_ONLY)) {
        //Overallocate in case smallPageCount is not divisible by smallPagesPerBigPage
        const auto numBigPages = divideAndRoundUp(smallPageCount, mm::PageAllocator::smallPagesPerBigPage);
        const auto allocatedPages = freeBigPages.bulkReadBestEffort(numBigPages, [&](size_t, const auto& metadata) {
            const auto pageAddr = metadata -> baseAddr();
            metadata -> allocAll();
            cb(PageRef::big(pageAddr));
        });
        return allocatedPages * mm::PageAllocator::smallPagesPerBigPage;
    }
    //First try allocating as much as we can from big pages
    const auto numBigPages = divideAndRoundDown(smallPageCount, mm::PageAllocator::smallPagesPerBigPage);
    size_t allocatedPages = freeBigPages.bulkReadBestEffort(numBigPages, [&](size_t, const auto& metadata) {
        const auto pageAddr = metadata -> baseAddr();
        metadata -> allocAll();
        cb(PageRef::big(pageAddr));
    }) * mm::PageAllocator::smallPagesPerBigPage;

    smallPageCount -= allocatedPages;

    const auto pid = arch::getCurrentProcessorID();

    const auto allocateFromPAPages = [&](const size_t maxRetries) {
        while (smallPageCount > 0) {
            if (size_t paIndex; paPages.getAny(pid, paIndex, maxRetries) == AtomicBitPool::GetResult::Success) {
                auto& bigPage = bigPageMetadataBuffer[paIndex];
                bigPage.markAllocHolder(pid);
                OccupancyTransition stateChange {};
                while (true) {
                    const auto allocated = bigPage.allocatePages(smallPageCount, cb, stateChange);
                    assert(allocated > 0, "A page in the PAPage bitmap should never be full");
                    smallPageCount -= allocated;
                    allocatedPages += allocated;
                    if (stateChange.becameFull()) {
                        bigPage.releaseAllocHolder();
                        break;
                    }
                    if (smallPageCount == 0) {
                        paPageRemaining = &bigPage;
                        bigPage.releaseAllocHolder();
                        break;
                    }
                }
            }
            else {
                return;
            }
        }
    };

    allocateFromPAPages(PA_BITPOOL_RELAXED_RETRIES);
    while (smallPageCount > 0) {
        const auto requiredPages = divideAndRoundUp(smallPageCount, mm::PageAllocator::smallPagesPerBigPage);
        const auto bigPageDivisible = ((smallPageCount & (mm::PageAllocator::smallPagesPerBigPage - 1)) == 0);
        const auto grabbedPages = freeBigPages.bulkReadBestEffort(requiredPages, [&](size_t index, auto& metadata) {
            assert(metadata -> isEmpty(), "Big pages in the free pool should be FREE");
            if (index == 0 && !bigPageDivisible) {
                paPageRemaining = metadata;
            }
            else {
                const auto pageAddr = metadata -> baseAddr();
                metadata -> allocAll();
                cb(PageRef::big(pageAddr));
            }
        });
        if (grabbedPages == 0) {
            break;
        }
        if (bigPageDivisible) {
            // We successfully gave out grabbedPages as whole big pages!
            allocatedPages += grabbedPages * mm::PageAllocator::smallPagesPerBigPage;
            smallPageCount -= grabbedPages * mm::PageAllocator::smallPagesPerBigPage;

            // Continue the loop. If we fulfilled the whole request, smallPageCount
            // is now 0 and the while loop naturally terminates.
            continue;
        }
        if (grabbedPages < requiredPages) {
            const auto pageAddr = paPageRemaining -> baseAddr();
            paPageRemaining -> allocAll();
            cb(PageRef::big(pageAddr));
            paPageRemaining = nullptr;
            allocatedPages += grabbedPages * mm::PageAllocator::smallPagesPerBigPage;
            smallPageCount -= grabbedPages * mm::PageAllocator::smallPagesPerBigPage;
        }
        else {
            allocatedPages += (grabbedPages - 1) * mm::PageAllocator::smallPagesPerBigPage;
            smallPageCount -= (grabbedPages - 1) * mm::PageAllocator::smallPagesPerBigPage;
            paPageRemaining->markAllocHolder(pid);
            const auto smallAllocd = paPageRemaining -> allocatePages(smallPageCount, cb);
            paPageRemaining->releaseAllocHolder();
            smallPageCount -= smallAllocd;
            allocatedPages += smallAllocd;
            assert(smallPageCount == 0, "OK, we should only be here if we need less than a full big page, and yet the allocation wasn't totally fulfilled");
            return allocatedPages;
        }
    }
    allocateFromPAPages(PA_BITPOOL_DETERMINED_RETRIES);

    return allocatedPages;
}

void NUMAPool::freePages(PageRef *pages, size_t count) {
    const auto freeBigPageRun = [&](BigPageMetadata* firstMetadata, PageRef *runStart, size_t runSize) {
        freeBigPages.bulkWrite(runSize, [&](size_t index, BigPageMetadata*& entry) {
            const auto metadataIndex = divideAndRoundDown(runStart[index].value - runStart[0].value, static_cast<uint64_t>(arch::bigPageSize));
            assert(firstMetadata[metadataIndex].isFull(), "We should only take this path when freeing totally occupied big pages");
            firstMetadata[metadataIndex].freeAll();
            entry = &(firstMetadata[metadataIndex]);
        });
    };
    const auto freeSmallPageRun = [&](BigPageMetadata* superpage, PageRef *runStart, size_t runSize) {
        OccupancyTransition transition {};
        superpage->freePages(runStart, runSize, transition);

        if (transition.becameEmpty()) {
            // If the page is cached in a LocalPool it is not in paPages, so skip the
            // remove. The LocalPool will call returnPage() when it evicts or uses it up,
            // which routes it correctly at that point.
            if (superpage->hasAllocHolder()) {
                // nothing to do
            } else if (transition.before == OccupancyState::Partial) {
                // The page was partial, so it may be in paPages. Attempt to remove it and
                // move it to freeBigPages. If remove returns NotPresent, a concurrent free
                // of the same page beat us here: that thread will have already added it to
                // paPages (Full→Partial) but our Partial→Empty remove will see NotPresent,
                // leaving an empty page stranded in paPages. This is a known benign race:
                // the page remains fully allocatable for small-page requests and will find
                // its way back to freeBigPages through normal allocation activity. The only
                // practical consequence is that BIG_PAGE_ONLY allocations cannot see it
                // until it is reclaimed.
                if (paPages.remove(metadataIndex(superpage)) != AtomicBitPool::RemoveResult::NotPresent) {
                    // ABA TRAP: We successfully removed it from paPages, but we must verify
                    // it didn't get reused and become non-empty while we were preempted!
                    if (!superpage->isEmpty()) {
                        paPages.add(metadataIndex(superpage));
                    } else {
                        freeBigPages.write(superpage);
                    }
                }
            } else {
                // Full→Empty: page was never in paPages, return it directly.
                freeBigPages.write(superpage);
            }
        }
        else if (transition.becameAvailable()) {
            // Full→Partial: page newly has free subpages; publish it for small-page allocation.
            size_t spinCount = 0;
            while (superpage->hasAllocHolder()) {
                tight_spin();
                spinCount++;
                assert(spinCount < 100000, "stuck waiting for alloc holder to release hold");
            }
            paPages.add(metadataIndex(superpage));
        }
    };

    // Subrange cursor: only advances forward since pages are sorted by address.
    size_t subrangeIdx = 0;
    size_t i = 0;

    while (i < count) {
        if (pages[i].size() == mm::PageSize::BIG) {
            const uint64_t addr = pages[i].addr().value;

            // Advance the subrange cursor past entries that end before this big page.
            while (subrangeIdx < subrangeCount && addr >= subrangeInfo[subrangeIdx].rangeEnd.value) {
                subrangeIdx++;
            }

            // Collect consecutive big pages within the same subrange.
            assert(subrangeIdx < subrangeCount, "NUMAPool::freePages: big page address outside all subranges");
            const size_t runStart = i;
            const uint64_t subrangeEnd = subrangeInfo[subrangeIdx].rangeEnd.value;
            while (i < count && pages[i].size() == mm::PageSize::BIG
                              && pages[i].addr().value < subrangeEnd) {
                i++;
            }

            freeBigPageRun(findMetadata(pages[runStart].addr()), &pages[runStart], i - runStart);
        } else {
            // Small page: collect consecutive small pages in the same superpage.
            const uint64_t bigPageBase = pages[i].addr().value & ~static_cast<uint64_t>(arch::bigPageSize - 1);
            const size_t runStart = i;
            while (i < count && pages[i].size() == mm::PageSize::SMALL
                              && (pages[i].addr().value & ~static_cast<uint64_t>(arch::bigPageSize - 1)) == bigPageBase) {
                i++;
            }

            freeSmallPageRun(findMetadata(pages[runStart].addr()), &pages[runStart], i - runStart);
        }
    }
}

void PageAllocatorImpl::freePages(PageRef *pages, size_t count) {
    algorithm::sort(pages, count, [](const PageRef& p1, const PageRef& p2) {
        return p1.addr().value < p2.addr().value;
    });

    // Walk the sorted page list alongside the sorted domain table.
    // Both are ordered by address, so tableIdx only ever advances forward.
    size_t tableIdx = 0;
    size_t runStart = 0;
    NUMAPool* runPool = nullptr;

    for (size_t i = 0; i < count; i++) {
        const uint64_t addr = pages[i].addr().value;

        // Advance the table cursor past entries whose range ends before this address.
        while (tableIdx < domainTableSize && addr >= domainTable[tableIdx].rangeEnd.value) {
            tableIdx++;
        }

        // Determine the pool that owns this page (nullptr if outside all known ranges).
        NUMAPool* pagePool = nullptr;
        if (tableIdx < domainTableSize && addr >= domainTable[tableIdx].rangeStart.value) {
            pagePool = domainTable[tableIdx].pool;
        }

        // Pool changed — flush the accumulated run to the previous pool.
        if (pagePool != runPool) {
            if (runPool != nullptr) {
                runPool->freePages(&pages[runStart], i - runStart);
            }
            runStart = i;
            runPool = pagePool;
        }
    }

    // Flush the final run.
    if (runPool != nullptr) {
        runPool->freePages(&pages[runStart], count - runStart);
    }
}

#ifdef CROCOS_TESTING
size_t PageAllocatorImpl::countFreePages() const {
    size_t total = 0;
    for (size_t i = 0; i < numDomains; i++) {
        if (numaPools[i]) total += numaPools[i]->countTotalFreePages();
    }
    if (unownedPool) total += unownedPool->countTotalFreePages();
    return total;
}

bool PageAllocatorImpl::isPageAllocated(PageRef page) {
    BigPageMetadata* meta = findMetadata(page.addr());
    if (meta == nullptr) return false;
    if (page.size() == kernel::mm::PageSize::BIG) {
        return meta->isFull();
    }
    return meta->isSubpageAllocated(page);
}
#endif

// ==================== Global allocator and wrappers ====================

PageAllocatorImpl* gPageAllocator = nullptr;

namespace kernel::mm::PageAllocator {

    // ---- Single-page allocation ----

    phys_addr allocateSmallPage() {
        phys_addr result{};
        (void)gPageAllocator->allocatePages(1, [&](PageRef ref) { result = ref.addr(); });
        assert(result.value < 0xffe0000, "Result out of bounds???");
        return result;
    }

    phys_addr allocateSmallPage(numa::DomainID targetDomain) {
        phys_addr result{};
        (void)gPageAllocator->allocatePages(1, [&](PageRef ref) { result = ref.addr(); }, targetDomain);
        assert(result.value < 0xffe0000, "Result out of bounds???");
        return result;
    }

    phys_addr allocateSmallPage(arch::ProcessorID targetProc) {
        phys_addr result{};
        (void)gPageAllocator->allocatePages(1, [&](PageRef ref) { result = ref.addr(); }, targetProc);
        assert(result.value < 0xffe0000, "Result out of bounds???");
        return result;
    }

    phys_addr allocateBigPage() {
        phys_addr result{};
        (void)gPageAllocator->allocatePages(smallPagesPerBigPage, [&](PageRef ref) { result = ref.addr(); }, AllocBehavior::BIG_PAGE_ONLY);
        return result;
    }

    phys_addr allocateBigPage(numa::DomainID targetDomain) {
        phys_addr result{};
        (void)gPageAllocator->allocatePages(smallPagesPerBigPage, [&](PageRef ref) { result = ref.addr(); }, targetDomain, AllocBehavior::BIG_PAGE_ONLY);
        return result;
    }

    phys_addr allocateBigPage(arch::ProcessorID targetProc) {
        phys_addr result{};
        (void)gPageAllocator->allocatePages(smallPagesPerBigPage, [&](PageRef ref) { result = ref.addr(); }, targetProc, AllocBehavior::BIG_PAGE_ONLY);
        return result;
    }

    // ---- Bulk allocation ----

    static size_t allocatePagesImpl(size_t count, FunctionRef<void(PageRef)> cb, AllocFlags flags) {
        if (flags.has(AllocBehavior::BIG_PAGE_ONLY))
            return gPageAllocator->allocatePages(count * smallPagesPerBigPage, cb, flags) / smallPagesPerBigPage;
        return gPageAllocator->allocatePages(count, cb, flags);
    }

    size_t allocatePages(size_t count, FunctionRef<void(PageRef)> cb, AllocFlags flags) {
        return allocatePagesImpl(count, cb, flags);
    }

    size_t allocatePages(size_t count, FunctionRef<void(PageRef)> cb, numa::DomainID targetDomain, AllocFlags flags) {
        if (flags.has(AllocBehavior::BIG_PAGE_ONLY))
            return gPageAllocator->allocatePages(count * smallPagesPerBigPage, cb, targetDomain, flags) / smallPagesPerBigPage;
        return gPageAllocator->allocatePages(count, cb, targetDomain, flags);
    }

    size_t allocatePages(size_t count, FunctionRef<void(PageRef)> cb, arch::ProcessorID targetProc, AllocFlags flags) {
        if (flags.has(AllocBehavior::BIG_PAGE_ONLY))
            return gPageAllocator->allocatePages(count * smallPagesPerBigPage, cb, targetProc, flags) / smallPagesPerBigPage;
        return gPageAllocator->allocatePages(count, cb, targetProc, flags);
    }

    // ---- Single-page free ----

    void freeSmallPage(phys_addr addr) {
        PageRef ref = PageRef::small(addr);
        gPageAllocator->freePages(&ref, 1);
    }

    void freeBigPage(phys_addr addr) {
        PageRef ref = PageRef::big(addr);
        gPageAllocator->freePages(&ref, 1);
    }

    // ---- Bulk free ----

    void freePages(PageRef* pages, size_t count) {
        gPageAllocator->freePages(pages, count);
    }
}
