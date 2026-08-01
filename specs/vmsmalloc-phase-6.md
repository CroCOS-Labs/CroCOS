---
kind: leaf
status: drafting
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 6 — `vmsfree` happy path

> Implement the body of `kernel::mm::VMSubstrate::vmsfree(void*)` declared in
> `kernel/include/mem/VMSubstrate.h:22`. Covers DEC-026's validation chain (range →
> page-aligned dispatch / freshness → magic → slot-range → modulo → poison → bookkeeper
> freeSlot → Full→Partial publish), the DEC-029 whole-page bypass, DEC-024 debug poison,
> and the DEC-019/DEC-034 publish split: **same-domain** extends the local magazine (with
> a `pushChain` flush when `m.depth` reaches `partialFor(localDomain)[c].getMaxChainLength()`),
> **cross-domain** calls `push(element)` on the home-domain stack (extending the existing
> top chain if it's below max, otherwise pushing a new singleton — confirmed user direction
> 2026-05-27). Reuses Phase 5's TU (`kernel/mm/vmsmalloc.cpp`) and its `dispatchOnClass`
> helper. The DEC-023 nullptr assert and the DEC-014 IRQ/NMI/#GP/#MC context assert are
> Phase 7; Phase 6 carries the load-bearing validation steps (range check, magic check,
> slot-range, modulo) because skipping them would be a correctness violation, not just a
> debug-friendliness gap.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No DEC-023 nullptr assert** at function entry — Phase 7. `vmsfree(nullptr)` will silently
  pass the range check (because `vmsBase > 0` on any real configuration) and crash inside
  `ensureTLBEntryFresh` or the magic check; Phase 7 promotes it to a clean assert.
- **No DEC-014 IRQ/NMI/#GP/#MC context assert** at function entry — Phase 7.
- **No DEC-030 preemption / migration debug assert** at function entry — Phase 7 (and only
  meaningfully wireable once CroCOS has a preemptive scheduler; today the obligation holds
  trivially).
- **No `LibAlloc::SlabBookkeeper` API changes.** Phase 1 already exposes
  `freeSlot(slotIndex, OccupancyTransition&)` and the `becameAvailable()` predicate; Phase 6
  consumes them.
- **No tuning policy.** `MagazineTuning::overflowCount` is bumped on each flush-CAS retry
  via Phase 4's `Hooks::onCasFailure` (P4-DEC-010), but the policy that reads and adjusts
  `maxChainLength` is Phase 10.
- **No `make<T>` / `destroy<T>` changes.** `destroy<T>` already exists at
  `VMSubstrate.h:52–58` and calls through `vmsfree`. Phase 6 lights it up by giving
  `vmsfree` a body.
- **No new tests beyond extending Phase 5's boot smoke.** The userspace integration harness
  is Phase 8. Phase 6 extends the Phase-5 smoke to do alloc → free → alloc cycles,
  exercising the becameAvailable publish and the magazine refill from the published chain.
- **No new VMSubstrate primitives.** Phase 3's `reservePerDomainStaticBuffer` /
  `localCachePageFor` and the existing `allocPage` / `freePage` / `ensureTLBEntryFresh` are
  the substrate Phase 6 builds on.
- **No allocator-side path changes.** Phase 5's `vmsmalloc` is final at end of Phase 5;
  Phase 6 only adds the freer-side body.

## Consumer Contract

### `void kernel::mm::VMSubstrate::vmsfree(void* p)`

Already declared at `kernel/include/mem/VMSubstrate.h:22`. Phase 6 supplies the body in
`kernel/mm/vmsmalloc.cpp` (same TU as Phase 5's `vmsmalloc` — they share the file-scope
`VmsHeadEncoding`, `VmsmallocHooks`, `dispatchOnClass`, and `vmsBase` / `vmsSize`
constants).

**Contract (Phase 6 portion):**

- For a pointer `p` returned by an earlier `vmsmalloc(size)` call: `vmsfree(p)` releases
  the slot back to the allocator. The bookkeeper transitions; if the transition is
  Full→Partial, the slab is published per DEC-019/DEC-034:
  - **Same-domain** (`desc->numaDomain == NUMAPolicy::domainFor(currentCPU())`): the slab
    is prepended to the local magazine `kernel::cpuLocal().magazines[desc->sizeClass]`. If
    the magazine depth reaches `partialFor(localDomain)[c].getMaxChainLength()`, the
    magazine flushes its entire chain to the shared stack via Phase-4
    `ChainedTreiberStack::pushChain(m.head, m.depth)`. After flush, `m.head = nullptr;
    m.depth = 0`.
  - **Cross-domain** (`desc->numaDomain != NUMAPolicy::domainFor(currentCPU())`): the
    freer calls `partialFor(desc->numaDomain)[c].push(desc)` directly — Phase 4's
    `push(element)` semantics. The cross-domain freer **may extend the existing top chain**
    of the home-domain stack if it's below `maxChainLength`; otherwise `push(element)`
    starts a new singleton chain. This is the user-confirmed cross-domain routing
    (2026-05-27), distinct from the parent-spec Concurrency Model's literal "publish as
    singleton" pseudocode — the choice favors shared-stack amortization.
