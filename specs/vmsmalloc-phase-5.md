---
kind: leaf
status: drafting
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 5 — `vmsmalloc` happy path

> Implement the body of `kernel::mm::VMSubstrate::vmsmalloc(size_t)` declared in
> `kernel/include/mem/VMSubstrate.h:21`. Covers DEC-029's whole-page bypass, the DEC-037
> unified-magazine fast path with DEC-039 pre-read, the DEC-034/DEC-041 chained-Treiber refill
> with lazy DEC-040 first-touch freshness (via Phase-4 P4-DEC-010 hooks on `pop()` plus
> per-`m.head`-transition calls in Phase 5), the DEC-036 eager-free walk, and the DEC-018 fresh-slab slow
> path. The DEC-015 packed-tagged-head encoding lands as the vmsmalloc-supplied `HeadEncoding`
> policy for Phase 4's `Core::ChainedTreiberStack`. The 8-way `SlabDescriptor<N>` dispatch
> lives inline in `vmsmalloc.cpp` per P5-DEC-001. Edge-input asserts (`size == 0`,
> `size > pageSize`, IRQ/NMI/#GP/#MC context) and the `vmsfree` body are Phase 7 / Phase 6;
> Phase 5 keeps a single defensive `size <= pageSize` check on the whole-page branch to avoid
> silently returning a 4 KiB page for a 16 KiB request.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No `vmsfree`** — Phase 6 covers DEC-026 validation, the descriptor-magic check, debug
  poison, bookkeeper free, and the Full→Partial publish.
- **No DEC-014/DEC-023/DEC-004 entry-point assertions.** Phase 7 wires the
  "not in IRQ/NMI/#GP/#MC" and `size != 0` debug-build asserts. Phase 5 carries only the one
  load-bearing `size <= pageSize` assertion on the whole-page branch (without it,
  `vmsmalloc(8000)` silently returns a 4 KiB page, which is a correctness violation, not just
  a debug-friendliness gap).
- **No `make<T>` / `destroy<T>` changes.** Both already exist in `VMSubstrate.h:46–58` and call
  through `vmsmalloc` / `vmsfree`. Phase 5 lights them up by giving `vmsmalloc` a body.
  Adding the DEC-025 `static_assert(alignof(T) <= slotAlignment(class))` to `make<T>` is
  deferred (it requires the `slotAlignment(c)` accessor from Phase 2 to be reachable from
  `VMSubstrate.h`, which means including `VMSubstrateSlab.h` from the public header — a
  scope decision better made in Phase 7 with the assertion-paths work).
- **No internal-header migration of `vmsmalloc` declaration.** Per parent-spec DEC-028
  amendment (2026-05-27), `vmsmalloc` / `vmsfree` must stay declared in the public
  `VMSubstrate.h` because `make<T>` / `destroy<T>` are templates whose bodies must live
  there. Phase 5 adds an in-source comment at the declarations indicating that external
  callers should prefer `make<T>` / `destroy<T>` — convention only, no structural enforcement.
- **No magazine tuning policy.** `MagazineTuning::overflowCount` and `starvationCount` are
  bumped on the relevant slow-path events (per the Concurrency Model), but the policy that
  reads, resets, and adjusts `currentK` lands in Phase 10. Phase 5 leaves `currentK` at
  `kInitialK` for the lifetime of the kernel.
- **No vmsfree-path `ensureTLBEntryFresh` discipline** (Phase 6 owns DEC-016 / DEC-026).
  Phase 5 carries the *allocator-side* freshness call (DEC-040 amended — lazy first-touch) at each `m.head` transition —
  no read of any Treiber-popped descriptor field happens without the corresponding
  `ensureTLBEntryFresh` having run.
- **No `LibAlloc::SlabBookkeeper` API changes.** Phase 1 already exposes
  `allocSlot(transition)` / `freeSlot(slotIndex, transition)` with `OccupancyTransition`
  predicates `becameFull()` / `becameAvailable()`. Phase 5 consumes them.
- **No userspace tests of `vmsmalloc` itself.** The integration harness is Phase 8. Phase 5
  ships only an in-kernel smoke exercise (one alloc per size class at boot, logged via
  `klog`) — enough to catch obvious init bugs and confirm the slow path constructs a slab.
  Existing `naiveTest` regression is also a gate.

## Consumer Contract

### `void* kernel::mm::VMSubstrate::vmsmalloc(size_t size)`

Already declared at `kernel/include/mem/VMSubstrate.h:21`. Phase 5 supplies the body in
`kernel/mm/vmsmalloc.cpp` (new file; symmetric with the existing `VMSubstrate.cpp`).

**Contract (Phase 5 portion):**

- For `size > largestSizeClass` (= 512 B under DEC-003) and `size <= arch::smallPageSize`:
  returns the page base address of a fresh `VMSubstrate::allocPage()` call. Page-aligned, no
  slab descriptor, no Treiber-stack interaction. (DEC-029.)
- For `size > arch::smallPageSize`: Phase 5 asserts (`size <= pageSize` is load-bearing for
  the whole-page branch's correctness). Phase 7 hoists this to the entry-point assert chain;
  Phase 5's assert is positionally the branch guard.
- For `0 < size <= largestSizeClass`: returns a pointer to an `slotSize` slot in a slab on
  the caller's NUMA domain (DEC-018). Pointer alignment per DEC-001. The slab descriptor
  is initialized once at slab creation (DEC-018, DEC-044). Subsequent allocations from the
  same slab on the same CPU drain the magazine head (DEC-037).
- For `size == 0`: Phase 5 does not assert — `sizeClassIndex<kSlabSizeClasses>(0)` returns
  index 0 (smallest class), so a `vmsmalloc(0)` call silently allocates an 8 B slot. Phase 7
  adds the DEC-023 assert. (Phase 5's tests do not exercise `size == 0`.)
- Concurrency: per parent-spec Concurrency Model. The fast path is uncontended (DEC-014 /
  DEC-030 guarantees), the slow path uses one ChainedTreiberStack pop (acq-rel CAS) plus an
  amortized lazy freshness (one `ensureTLBEntryFresh` per slab consumed, paid at first touch). No path returns null — `allocPage` failure panics
  (DEC-012).
- Calling-context contract: caller must hold the thread non-preemptible and pinned to its
  current CPU (DEC-030). Phase 5 does not assert this in debug builds (Phase 7 adds the
  assert); under the current no-scheduler kernel the obligation holds trivially.

### DEC-015 head encoding policy (`VmsHeadEncoding`)

New struct in `kernel/mm/vmsmalloc.cpp` (file-scope, internal linkage), supplied to Phase 4's
`Core::ChainedTreiberStack` as the `HeadEncoding` policy parameter:

```cpp
struct VmsHeadEncoding {
    using Storage = uint64_t;
    using Tag     = uint64_t;

    // DEC-015: head = (counter << 27) | descPageOffset; counter is 37 bits; offset is 27 bits.
    static constexpr unsigned kOffsetBits  = 27;
    static constexpr unsigned kCounterBits = 37;
    static constexpr Tag      kCounterMask = (Tag{1} << kCounterBits) - 1;
    static constexpr Storage  kOffsetMask  = (Storage{1} << kOffsetBits) - 1;

    static Storage pack(SlabDescriptorBase* p, Tag t) {
        if (p == nullptr) return (t & kCounterMask) << kOffsetBits;   // empty stack
        const uintptr_t off = (reinterpret_cast<uintptr_t>(p) - vmsBase) >> log2(smallPageSize);
        // off fits in kOffsetBits because the VMSubstrate VA window is a single PDPT (boot-asserted).
        return ((t & kCounterMask) << kOffsetBits) | (off & kOffsetMask);
    }
    static SlabDescriptorBase* unpackPointer(Storage s) {
        const Storage off = s & kOffsetMask;
        if (off == 0) return nullptr;  // see Invariants — offset 0 reserved for the empty marker
        const uintptr_t va = vmsBase + (off << log2(smallPageSize));
        return reinterpret_cast<SlabDescriptorBase*>(va);
    }
    static Tag advanceTag(Tag t) { return (t + 1) & kCounterMask; }  // DEC-015 push-only, masked
    static Tag unpackTag(Storage s) { return (s >> kOffsetBits) & kCounterMask; }
};
```

Hands the boot-time `vmsBase` constant to the file-scope inline pack/unpack via the captured
`vmsBase` variable populated by `vmsmallocInit` (Phase 3). The "empty stack" sentinel is
`offset == 0` with any counter — `vmsBase` itself can never be a slab descriptor because
arena 0 is the static-buffer region under Phase-3 DEC-033 / P3-DEC-003 (and that slot's first
page contains no allocatable descriptor). The Phase-4 `TreiberHeadEncoding` concept's
"zero-init Storage decodes to empty pointer" invariant is satisfied: `unpackPointer(0)`
returns `nullptr` because the offset field is zero.

Note: the boot-time assertion that the VMSubstrate VA window fits in a single PDPT (so 27
offset bits suffice) is added in Phase 5 alongside the encoding (the assertion lives next to
the encoding, not in VMSubstrate itself, because vmsmalloc is the consumer of the
single-PDPT property).

### Internal symbols added in `kernel/mm/vmsmalloc.cpp`

```cpp
namespace kernel::mm::VMSubstrate {
    // file-scope, internal linkage
    namespace {
        uintptr_t vmsBase;
        size_t    vmsSize;

        // Phase-4 stack alias parameterized by the vmsmalloc-specific encoding policy.
        using PartialStack = Core::ChainedTreiberStack<
            vmsmalloc::SlabDescriptorBase,
            vmsmalloc::SlabNextLinkage,      // ::next via &SlabDescriptorBase::next
            vmsmalloc::SlabChainLinkage,     // ::chainNext + chainDepth via that field
            VmsHeadEncoding>;

        // partialStackFor(d) constructs a thin wrapper view over `partialFor(d)[c]`.
        // See P5-DEC-002.
        PartialStack* partialStackFor(DomainID d, size_t c);

        // Slab-creation slow path: allocPage + descriptor init + bookkeeper seed for class c.
        SlabDescriptorBase* createFreshSlab(arch::ProcessorID i, size_t c);
    }

    void* vmsmalloc(size_t size) { /* ... */ }
}
```

`SlabNextLinkage` / `SlabChainLinkage` are tiny extractor structs satisfying the Phase-4
`TreiberLinkageExtractor` / `ChainedTreiberChainLinkage` concepts, defined in
`vmsmalloc.cpp`. They wire `getNext` / `setNext` to `SlabDescriptorBase::next` and the
chain methods to `chainNext` / `chainDepth`.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 1 — extended `LibAlloc::SlabBookkeeper` | Provides `allocSlot(transition)` returning `OccupancyTransition` with `becameFull()` / `becameAvailable()` predicates; arbitrary `SlotCount`; acq-rel atomics per DEC-042 #4. Phase 5's fast path calls `allocSlot`. | Yes — Phase 1 must land. |
| Phase 2 — `VMSubstrateSlab.h` types and constants | `SlabDescriptorBase`, `SlabDescriptor<N>`, `kSlabDescriptorMagic`, `kSlabSizeClasses`, `kNumSizeClasses`, `slotCount(c)` / `slotSize(c)` / `slot0Offset(c)` / `slotAlignment(c)`, and the constexpr fixpoint tables. | Yes — Phase 2 must land. |
| Phase 3 — VMSubstrate metadata storage + `vmsmallocInit` | `TreiberHead`, `Magazine`, `MagazineTuning` types; `partialFor(d)` / `tuningFor(d)` accessors; `perDomainBufs[]` populated; `kInitialK` seeded into every tuning row. **Magazines accessed via `kernel::cpuLocal().magazines[c]` per P7-DEC-010** (Phase 7 amendment 2026-05-27); the magazines live in the per-CPU `CpuLocal` struct in arena metadata. Phase 5 also reads `vmsBase` / `vmsSize` captured by `vmsmallocInit`. | Yes — Phase 3 must land. |
| Phase 4 — `Core::ChainedTreiberStack<T, HeadLinkage, ChainLinkage, HeadEncoding>` | Provides `pop()` (single CAS, returns `PoppedChain{head, depth}`), `push(element)` (extend-or-singleton, used for the cross-domain singleton free in Phase 6 — Phase 5 does not call `push`), and `pushChain(chainHead, depth)` (single CAS publish, **used by the magazine-flush path in Phase 6**, not Phase 5). Phase 5 calls only `pop()` and constructs a `PartialStack` view over each `TreiberHead`. The named ordering constants (`kTreiberPopLoadOrder` etc.) are reused. | Yes — Phase 4 must land. |
| `kernel::mm::VMSubstrate::allocPage()` / `freePage()` / `ensureTLBEntryFresh()` | `allocPage` for slow-path slab creation (DEC-018) and the DEC-029 large-request bypass (panic on failure per DEC-012); `freePage` for the DEC-036 eager-free walk; `ensureTLBEntryFresh` for the DEC-040-amended lazy first-touch freshness (called inside Phase-4 `pop()` via `Hooks::onPreTouch` P4-DEC-010, plus at every Phase-5 `m.head` transition). Per DEC-046, `allocPage` invalidates the calling CPU's own TLB entry — Phase 5 relies on this so the fresh-slab path does not need `ensureTLBEntryFresh`. | Yes — live; Phase 3 also adds `reservePerDomainStaticBuffer` / `localCachePageFor` (init-only, not on Phase 5's hot paths). |
| `arch::getCurrentProcessorID()` (or whichever helper returns the calling CPU's logical ID) | Read on every `vmsmalloc` call to index `magazinesFor(i)`. **Action item:** confirm the existing accessor's name and signature against `kernel/include/arch/amd64/amd64.h` during step 1 of implementation. If the existing primitive is `arch::getCurrentCPU()` or similar, follow that naming in `vmsmalloc.cpp`. | Yes — live. |
| `kernel::mm::NUMAPolicy::domainFor(arch::ProcessorID)` | Read on every fast-path miss to compute `numaOf(currentCPU())` for the `partial[d][c]` Treiber lookup. | Yes — live. |
| `Core::SizeClass.h`'s `sizeClassIndex<array>(size)` | Log2-jump-table size-class lookup. Used by both the fast path (class lookup for the magazine row) and the DEC-029 bypass branch (returns `npos = -1` when `size > largestSizeClass`). | Yes — live (`InternalAllocator.cpp:724` is a precedent). |
| `kernel::klog::info` / `klog::debug` | Boot-time klog line confirming `vmsmalloc` is operational; in the Phase-5 smoke exercise. | Yes — live. |
| `kernel/general.icd` | `[VMSubstrateSlab]` entry already registered by Phase 3. No change required. | Yes — Phase 3 output. |

## Invariants

<!-- Conditions Phase 5's code must preserve at all times. -->

- **Hot-path magazine ownership.** Per parent-spec DEC-037/DEC-014/DEC-030: on entry to the
  fast path on CPU `i`, no other CPU is writing `magazines[i][c]`. Phase 5's code reads and
  writes `m.head` / `m.depth` without atomics. A violation would require the calling thread
  to be preempted or migrated mid-call (DEC-030 forbids) or for an IRQ handler to call
  vmsmalloc on the same CPU (DEC-014 forbids; Phase 7 adds the debug assert).
- **DEC-039 pre-read positional discipline.** The load `nextLocal = head->chainNext` **must
  precede** the call to `bookkeeper.allocSlot(...)` in CPU program order on the fast path.
  Phase 5's source carries a comment `// DEC-039: pre-read m.head->chainNext before allocSlot.`
  at the load. Any refactor that consolidates the load into a helper called after `allocSlot`
  silently reintroduces ITEM-041 / ITEM-045 chain corruption. The bookkeeper's acq-rel atomic
  on `allocSlot` (DEC-042 #4 — Phase 1) provides the synchronizes-with edge that orders the
  pre-read before any freer's subsequent `chainNext` write.
- **Lazy first-touch freshness (DEC-040 amended 2026-05-27).** Two integration points: (a)
  Phase-4 `pop()` fires `Hooks::onPreTouch(topPtr)` (P4-DEC-010) AFTER the head acquire-load
  but BEFORE any read of `*topPtr` — vmsmalloc's hook calls `ensureTLBEntryFresh(topPtr)`.
  This is **load-bearing for shared-stack correctness**, not just for the popper's later
  consumption: `topPtr->next` is read pre-CAS and published as the new shared-stack head, so
  a TLB-stale read would corrupt the structure for every CPU. The acquire-load synchronizes
  value visibility through the *current* PTE but does NOT refresh this CPU's cached VA→PA
  mapping. (b) Phase 5 calls `ensureTLBEntryFresh(newHead)` at every `m.head` transition that
  advances to a previously-untouched chain element — the becameFull synchronous pop (DEC-037)
  and the eager-free walk (DEC-036). The first iteration's `m.head` is already fresh from
  the pop hook; subsequent transitions need explicit calls. No "eager sweep" walks the entire
  chain at refill — freshness is paid one-per-slab at first touch, same total cost as the
  prior eager scheme but spread across consumption.
- **`m.depth == 0 ⇔ m.head == nullptr`** (parent-spec Invariants). Phase 5 preserves this
  on every magazine mutation: refill writes both fields together; eager-free walks decrement
  `depth` only when advancing `head` along `chainNext`; the becameFull synchronous pop
  decrements `depth` and advances `head`. Failure mode: a path that sets `m.head = nullptr`
  without zeroing `m.depth` (or vice versa) breaks the invariant; Phase 6's freer push and
  Phase 5's refill exit are the only sites that touch both fields together.
- **Fresh-slab `chainDepth = 1`, `chainNext = nullptr`.** A newly-`allocPage`'d slab is a
  singleton chain; the fast path sets `m.depth = 1, m.head = desc` after construction. The
  next fast-path iteration sees a Partial slab with `depth = 1` and allocates from it. If
  this is the only slab in the magazine and it becomes Full, the synchronous pop sets
  `m.head = chainNext = nullptr, m.depth = 0`, returning the magazine to the empty state —
  the next call will hit the refill path.
- **Magazine occupancy at fast-path entry.** Per parent-spec Invariants ("Magazine occupancy
  states"), at every externally observable moment slabs in the magazine chain are Partial or
  Empty — never Full. Phase 5 enforces this by the synchronous pop on `becameFull` *before*
  vmsmalloc returns. The transient between `allocSlot`'s atomic and the pop is not externally
  observable (single-CPU, no preemption).
- **DEC-029 dispatch:** every slab-backed slot returned satisfies `(p & (pageSize - 1)) != 0`
  (slot 0 sits at `slot0Offset(c) >= sizeof(SlabDescriptorBase) > 0`). Phase 5's fast path
  returns `slotZeroAddr(desc) + slotIndex * slotSize(c)`; the lower bound is automatically
  satisfied by the descriptor occupying offset 0. Phase 6's freer relies on this to
  disambiguate slab slots from whole-page allocations.
- **`desc->numaDomain = numaOf(currentCPU())` at slab creation** (DEC-018). Phase 5's
  `createFreshSlab` reads `arch::getCurrentProcessorID()` (= `i`) once and writes
  `desc->numaDomain = NUMAPolicy::domainFor(i)`. The recorded domain may diverge from the
  physical placement of the page under local-exhaustion fallback (parent-spec accepts this).
- **No allocator-side write to `partial[d][c]` in Phase 5.** Phase 5 only `pop()`s from the
  shared stack; pushes are exclusively a freer-side operation (Phase 6: magazine flush
  inside the freer when `m.depth >= currentK`, cross-domain singleton push on the cross-domain
  gate). The DEC-036 eager-free walk calls `VMSubstrate::freePage` on chain elements drained
  from the magazine — those elements never reached the shared stack, so no shared-stack
  cleanup is needed.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `VMSubstrate::allocPage()` returns null on the slow path | Panic (DEC-012). The panic is delegated to `allocPage` itself; if it ever starts returning null instead of panicking, Phase 5's `createFreshSlab` adds a `kassert(desc != nullptr)`. | No |
| `VMSubstrate::allocPage()` returns null on the DEC-029 whole-page bypass | Same — panic via `allocPage`'s own guarantee. | No |
| `size > arch::smallPageSize` | Assert in the DEC-029 branch (`assert(size <= arch::smallPageSize)`). Phase 7 hoists this to the entry-point assert. | No |
| `size == 0` reaches the fast path | Undefined under Phase 5 (`sizeClassIndex` returns 0, an 8 B slot is allocated). Phase 7 adds the DEC-023 assert. Phase 5's tests do not exercise this. | No |
| Refill `pop()` returns `{nullptr, 0}` (shared stack empty for `(localDomain, c)`) | Falls through to the DEC-019 fresh-slab path. Phase 5 increments `tuningFor(localDomain)[c].starvationCount` (RELAXED add) before the fall-through. | Normal path |
| `chainDepth` returned by `pop()` is zero (Phase-4 contract violation: a pushed chain had `depth == 0`) | Not detected by Phase 5. Falls through to the post-pop walk loop which terminates immediately (`m.depth = 0`), and the next iteration triggers another pop. If the shared stack contains a depth-0 chain head, Phase 5 leaks the slab. **Phase-4 caller-bug per its own Failure Modes table.** | No (Phase-4 caller bug) |
| TLB freshness call on a popped chain element page-faults (page never mapped) | This would indicate a `desc` whose VA falls outside any active arena — i.e., a stale head value passed the DEC-015 ABA discipline (essentially impossible). The fault is caught by the kernel's existing page-fault handler; not a Phase-5 concern. | No |
| Eager-free walk would `freePage` the last cached slab | Prevented by the DEC-036 floor (`m.depth > 1` required). The walk exits with `m.depth == 1` even if the floor slab is Empty. | Normal path |
| Concurrent freer pushes a singleton chain to `partial[localDomain][c]` between this CPU's pop-miss and its fresh-slab construction | Benign — the freshly constructed slab lands on this CPU's magazine; the singleton sits on the shared stack for the next refill. No correctness gap. (Worst case: one extra `allocPage` call when a `pop()` retry would have served us. The cost is a single page, reclaimable on the next slow path via DEC-036 if the slab becomes Empty.) | Normal path |
| Concurrent refill on a different CPU pops the same chain | Cannot happen — `pop()` is a CAS-on-head transaction; one CPU's success is the other CPU's CAS failure with retry. Phase 4's invariant. | N/A |

## Questions

<!-- Open questions for resolution during implementation. -->

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P5-ITEM-001 | Resolved (user direction 2026-05-27) | | | Should the file be named `kernel/mm/vmsmalloc.cpp` or fold into `kernel/mm/VMSubstrateSlab.cpp` (Phase 3's TU)? | Resolved: separate `kernel/mm/vmsmalloc.cpp`. Keeps init code (Phase 3) and hot-path code (Phase 5) in distinct TUs; the linker sees both. |
| P5-ITEM-002 | Resolved 2026-05-27 → lazy freshness | | | The original concern was about the cost of an eager freshness sweep across long chains (up to 63 `ensureTLBEntryFresh` calls per refill at `kMaxK = 64`). Is the cost acceptable? | Resolved by parent-spec DEC-040 amendment 2026-05-27: drop the eager sweep entirely. Lazy first-touch freshness fires one `ensureTLBEntryFresh` per slab consumed, at the natural `m.head` transition point (becameFull pop / eager-free advance) — same total cost as the eager sweep, but spread across the chain's lifetime rather than batched at refill. The load-bearing pop-internal call lives inside Phase 4's `pop()` via the P4-DEC-010 `Hooks::onPreTouch` callback. |
| P5-ITEM-003 | Resolved 2026-05-27 (the prior "yes safe" answer was wrong) | | | Is the Phase-4 `ChainedTreiberStack::pop`'s pre-CAS read of `topPtr->chainDepth` and `topPtr->next` safe without an explicit freshness call? | Resolved: **No, not without a freshness call.** The earlier "yes" answer conflated value visibility with TLB freshness. The acq-load on `head` synchronizes value visibility through the *current* PTE, but does NOT refresh this CPU's cached VA→PA mapping — if the popper's TLB caches a stale entry from a previous incarnation of the same page, the read of `*topPtr` goes through the wrong PA and sees content from the previous backing. **Critical consequence:** `topPtr->next` is published as the new shared-stack head via the pop CAS, so a TLB-stale read corrupts the structure visibly to all CPUs. Resolution: Phase 4 P4-DEC-010 added `Hooks::onPreTouch(topPtr)`, fired after the head acquire-load but before any read of `*topPtr`. vmsmalloc's Hooks calls `ensureTLBEntryFresh(topPtr)`. Parent-spec DEC-040 amended to record the load-bearing nature of the pop-internal freshness call. |
| P5-ITEM-004 | Resolved 2026-05-27 | | | The DEC-015 encoding's `vmsBase` constant is captured by `vmsmallocInit` (Phase 3). Should Phase 5's `VmsHeadEncoding` read it via a file-scope variable, or via an inline-constexpr-but-not-really `static const` initialized at `vmsmallocInit`? | Resolved: file-scope `static const uintptr_t vmsBase` written exactly once during `vmsmallocInit` then treated as immutable. The encoding's pack/unpack read it without atomics (no concurrent writers post-init). |
| P5-ITEM-005 | Resolved 2026-05-27 | | | DEC-015 encoding's empty-stack marker is `offset == 0`. The Phase-4 `TreiberHeadEncoding` concept requires `unpackPointer(0) == nullptr`. The Phase-3 zero-fill of per-domain buffers makes every initial `TreiberHead::head == 0`, so the Treiber stacks start empty by construction. Does this hold if any descriptor happens to land at exactly `vmsBase`? | Resolved: by Phase-3 P3-DEC-003 the static-buffer region occupies the topmost arena slot, so `vmsBase` (and the first page of the VMSubstrate VA window) is reserved for VMSubstrate's own use — no descriptor can be allocated there. A boot-time runtime check pins this (P5-DEC-003 already includes the range-check assert). |
| P5-ITEM-006 | Resolved 2026-05-27 → P5-DEC-007 | | | Where should the on-CAS-failure / on-empty-stack hook integration live — Phase 4 primitive, Phase 5 external bumps, or both? | Resolved: Phase 5 ships external counter bumps in `vmsmalloc.cpp` (P5-DEC-007). Phase 4 template-parameter hooks on `ChainedTreiberStack` is a recommended follow-up evolution, naturally tied to Phase 10's tuning policy work. |

## Decisions

<!-- Settled decisions specific to Phase 5. -->

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P5-DEC-001 | Settled | **8-way `SlabDescriptor<N>` dispatch lives as an inline `switch` statement at the slab-creation site in `vmsmalloc.cpp`.** Phase 5's `createFreshSlab` body is a `switch (c)` over `[0, kNumSizeClasses)` that placement-news the correct `SlabDescriptor<slotCount(c)>` at the just-`allocPage`'d page and seeds its bookkeeper. Phase 6's `vmsfree` dispatch over `desc->sizeClass` is a sibling switch (one per phase, no shared helper). | Per user direction: keep the dispatch direct rather than introducing a templated-lambda helper. The two switches will be visually parallel; a future refactor that wants to consolidate them can introduce a helper later without breaking the per-phase code. The switch is 8 cases; the compiler will likely turn it into a jump table. |
| P5-DEC-002 | Settled (per user direction 2026-05-27) | **`partial[d][c]` storage is a full `Core::ChainedTreiberStack` instance (or a struct containing one), not a bare 64 B `TreiberHead`.** `maxChainLength` is the Bonwick-Adams magazine depth — logically equal to what the earlier-draft `MagazineTuning::currentK` was tracking — and is runtime-tunable by the Phase-10 policy. Phase 5 calls `partialFor(d)[c].pop()` directly; no view class, no `reinterpret_cast`. The **precise layout of the per-domain buffer is intentionally under-specified at the spec level** and resolved at implementation time: whether `ChainedTreiberStack` instances sit alongside the remaining tuning counters (`overflowCount`, `starvationCount`, `policyLock`) in one struct, in parallel arrays, or via a hybrid layout is an implementation choice. Phase 3's `partialFor` / `tuningFor` accessor signatures may evolve accordingly (e.g., `partialFor` returning `ChainedTreiberStack*`). | Per user direction (2026-05-27): treat `maxChainLength` as the magazine-depth knob, not a separate `currentK`. The earlier bare-`TreiberHead` framing was an artifact of carrying the layout assumption forward from when the spec spelled out a single `uint64_t` head storage; carrying the full stack instance is cleaner and removes the layout-mismatch hazard. Under-specifying the buffer layout leaves room for additional performance counters (parent-spec hazard "Magazine tuning counter overflow" and follow-up tuning work) without needing to revisit the buffer-layout decision per counter added. |
| P5-DEC-003 | Settled | **The DEC-015 `VmsHeadEncoding` policy supplies `kSlabSizeClasses`-aware compile-time validation of the encoding's bit budget**: `static_assert((static_cast<uintptr_t>(1) << kOffsetBits) * arch::smallPageSize >= vmsSizeUpperBound, ...)`. The runtime `vmsBase` capture in `vmsmallocInit` is paired with a one-shot boot check that `vmsSize` (also captured) actually fits in `(1 << kOffsetBits) * smallPageSize`. | Codifies the parent-spec DEC-015 single-PDPT assumption inside the encoding, not as a free-floating assertion elsewhere. The compile-time check uses a conservative `vmsSizeUpperBound = 512 GiB` constant (one PDPT); the runtime check tightens to the actual VMSubstrate VA-window size. |
| P5-DEC-004 | Settled (per Phase 1 P1-DEC-001 amendment 2026-05-27) | **DEC-036 eager-free walk uses `bookkeeper.isEmpty()`.** Phase 1's amended P1-DEC-001 routes tail-bit masking through `SlabBookkeeper::reserveSlot` (which increments `reservedCount`) and amends `isEmpty()` to subtract the compile-time `kTailBits` constant: `allocatedCount == 0 && reservedCount == kTailBits`. For vmsmalloc slabs `reservedCount == kTailBits` immediately after seeding (the only caller of `reserveSlot` is `seedAllAvailable` itself; vmsmalloc never makes additional `reserveSlot` calls because the descriptor area sits *outside* the slot space — slots begin at `slot0Offset(c) > 0`). Therefore `isEmpty()` correctly reduces to `allocatedCount == 0` for vmsmalloc, which is the DEC-036 predicate. Loop body: `while (m.depth > 1 && m.head->bookkeeper.isEmpty()) { next = m.head->chainNext.load(RELAXED); VMSubstrate::freePage(m.head); m.head = next; m.depth--; }`. | Floor preserved as `m.depth == 1` (DEC-036 single-slab floor); no `freePage` on the last cached slab. `chainNext` load is RELAXED because the chain element belongs exclusively to this CPU's magazine (DEC-034 invariant) — the cross-CPU happens-before edge was already established by the refill `pop`'s ACQUIRE-load. The dual semantics of `isEmpty()` are coherent under the amended formula: for PageAllocator (where `kTailBits == 0` because `SlotCount % 64 == 0`) it means "fully reclaimable — no allocations, no caller reservations"; for vmsmalloc (where `kTailBits > 0` for most classes and `reservedCount` equals exactly that) it reduces to "no live allocations". |
| P5-DEC-005 | Settled | **The Phase-5 boot smoke exercise runs one `vmsmalloc` per size class from `kernel::mm::vmsmalloc::vmsmallocBootSmoke()`, called from `vmsmallocInit` immediately after init logging.** The exercise allocates 8 slots (one per class), logs each returned address, then leaks them — Phase 5 has no `vmsfree` body, so the slabs stay live for the kernel's lifetime. Cost: 8 size-class entries × 1 page each = 32 KiB pinned across the kernel run. | Catches the obvious init bugs (slab creation, descriptor magic, slot arithmetic, returning a slab-backed pointer) without depending on Phase 6 / Phase 8 infrastructure. The leak is acceptable for a development phase; Phase 6 will let the smoke `vmsfree` the slots before logging. The exercise sits behind `#ifdef CROCOS_VMSMALLOC_BOOT_SMOKE` so it can be excised once the integration harness lands. |
| P5-DEC-006 | Settled | **`createFreshSlab` initialization order is: `desc->magic = kSlabDescriptorMagic` *first*, then the other prefix fields (`numaDomain`, `sizeClass`, `chainDepth = 1`, `chainNext = nullptr`, `next = nullptr`), then bookkeeper construction with the reserved-slots tail-bit mask seeded.** The descriptor write is exposed to remote freers only when the slab is later flushed (Phase 6) — at that point the flush's release-CAS propagates every prior write. While the slab is on this CPU's magazine (the entirety of Phase 5's interaction with it), no remote CPU can reach the descriptor. | Pinning the order documents the obvious-but-not-stated discipline so a future refactor doesn't reorder `desc->magic` to last. The flush's release-CAS handles cross-CPU publication (Phase 6's concern); Phase 5 only requires same-CPU program-order consistency, which the language guarantees. |
| P5-DEC-007 | Settled (superseded 2026-05-27 by Phase 4 P4-DEC-010) | **Counter bumps (`starvationCount`, `overflowCount`) and freshness calls live inside Phase 4's `Hooks` policy, not in Phase 5's `vmsmalloc.cpp`.** Phase 5 instantiates `Core::ChainedTreiberStack<..., VmsmallocHooks>` where `VmsmallocHooks` is a stateful struct carrying a `MagazineTuning*` pointer. The struct's `onEmptyStack()` bumps `starvationCount`, `onCasFailure()` bumps `overflowCount`, and `onPreTouch(topPtr)` calls `VMSubstrate::ensureTLBEntryFresh(topPtr)`. The initial P5-DEC-007 "bumps live externally in `vmsmalloc.cpp`" plan was superseded the same day as part of the lazy-freshness amendment — once the freshness call needed to live inside `pop()` anyway (so it can fire *before* the pre-CAS read of `topPtr->next`), pushing the counter bumps inside via the same Hook mechanism is the natural consolidation. | Phase 4 P4-DEC-010's hook integration moved the load-bearing freshness call inside `pop()` (no external alternative for that) and exposed `onCasFailure` / `onEmptyStack` as natural sibling hooks. Bundling the tuning-counter bumps into the same Hooks struct gives one point-of-truth and avoids duplicate bookkeeping across vmsmalloc and Phase 4. `[[no_unique_address]] Hooks hooks{}` ensures default-instantiated Core consumers (e.g., future LibAlloc or test-only stacks) pay zero bytes for the unused hook slots; vmsmalloc's stateful hooks carry ~8 B per `(d, c)` stack. |

## Hazards

- **DEC-039 pre-read positional discipline (refactor-fragile).** Pinned by parent-spec
  Hazards. The `head->chainNext` load **must** appear before `bookkeeper.allocSlot(head, ...)`
  in source order; a future "helpful" refactor that moves the load into a post-allocSlot
  block reintroduces ITEM-041 / ITEM-045 chain corruption. Phase 5 carries a comment at the
  load site citing DEC-039. Phase 8's integration test ("Same-domain becameFull chain
  integrity") is the regression gate.
- **Hooks::onPreTouch must fire BEFORE the read of `topPtr->next` inside Phase-4 `pop()`.**
  Per DEC-040 (amended) and P4-DEC-010, the pop hook is load-bearing for shared-stack
  correctness — `topPtr->next` is read pre-CAS and republished as the new shared head. A
  TLB-stale read corrupts the stack visibly to every CPU. Phase 4's pop body has a comment
  at the hook site citing DEC-040; a refactor that moves the hook call past any read of
  `*topPtr` silently reintroduces the corruption hazard.
- **Lazy-freshness positional discipline.** `ensureTLBEntryFresh(m.head)` must fire AFTER
  `m.head` advances to a new chain element and BEFORE any read of the new head's fields.
  The becameFull pop and the eager-free walk both rely on this ordering. A refactor that
  delays the call (e.g., to "the start of the next vmsmalloc call") would race with a
  concurrent same-domain freer push that touches the new head's `chainNext`.
- **`SlabBookkeeper::isEmpty()` correctness depends on the amended P1-DEC-001 invariant
  `reservedCount == kTailBits` after `seedAllAvailable`.** Per P5-DEC-004, vmsmalloc relies
  on `isEmpty()` evaluating to `allocatedCount == 0` after the `kTailBits` subtraction — i.e.,
  the bookkeeper post-init has `reservedCount == kTailBits` exactly, with the offset baked
  into the `isEmpty()` body. If a future change to `seedAllAvailable(usableCount)` stops
  routing tail-bit masking through `reserveSlot` (e.g., reverts to `bitmap.reserveBit`),
  `reservedCount` would drop to 0 and `isEmpty()`'s `reservedCount == kTailBits` check
  underflows — the DEC-036 walk would silently stop draining Empty chain heads. Watch for
  any change to `seedAllAvailable(usableCount)`'s body in `Slab.h` (P1-DEC-001 amendment
  is the source of truth).
- **Per-domain buffer layout is intentionally under-specified.** P5-DEC-002 defers the
  precise placement of `ChainedTreiberStack` instances, tuning counters, and any future
  performance counters to implementation time. The trade-off: the spec doesn't pin the
  layout, so an implementer must make a coherent choice (e.g., one struct-of-stack-plus-counters
  per `(d, c)`, or parallel arrays). The Phase-3 accessor signatures (`partialFor`,
  `tuningFor`) may evolve. Watch for: any code site that assumes `partialFor(d)` returns a
  bare `TreiberHead*` (which the earlier draft specified) needs updating to whatever shape
  the implementation lands on.
- **DEC-015 single-PDPT assumption.** If a future VMSubstrate change expands the VA window
  beyond a single 512 GiB PDPT, the 27-bit offset field overflows and pack/unpack silently
  corrupt the encoded pointer. The P5-DEC-003 runtime check at `vmsmallocInit` panics on
  violation; preserve the check whenever VMSubstrate's VA layout changes.
- **DEC-015 counter wraparound.** 2^37 successful pushes per `(domain, class)` stack; ~14
  hours at 10 M pushes/sec amortized across chain transfer (parent-spec discussion). Not
  reachable in any realistic stress test; documented for completeness. The explicit
  `& kCounterMask` in `advanceTag` defends against the silent-overflow-into-offset hazard
  noted in parent-spec Hazards (c).
- **Slab-creation failure on `allocPage` panic.** `allocPage` panics on arena exhaustion
  (DEC-012). The panic message should distinguish Phase-5 vmsmalloc-slow-path callers from
  whole-page-bypass callers; today both use the same `allocPage`. If the diagnostic matters
  later, Phase 5 can wrap each call site with a `kassert(p != nullptr, "vmsmalloc: ...")`
  that catches a `nullptr` return *before* the panic; the panic itself is loud enough that
  this is optional.
- **Magazine state corruption on a non-flush write.** Phase 5 writes `m.head` and `m.depth`
  on the fast-path becameFull pop, on the refill `pop`-success branch, on the eager-free
  walk, and on fresh-slab construction. Every write touches both fields together (or zeroes
  both, when emptying the magazine). A future change that splits the write order (e.g.,
  `m.depth--` then `m.head = next` separated by a function call) is correct only because
  Phase 5's per-CPU invariant prohibits intra-CPU reentry (DEC-014/030). On a future
  preemptive scheduler with DEC-030 enforced by callers, the writes remain safe; on a
  preemptive scheduler with DEC-030 *not* enforced, the split would expose a transient
  inconsistent magazine state to a same-CPU preempting allocator. Watch for any future
  IRQ-context or fault-context vmsmalloc consumer.
- **Boot smoke leaks 8 pages per kernel run.** P5-DEC-005 — the smoke exercise leaks one
  slab per size class (32 KiB total). Phase 6 will let the smoke `vmsfree` the slots before
  logging; until then the leak is intentional and bounded.
- **`createFreshSlab` does not initialize the slab's free-bitmap tail bits.** Per DEC-011,
  Phase 1's bookkeeper accepts arbitrary `SlotCount` and the tail bits beyond `slotCount(c)`
  are permanently masked. Phase 5's slab construction must invoke whichever Phase-1 mechanism
  seeds those bits — `seedAllAvailable(slotCount(c))` per Phase 1's draft. If Phase 1 names
  the mechanism differently, Phase 5 follows suit. Implementation step calls this out
  explicitly.

## Verification Targets

| Property | Method |
|---|---|
| `vmsmalloc(8)` returns a valid 8 B-aligned pointer inside the VMSubstrate VA window on the BSP at boot | Phase 5 smoke exercise via `vmsmallocBootSmoke`; klog line confirms address |
| `vmsmalloc(size)` for `size ∈ {8, 16, 32, 64, 96, 128, 256, 512}` returns a pointer whose alignment matches DEC-001 (pow2 classes: size; non-pow2 96 B class: 16 B) | Smoke exercise asserts `(p & (slotAlignment(c) - 1)) == 0` for every class |
| `vmsmalloc(size)` for `size ∈ {513, 1024, 2048, 4096}` returns a page-aligned pointer (DEC-029 bypass) | Smoke exercise asserts `(p & (pageSize - 1)) == 0` |
| `vmsmalloc(size)` for `size > pageSize` asserts | Phase-7 test (Phase 5 carries the assert but doesn't run a test that triggers it) |
| Two `vmsmalloc(8)` calls in succession return different slot addresses within the same slab | Smoke exercise calls `vmsmalloc(8)` twice and logs `(p2 - p1) == 8` (next slot, since the same slab is used both times) |
| `naiveTest` (existing kernel stress test) continues to pass after Phase 5 lands | `cmake --build cmake-build-debug --target run` produces existing-behavior log; no PageAllocator regression |
| `make run`, `make run_numa`, `make run_numa_hmat` all boot through the Phase-5 smoke without panic | QEMU run, klog inspection |
| The DEC-015 encoding's boot-time bit-budget assert fires if the VMSubstrate VA window exceeds 512 GiB | Negative test: temporarily double VMSubstrate's VA window in a throwaway branch; confirm `vmsmallocInit` panics |
| DEC-039 pre-read appears as the documented pattern in `vmsmalloc.cpp` | Code review during merge; comment cites DEC-039 at the load site |
| DEC-040 (amended) `ensureTLBEntryFresh` fires inside Phase-4 `pop()` via the `onPreTouch` hook before any read of `*topPtr`, AND at every `m.head` transition in Phase 5 | Code review during merge; comments cite DEC-040 amended + P4-DEC-010 |
| Allocating one slab of each class fills the slab to single-slot occupancy with the magazine head pointing at that slab | Smoke exercise reads `kernel::cpuLocal().magazines[c].head` after each first-class allocation and asserts non-null |
| `tuningFor(localDomain)[c].starvationCount` is incremented on the first slow-path allocation for any class (the shared stack starts empty) | Smoke exercise reads the counter pre/post the first allocation; expects exactly one increment per class |
| TSan-on-ARMv8 (userspace exercise of the head-encoding policy and the `PartialStack` view) reports no races | Phase 8 deliverable; Phase 5 prepares the encoding policy to be exercisable in userspace by keeping `vmsBase` an injected constant (test-only override) |

## Testing Approach

- **Phase 5 ships an in-kernel smoke exercise** (P5-DEC-005), not a userspace test. The
  rationale matches Phase 3's: the slow path's `allocPage` dependency, the magazine state,
  and the DEC-015 encoding all need the VMSubstrate runtime, which is harder to mock than
  it's worth at this phase.
- The userspace test harness for vmsmalloc integration is Phase 8. Phase 5's design must
  *not* preclude userspace testing; in particular, the `VmsHeadEncoding` policy reads
  `vmsBase` from a file-scope variable so a userspace fixture can supply its own base
  without recompiling Phase 4's `ChainedTreiberStack`.
- **In-kernel smoke exercise:** runs from `vmsmallocBootSmoke` immediately after
  `vmsmallocInit`. Allocates one slot per class, logs each address with size class. Asserts
  alignment (DEC-001) and that the slab descriptor's magic matches `kSlabDescriptorMagic`.
  Allocates twice from class 0 (8 B) to confirm in-magazine reuse. Leaves the slabs live
  for the kernel's lifetime (no `vmsfree` yet).
- **Existing regression gates:** `naiveTest`, `make run` boot, and the three NUMA boot
  configurations.
- **No TSan variant.** Phase 5's hot path runs on the BSP only until SMP brings APs online,
  and the in-kernel test exercises only one CPU. The TSan validation of vmsmalloc's
  concurrency happens via the userspace harness in Phase 8 (built on Phase 4's
  `CoreTestRunnerTSan` and Phase 1's `LibAllocTestRunnerTSan`).

## Implementation Phases

<!-- Concrete ordered steps for Phase 5 itself. -->

1. **Confirm starting state.**
   - Phases 1–4 merged. `Slab.h` exposes `allocSlot(transition)`, `OccupancyTransition`,
     `becameFull()` / `becameAvailable()`, `allocatedSlotCount()` (Slab.h:185 — Phase 5
     uses this for DEC-036, not `isEmpty()` per P5-DEC-004), `reserveSlot` (for the
     descriptor-area carve-out at slab creation per DEC-008/DEC-045).
   - `VMSubstrateSlab.h` provides `SlabDescriptorBase`, `SlabDescriptor<N>` template,
     `kSlabDescriptorMagic`, `kSlabSizeClasses`, `kNumSizeClasses`, `slotSize(c)` /
     `slotCount(c)` / `slot0Offset(c)` / `slotAlignment(c)`. **Per P5-DEC-002**, the
     `partialFor` / `tuningFor` accessor signatures and per-domain buffer layout are
     under-specified at the spec level — confirm the Phase-3 implementation's chosen shape
     here. Likely outcome: `partialFor(d)` returns a `ChainedTreiberStack*` pointing at
     `(d, c)`'s stack instance, and `tuningFor(d)` returns the residual counter struct
     (overflow / starvation / policyLock) without `currentK` (which lives inside the stack
     as `maxChainLength`).
   - `Core::ChainedTreiberStack` provides `pop() -> {head, depth}` and is instantiable
     over a 64-bit `Storage` `HeadEncoding` (the existing `_use_intrinsic_atomic_ops`
     already covers size 8; Phase 4's extension to size 16 was for the
     `Uint128HeadEncoding` default). Phase 5 supplies the 64-bit `VmsHeadEncoding`.
   - `core/SizeClass.h`'s `sizeClassIndex<kSlabSizeClasses>(size)` is callable from kernel
     code. Returns `size_t(-1)` for oversized requests; Phase 5 treats
     `index >= kNumSizeClasses` as the DEC-029 bypass signal.
   - `arch::getCurrentProcessorID()` (or its existing equivalent) returns the calling
     CPU's logical ID. Confirm the exact name; use it consistently.
   - `kernel::mm::VMSubstrate::vmsmalloc` is declared at `kernel/include/mem/VMSubstrate.h:21`
     and currently has no definition. Confirm no other TU defines it (a missing-symbol link
     error would have surfaced).

2. **(Reserved — formerly the "add `isEmpty()` to Phase 1" step, now unnecessary.)** Phase 1
   already exposes `isEmpty()` (Slab.h:179) and `allocatedSlotCount()` (Slab.h:185). Per
   P5-DEC-004 Phase 5 uses `allocatedSlotCount()` for the DEC-036 walk because vmsmalloc
   slabs always have reserved descriptor slots and `isEmpty()` would never fire.

3. **(Reserved — formerly the "view class" step, now revoked by P5-DEC-002.)** Phase 5
   consumes the full `ChainedTreiberStack` instance held by Phase 3's per-domain buffer;
   no view class is introduced. Counter bumps and the load-bearing TLB-freshness call now
   live inside Phase 4's `Hooks` policy per the amended P5-DEC-007 / P4-DEC-010.

4. **Create `kernel/mm/vmsmalloc.cpp`.**
   - Includes: `<mem/VMSubstrate.h>`, `<mm/VMSubstrateSlab.h>`, `<core/atomic/TreiberStack.h>`,
     `<core/SizeClass.h>`, `<liballoc/Slab.h>`, `<arch/amd64/amd64.h>`, `<mem/NUMA.h>`,
     `<klog.h>`.
   - File-scope (`namespace { ... }` inside `namespace kernel::mm::VMSubstrate`):
     - `uintptr_t vmsBase = 0; size_t vmsSize = 0;` (populated by `vmsmallocInit` —
       expose a small `setVmsRange(uintptr_t, size_t)` from this TU, called by Phase 3's
       init).
     - `struct VmsHeadEncoding { ... }` per the Consumer Contract section above.
     - `struct SlabNextLinkage { static SlabDescriptorBase* getNext(SlabDescriptorBase& n) { return n.next; } static void setNext(SlabDescriptorBase& n, SlabDescriptorBase* p) { n.next = p; } };`
     - `struct SlabChainLinkage { ... };` analogous with `getChainNext` / `setChainNext` /
       `getChainDepth` / `setChainDepth`.
     - **`struct VmsmallocHooks`** — stateful Hooks policy per Phase-4 P4-DEC-010:
       ```cpp
       struct VmsmallocHooks {
           vmsmalloc::MagazineTuning* tuning;   // bound at stack construction

           template <typename T>
           void onPreTouch(T* topPtr) const {
               // DEC-040 amended: load-bearing pop-internal freshness call.
               // Must fire BEFORE any read of *topPtr.
               VMSubstrate::ensureTLBEntryFresh(topPtr);
           }
           void onCasFailure() const {
               tuning->overflowCount.fetch_add(1, memory_order_relaxed);
           }
           void onEmptyStack() const {
               tuning->starvationCount.fetch_add(1, memory_order_relaxed);
           }
       };
       ```
     - `using PartialStack = Core::ChainedTreiberStack<vmsmalloc::SlabDescriptorBase, SlabNextLinkage, SlabChainLinkage, VmsHeadEncoding, VmsmallocHooks>;`
     - `static_assert((static_cast<uintptr_t>(1) << VmsHeadEncoding::kOffsetBits) * arch::smallPageSize >= /* one PDPT */ (uintptr_t(1) << 39), "DEC-015 single-PDPT bit budget");`

5. **Confirm Phase 3's `partialFor(d)` accessor returns the right shape, and arrange Hooks binding.**
   - Per P5-DEC-002 the per-domain buffer holds `PartialStack` instances directly;
     `partialFor(d)[c].pop()` is a direct call. If Phase 3's accessor still returns
     `TreiberHead*` (the under-specified-at-time-of-Phase-3 default), adjust to return
     `PartialStack*`. The Phase-3 spec acknowledges the layout is meant to be flexible
     at implementation time.
   - Each `PartialStack` is constructed with `VmsmallocHooks{ &tuningFor(d)[c] }` bound at
     `vmsmallocInit` time (extend Phase 3's init routine, or have Phase 3's buffer accessor
     do in-place construction). Each `(d, c)` stack's hooks point at its own tuning row.
   - No view class, no `reinterpret_cast` — the stack instance lives in the buffer as-is.

6. **Implement `createFreshSlab(arch::ProcessorID i, size_t c)`.**
   - Body:
     ```cpp
     SlabDescriptorBase* createFreshSlab(arch::ProcessorID i, size_t c) {
         void* page = VMSubstrate::allocPage();   // panics on failure per DEC-012
         SlabDescriptorBase* base = nullptr;
         switch (c) {
             case 0: { auto* d = new (page) SlabDescriptor<slotCount(0)>(); base = d;
                       d->bookkeeper.seedAllAvailable(slotCount(0));  // DEC-011 tail-bit mask
                       break; }
             case 1: { auto* d = new (page) SlabDescriptor<slotCount(1)>(); base = d;
                       d->bookkeeper.seedAllAvailable(slotCount(1)); break; }
             // ... cases 2..7 ...
             default: panic("vmsmalloc: createFreshSlab: invalid class %zu", c);
         }
         // P5-DEC-006 — magic first, then the rest of the prefix.
         base->magic       = kSlabDescriptorMagic;
         base->sizeClass   = static_cast<uint8_t>(c);
         base->numaDomain  = static_cast<uint16_t>(NUMAPolicy::domainFor(i).raw());
         base->chainDepth  = 1;
         base->chainNext.store(nullptr, memory_order_relaxed);
         base->next        = nullptr;
         return base;
     }
     ```
   - The 8-case switch is the P5-DEC-001 dispatch. Cases are bare since the body for each
     is identical modulo the template `N`; consider a macro `CASE(idx)` that expands to the
     three-line body to keep the source readable.

7. **Implement `vmsmalloc(size)`.**
   - Body, with DEC-039 / DEC-040 / DEC-029 / DEC-036 markers in source comments:
     ```cpp
     void* vmsmalloc(size_t size) {
         // Phase 5 carries only the load-bearing whole-page-branch guard.
         // Phase 7 hoists "size != 0", "not in IRQ/NMI/...", "size <= pageSize" to entry.

         const size_t c = sizeClassIndex<vmsmalloc::kSlabSizeClasses>(size);
         if (c >= kNumSizeClasses) {           // DEC-029 whole-page bypass
             kassert(size <= arch::smallPageSize, "vmsmalloc: size exceeds page");
             return VMSubstrate::allocPage();  // panics on failure per DEC-012
         }

         const arch::ProcessorID i = arch::getCurrentProcessorID();
         const DomainID localDomain = NUMAPolicy::domainFor(i);
         Magazine& m = kernel::cpuLocal().magazines[c];   // P7-DEC-010

         while (true) {
             if (m.depth > 0) {
                 // Fast path — DEC-037 unified magazine + DEC-039 pre-read.
                 SlabDescriptorBase* head = m.head;
                 SlabDescriptorBase* nextLocal = head->chainNext.load(memory_order_relaxed);
                 // DEC-039: nextLocal is captured before allocSlot. Do NOT re-read
                 //         head->chainNext after allocSlot — a same-domain freer's push
                 //         may have overwritten it.
                 OccupancyTransition transition;
                 auto slot = dispatchOnClass(head, [&](auto* concrete) {
                     return concrete->bookkeeper.allocSlot(transition);
                 });
                 if (transition.becameFull()) {
                     m.head  = nextLocal;
                     m.depth--;
                     if (m.depth > 0) {
                         // Lazy first-touch freshness on the new m.head (DEC-040
                         // amended). The previous head was filled and orphaned;
                         // the new head is a previously-untouched chain element on
                         // this CPU since the refill pop. ensureTLBEntryFresh now
                         // so the next fast-path iteration reads its fields safely.
                         VMSubstrate::ensureTLBEntryFresh(m.head);
                     }
                 }
                 return slotZeroAddr(head) + slot.index * slotSize(c);
             }

             // m.depth == 0 — refill from the shared stack. The Phase-4 pop's
             // Hooks::onPreTouch (P4-DEC-010 / DEC-040 amended) already fired
             // ensureTLBEntryFresh(popped.head) inside pop() before reading
             // popped.head's fields, so popped.head is fresh on return.
             auto popped = partialFor(localDomain)[c].pop();   // P5-DEC-002
             if (popped.head != nullptr) {
                 m.head  = popped.head;     // already TLB-fresh per the pop hook
                 m.depth = popped.depth;
                 // DEC-036 eager-free walk: drain Empty heads above the m.depth == 1
                 // floor. Per P5-DEC-004, isEmpty() is the correct predicate
                 // (vmsmalloc slabs have reservedCount == kTailBits, baked into
                 // the amended isEmpty body). The first iteration's m.head is fresh
                 // from the pop hook; subsequent iterations call ensureTLBEntryFresh
                 // after advancing m.head (lazy first-touch per DEC-040 amended).
                 while (m.depth > 1 && m.head->bookkeeper.isEmpty()) {
                     SlabDescriptorBase* next = m.head->chainNext.load(memory_order_relaxed);
                     VMSubstrate::freePage(m.head);
                     m.head  = next;
                     m.depth--;
                     if (m.depth > 0) {
                         VMSubstrate::ensureTLBEntryFresh(m.head);  // lazy first-touch
                     }
                 }
                 continue;   // retry fast path
             }

             // Shared stack empty for (localDomain, c). The pop's Hooks::onEmptyStack
             // already bumped starvationCount inside Phase 4. Build a fresh slab.
             // (Counter bump is no longer external — see P5-DEC-007 supersession.)
             m.head  = createFreshSlab(i, c);
             m.depth = 1;
             // No ensureTLBEntryFresh — allocPage already invalidated this CPU's TLB
             // entry per DEC-046.
             continue;   // retry fast path
         }
     }
     ```
   - The helper `dispatchOnClass(head, f)` is a sibling of the slab-creation switch, but
     for *reading* a slab whose `sizeClass` is known at runtime. Implement as another inline
     8-case switch invoking `f(static_cast<SlabDescriptor<slotCount(c)>*>(head))`. Keep both
     switches in the same TU (one for write, one for read).
   - `bookkeeperOf(desc)` is shorthand for the same dispatch returning a reference to the
     concrete bookkeeper. The lambda-friendly form `dispatchOnClass(head, [&](auto* d) { ... })`
     covers both uses.
   - `slotZeroAddr(head)` is `reinterpret_cast<uint8_t*>(head) + slot0Offset(c)`.

8. **Wire `setVmsRange` into Phase 3's `vmsmallocInit`.**
   - In `kernel/mm/VMSubstrateSlab.cpp`, after step (4) of `vmsmallocInit` (which captures
     `vmsBase` / `vmsSize`), call `VMSubstrate::setVmsRange(vmsBase, vmsSize)` to publish the
     constants to `vmsmalloc.cpp`'s file-scope variables.
   - Add the P5-DEC-003 runtime bit-budget check inside `setVmsRange`:
     `kassert(vmsSize <= (uint64_t(1) << VmsHeadEncoding::kOffsetBits) * arch::smallPageSize, "DEC-015 offset budget");`

9. **Add the boot smoke exercise.**
   - In `vmsmalloc.cpp`, add `void vmsmallocBootSmoke();` and call it from
     `vmsmallocInit` (Phase 3) immediately after the existing klog line.
   - Body: for each class `c ∈ [0, kNumSizeClasses)`, call
     `vmsmalloc(kSlabSizeClasses[c])`, assert alignment, log the address. For class 0,
     allocate twice and assert the second address equals the first + 8 (consecutive
     in-slab slot).
   - Gate behind `#ifdef CROCOS_VMSMALLOC_BOOT_SMOKE` (or always-on for the Phase-5 merge,
     then gated later). User latitude.

10. **Build and smoke-test.**
    - `cmake --build cmake-build-debug --target Kernel` succeeds.
    - `cmake --build cmake-build-debug --target run` boots; serial log shows the smoke
      exercise output and the existing `naiveTest` line.
    - `make run_numa` / `make run_numa_hmat` boot through the smoke without panic.
    - No new compile warnings in the Phase-5 TU.

11. **Audit and document.**
    - `grep "DEC-039\|DEC-040\|DEC-029\|DEC-036" kernel/mm/vmsmalloc.cpp` — every cited
      decision has a comment at its load-bearing site.
    - Confirm `bookkeeper.allocSlot` callers in this TU never re-read `chainNext` post-call.
    - Update `[[project_slab_abstraction_plan]]` memory: Phase 5 status → "drafted /
      implemented" as appropriate.

12. **Optional follow-ups (under user latitude).**
    - Add the DEC-025 `static_assert(alignof(T) <= slotAlignment(class))` to `make<T>` in
      `VMSubstrate.h:46`. Requires `slotAlignment(c)` to be reachable from the public header;
      simplest path is `#include "VMSubstrateSlab.h"` from `VMSubstrate.h` (transitively
      pulls in the size-class constants, acceptable since DEC-028's amendment dropped the
      narrow-public-API goal).
    - Phase 4 evolution per P5-ITEM-006: add optional template-parameter hooks on
      `ChainedTreiberStack` for on-CAS-failure and on-empty-stack so Phase 10's tuning
      policy can be plumbed cleanly. Phase 5's external counter bumps (P5-DEC-007) still
      work — the hooks just move the increment site inside the primitive.
    - Add a comment near `vmsmalloc` / `vmsfree` in `VMSubstrate.h:21–22` per DEC-028
      amendment: `// Convention-internal: external callers should prefer make<T> / destroy<T>.`

## References

- `kernel/include/mem/VMSubstrate.h:21` — existing `vmsmalloc` declaration (this phase
  supplies its body).
- `kernel/include/mem/VMSubstrate.h:28–58` — `SafePtr<T>` and `make<T>` / `destroy<T>`
  (already implemented; lit up by this phase once `vmsmalloc` has a body).
- `kernel/mm/VMSubstrate.cpp:585` (`allocPage`), `:624` (`freePage`), `:694`
  (`ensureTLBEntryFresh`) — primitives Phase 5 calls.
- `kernel/mm/VMSubstrateSlab.h` — Phase 2/3 output: types, constants, accessors.
- `kernel/mm/VMSubstrateSlab.cpp` — Phase 3 output: `vmsmallocInit`; extended in step 8 with
  the `setVmsRange` call and the smoke-exercise invocation.
- `libraries/Core/include/core/atomic/TreiberStack.h` — Phase 4 output: `ChainedTreiberStack`
  (Phase 5 may extend with `ChainedTreiberStackView` per step 3).
- `libraries/Core/include/core/SizeClass.h:36` — `sizeClassIndex<array>(size)`.
- `libraries/LibAlloc/InternalAllocator.cpp:724` — precedent for `sizeClassIndex<slabSizeClasses>` usage.
- `libraries/LibAlloc/include/liballoc/Slab.h` — Phase 1 output: `SlabBookkeeper<N>` with
  `allocSlot(transition)`, `freeSlot(slotIndex, transition)`, `OccupancyTransition`,
  `isEmpty()`.
- Parent spec `specs/vmsmalloc.md`:
  - DEC-001 / DEC-022 — alignment contract.
  - DEC-015 — packed-tagged-head encoding (Phase 5 supplies the policy implementation).
  - DEC-018 — slab home-domain recording.
  - DEC-019 — no cross-domain steal.
  - DEC-029 — large-request whole-page bypass.
  - DEC-034 — chained-transfer magazines.
  - DEC-036 — bounded eager-free walk on the allocator path.
  - DEC-037 — unified magazine, `current[]` collapsed into `magazines[i][c].head`.
  - DEC-039 — pre-read `chainNext` before `allocSlot`.
  - DEC-040 (amended 2026-05-27) — lazy first-touch freshness via Phase-4 P4-DEC-010 hooks + per-`m.head`-transition calls.
  - DEC-041 — head-linkage; `m.tail` removed.
  - DEC-042 — memory-ordering policy (Phase 5 consumes Phase 4's named constants).
  - DEC-046 — `allocPage` invalidates the calling CPU's own TLB entry.
  - Concurrency Model section "Allocator path on CPU `i`" — the source of truth Phase 5
    implements.
- Phase 1 spec `specs/vmsmalloc-phase-1.md` — `SlabBookkeeper` API contract.
- Phase 2 spec `specs/vmsmalloc-phase-2.md` — descriptor layout, `kSlabSizeClasses`, accessors.
- Phase 3 spec `specs/vmsmalloc-phase-3.md` — `vmsmallocInit`, metadata storage,
  `vmsBase` / `vmsSize` capture point (step 8 extension).
- Phase 4 spec `specs/vmsmalloc-phase-4.md` — `ChainedTreiberStack` and the
  `TreiberHeadEncoding` policy concept.
- Memory: `[[project_slab_abstraction_plan]]` — phase plan; updated on Phase 5 completion.
- Memory: `[[project_armv8_dev_tsan]]` — TSan-on-ARMv8 default test target (relevant when
  Phase 8 lights up the userspace harness against the encoding).
