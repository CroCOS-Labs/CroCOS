//
// vmsmalloc Phase 8 — kernel/arch environment stubs for the harness.
//
// Provides userspace bodies for the kernel/arch symbols vmsmalloc.cpp pulls in
// transitively: the per-CPU base register (thread_local), the logical-CPU-ID
// accessor (per-thread bound CPU), klog/emergencyLog/panic plumbing, the mock
// NUMA policy, and the all-zeros interrupt-context depths (no real interrupts
// in userspace, so vmsmalloc's DEC-014 forbidden-context check never fires —
// by design, per the Phase-8 spec).
//

#include <arch.h>
#include <kernel.h>
#include <panic.h>
#include <mem/NUMA.h>                          // mock
#include <mem/VMSubstrate.h>                   // mock (cpuLocalPageFor)
#include <CpuLocal.h>                          // real
#include <interrupts/InterruptContextDepths.h> // real
#include "MockCpuLocal.h"

// ─── arch: per-CPU base register + logical ID (thread_local) ───────────────
namespace arch {
namespace {
    thread_local void*       tlCpuLocalBase = nullptr;
    thread_local ProcessorID tlBoundCpu     = 0;
    size_t                   gProcessorCount = 8;
}

void  setCurrentCpuLocalBase(void* ptr) noexcept { tlCpuLocalBase = ptr; }
void* getCurrentCpuLocalBase() noexcept          { return tlCpuLocalBase; }

ProcessorID getCurrentProcessorID() { return tlBoundCpu; }
size_t      processorCount()        { return gProcessorCount; }
size_t      getCacheLineSize()      { return 64; }

namespace test {
    void setBoundCpu(ProcessorID i) { tlBoundCpu = i; }
    // RCU Phase 2 sizes its slot array off processorCount(), so a harness that
    // configures N CPUs must be able to say so — a fixed 8 would either
    // over-reserve or, worse, under-reserve relative to the CPUs tests bind to.
    void setProcessorCount(size_t n) { gProcessorCount = n; }
}
}

// ─── kernel: logging + panic plumbing ──────────────────────────────────────
namespace kernel {
namespace {
    class NullStream : public Core::PrintStream {
    protected:
        void putString(const char*) override {}
    };
    NullStream gNullStream;
}

Core::AtomicPrintStream klog()        { return Core::AtomicPrintStream(gNullStream); }
Core::PrintStream&      emergencyLog() { return gNullStream; }
void print_stacktrace()           {}
void print_stacktrace(uintptr_t*) {}
}

// ─── kernel::numa: configurable mock policy ────────────────────────────────
namespace kernel::numa {
namespace { NUMAPolicy gPolicy; }
const NUMAPolicy& numaPolicy() { return gPolicy; }
namespace test {
    void configure(size_t cpus, size_t domains, NUMAPolicy::Mapping mapping) {
        gPolicy.configure(cpus, domains, mapping);
    }
}
}

// ─── kernel::interrupts: settable per-thread depths ─────────────────────────
//
// Zero by default, so vmsmalloc's own tests never see a forbidden context (no
// real interrupts in userspace). RCU Phase 2 needs them SETTABLE: its two-tier
// context rules (retire tolerates #PF, synchronize/barrier do not) are only
// distinguishable if a test can put the thread in a specific context, and with
// permanently-zero depths that whole class of assertion is untested. Per-thread
// because each harness thread models one CPU.
namespace kernel::interrupts {
namespace { thread_local InterruptContextDepths tlDepths{}; }
const InterruptContextDepths& currentCpuInterruptDepths() noexcept { return tlDepths; }

// Mirrors the real definition in kernel/interrupts/InterruptContextDepths.cpp,
// which this harness does not build. Kept as a real implementation over
// tlDepths rather than a hardcoded `false`, so the masking logic itself is
// exercised rather than stubbed past.
bool inForbiddenContext(uint64_t forbiddenMask) noexcept {
    return (currentCpuInterruptDepths().packed() & forbiddenMask) != 0;
}

namespace test {
    InterruptContextDepths& mutableInterruptDepths() noexcept { return tlDepths; }
    void resetInterruptDepths() noexcept { tlDepths = InterruptContextDepths{}; }
}
}

// ─── kernel::test: per-thread CpuLocal binding ─────────────────────────────
namespace arch { namespace test { void setBoundCpu(ProcessorID); } }
namespace kernel::test {
void bindThreadToCpu(arch::ProcessorID i) {
    void* page = kernel::mm::VMSubstrate::cpuLocalPageFor(i);
    auto* cl = static_cast<kernel::CpuLocal*>(page);
    cl->logicalID = i;
    arch::setCurrentCpuLocalBase(page);
    arch::test::setBoundCpu(i);
}
}
