//
// Created by Spencer Martin on 2/12/25.
//

#include "arch/amd64/amd64.h"
#include <kernel.h>
#include <kconfig.h>
#include <core/str.h>
#include <acpi.h>
#include <assert.h>
#include <timing/timing.h>
#include <core/math.h>
#include "multiboot.h"
#include <arch/amd64/interrupts/LegacyPIC.h>
#include <arch/amd64/smp.h>
#include <arch/amd64/interrupts/APIC.h>
#include <arch/amd64/interrupts/AuxiliaryDomains.h>
#include <arch/amd64/timers/HPET.h>
#include <kmemlayout.h>
#include <mem/NUMA.h>
#include <acpi/NUMAIterators.h>

extern uint32_t mboot_magic;
extern uint32_t mboot_table;

alignas(4096) uint64_t bootstrapPageDir[512];
extern volatile uint64_t boot_page_directory_pointer_table[512];

size_t archProcessorCount;

namespace arch::amd64{

    void flushTLB(){
        asm volatile("mov %cr3, %rax\n"
                     "mov %rax, %cr3");
    }

    bool supportsFSGSBASE() {
        uint32_t eax, ebx, ecx, edx;
        eax = 0x07;
        ecx = 0;
        asm volatile("cpuid"
                : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "c"(ecx));
        return (ebx & (1 << 0)) != 0;
    }

