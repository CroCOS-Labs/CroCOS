//
// Created by Spencer Martin on 4/26/26.
//

#ifndef CROCOS_VMSUBSTRATE_H
#define CROCOS_VMSUBSTRATE_H

#include <stddef.h>
#include <mem/MemTypes.h>
#include <mem/NUMA.h>
#include <arch.h>
#include <kmemlayout.h>   // pageTableLevelForKMemRegion (for kWindowAddressBits)
// P7-DEC-007: make<T>'s compile-time alignment/size checks need the size-class
// accessors (sizeClassFor / slotAlignment / kNumSizeClasses). VMSubstrateSlab.h
// is the implementation-internal vmsmalloc header; every includer of this
// public header is a kernel TU with kernel/mm on its include path.
#include <VMSubstrateSlab.h>

namespace kernel::mm::VMSubstrate {
    bool init();

    // Width, in bits, of the VMSubstrate VA window — one entry at the top of
    // its page-table subtree (PML4[VMM_SUBSTRATE_ROOT_INDEX] on AMD64 = 39
    // bits / 512 GiB). This is the substrate's own geometry; vmsmalloc's
    // DEC-015 packed-tagged-head encoding consumes it to size the page-offset
    // field (kOffsetBits = kWindowAddressBits - log2(smallPageSize)) instead
    // of reaching into arch::pageTableDescriptor directly. The userspace test
    // harness supplies the same constant from its MockVMSubstrate.
    inline constexpr unsigned kWindowAddressBits =
        arch::pageTableDescriptor.getVirtualAddressBitCount(
            pageTableLevelForKMemRegion() - 1);

    // Returns the base virtual address of the arena at the given index.
    virt_addr arenaVirtualBase(size_t index);
    void* allocPage();
    void freePage(void*);

    // DEC-047: slab-reclaim sibling of freePage. Used only by vmsmalloc's
    // DEC-036 eager-free walk. Behaves like freePage (real frame returned to the
    // allocator, VA released) but leaves the freed VA mapped read-only onto a
    // shared sentinel page, so a concurrent lock-free Treiber pop mid-flight on
    // the reclaimed slab descriptor reads harmless garbage instead of faulting
    // on a torn-down PTE. Whole-page frees keep using freePage (never Treiber
    // nodes). The userspace mock recycles like freePage (no page tables).
    void reclaimSlabPage(void*);

    // Returns whether it actually had to invalidate — i.e. whether this CPU's
    // mapping for that page WAS stale. Callers may ignore it; SafePtr does.
    // It exists so an instrumented consumer can distinguish "the freshness call
    // ran" from "the freshness call saved us", which is the difference between
    // exercising the DEC-047 hazard and merely walking past it (P4-ITEM-001).
    bool ensureTLBEntryFresh(void*);

#ifdef CROCOS_FRESHNESS_STATS
    // Count of reclaimSlabPage calls since boot (P4-DEC-006). Declared only
    // under the flag so a stats-less build cannot silently report 0 and be
    // misread as "the reclaim path never ran".
    uint64_t reclaimedSlabPageCount();
#endif

    // Convention-internal: external callers should prefer make<T> / destroy<T>.
    // These are unavoidably declared here because make<T> / destroy<T> are
    // templates whose bodies must live in this public header; structural hiding
    // is not possible under the template-visibility constraint. See parent-spec
    // DEC-028 (amended).
    void* vmsmalloc(size_t size);
    void vmsfree(void*);

    // Maps a physical MMIO page into the current CPU's arena with cache-disable semantics.
    // paddr must be page-aligned.  The returned virtual address is permanently mapped.
    void* mapMMIOPage(phys_addr paddr);

    // ─── vmsmalloc Phase 3 — init-only NUMA-aware pinned-buffer primitive ───
    //
    // Allocates `divideAndRoundUp(byteSize, smallPageSize)` physical pages
    // on NUMA domain `d`, maps them contiguously inside VMSubstrate's
    // static-buffer region, zero-fills, and returns the base VA. The
    // returned VA is page-aligned and pinned for the kernel's lifetime
    // (no freeing API). Single-threaded init context only — asserts in
    // debug builds; panics on PageAllocator failure or region exhaustion.
    void* reservePerDomainStaticBuffer(size_t byteSize, numa::DomainID d);

    // Returns the base VA of CPU `i`'s per-CPU CpuLocal page (sized via
    // kernel::kCpuLocalBytes in cpu_local.h). The page is allocated and
    // mapped during createArena(i) on the arena owner's NUMA domain, and
    // is zero-filled at that time. Pure address arithmetic — safe to call
    // from any context after VMSubstrate::init returns.
    void* cpuLocalPageFor(arch::ProcessorID i);

    template <typename T>
    struct SafePtr {
    private:
        T* ptr;
    public:
        SafePtr(T* p) : ptr(p) {}
        SafePtr(SafePtr& other) : ptr(other.ptr) {}
        SafePtr(const SafePtr& other) : ptr(other.ptr) {}
        SafePtr(SafePtr&& other) noexcept : ptr(other.ptr) {other.ptr=nullptr;}
        T& operator*() const { ensureTLBEntryFresh(ptr); return *ptr; }
        T* operator->() const { ensureTLBEntryFresh(ptr); return ptr; }
        bool operator==(const SafePtr & other) const { return ptr == other.ptr; }
        bool operator!=(const SafePtr & other) const { return ptr != other.ptr; }
        operator bool() const { return ptr != nullptr; }
        SafePtr<T>& operator=(const SafePtr<T>& other) = default;
        void* raw() const { return ptr; }
    };

    template <typename T, typename... Ts>
    SafePtr<T> make(Ts&&... args) {
        // DEC-004/DEC-029: T must fit in a page (slab class or whole-page bypass).
        constexpr size_t c = vmsmalloc::sizeClassFor(sizeof(T));
        static_assert(c < vmsmalloc::kNumSizeClasses || sizeof(T) <= arch::smallPageSize,
                      "VMSubstrate::make<T>: sizeof(T) exceeds a page");
        // DEC-025: for slab-backed T, alignof(T) must not exceed its size class's
        // slot alignment. Short-circuits for the whole-page bypass (c ==
        // kNumSizeClasses), whose pageSize alignment dominates any alignof(T) —
        // the `c >= kNumSizeClasses` term also keeps slotAlignment(c) from being
        // evaluated out of range in that case.
        static_assert(c >= vmsmalloc::kNumSizeClasses ||
                          alignof(T) <= vmsmalloc::slotAlignment(c),
                      "VMSubstrate::make<T>: alignof(T) exceeds the slot alignment of its "
                      "size class. Pad T into a power-of-two size class, split the "
                      "over-aligned subobject, or use a different allocator.");
        auto* mem = vmsmalloc(sizeof(T));
        // malloc (not calloc) semantics, per DEC-024 ("does not zero-initialize
        // returned memory in any build configuration"): with no constructor
        // arguments, *default*-initialize — a trivially-constructible T is left
        // UNINITIALIZED rather than zero-filled. (Value-init `T()` would memset
        // the storage, which dominated allocator profiles.) Callers needing
        // zeroed storage construct it explicitly (e.g. make<T>(T{}) or a T whose
        // constructor zeroes).
        if constexpr (sizeof...(Ts) == 0) {
            return new (mem) T;                       // default-initialization
        } else {
            return new (mem) T(forward<Ts>(args)...); // direct-initialization
        }
    }

    template <typename T>
    void destroy(SafePtr<T> obj) {
        if (obj) {
            obj->~T();
            vmsfree(obj.raw());
        }
    }
}

#endif //CROCOS_VMSUBSTRATE_H
