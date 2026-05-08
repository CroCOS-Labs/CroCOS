//
// Created by Spencer Martin on 2/13/25.
//
#include <kernel.h>
#include <core/Object.h>
#include <core/algo/GraphAlgorithms.h>
#include <interrupts/interrupts.h>
#include <timing/timing.h>
#include <arch/amd64/smp.h>
#include <init.h>
#include <arch.h>
#include <mem/VMSubstrate.h>

// NOLINTBEGIN
extern "C" void (*__init_array_start[])(void) __attribute__((weak));
extern "C" void (*__init_array_end[])(void) __attribute__((weak));
extern "C" uint32_t __bss_virt_start;
extern "C" uint32_t __bss_virt_end;
// NOLINTEND

namespace kernel{
    arch::SerialPrintStream EarlyBootStream;
    Core::PrintStream& klogStream = EarlyBootStream;

    Core::AtomicPrintStream klog() {
        return Core::AtomicPrintStream(klogStream);
    }

    Core::PrintStream& emergencyLog() {
        return klogStream;
    }

    bool zeroBSS(){
        memset(&__bss_virt_start, 0, reinterpret_cast<size_t>(&__bss_virt_end) - reinterpret_cast<size_t>(&__bss_virt_start));
        return true;
    }

    bool runGlobalConstructors(){
        for (void (**ctor)() = __init_array_start; ctor != __init_array_end; ctor++) {
            (*ctor)();
        }
        return true;
    }

    bool initCRClassMetadata() {
        presort_object_parent_lists();
        return true;
    }

    using PagePool = void*[2048];

    PagePool page_pools[16];

    [[noreturn]] bool naiveTest() {
        klog() << "Running VMSubstrate test on CPU " << arch::getCurrentProcessorID() << "\n";
        constexpr size_t kPageCount = sizeof(PagePool) / sizeof(void*);
        while (true) {
            auto& pool = page_pools[arch::getCurrentProcessorID()];
            for (size_t i = 0; i < kPageCount; i++) {
                pool[i] = mm::VMSubstrate::allocPage();
                assert(pool[i] != nullptr, "VMSubstrate::allocPage returned null");
                *static_cast<volatile size_t*>(pool[i]) = i;
            }
            for (size_t i = 0; i < kPageCount; i++) {
                assert(*static_cast<volatile size_t*>(pool[i]) == i, "VMSubstrate page content corrupted");
                mm::VMSubstrate::freePage(pool[i]);
            }
        }
    }

    bool enqueueShutdown() {
        arch::amd64::sti();
        klog() << "Enqueuing shutdown\n";
        timing::enqueueEvent([] {
            klog() << "\nGoodbye :)\n";
            asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
        }, 2000);
        return true;
    }

    extern "C" [[noreturn]] void kernel_main() {
        klog() << "\n"; // newline to separate from the "Booting from ROM.." message from qemu
        init::kinit(true, KERNEL_INIT_LOG_LEVEL, false);

        for (;;)
            asm volatile("hlt");
        //asm volatile("outw %0, %1" ::"a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)); //Quit qemu
    }
}