- For a page-aligned pointer (DEC-029 whole-page allocation from Phase 5's bypass branch):
  `vmsfree(p)` calls `VMSubstrate::freePage(p)` directly. No slab descriptor read, no
  bookkeeper interaction, no freshness call. Disambiguation via the `(p & (pageSize - 1)) == 0`
  branch happens **after** the range check but **before** `ensureTLBEntryFresh` (so a
  bad-but-page-aligned pointer doesn't fault inside `ensureTLBEntryFresh` on an unmapped
  page).
- For an invalid pointer (out of range, not slab-backed, misaligned within a slab):
  asserts at the relevant validation step. Phase 6 ships the load-bearing checks (range,
  magic, slot-range, modulo); Phase 7 adds the cosmetic-but-helpful nullptr and IRQ-context
  asserts.
- Concurrency: per parent-spec Concurrency Model "Freer path on any CPU". Same-domain
  freer mutations of `kernel::cpuLocal().magazines[c]` are uncontended (DEC-014/030 forbid
  reentry); cross-domain singleton push uses Phase-4 atomic CAS. Cross-domain freer never
  touches any magazine. The bookkeeper's `freeSlot` is multi-CPU-safe by Phase 1 design
  (SplitBitmap with separate alloc/free halves).
- **Reader-side TLB freshness (DEC-016):** `ensureTLBEntryFresh(p)` is the **first**
  operation after the range check and page-aligned dispatch. Every subsequent read of
  `*desc` (magic, sizeClass, numaDomain, bookkeeper) is from a fresh TLB-mapped PA. This
  is the canonical freer-side freshness call — distinct from the lazy first-touch model
  on the allocator side.

### Helpers added to `kernel/mm/vmsmalloc.cpp`

```cpp
namespace kernel::mm::VMSubstrate {
namespace {
    // Phase 6 additions; Phase 5's vmsmalloc.cpp already has dispatchOnClass.

    // Recover the slot index from a slab-backed pointer + its descriptor.
    // Caller has already validated p is in slab's data range and properly aligned.
    inline size_t slotIndexOf(void* p, SlabDescriptorBase* desc) {
        const uintptr_t off = reinterpret_cast<uintptr_t>(p)
                            - reinterpret_cast<uintptr_t>(slotZeroAddr(desc));
        return off / slotSize(desc);
    }

    // Cross-domain publish path (DEC-019 gate, P5-DEC-002 + user direction 2026-05-27).
    // Calls Phase-4 push(element) which may extend or start a new singleton.
    void crossDomainPublish(SlabDescriptorBase* desc) {
        partialFor(desc->numaDomain)[desc->sizeClass].push(*desc);
    }

    // Same-domain publish path (DEC-034). Prepends desc to the local magazine,
    // checks the flush trigger.
    void sameDomainPublishAndMaybeFlush(arch::ProcessorID i,
                                        SlabDescriptorBase* desc) {
        const size_t c = desc->sizeClass;
        Magazine& m = kernel::cpuLocal().magazines[c];   // P7-DEC-010
        // Extend at head (non-atomic — magazine is CPU-private per DEC-014/030).
        desc->chainNext.store(m.head, memory_order_relaxed);
        m.head = desc;
        m.depth++;

        // Flush trigger: maxChainLength lives on the stack instance (P5-DEC-002).
        const uint32_t kmax = partialFor(NUMAPolicy::domainFor(i))[c]
                                  .getMaxChainLength();
        if (m.depth >= kmax) {
            // DEC-034 flush: set chainDepth on the new head, publish chain via pushChain.
            // pushChain's RELEASE-CAS publishes all prior writes to chain elements
            // (chainDepth, chainNext, etc.) per DEC-042 #1.
            m.head->chainDepth = m.depth;
            partialFor(NUMAPolicy::domainFor(i))[c]
                .pushChain(*m.head, m.depth);
            m.head  = nullptr;
            m.depth = 0;
        }
    }

    // vmsfree body lives in this same anonymous namespace; declared at the top of
    // the TU.
}

void vmsfree(void* p) { /* see Implementation Phases step 3 */ }
}  // namespace kernel::mm::VMSubstrate
```

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 1 — extended `LibAlloc::SlabBookkeeper` | Provides `freeSlot(slotIndex, OccupancyTransition&)`; `becameAvailable()` predicate; acq-rel atomics on `allocatedCount.fetch_sub` per DEC-042 #4. **Phase 1 P1-DEC-001 amended also pins `kTailBits` and the amended `isEmpty()`** — relevant to Phase 6 because the bookkeeper's `freeSlot` decrements `allocatedCount`; the transition predicate uses `SlotCount - reservedCount` (which equals `usableCount = SlotCount - kTailBits` for vmsmalloc slabs) as the "Full" threshold. | Yes — Phase 1 must land. |
| Phase 2 — `VMSubstrateSlab.h` | `SlabDescriptorBase` (with `magic`, `sizeClass`, `numaDomain`, `chainNext`, `chainDepth`, `next`); `kSlabDescriptorMagic`; `slotSize(desc)` / `slotCount(desc)` / `slot0Offset(desc)` constexpr-table accessors (ITEM-049). | Yes — Phase 2 must land. |
| Phase 3 — VMSubstrate metadata storage | `Magazine` type; `partialFor` / `tuningFor` accessors. **Per P5-DEC-002 (amended) Phase 3's `partialFor` returns a `PartialStack&` (a `ChainedTreiberStack`-instance reference).** Phase 6 calls `partialFor(d)[c].pushChain(...)` and `.push(...)` and `.getMaxChainLength()` directly. **Per P7-DEC-010 (amended 2026-05-27) the per-CPU magazine array lives in `kernel::CpuLocal`** (accessed via `kernel::cpuLocal().magazines[c]`), not via a separate `magazineFor(i)` accessor. | Yes — Phase 3 must land with the P5-DEC-002 + P7-DEC-010 layout (or implementation-time resolution). |
| Phase 4 — `Core::ChainedTreiberStack` with Hooks (P4-DEC-010) | Provides `push(element)` (extend-or-singleton; used for cross-domain singleton free) and `pushChain(chainHead, depth)` (always-new-chain single-CAS publish; used for same-domain flush). Both fire `Hooks::onPreTouch` / `onCasFailure` as appropriate; vmsmalloc's `VmsmallocHooks` (defined in Phase 5's TU) calls `ensureTLBEntryFresh` and bumps tuning counters. | Yes — Phase 4 must land with P4-DEC-010. |
| Phase 5 — `vmsmalloc.cpp` TU and helpers | `VmsHeadEncoding`, `VmsmallocHooks`, `dispatchOnClass`, `vmsBase` / `vmsSize` capture, slow-path slab construction, the 8-way `SlabDescriptor<N>` dispatch (P5-DEC-001) — all already defined. Phase 6 adds `vmsfree` to the same TU. | Yes — Phase 5 must land. |
| `kernel::mm::VMSubstrate::ensureTLBEntryFresh(p)` | Freer-side first-touch freshness (DEC-016/DEC-026). Steady-state cost is one atomic dirty-bit test; `invlpg` only fires on actual first-touch. | Yes — live. |
| `kernel::mm::VMSubstrate::freePage(p)` | Whole-page free path for DEC-029 bypass. Already used by Phase 5's eager-free walk. | Yes — live. |
| `arch::getCurrentProcessorID()` | Read once per `vmsfree` call to compute `localDomain = NUMAPolicy::domainFor(...)`. Same helper Phase 5 uses. | Yes — live. |
| `kernel::mm::NUMAPolicy::domainFor` | Read to compute `localDomain` for the cross-domain gate. | Yes — live. |

