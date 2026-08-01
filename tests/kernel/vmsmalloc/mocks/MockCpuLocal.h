//
// vmsmalloc Phase 8 — per-thread CpuLocal binding for the harness.
//
// The harness reuses the *real* kernel CpuLocal.h (it compiles cleanly under
// CROCOS_TESTING): kernel::cpuLocal() reads arch::getCurrentCpuLocalBase(),
// whose userspace backend is a thread_local pointer (MockKernelEnv.cpp). Each
// std::thread models one CroCOS CPU and calls bindThreadToCpu(i) once at
// thread start to install its per-CPU CpuLocal page and pin its logical ID.
//

#ifndef CROCOS_MOCK_CPULOCAL_H
#define CROCOS_MOCK_CPULOCAL_H

#include <arch.h>
#include <CpuLocal.h>   // real kernel header (compiles under CROCOS_TESTING)

namespace kernel::test {
    // Bind the calling thread to logical CPU `i`: points this thread's
    // CpuLocal base at i's arena-resident page (from MockVMSubstrate),
    // writes logicalID = i, and makes arch::getCurrentProcessorID() return i.
    void bindThreadToCpu(arch::ProcessorID i);
}

namespace arch::test {
    // Set what arch::processorCount() reports. RCU Phase 2 sizes its slot array
    // off it at Domain::init, so a harness that configures N CPUs and then binds
    // threads to them must say so first — the mock's default of 8 would
    // under-reserve for a larger configuration and over-reserve for a smaller.
    void setProcessorCount(size_t n);
}

#endif // CROCOS_MOCK_CPULOCAL_H
