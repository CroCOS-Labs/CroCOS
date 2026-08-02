//
// vmsmalloc Phase 8 — mock <mem/VMSubstrate.h> for the userspace harness.
//
// Shadows the real kernel header via include-path ordering. Declares the same
// VMSubstrate API surface vmsmalloc.cpp links against, but the page-allocation
// primitives are backed by an mmap region (see MockVMSubstrate.cpp) rather than
// kernel page tables. ensureTLBEntryFresh is a no-op (no TLB in userspace).
//
// kWindowAddressBits is the literal 39 (one AMD64 PML4 entry / 512 GiB), so
// vmsmalloc's DEC-015 encoding uses the same 27-bit offset / 37-bit counter
// split as production (P8-DEC-007). The mmap region is far smaller, so the
// upper offset bits stay zero — identical math, no test-only code path.
//
// make<T> / destroy<T> / SafePtr mirror the real header so the (deferred)
// MakeStaticAssertsTest can exercise them; the integration tests call
// vmsmalloc / vmsfree directly.
//

#ifndef CROCOS_MOCK_VMSUBSTRATE_H
#define CROCOS_MOCK_VMSUBSTRATE_H

#include <stddef.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <arch.h>
#include <core/utility.h>      // forward, placement new
#include <VMSubstrateSlab.h>   // sizeClassFor / slotAlignment / kNumSizeClasses

namespace kernel::mm::VMSubstrate {

    // VA-window width: literal 39 bits in the harness (P8-DEC-007). vmsmalloc's
    // VmsHeadEncoding derives kOffsetBits = 39 - log2(pageSize) = 27 from this.
    inline constexpr unsigned kWindowAddressBits = 39;

    // Page primitives — mmap-backed (MockVMSubstrate.cpp).
    void* allocPage();
    void  freePage(void* p);
    // DEC-047 slab-reclaim sibling of freePage. The harness does not simulate
    // the read-only sentinel remap (no page tables in userspace); it recycles
    // the page like freePage. See MockVMSubstrate.cpp.
    void  reclaimSlabPage(void* p);
    void* mapMMIOPage(phys_addr paddr);

    // No-op in userspace: no TLB, no dirty bitmap. The call site in
    // vmsmalloc.cpp is still exercised (source-order correctness).
    inline bool ensureTLBEntryFresh(void*) noexcept { return false; }  // userspace: never stale

    // Phase-3 storage primitives.
    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);
    void* cpuLocalPageFor(arch::ProcessorID i);
    virt_addr arenaVirtualBase(size_t index);

    // Convention-internal (defined by vmsmalloc.cpp, compiled into the harness).
    void* vmsmalloc(size_t size);
    void  vmsfree(void*);

    template <typename T>
    struct SafePtr {
    private:
        T* ptr;
    public:
        SafePtr(T* p) : ptr(p) {}
        SafePtr(const SafePtr& other) : ptr(other.ptr) {}
        SafePtr(SafePtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
        T& operator*() const { ensureTLBEntryFresh(ptr); return *ptr; }
        T* operator->() const { ensureTLBEntryFresh(ptr); return ptr; }
        bool operator==(const SafePtr& other) const { return ptr == other.ptr; }
        bool operator!=(const SafePtr& other) const { return ptr != other.ptr; }
        operator bool() const { return ptr != nullptr; }
        SafePtr<T>& operator=(const SafePtr<T>& other) = default;
        void* raw() const { return ptr; }
    };

    template <typename T, typename... Ts>
    SafePtr<T> make(Ts&&... args) {
        constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
        static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                      "VMSubstrate::make<T>: sizeof(T) exceeds a page");
        static_assert(c >= vmsmalloc::kNumSizeClasses ||
                          alignof(T) <= vmsmalloc::slotAlignment(c),
                      "VMSubstrate::make<T>: alignof(T) exceeds the slot alignment of its "
                      "size class. Pad T into a power-of-two size class, split the "
                      "over-aligned subobject, or use a different allocator.");
        auto* mem = vmsmalloc(sizeof(T));
        // malloc (not calloc) semantics — mirror the real header: default-init
        // with no args so trivial T is left uninitialized, not zero-filled.
        if constexpr (sizeof...(Ts) == 0) {
            return new (mem) T;
        } else {
            return new (mem) T(forward<Ts>(args)...);
        }
    }

    template <typename T>
    void destroy(SafePtr<T> obj) {
        if (obj) { obj->~T(); vmsfree(obj.raw()); }
    }

    // ─── Harness control surface (not in the real header) ───
    namespace test {
        // mmap the VA region, carve per-domain + per-CPU storage, construct the
        // per-(domain,class) ChainedTreiberStacks (via vmsmallocLateInit).
        void initialize(size_t cpuCount, size_t domainCount);
        void shutdown();              // munmap + reset all state
        size_t activePageCount();     // live slab/whole-page pages (leak check)
    }
}

#endif // CROCOS_MOCK_VMSUBSTRATE_H