## Invariants

<!-- Conditions Phase 6's code must preserve at all times. -->

- **Validation chain ordering (DEC-026 amended).** The validation chain must execute in
  the order: (Phase 7 entry asserts) → range check → page-aligned dispatch (DEC-029) →
  `ensureTLBEntryFresh` → magic check → slot-range lower-bound (ITEM-048) → slot-range
  upper-bound → modulo → (debug-only poison, DEC-024) → bookkeeper `freeSlot` → conditional
  publish. **Each step depends on its predecessors:** the range check protects
  `ensureTLBEntryFresh` from arbitrary VAs; the freshness call protects the magic check
  from stale-TLB reads; the magic check protects the slot-range arithmetic from non-slab
  pointers; the slot-range check protects the modulo from underflow.
- **Page-aligned dispatch sits BETWEEN range check and freshness call.** The DEC-029
  whole-page free path neither reads any descriptor field nor calls
  `ensureTLBEntryFresh` — it hands `p` directly to `VMSubstrate::freePage`. The dispatch
  must precede the freshness call so a bad-but-page-aligned pointer that happens to land
  in an unpopulated arena (DEC-031 documented gap) doesn't fault inside
  `ensureTLBEntryFresh` reading a nonexistent dirty bitmap.
- **`desc->numaDomain` is read after the magic check.** Reading it before would trust
  arbitrary bytes from a non-slab page as a domain index. Phase 6's source order: magic
  → slot-range → modulo → poison → freeSlot → (if becameAvailable) read `numaDomain` for
  the cross-domain gate.
- **Same-domain push extends at head (non-atomic, magazine-private).** The freer writes
  `desc->chainNext.store(m.head, RELAXED)` and `m.head = desc; m.depth++`. No atomics on
  `m.head` / `m.depth` (DEC-014/030 ensure no concurrent same-CPU access). The store on
  `desc->chainNext` is RELAXED per DEC-042 #3; the `Atomic<>` wrapper is for C++ data-race
  freedom (parent-spec DEC-042 #3 clarification 2026-05-27) — same generation as the
  amended discussion of cross-CPU access during push-extend.
- **Flush sets `m.head->chainDepth` before calling `pushChain`.** Phase 4's
  `pushChain(chainHead, depth)` debug-asserts `ChainLinkage::getChainDepth(chainHead) == depth`.
  The freer writes `m.head->chainDepth = m.depth` immediately before the pushChain call;
  the write is single-CPU and lives in the freer's program order before the publishing
  release-CAS, so it is correctly published.
- **`m.head` and `m.depth` move together on flush.** After a successful `pushChain` CAS,
  `m.head = nullptr; m.depth = 0` in source order. The invariant `m.depth == 0 ⇔ m.head == nullptr`
  (parent Invariants) is preserved.
- **Cross-domain publish bypasses the local magazine entirely.** The cross-domain freer
  never touches `kernel::cpuLocal().magazines[c]`. The home-domain stack's Hooks fire (bound
  to `tuningFor(desc->numaDomain)[c]`); the freer's own domain's tuning row is not touched.
- **Magazine-domain consistency invariant (parent Invariants).** Every slab in
  `cpuLocal().magazines[c]`'s chain (on each CPU `i`) has `desc->numaDomain == NUMAPolicy::domainFor(i)`. Phase 6
  preserves this: the cross-domain gate routes foreign-domain slabs to the home-domain
  stack, never to the local magazine.
- **Debug poison precedes bookkeeper `freeSlot` (DEC-024).** The poison memset writes
  `0xCC` to the slot's `slotSize` bytes BEFORE the bookkeeper marks the slot free. A
  concurrent stress-test reader catching a half-poisoned slot is impossible — the slot is
  still allocated from the bookkeeper's perspective until `freeSlot` returns.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `p == nullptr` reaches `vmsfree` | Phase 6: undefined (Phase 7 adds the DEC-023 assert). In practice the range check rejects `nullptr` because `vmsBase > 0` on any real configuration — the assert fires at "p outside VMSubstrate range" rather than at "nullptr". Diagnostically less self-explanatory but functionally safe. | No (caller bug, Phase 7) |
| `p` outside `[vmsBase, vmsBase + vmsSize)` | Asserts at the range check (DEC-026 step 3). | No (caller bug) |
| `p` page-aligned, points at an unmapped page in the VMSubstrate range | `VMSubstrate::freePage(p)` asserts on its own validation. Diagnostic message is `freePage`'s, not `vmsfree`'s — slightly less ergonomic than a vmsmalloc-side message but accepted (parent-spec DEC-029 hazard). | No (caller bug) |
| `p` page-aligned, points at an active slab page (caller passed the page base instead of a slot pointer) | DEC-029 dispatch sends this to `VMSubstrate::freePage`, which unmaps the slab and corrupts the slab's live slots. **Documented hazard** (parent-spec "Whole-page validation gap"). Phase 6 inherits this; no new mitigation added. Caller is expected to never pass a page-aligned pointer to a slab. | No (caller bug; unmitigated) |
| `p` in VMSubstrate range, non-page-aligned, lands in an unmapped arena | `ensureTLBEntryFresh` faults on the nonexistent dirty bitmap. Documented gap (DEC-031). | No (caller bug) |
| `desc->magic != kSlabDescriptorMagic` after the magic check | Asserts (DEC-013). Probabilistic catch — magic value is non-canonical AMD64 VA (DEC-044) so accidental match is vanishingly unlikely. | No (caller bug) |
| `p < slotZeroAddr(desc)` (pointer inside the descriptor region) | Asserts at the slot-range lower-bound check (ITEM-048). Closes the underflow corner that would otherwise pass the modulo via two's-complement wraparound. | No (caller bug) |
| `p >= slotZeroAddr(desc) + slotSize * slotCount` (pointer past the slot region) | Asserts at the slot-range upper-bound check. | No (caller bug) |
| `(p - slotZeroAddr(desc)) % slotSize != 0` | Asserts at the modulo check (DEC-013). | No (caller bug) |
| Double-free of a slot | `bookkeeper.freeSlot` calls `bitmap.releaseBit`, which asserts at `SplitBitmap.h:180` (per Phase 1 P1-DEC-001 — the existing assertion was already in place). | No (caller bug) |
| Concurrent same-domain free of a different slab while we're flushing | The flush's `pushChain` CAS may fail (head changed); `VmsmallocHooks::onCasFailure` bumps `overflowCount` and the CAS retries. Chain integrity preserved because each CPU's magazine is private. | Normal path |
| Concurrent cross-domain free pushing onto the same home-domain stack while a local allocator on the home domain pops | One CAS wins (push or pop); the loser retries via Phase 4's CAS loop. No chain corruption. | Normal path |
| `partialFor(desc->numaDomain)` returns nullptr (CPU-less domain — DEC-038) | Cannot happen for vmsmalloc slabs: per DEC-018 `desc->numaDomain` was set to the creator's home domain, which always has at least the creator CPU. The accessor's null-check assertion catches any future regression. | No (impossible by construction) |
| `desc->bookkeeper.freeSlot` reports `becameAvailable` but `desc` is already on the shared stack (somehow republished by a race) | Cannot happen: DEC-037's synchronous becameFull pop ensures a Full slab is removed from the magazine before any freer can republish it. The first freer to drive Full→Partial is the *unique* republisher. | No (impossible by construction) |
| `m.depth >= maxChainLength` reaches a path where `m.head == nullptr` | Impossible by the invariant `m.depth == 0 ⇔ m.head == nullptr` and the fact that `m.depth++` only fires after `m.head = desc`. The flush trigger reads a non-zero depth. | No (impossible by construction) |

