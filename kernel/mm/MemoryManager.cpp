//
// Created by Spencer Martin on 2/17/25.
//

#include <mem/mm.h>
#include <mem/PageAllocator.h>
#include <mem/BootstrapMapper.h>
#include <mem/TempWindow.h>

#include <kernel.h>
#include <kmemlayout.h>
#include <mem/NUMA.h>

extern uint32_t phys_end;

static PageAllocatorImpl gPageAllocatorImpl;

namespace kernel::mm{
    template<size_t level>
    void unmapIdentity(arch::PageTable<level>& pageTable) {
        constexpr auto unmapCeiling = pageTableLevelForKMemRegion();
        constexpr auto tableEntries = arch::pageTableDescriptor.entryCount[level];
        constexpr auto topEntry = tableEntries - 1;
        if constexpr (level < unmapCeiling - 1) {
            const auto subtablePaddr = pageTable[topEntry].getPhysicalAddress();
            const auto subtable = early_boot_phys_to_virt(subtablePaddr).template as_ptr<arch::PageTable<level + 1>>();
            unmapIdentity<level + 1>(*subtable);
        }
        pageTable[0] = {};
    }

    template<size_t level>
    void remapIdentity(arch::PageTable<level>& pageTable) {
        constexpr auto unmapCeiling = pageTableLevelForKMemRegion();
        static_assert(unmapCeiling > level);
        constexpr auto tableEntries = arch::pageTableDescriptor.entryCount[level];
        constexpr auto topEntry = tableEntries - 1;
        if constexpr (level < unmapCeiling - 1) {
            auto subtablePaddr = pageTable[topEntry].getPhysicalAddress();
            auto subtable = early_boot_phys_to_virt(subtablePaddr).template as_ptr<arch::PageTable<level + 1>>();
            remapIdentity<level + 1>(*subtable);
        }
        pageTable[0] = pageTable[topEntry];
    }

    void unmapIdentity() {
        unmapIdentity<0>(bootPageTable);
    }

    void remapIdentity() {
        remapIdentity<0>(bootPageTable);
    }

    using KMemRegionEntryType = arch::PTE<pageTableLevelForKMemRegion() - 1>;

    template<size_t level>
    arch::PageTable<level + 1>& getTopmostSubtable(const arch::PageTable<level>& pageTable) {
        auto paddr = pageTable[arch::pageTableDescriptor.entryCount[level] - 1].getPhysicalAddress();
        return *early_boot_phys_to_virt(paddr).template as_ptr<arch::PageTable<level + 1>>();
    }

    template <size_t desiredLevel, size_t currentLevel>
    arch::PageTable<desiredLevel>& getTopmostTable(arch::PageTable<currentLevel>& current) {
        static_assert(desiredLevel >= currentLevel);
        if constexpr (currentLevel == desiredLevel) {
            return current;
        }
        else {
            return getTopmostTable<desiredLevel, currentLevel + 1>(getTopmostSubtable<currentLevel>(current));
        }
    }

    template <size_t desiredLevel>
    arch::PageTable<desiredLevel>& getTopmostTable() {
        return getTopmostTable<desiredLevel, 0>(bootPageTable);
    }

    KMemRegionEntryType& getPageTableEntryForZone(const size_t zone) {
        const auto zoneIndex = arch::pageTableDescriptor.entryCount[pageTableLevelForKMemRegion() - 1] - zone - 1;
        return getTopmostTable<pageTableLevelForKMemRegion() - 1>()[zoneIndex];
    }

    // Shared zone counter across all reservePageAllocatorBufferForRange overloads.
    static size_t gMappedPageAllocatorBuffers = 0;