    bool enableFSGSBase(){
        //TODO implement a fallback method
        //really I ought to just do this with a proper GDT or whatever... but this should be enough
        //to get the rudiments of SMP working.
        //temporaryHack(3, 3, 2026, "Implement a proper GDT");
        assert(supportsFSGSBASE(), "Your CPU doesn't support FSGSBASE");
        uint64_t cr4;
        asm volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 16);
        asm volatile ("mov %0, %%cr4" :: "r"(cr4));
        return true;
    }

    const uint64_t kernel_code_descriptor =
            (1ul << 53) |                                 //Marks long mode in flags
            (1ul << 7 | 1ul << 4 | 1ul << 3 | 3ul) << 40; //Present, non-system segment, executable, RW, and accessed
    const uint64_t kernel_data_descriptor =
            (1ul << 53) |                                 //Marks long mode in flags
            (1ul << 7 | 1ul << 4 | 0ul << 3 | 3ul) << 40; //Present, non-system segment, executable, RW, and accessed

    __attribute__((aligned(16)))
    static uint64_t gdt[] = {
            0x0000000000000000, // Null descriptor
            kernel_code_descriptor,
            kernel_data_descriptor
    };

    struct {
        uint16_t size;
        uint64_t base;
    } __attribute__((packed)) gdtr = {
            .size = sizeof(gdt) - 1,
            .base = (uint64_t)&gdt
    };

    extern "C" void load_gdt(void*);

    bool initGDT() {
        load_gdt(&gdtr);
        return true;
    }

    bool searchForACPITables() {
        if (tryFindACPI() != ACPIDiscoveryResult::SUCCESS) {
            return false;
        }
        auto& madt = kernel::acpi::the<MADT>();
        archProcessorCount = madt.getEnabledProcessorCount();
        if (archProcessorCount == 0) {
            return false;
        }
        return true;
    }

    bool bspSetPID() {
        smp::setLogicalProcessorID(0);
        return true;
    }

    bool apSetPID() {
        auto lapicID = interrupts::getLAPICDomain() -> getID();
        auto pinfo = smp::getProcessorInfoForLapicID(static_cast<uint8_t>(lapicID));
        smp::setLogicalProcessorID(pinfo.logicalID);
        return true;
    }

    void pageFaultHandler(InterruptFrame& frame) {
        void* faultingAccess;
        asm volatile("mov %%cr2, %0" : "=r"(faultingAccess));
        klog() << "Pagefault at " << reinterpret_cast<void*>(frame.rip) << " accessing " << faultingAccess << "\n";
        print_stacktrace(&frame.rbp);
        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        asm volatile("cli; hlt");
        __builtin_unreachable();
    }

    bool supportsNXBit() {
        uint32_t eax, ebx, ecx, edx;
        eax = 0x80000001;
        ecx = 0;
        asm volatile("cpuid"
                : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                : "c"(ecx));
        return (edx & (1 << 20)) != 0;
    }

    bool enableNXBit() {
        assert(supportsNXBit(), "Your CPU doesn't support the NX bit");
        constexpr uint32_t IA32_EFER_MSR = 0xC0000080;
        uint64_t efer = rdmsr(IA32_EFER_MSR);
        wrmsr(IA32_EFER_MSR, efer | (1ULL << 11));
        return true;
    }

    void unhandledExceptionHandler(InterruptFrame& frame) {
        klog() << "Unhandled exception " << frame.vector_index << " at IP " << (void*)frame.rip << " on processor " << getCurrentProcessorID() << "\n";
        print_stacktrace(&frame.rbp);
        asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        asm volatile("cli; hlt");
        __builtin_unreachable();
    }

    bool setupInterruptControllers() {
        auto& madt = kernel::acpi::the<acpi::MADT>();
        interrupts::disableLegacyPIC();
        kernel::interrupts::platform::setupCPUInterruptVectorFile(INTERRUPT_VECTOR_COUNT);
        interrupts::setupAPICs(madt);
        const auto exceptionVectors = make_shared<interrupts::ExceptionVectorDomain>(INTERRUPT_VECTOR_RESERVE_SIZE);
        kernel::interrupts::topology::registerDomain(exceptionVectors);
        kernel::interrupts::topology::connectAllOutputsExclusive(exceptionVectors,
            kernel::interrupts::platform::getCPUInterruptVectors(), INTERRUPT_VECTOR_RESERVE_START);

        using namespace kernel::interrupts::managed;
        for (size_t i = 0; i < INTERRUPT_VECTOR_RESERVE_SIZE; i++) {
            if (i == 14) {
                registerHandler(InterruptSourceHandle(exceptionVectors, i), pageFaultHandler);
            } else {
                registerHandler(InterruptSourceHandle(exceptionVectors, i), unhandledExceptionHandler);
            }
        }

        return true;
    }

    MultibootMMapIterator &MultibootMMapIterator::operator++() {
        currentEntry++;
        return *this;
    }

    MemoryMapEntry MultibootMMapIterator::operator*() {
        mm::phys_memory_range range = {mm::phys_addr(currentEntry -> addr),
            mm::phys_addr(currentEntry -> addr + currentEntry -> len)};
        MemoryMapEntryType type;
        switch(currentEntry -> type) {
            case MEM_ACPI_RECLAIMABLE:
                type = ACPI_RECLAIMABLE;
                break;
            case MEM_AVAILABLE:
                type = USABLE;
                break;
            case MEM_BAD:
                type = BAD;
                break;
            case MEM_NVS:
                type = ACPI_NVS;
                break;
            case MEM_RESERVED:
                type = RESERVED;
                break;
            default:
                type = UNKNOWN;
        }

        return {range,type};
    }

    bool MultibootMMapIterator::operator!=(const MultibootMMapIterator &other) const {
        return currentEntry != other.currentEntry;
    }

    IteratorRange<MultibootMMapIterator> getMemoryMap() {
        assert(mboot_magic == 0x2BADB002, "Somehow the multiboot magic number is wrong. How did we get here?");
        const auto mbootInfo = early_boot_phys_to_virt(mm::phys_addr(mboot_table)).as_ptr<mboot_info>();
        const auto mbootMmapBase = early_boot_phys_to_virt(mm::phys_addr(mbootInfo -> mmap_ptr)).as_ptr<mboot_mmap_entry>();
        return {MultibootMMapIterator(mbootMmapBase),
                MultibootMMapIterator(mbootMmapBase + mbootInfo -> mmap_len)};
    }

    size_t debugEarlyBootCPUID() {
        uint32_t eax, ebx, ecx, edx;
        cpuid(eax, ebx, ecx, edx, 1);
        return (ebx >> 24) & 0xff;
    }

    alignas(alignof(kernel::numa::NUMATopology)) char numaTopologyStorage[sizeof(kernel::numa::NUMATopology)];

    bool initNUMAInfo() {
        auto* topo = new(numaTopologyStorage) kernel::numa::NUMATopology(
            kernel::acpi::numa::buildNUMATopology());
        kernel::numa::initNUMAPolicy(*topo);
        return true;
    }
}

namespace arch {
    size_t getCacheLineSize() {
        uint32_t eax, ebx, ecx, edx;
        arch::amd64::cpuid(eax, ebx, ecx, edx, 0x01);
        // EBX[15:8] is the CLFLUSH line size in 8-byte units
        return ((ebx >> 8) & 0xFF) * 8;
    }
}