## Questions

<!-- Open questions for resolution during implementation. -->

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P6-ITEM-001 | Resolved 2026-05-27 | | | Parent-spec Concurrency Model "cross-domain gate" pseudocode (now lines 1009-1017) used a hand-rolled CAS that read as "literal singleton publish". Is it consistent with Phase 6's `push(element)` routing? | Resolved: parent-spec Concurrency Model updated 2026-05-27 to call `partial[d][c].push(*desc)` directly, with prose calling out the extend semantic (may grow the home-domain top chain rather than always publishing a singleton). The hand-rolled CAS is replaced with the Phase-4 API call. Done. |
| P6-ITEM-002 | Resolved 2026-05-27 | | | Does `dispatchOnClass` work for `freeSlot` the same way it works for `allocSlot`? | Resolved: yes, identical pattern. The helper dispatches on `desc->sizeClass` and `static_cast`s to the concrete `SlabDescriptor<slotCount(c)>*`. Phase 6's vmsfree calls `dispatchOnClass(desc, [&](auto* concrete) { concrete->bookkeeper.freeSlot(slotIdx, transition); })` — same 8-case switch, exercised on a `desc` recovered from `(p & ~(pageSize-1))` rather than `m.head`. |
| P6-ITEM-003 | Resolved 2026-05-27 → P6-DEC-006 | | | Boot smoke alloc/free/alloc cycle per class? | Resolved: P6-DEC-006 — extend the Phase-5 smoke to alloc → free → alloc per class, assert the realloc returns the same slot (becameAvailable publish → magazine head transitions back to Partial → next alloc reuses). Plus DEC-029 whole-page cycle once. |
| P6-ITEM-004 | Resolved 2026-05-27 | | | `crossDomainPublish` triggers Phase-4 `push(element)`'s `onPreTouch` hook on the home-domain top chain head from the cross-domain freer's CPU. Any subtlety? | Resolved: no subtlety. `ensureTLBEntryFresh(topPtr)` fires on the cross-domain freer X for the home-domain's top chain head — X's local dirty bit for that VA is the relevant signal (set by every prior `allocPage` / `freePage` of that VA per DEC-046 / DEC-016). Correct by construction; the hook is the same hook regardless of which CPU's TLB it's protecting. |
| P6-ITEM-005 | Resolved 2026-05-27 (deferred to Phase 8) | | | Should the boot smoke exercise the cross-domain free path on `run_numa` / `run_numa_hmat`? | Resolved (deferred): exercising cross-domain free at boot requires the freer call to actually execute on a remote CPU. Today (no scheduler), that means leveraging the SMP fan-out init infrastructure or constructing a one-shot remote IPI dispatch — more machinery than a "boot smoke" warrants. Phase 8's userspace integration harness covers cross-domain push under TSan with a clean multi-thread driver; defer there. Single-domain QEMU boots will exercise everything except the gate's `desc->numaDomain != localDomain` branch; the same-domain branch is fully covered by P6-DEC-006. |

## Decisions