    // Reserve and map a page allocator buffer for a physical memory range.
    //
    // This function carves out space at the top of the physical range for:
    // 1. Page table structures needed to map the buffer
    // 2. The buffer itself (requiredBufferSize bytes, rounded up to page granularity)
    //
    // The function modifies the input range to mark the reserved space as used,
    // sets up the page tables to map the buffer into the next available page
    // allocator zone, and returns a virtual pointer to the mapped buffer.
    //
    // Returns: Virtual address of the mapped buffer
    void* reservePageAllocatorBufferForRange(phys_memory_range& range, size_t requiredBufferSize) {
        klog() << "[PA] reserving " << requiredBufferSize << " bytes for page allocator\n";
        if constexpr (supportsSimpleBootstrapPageAllocatorMapping) {
            range.end &= ~(arch::smallPageSize - 1);
            range.start.value = roundUpToNearestMultiple(range.start.value, arch::smallPageSize);

            range.end -= requiredTableSizeForPageAllocator;
            const auto ptPhysicalBase = range.end;

            const size_t alignedBufferSize = roundUpToNearestMultiple(requiredBufferSize, arch::smallPageSize);
            assert(2 * alignedBufferSize <= getKernelMemRegionSize(), "Memory range is too big");

            phys_memory_range bufferRange(range.end - alignedBufferSize, range.end);
            range.end -= alignedBufferSize;

            virt_addr pageTableBase;
            PageTableInitializationResult data;
            {
                TempWindow<arch::PageTable<arch::pageTableDescriptor.LEVEL_COUNT - 1>> tempWindow(ptPhysicalBase);
                const size_t numPages = requiredTableSizeForPageAllocator / arch::smallPageSize;
                for (size_t i = 0; i < numPages; i++) {
                    memset(&tempWindow[i], 0, arch::smallPageSize);
                }
                pageTableBase = tempWindow.virtualBase();
                data = initializePageTable<pageTableLevelForKMemRegion(), true>(pageTableBase, bufferRange, ptPhysicalBase);

                auto ptentry = KMemRegionEntryType::subtableEntry(data.pageTableAddress);
                ptentry.markPresent();
                ptentry.enableWrite();
                ptentry.enableExecute(false);
                getPageTableEntryForZone(PAGE_ALLOCATOR_ZONE_START + gMappedPageAllocatorBuffers) = ptentry;
            } // TempWindow destructor clears temp zone and flushes TLB, activating the new zone entry

            auto mappedAddress = getKernelMemRegionStart(PAGE_ALLOCATOR_ZONE_START + gMappedPageAllocatorBuffers) + data.mappedAddressStartOffset;
            gMappedPageAllocatorBuffers++;
            return mappedAddress.as_ptr<void>();
        }
        static_assert(supportsSimpleBootstrapPageAllocatorMapping, "Page allocator buffer mapping not supported on this architecture with the simple mapping construction");
    }

