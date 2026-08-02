//
// Created by Spencer Martin on 8/1/26.
//

#include <kexit.h>
#include "arch/amd64/amd64.h"

namespace kernel {
    namespace {
        // QEMU's ACPI PM control register. Writing SLP_TYP=S5 | SLP_EN powers the
        // machine off and QEMU's process exits 0 — success is the only status this
        // channel can express, which is precisely why it cannot carry the others.
        constexpr uint16_t acpiPowerOffPort = 0x604;
        constexpr uint16_t acpiPowerOffValue = 0x2000;

        // QEMU's `isa-debug-exit` device, wired up by the run targets in the
        // top-level CMakeLists.txt as:
        //     -device isa-debug-exit,iobase=0xf4,iosize=0x04
        // A write of v makes QEMU exit with status (v << 1) | 1 — always odd, so
        // always nonzero, so never confusable with a clean shutdown. Failure
        // statuses map to 3 (Panic), 5 (PageFault), 7 (UnhandledException) and
        // 9 (Hang).
        //
        // The device is deliberately absent from the qmon/kdebug targets: those
        // are gdb stubs, where halting on a fault so it can be inspected beats
        // exiting out from under the debugger.
        constexpr uint16_t debugExitPort = 0xf4;
    }

    [[noreturn]] void exitToHost(const ExitStatus status) {
        // Give QEMU a moment to drain the serial port before the machine goes
        // away. A panic message that never makes it out is worse than a wrong
        // exit code, since the status says only that something failed.
        for (auto i = 0; i < 1000; i++) {
            asm volatile("pause");
        }

        if (status == ExitStatus::Success) {
            arch::amd64::outw(acpiPowerOffPort, acpiPowerOffValue);
        } else {
            // Emphatically NOT the ACPI port: that exits 0 and undoes the point.
            asm volatile("outl %0, %1" ::"a"(static_cast<uint32_t>(status)), "Nd"(debugExitPort));
        }

        // Reached only if the write went nowhere — no isa-debug-exit device, or
        // real hardware. Halt rather than return: a hung run is a failure a human
        // investigates, whereas returning would let a fault path carry on and
        // eventually reach the shutdown timer, reporting success.
        for (;;) {
            asm volatile("cli; hlt");
        }
    }
}