<!-- Settled decisions specific to Phase 6. -->

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P6-DEC-001 | Settled | **`vmsfree` body lives in `kernel/mm/vmsmalloc.cpp` (same TU as Phase 5's `vmsmalloc`)**, not a separate `vmsfree.cpp`. The two functions share `VmsHeadEncoding`, `VmsmallocHooks`, `dispatchOnClass`, `vmsBase` / `vmsSize`, the `partialFor` / `tuningFor` access patterns, the `kernel::cpuLocal()` accessor (per P7-DEC-010), and the `SlabDescriptor<N>` dispatch infrastructure. Splitting them across TUs would force duplication or forward-declarations of every shared symbol. | Same-TU is the right granularity: both functions are the public-facing hot path of the same allocator. The TU is still manageable in size (~600 lines projected for Phase 5 + Phase 6 combined). Phase 7's assertion paths will also live here. |
| P6-DEC-002 | Settled (user direction 2026-05-27) | **Cross-domain freer publish uses Phase 4's `push(element)`, NOT `pushChain(desc, 1)`.** The functional difference: `push(element)` may extend the existing top chain of `partial[desc->numaDomain][c]` if it has depth < `maxChainLength`, producing fewer-but-longer chains on the shared stack; `pushChain(desc, 1)` would always start a new singleton. The user confirmed `push(element)` is the intended routing for better shared-stack amortization. | Reduces shared-stack length under cross-domain free pressure: a single chain accumulating cross-domain singletons up to `maxChainLength` produces one CAS-target per `maxChainLength` frees, versus one chain per free with `pushChain(desc, 1)`. The parent-spec Concurrency Model wording (lines 967-970) describes a literal singleton publish — functionally compatible with `push(element)`'s singleton-branch but doesn't capture the extend semantic. Editorial update to the parent spec is a follow-up to align prose with behavior; not load-bearing for Phase 6. |
| P6-DEC-003 | Settled | **`crossDomainPublish` and `sameDomainPublishAndMaybeFlush` are file-scope inline helpers in `vmsmalloc.cpp`**, not methods on a class. Phase 6 introduces them at the top of the anonymous namespace alongside Phase 5's other helpers. Both helpers are called from exactly one site (vmsfree's publish branch); inlining them via `inline` lets the compiler decide whether to fold them into the caller. | Matches Phase 5's file-scope-helper convention. Avoids adding a class wrapper for state that already lives in module-local arrays. The `inline` keyword + single call site means the compiler will likely inline regardless. |
| P6-DEC-004 | Settled | **The DEC-024 debug poison fills exactly `slotSize(desc)` bytes starting at `p`.** Implementation: `#ifdef CROCOS_DEBUG ... memset(p, 0xCC, slotSize(desc)) ... #endif`. The fill writes to slot bytes only — never to the descriptor area at offset 0 (which is structurally before any slot per DEC-001 / DEC-008). Release builds elide the entire branch via the `#ifdef`. | Direct implementation of parent-spec DEC-024. The poison value `0xCC` is non-canonical AMD64 VA (per DEC-044's rationale) and is the x86 `int3` opcode, so a UAF-jumped instruction trip-wires. `slotSize(desc)` is the constexpr-table accessor from Phase 2 P2-DEC-002; one indirect dispatch per poison call (negligible in debug builds; absent in release). |
| P6-DEC-005 | Settled | **Phase 6 reads `desc->numaDomain` *after* the bookkeeper `freeSlot` call returns, only on the `becameAvailable` branch.** The earlier read order ("read numaDomain right after the magic check, hold it through the freeSlot call") was considered and rejected: the cross-domain gate decision is only needed if we're publishing, which only happens on `becameAvailable`. Keeping the read post-`freeSlot` removes one descriptor read on the common non-publishing path (most frees leave the slab Partial without transitioning). | Hot-path micro-optimization. The Atomic-cast safety isn't affected — `desc->numaDomain` is `uint16_t` non-atomic per P2-DEC-001 and is set once at slab creation (DEC-018); subsequent reads are race-free regardless of when they happen. Post-`freeSlot` is the natural slot for the read. |
| P6-DEC-006 | Settled | **The boot smoke exercise is extended to do `alloc(size_class[c]) → free → alloc` per class** (P6-ITEM-003 leaning resolved). Phase 5's smoke leaked the slots; Phase 6's extension frees them and reallocates. The realloc should return the *same* slot (because the magazine head transitions back to Partial after the free's becameAvailable). Log all three addresses per class. | Validates the becameAvailable publish + magazine-extend path end-to-end at boot, without requiring the Phase 8 harness. Cost: 3 vmsmalloc + 1 vmsfree calls per class × 8 classes = 32 ops at boot. Trivial. |

## Hazards

- **DEC-026 validation chain ordering is positional discipline (refactor-fragile).** The
  range check, page-aligned dispatch, freshness call, magic check, slot-range, and modulo
  must execute in source order. A future "helpful" refactor that consolidates the steps or
  reorders them (e.g., moving the magic check earlier so it can validate `desc` before
  `ensureTLBEntryFresh`) silently reintroduces the cross-VA-fault hazards each step was
  introduced to close. Source comments at every validation step cite the relevant
  parent-spec section.
- **DEC-029 whole-page free of a live slab page (parent-spec hazard).** A page-aligned
  pointer dispatches to `VMSubstrate::freePage` without consulting any slab descriptor.
  If the caller passes the slab's page base instead of a valid slot pointer, the slab is
  unmapped and its live slots are corrupted. Phase 6 inherits this hazard unchanged — the
  mitigation would be a side-bitmap of "whole-page vmsmalloc allocations" or a magic header
  on whole-page allocations (parent-spec discusses both as deferred). Caller discipline:
  never pass a page-aligned pointer that wasn't returned from `vmsmalloc`.
- **Flush sets `m.head->chainDepth` before the publishing CAS — but does not zero
  `chainNext` on the bottom of the magazine chain.** Phase 4 `pushChain` debug-asserts the
  chain's bottom node has `chainNext == nullptr`. The magazine chain's bottom was the
  initial pushed element (DEC-034: same-domain push extends at head, so the chain bottom
  is whichever element was the original `m.head` when the chain was first refilled). Per
  the parent-spec invariant, the bottom's `chainNext == nullptr` is maintained by the
  refill-pop path (chain elements come from the popped chain with chainNext already
  correctly terminated). Phase 6's same-domain push extends at head — it doesn't write
  to the bottom. As long as the refill-pop preserves the bottom's `nullptr` chainNext
  (it does — the popped chain came from a prior flush/cross-domain push that set bottom's
  chainNext to nullptr), the flush's pushChain caller invariant holds.
- **Cross-domain push extends another domain's top chain (P6-DEC-002).** The semantic
  difference between `push(element)` and `pushChain(desc, 1)` matters when the home domain's
  stack already has a chain near `maxChainLength`. With `push(element)`: the cross-domain
  freer's slab joins the existing top chain (chainDepth becomes max+1 after one more push,
  triggering the new-singleton branch). With `pushChain(desc, 1)`: every cross-domain free
  creates a new singleton. The extend behavior is preferred (P6-DEC-002 rationale) but
  changes the steady-state shared-stack structure under cross-domain free pressure. Worth
  watching in Phase 9 stress tests.
- **Same-domain freer extending a magazine whose chain was just refilled may flush
  immediately.** Scenario: magazine empties (m.depth = 0); allocator pops a chain of depth
  N from the shared stack (m.depth = N); the allocator transitions one slab Full, popping
  it (m.depth = N-1). Then the freer transitions a Full slab back to Partial, extends the
  magazine (m.depth = N). If `N == maxChainLength`, the flush fires immediately. The
  flushed chain includes the slabs we just popped. Net effect: a "thrash" where allocator
  refills and freer immediately flushes. Bounded by the requirement that *Full→Partial*
  transitions are rare (each requires a separate becameFull-pop preceding it). Not a
  correctness issue; potential perf issue if `maxChainLength` is set badly small. Phase 10's
  tuning policy will adjust `maxChainLength` based on the freer-side `overflowCount` and
  allocator-side `starvationCount` — should naturally avoid this regime.
- **`Atomic<>` wrapper on `desc->chainNext` is load-bearing for C++ data-race freedom**
  (parent-spec DEC-042 #3 clarification 2026-05-27). Phase 6's same-domain push writes
  `desc->chainNext.store(m.head, RELAXED)` — the RELAXED ordering compiles to a plain
  store on AMD64/ARMv8, but the `Atomic<>` typing prevents the C++ data race against the
  push-extend race window (a concurrent CPU mid-`push(element)` may be reading `desc->chainNext`
  speculatively). Do not "optimize" the same-domain push to use a `reinterpret_cast`
  non-atomic store — even though the magazine is CPU-private, `desc` itself may be
  concurrently observed by other CPUs through the shared-stack lifecycle.
- **Flush's `pushChain` caller invariants (Phase 4 P4-DEC-009).** `pushChain` debug-asserts
  `ChainLinkage::getChainDepth(chainHead) == depth` and `depth >= 1`. Phase 6 must write
  `m.head->chainDepth = m.depth` immediately before the call (with no intervening
  operations that could re-enter or otherwise invalidate the write). The `m.depth >= 1`
  invariant holds because we only flush after `m.depth >= maxChainLength` and
  `maxChainLength >= kMinK >= 2` (P3-DEC-002).
- **The poison fill (DEC-024) must precede `freeSlot`.** Poisoning after `freeSlot` would
  write to bytes that the bookkeeper considers unallocated — racing with a concurrent
  re-allocation of the same slot (which could happen on a different CPU after the same-domain
  publish + flush + remote refill, or after the cross-domain publish + remote pop). Source
  order: `memset(p, 0xCC, slotSize) → freeSlot → publish`. Comment cites DEC-024 explicitly.

## Verification Targets

| Property | Method |
|---|---|
| `vmsfree(p)` for `p` from `vmsmalloc(8)` returns the slot to availability (next `vmsmalloc(8)` returns the same slot) | Boot smoke alloc/free/alloc cycle per class (P6-DEC-006) |
| `vmsfree(p)` for `p` from `vmsmalloc(largestSizeClass + 1)` (DEC-029 whole-page) calls `freePage`, no descriptor read | Boot smoke: allocate a 1024-byte block, free it, log that the address was page-aligned |
| `vmsfree(nullptr)` reaches Phase 7's assert (post-Phase-7); in Phase 6 it traps in the range check | Phase-7 test (Phase 6 doesn't ship this assert) |
| `vmsfree(stackPointer)` traps in the range check | Phase-7 negative test |
| `vmsfree(slabBaseSlotPointer)` (a stray page-aligned pointer that happens to land on a slab page) silently corrupts the slab — documented hazard, no Phase-6 mitigation | None (hazard documented in parent spec; revisit if observed) |
| Magic-mismatch on a slab-page-shaped non-slab pointer asserts cleanly | Negative test: construct a pointer that's range-valid, page-base-shifted, but the page is a VMSubstrate metadata page (not a slab) — assertion fires at the magic check |
| Slot-range lower-bound rejects a pointer in the descriptor region | Negative test: `vmsfree(slabPage + 16)` where 16 is inside the descriptor area; expect assert at ITEM-048's lower-bound check |
| Modulo check rejects a pointer mid-slot | Negative test: `vmsfree(slot0Addr + 3)` for slotSize=8; expect assert |
| Same-domain free of a Full slab pushes it to the local magazine | Boot smoke: fill a slab to capacity (multiple allocs); free one slot; verify the magazine head now points at that slab |
| Same-domain free that triggers flush publishes the chain to the shared stack | Boot smoke: drive `m.depth` to `maxChainLength` via repeated alloc/free that produces becameAvailable transitions; verify the magazine empties and `partialFor(d)[c]` has a non-null head afterward |
| Cross-domain free pushes to the home-domain stack, not the local magazine | Multi-domain boot smoke (P6-ITEM-005, conditional on `run_numa`) — allocate on CPU A (domain D_A), free on CPU B (domain D_B ≠ D_A); verify `partialFor(D_A)[c]` grew and CPU B's `cpuLocal().magazines[c]` is unchanged |
| Debug poison fills slot bytes with `0xCC` in debug builds | Debug-build boot smoke: free a slot, read the bytes via the magazine head's `m.head->chainNext` view, verify `0xCC` pattern |
| Release builds skip the poison branch entirely (binary doesn't contain `0xCC` memset for vmsfree) | Disassembly grep for `0xCC` poison in release `Kernel` binary's `vmsfree` symbol — none expected |
| `naiveTest` regression: no change in PageAllocator behavior | `cmake --build cmake-build-debug --target run` continues to pass |
| Phase-5 boot smoke + Phase-6 free cycle completes without panic | Boot smoke klog inspection on `make run`, `make run_numa`, `make run_numa_hmat` |

## Testing Approach

- **Boot smoke extension** (P6-DEC-006) — `vmsmallocBootSmoke` (introduced in Phase 5) now
  does alloc → free → alloc per class. Logs all three addresses and asserts the third
  equals the first (same slot reused after free). Asserts the magazine head correctly
  reflects the freed slab's state.
- **DEC-029 whole-page exercise** — boot smoke allocates and frees a 1024-byte block
  (above largestSizeClass = 512). Logs that the returned pointer is page-aligned and that
  the free went through DEC-029 dispatch (klog at the dispatch site, debug builds only).
- **Multi-domain exercise** (conditional, P6-ITEM-005) — under `make run_numa` /
  `make run_numa_hmat`, if the kernel can identify a CPU on a non-BSP domain (e.g., via
  `NUMAPolicy::iterCPUsOnDomain`), allocate from BSP and free from that remote CPU. Note:
  this requires the freer call to actually execute on the remote CPU, which today (no
  scheduler) means using the SMP init-time fan-out infrastructure. If that's too invasive
  for a boot smoke, defer to Phase 8.
- **Negative tests** for the validation chain — Phase 6 ships a small in-kernel test
  routine `vmsfreeNegativeSmoke()` gated by `#ifdef CROCOS_VMSMALLOC_NEGATIVE_TESTS` that
  exercises each assertion. Runs once at boot if enabled. Default: enabled in debug
  builds, disabled in release.
- **No new userspace test variant.** The userspace integration harness for vmsmalloc /
  vmsfree is Phase 8 (mocks VMSubstrate, exercises the magazine state machine under
  TSan). Phase 6 keeps the boot-smoke approach matching Phase 5.
- **Existing regression gates:** `naiveTest`, `make run` boot, three NUMA boot
  configurations. All must pass after Phase 6.

## Implementation Phases

<!-- Concrete ordered steps for Phase 6 itself. -->

1. **Confirm starting state.**
   - Phase 5 is merged. `kernel/mm/vmsmalloc.cpp` exists with `vmsmalloc`'s body,
     `VmsHeadEncoding`, `VmsmallocHooks`, `dispatchOnClass`, file-scope `vmsBase` /
     `vmsSize`, and `kernel::mm::VMSubstrate::vmsmalloc` linkable.
   - `kernel::mm::VMSubstrate::vmsfree` is declared at `kernel/include/mem/VMSubstrate.h:22`
     with no definition. Confirm no other TU defines it.
   - `LibAlloc::SlabBookkeeper::freeSlot(size_t, OccupancyTransition&)` is present
     (Phase 1 P1-DEC-001 amended).
   - `Core::ChainedTreiberStack::push(T&)` and `::pushChain(T&, uint32_t)` are present
     (Phase 4 + P4-DEC-010 evolution).
   - `partialFor(d)` returns a `PartialStack&` per P5-DEC-002. (If Phase 3's
     implementation still returns `TreiberHead*`, update it now — Phase 3 left this
     under-specified.)
   - Confirm `kSlabDescriptorMagic`, `slotSize(desc)`, `slotCount(desc)`,
     `slot0Offset(desc)`, `slotZeroAddr(desc)` are all reachable from `vmsmalloc.cpp`'s
     includes (Phase 2 P2-DEC-002 accessor pattern).

2. **Add the publish helpers and `slotIndexOf` at the top of `vmsmalloc.cpp`'s anonymous namespace.**
   - `slotIndexOf(p, desc)`: pure arithmetic, returns `size_t`.
   - `crossDomainPublish(desc)`: one-liner calling `partialFor(desc->numaDomain)[desc->sizeClass].push(*desc)`.
   - `sameDomainPublishAndMaybeFlush(i, desc)`: writes `desc->chainNext`, increments
     `m.depth`, checks `maxChainLength`, calls `pushChain` if triggered, resets the
     magazine on flush success.

3. **Implement `vmsfree(void* p)`.**
   - Body:
     ```cpp
     void vmsfree(void* p) {
         // Phase 7 will add: assert(p != nullptr) and IRQ/NMI/#GP/#MC context asserts.
         // Phase 6 starts here.

         // DEC-026 step 3: range check (BEFORE freshness — protects ensureTLBEntryFresh
         // from arbitrary VAs).
         kassert(reinterpret_cast<uintptr_t>(p) >= vmsBase &&
                 reinterpret_cast<uintptr_t>(p) <  vmsBase + vmsSize,
                 "vmsfree: pointer outside VMSubstrate range");

         // DEC-029 page-aligned dispatch (BEFORE freshness — a page-aligned bad pointer
         // in an unpopulated arena shouldn't fault inside ensureTLBEntryFresh).
         if ((reinterpret_cast<uintptr_t>(p) & (arch::smallPageSize - 1)) == 0) {
             VMSubstrate::freePage(p);  // freePage asserts on its own validation
             return;
         }

         // DEC-026 step 4: freshness call before any descriptor read.
         VMSubstrate::ensureTLBEntryFresh(p);

         // DEC-026 step 5: derive descriptor.
         SlabDescriptorBase* desc = reinterpret_cast<SlabDescriptorBase*>(
             reinterpret_cast<uintptr_t>(p) & ~(arch::smallPageSize - 1));

         // DEC-026 step 6: magic check (DEC-013 + DEC-044).
         kassert(desc->magic == kSlabDescriptorMagic,
                 "vmsfree: descriptor magic mismatch (not a slab page)");

         // DEC-026 step 6a (ITEM-048): slot-range lower bound. Critical — rejects
         // pointers inside the descriptor area that would pass the modulo via
         // two's-complement underflow.
         const uintptr_t slot0 = reinterpret_cast<uintptr_t>(slotZeroAddr(desc));
         const size_t    ss    = slotSize(desc);
         const size_t    nslot = slotCount(desc);
         kassert(reinterpret_cast<uintptr_t>(p) >= slot0,
                 "vmsfree: pointer inside slab descriptor region");
         kassert(reinterpret_cast<uintptr_t>(p) < slot0 + ss * nslot,
                 "vmsfree: pointer past slab data region");

         // DEC-026 step 6b: modulo (DEC-013).
         const uintptr_t off = reinterpret_cast<uintptr_t>(p) - slot0;
         kassert(off % ss == 0, "vmsfree: misaligned slot pointer");
         const size_t slotIdx = off / ss;

         // DEC-026 step 7 (DEC-024): debug-only poison.
         #ifdef CROCOS_DEBUG
             memset(p, 0xCC, ss);
         #endif

         // DEC-026 step 8: bookkeeper free. 8-way dispatch on sizeClass (P5-DEC-001 sibling).
         OccupancyTransition transition;
         dispatchOnClass(desc, [&](auto* concrete) {
             concrete->bookkeeper.freeSlot(slotIdx, transition);
         });

         // DEC-026 step 9: conditional publish on Full→Partial.
         if (transition.becameAvailable()) {
             // P6-DEC-005: read numaDomain only on the publishing branch.
             const DomainID home = static_cast<DomainID>(desc->numaDomain);
             const arch::ProcessorID i = arch::getCurrentProcessorID();
             const DomainID localDomain = NUMAPolicy::domainFor(i);

             // DEC-019/DEC-034 cross-domain gate.
             if (home == localDomain) {
                 sameDomainPublishAndMaybeFlush(i, desc);
             } else {
                 crossDomainPublish(desc);   // P6-DEC-002: push(element) per user direction
             }
         }
     }
     ```

4. **Update `vmsmallocBootSmoke` (per P6-DEC-006).**
   - For each class `c ∈ [0, kNumSizeClasses)`:
     - `void* p1 = vmsmalloc(kSlabSizeClasses[c]);`
     - `vmsfree(p1);`
     - `void* p2 = vmsmalloc(kSlabSizeClasses[c]);`
     - `kassert(p2 == p1, "vmsfree+vmsmalloc cycle did not reuse the slot");`
     - Log the three addresses.
   - Add a separate DEC-029 cycle: `void* w = vmsmalloc(1024); kassert((w & (pageSize-1)) == 0); vmsfree(w);`.
     Log "DEC-029 whole-page cycle OK".

5. **Add `vmsfreeNegativeSmoke()` (optional, gated by `CROCOS_VMSMALLOC_NEGATIVE_TESTS`).**
   - Construct a stack pointer and confirm `vmsfree(stack)` would trap in the range check.
     Use a fault-trap helper (existing kernel assert infrastructure) so the negative test
     doesn't actually panic — or use a debug-mode "expected to assert" capture macro if
     one exists.
   - Construct a slab-page-base pointer (page-aligned, slab page): confirm DEC-029 dispatch
     unmaps it. (Skipped if the assert at `freePage` is too disruptive for a smoke.)
   - Construct a pointer inside the descriptor region: confirm the slot-range lower-bound
     assert fires.
   - These are best-effort — if the kernel's assert infrastructure makes negative testing
     awkward, defer to Phase 8.

6. **Build and smoke-test.**
   - `cmake --build cmake-build-debug --target Kernel` succeeds.
   - `cmake --build cmake-build-debug --target run` boots; klog shows the Phase-5 alloc
     output AND the Phase-6 free/realloc cycle output with matching addresses.
   - `make run_numa` and `make run_numa_hmat` boot without panic; klog shows the
     per-domain init line from Phase 3 unchanged.
   - `naiveTest` regression: unchanged behavior.

7. **Audit and document.**
   - `grep "DEC-026\|DEC-029\|DEC-019\|DEC-034\|DEC-024" kernel/mm/vmsmalloc.cpp` — every
     cited decision has a comment at its load-bearing site in `vmsfree`'s body.
   - Confirm `desc->numaDomain` is NOT read before `bookkeeper.freeSlot` (P6-DEC-005).
   - Confirm `m.head->chainDepth` is written immediately before `pushChain` with no
     intervening operations (Phase-4 P4-DEC-009 caller invariant).
   - Update `[[project_slab_abstraction_plan]]` memory: Phase 6 status → drafted /
     implemented as appropriate.

8. **Optional follow-ups (under user latitude).**
   - ~~Editorial update to the parent-spec Concurrency Model "cross-domain gate"
     pseudocode~~ — done 2026-05-27 (P6-ITEM-001 resolution); parent spec now calls
     `partial[desc->numaDomain][desc->sizeClass].push(*desc)` directly with the extend
     semantic called out.
   - Surface a `vmsmallocStatsDump()` helper (debug builds only) that walks the
     `tuningFor(d)[c]` rows and logs `overflowCount` / `starvationCount`. Useful as soon
     as Phase 6 lands because the hook bumps mean we have real data to observe.

## References

- `kernel/include/mem/VMSubstrate.h:22` — existing `vmsfree` declaration (this phase
  supplies its body).
- `kernel/include/mem/VMSubstrate.h:52–58` — `destroy<T>` template (already implemented;
  lit up by this phase once `vmsfree` has a body).
- `kernel/mm/vmsmalloc.cpp` — Phase 5 TU, extended in this phase with the `vmsfree` body
  and publish helpers.
- `kernel/mm/VMSubstrate.cpp:624` (`freePage`), `:694` (`ensureTLBEntryFresh`) —
  primitives Phase 6 calls.
- `kernel/mm/VMSubstrateSlab.h` — Phase 2/3 output; `SlabDescriptorBase`,
  `kSlabDescriptorMagic`, `slotSize(desc)` / `slotCount(desc)` / `slotZeroAddr(desc)`
  accessors, `Magazine` type (folded into `kernel::CpuLocal` per P7-DEC-010), `partialFor` / `tuningFor`.
- `libraries/Core/include/core/atomic/TreiberStack.h` — Phase 4 output with P4-DEC-010
  hooks; `push(element)` (extend-or-singleton) and `pushChain(chainHead, depth)`
  (always-new-chain single-CAS).
- `libraries/LibAlloc/include/liballoc/Slab.h` — Phase 1 output;
  `SlabBookkeeper::freeSlot(size_t, OccupancyTransition&)`, `becameAvailable()` predicate,
  `kTailBits` constexpr, amended `isEmpty()`.
- Parent spec `specs/vmsmalloc.md`:
  - DEC-013 — double-free / misaligned-pointer asserts.
  - DEC-016 — freer-side `ensureTLBEntryFresh` ordering.
  - DEC-019 — no cross-NUMA-domain steal (paired with DEC-034 cross-domain gate).
  - DEC-024 — debug-only `0xCC` poison fill before bookkeeper free.
  - DEC-026 (amended ITEM-048) — full validation chain.
  - DEC-029 — page-aligned dispatch to `VMSubstrate::freePage`.
  - DEC-034 — chained-transfer magazines and the same-domain extend / cross-domain
    singleton-push split.
  - DEC-037 — unified magazine (`m.head` is both active-allocation target and
    freer-extension target).
  - DEC-041 — head-linkage; `m.tail` removed; `chainNext == nullptr` on chain bottom.
  - DEC-042 #1 / #3 — push CAS RELEASE publishes chain element writes; chainNext stays
    `Atomic<>` RELAXED for C++ data-race freedom (2026-05-27 clarification).
  - DEC-044 — magic constant value.
  - DEC-045 (amended ITEM-049) — `slotSize(desc)` / `slotCount(desc)` accessor pattern.
- Phase 1 spec `specs/vmsmalloc-phase-1.md` — `freeSlot` contract, `kTailBits` semantics.
- Phase 2 spec `specs/vmsmalloc-phase-2.md` — descriptor layout, accessors, `dispatchOnClass`
  precedent.
- Phase 3 spec `specs/vmsmalloc-phase-3.md` — `Magazine` type (folded into CpuLocal per P7-DEC-010), `partialFor` / `tuningFor`,
  `vmsmallocInit`, per-domain buffer layout (under-specified per P5-DEC-002).
- Phase 4 spec `specs/vmsmalloc-phase-4.md` — `push(element)` and `pushChain(head, depth)`
  semantics, P4-DEC-010 hooks.
- Phase 5 spec `specs/vmsmalloc-phase-5.md` — TU layout, `VmsmallocHooks`, `dispatchOnClass`,
  lazy-freshness model, P5-DEC-002 layout flexibility.
- Memory: `[[project_slab_abstraction_plan]]` — phase plan; updated on Phase 6 completion.
- Memory: `[[project_armv8_dev_tsan]]` — TSan-on-ARMv8 default test target (relevant for
  Phase 8 when it exercises the full alloc/free cycle).
