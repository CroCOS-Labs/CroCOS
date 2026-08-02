//
// Created by Spencer Martin on 8/1/26.
//

#ifndef CROCOS_KEXIT_H
#define CROCOS_KEXIT_H

#include "stdint.h"

namespace kernel {
    // How the kernel terminated, reported to whatever is hosting it.
    //
    // Before this existed, every termination site wrote the same ACPI poweroff
    // and QEMU exited 0 through all of them, so a clean shutdown, a debug PANIC
    // and a page fault on every CPU were indistinguishable to an automated
    // caller — any CI checking the `run` target's exit code was reading a
    // constant. See specs/rcu-phase-4.md, P4-ITEM-006.
    enum class ExitStatus : uint8_t {
        Success = 0,            // the shutdown timer fired; the kernel is done
        Panic = 1,              // kernel::panic — a failed assert or an explicit PANIC
        PageFault = 2,          // an unhandled page fault
        UnhandledException = 3, // any other exception reaching the default handler
        Hang = 4,               // a watchdog observed a CPU making no forward progress
    };

    // Terminate the machine, reporting `status` to the host. Never returns.
    //
    // On a target with no way to report a status, the failure statuses halt
    // rather than fall through, so that the absence of a reporting channel can
    // never be mistaken for success.
    [[noreturn]] void exitToHost(ExitStatus status);
}

#endif //CROCOS_KEXIT_H
