//
// Created by Spencer Martin on 2/16/25.
//

#ifndef CROCOS_PANIC_H
#define CROCOS_PANIC_H

#include "kernel.h"
#include "kexit.h"

#define PANIC(...) kernel::panic<true>(__FILE__, __LINE__, __VA_ARGS__)

#define PANIC_NO_STACKTRACE(...) kernel::panic<false>(__FILE__, __LINE__, __VA_ARGS__)

namespace kernel{
    void print_stacktrace();
    void print_stacktrace(uintptr_t* rbp);
    Core::PrintStream& emergencyLog();
    template <bool stacktrace, typename... Args>
    [[noreturn]]
    void panic(const char* filename, const uint32_t line, Args&&... args){
        emergencyLog() << "Panic: ";
        (emergencyLog() << ... << forward<Args>(args));
        emergencyLog() << "\nIn file " << filename << " line " << line << "\n";
        if constexpr (stacktrace) {
            print_stacktrace();
        }
        // Reports a NONZERO status to the host, unlike the shutdown path — a
        // panic and a clean `Goodbye :)` used to be indistinguishable to any
        // automated caller (specs/rcu-phase-4.md, P4-ITEM-006).
        //
        // Excluded under CROCOS_TESTING: the kernel sources that reach here are
        // also compiled into the userspace test runners, which have no ports to
        // write to and their own way of reporting a failed assertion.
#if defined(__x86_64__) && !defined(CROCOS_TESTING)
        exitToHost(ExitStatus::Panic);
#endif
    }
}

#endif //CROCOS_PANIC_H