    bool initPageAllocator() {
        const numa::NUMATopology* topology = numa::getCurrentTopology();

        // Collect usable physical ranges from the firmware memory map.
        Vector<phys_memory_range> usableRanges;
        for (auto entry : arch::getMemoryMap()) {
            if (entry.type == arch::USABLE && entry.range.getSize() > arch::bigPageSize * 2) {
                usableRanges.push(entry.range);
            }
        }

        // Per-processor LocalPool pointer table (indexed by ProcessorID).
        static LocalPool* localPools[arch::MAX_PROCESSOR_COUNT] = {};

        Vector<NUMAPool*> numaPools;
        NUMAPool* unownedPool = nullptr;
        size_t processorCount = arch::processorCount();
        const kernel::numa::NUMAPolicy* allocPolicy = nullptr;

        if (topology != nullptr) {
            // NUMA-aware path: partition ranges by proximity domain.
            numa::NUMAMemoryPartition partition =
                numa::partitionMemoryByDomain(usableRanges, processorCount, *topology);
            processorCount = partition.processorDomain.size();

            bool anyDomainHasMemory = false;
            for (size_t d = 0; d < partition.rangesPerDomain.size(); d++) {
                if (!partition.rangesPerDomain[d].empty()) { anyDomainHasMemory = true; break; }
            }

            if (!anyDomainHasMemory) {
                // Topology exists but has no SRAT memory regions (e.g., trivial single-domain
                // on QEMU): all usable memory ended up in unownedRanges. Fall back to a single
                // pool with no NUMA policy so createPageAllocator's invariants hold.
                auto& ranges = partition.unownedRanges;
                BootstrapAllocator measuringAlloc;
                createNumaPool(measuringAlloc, ranges);
                for (size_t cpu = 0; cpu < processorCount; cpu++)
                    createLocalPool(measuringAlloc, nullptr);
                phys_memory_range* largestRange = &ranges[0];
                for (size_t i = 1; i < ranges.size(); i++)
                    if (ranges[i].getSize() > largestRange->getSize()) largestRange = &ranges[i];
                void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());
                BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
                NUMAPool* singlePool = createNumaPool(realAlloc, ranges);
                numaPools.push(singlePool);
                for (size_t cpu = 0; cpu < processorCount; cpu++)
                    localPools[cpu] = createLocalPool(realAlloc, nullptr, singlePool, static_cast<arch::ProcessorID>(cpu));
            } else {
                for (size_t domainIdx = 0; domainIdx < partition.rangesPerDomain.size(); domainIdx++) {
                    auto& ranges = partition.rangesPerDomain[domainIdx];
                    if (ranges.empty()) {
                        numaPools.push(nullptr);
                        continue;
                    }
                    const numa::DomainID domainId{static_cast<uint16_t>(domainIdx)};

                    // ---- Measure required memory ----
                    BootstrapAllocator measuringAlloc;
                    createNumaPool(measuringAlloc, ranges, domainId);
                    for (size_t cpu = 0; cpu < processorCount; cpu++) {
                        if (partition.processorDomain[cpu] == domainId)
                            createLocalPool(measuringAlloc, topology);
                    }

                    // ---- Carve out buffer from the largest range ----
                    phys_memory_range* largestRange = &ranges[0];
                    for (size_t i = 1; i < ranges.size(); i++)
                        if (ranges[i].getSize() > largestRange->getSize()) largestRange = &ranges[i];
                    void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());

                    // ---- Construct pools ----
                    BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
                    NUMAPool* domainPool = createNumaPool(realAlloc, ranges, domainId);
                    numaPools.push(domainPool);
                    for (size_t cpu = 0; cpu < processorCount; cpu++) {
                        if (partition.processorDomain[cpu] == domainId)
                            localPools[cpu] = createLocalPool(realAlloc, topology, domainPool, static_cast<arch::ProcessorID>(cpu));
                    }
                }

                allocPolicy = &kernel::numa::numaPolicy();

                // ---- Unowned ranges (no NUMA affinity) ----
                if (!partition.unownedRanges.empty()) {
                    BootstrapAllocator measuringAlloc;
                    createNumaPool(measuringAlloc, partition.unownedRanges);
                    phys_memory_range* largestRange = &partition.unownedRanges[0];
                    for (size_t i = 1; i < partition.unownedRanges.size(); i++)
                        if (partition.unownedRanges[i].getSize() > largestRange->getSize())
                            largestRange = &partition.unownedRanges[i];
                    void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());
                    BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
                    unownedPool = createNumaPool(realAlloc, partition.unownedRanges);
                }
            }
        } else {
            // Non-NUMA path: single pool for all memory.
            BootstrapAllocator measuringAlloc;
            createNumaPool(measuringAlloc, usableRanges);
            for (size_t cpu = 0; cpu < processorCount; cpu++)
                createLocalPool(measuringAlloc, topology);
            phys_memory_range* largestRange = &usableRanges[0];
            for (size_t i = 1; i < usableRanges.size(); i++)
                if (usableRanges[i].getSize() > largestRange->getSize()) largestRange = &usableRanges[i];
            void* buffer = reservePageAllocatorBufferForRange(*largestRange, measuringAlloc.bytesNeeded());
            BootstrapAllocator realAlloc(buffer, measuringAlloc.bytesNeeded());
            NUMAPool* singlePool = createNumaPool(realAlloc, usableRanges);
            numaPools.push(singlePool);
            for (size_t cpu = 0; cpu < processorCount; cpu++)
                localPools[cpu] = createLocalPool(realAlloc, topology, singlePool, static_cast<arch::ProcessorID>(cpu));
        }

        gPageAllocatorImpl = createPageAllocator(move(numaPools), localPools, processorCount, unownedPool, allocPolicy);
        gPageAllocator = &gPageAllocatorImpl;
        klog() << "[PA] initPageAllocator complete, gPageAllocator=" << (void*)gPageAllocator << " localPools[0]=" << (void*)localPools[0] << "\n";

        // Reserve the kernel image so it can never be handed out as free memory.
        phys_memory_range kernelRange{.start = phys_addr(nullptr), .end = phys_addr(&phys_end)};
        klog() << "[PA] kernelRange.end = " << (void*)kernelRange.end.value << "\n";
        gPageAllocator->reserveRange(kernelRange);
        klog() << "[PA] reserveRange done\n";

        return true;
    }
}
