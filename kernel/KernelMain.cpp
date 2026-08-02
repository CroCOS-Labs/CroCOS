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
// Bracket the boot-reserved .bss block (boot stack, saved multiboot info, and
// boot page tables) defined in multiboot1.asm. This block is still live during
// cpp_init, so zeroBSS must not zero it.
extern "C" uint8_t __boot_reserved_start;
extern "C" uint8_t __boot_reserved_end;
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
        // The boot stack, saved multiboot info, and boot page tables live in a
        // contiguous .bss block (multiboot1.asm) that is STILL LIVE here: we are
        // running on that stack and CR3 points at bootPageTable. Zeroing it out
        // from under the kernel triple-faults under -O2 (the first page walk
        // after the PML4 is cleared faults; zeroing the stack clobbers our own
        // return address). It is set up by _start and must persist until the
        // memory_management phase rebuilds paging, so zero .bss in the two
        // segments surrounding the reserved block rather than straight through.
        auto* bssStart = reinterpret_cast<uint8_t*>(&__bss_virt_start);
        auto* bssEnd   = reinterpret_cast<uint8_t*>(&__bss_virt_end);
        auto* resStart = &__boot_reserved_start;
        auto* resEnd   = &__boot_reserved_end;
        memset(bssStart, 0, static_cast<size_t>(resStart - bssStart));
        memset(resEnd,   0, static_cast<size_t>(bssEnd - resEnd));
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

    bool enqueueShutdown() {
        arch::amd64::sti();
        klog() << "Enqueuing shutdown\n";
        timing::enqueueEvent([] {
            klog() << "\nGoodbye :)\n";
            // The one path that reports success. Every other termination site
            // reports a nonzero status (specs/rcu-phase-4.md, P4-ITEM-006).
            exitToHost(ExitStatus::Success);
        }, 20000);
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