---
kind: leaf
status: drafting
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 4.5 — CpuLocal infrastructure

> Architecture-portable per-CPU storage abstraction. Replaces the existing
> "GSBase low 8 bits = ProcessorID" scheme with "GSBase = pointer to a
> `kernel::CpuLocal` struct" hosted in each VMSubstrate arena's metadata page
> (extending Phase 3's per-CPU-arena pattern). `CpuLocal` carries the kernel-wide
> per-CPU state — logical CPU ID, interrupt-context depth counters (struct only
> here; consumers in Phase 7), the vmsmalloc magazine array — and is accessed via
> `kernel::cpuLocal()`. Architecture-portable `arch::setCurrentCpuLocalBase` /
> `getCurrentCpuLocalBase` primitives split the AMD64-specific backend
> (`wrgsbase` / `rdgsbase`) from the kernel-wide consumer code; ARMv8 (`TPIDR_EL1`)
> and RISC-V (`tp`) backends are designed to slot in without changing call sites.
> A `cpuLocalReady` flag gates `cpuLocal()` / `getLogicalProcessorID()` calls to
> after VMSubstrate init has run; pre-VMSubstrate consumers are caller bugs that
> must use literal `BSP_LOGICAL_ID = 0` instead.
>
> Phase 4.5 was factored out of an earlier Phase-7 draft (user direction
> 2026-05-27) because Phases 5 and 6 consume `kernel::cpuLocal().magazines[c]` in
> their fast-path code and need the accessor available before they can be
> implemented.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No interrupt-context-guard / dispatchInterrupt integration.** Phase 4.5 ships
  only the `InterruptContextDepths` *struct* (because it's a field of `CpuLocal`).
  The `InterruptKind` enum, `kindForVector` mapping, `InterruptContextGuard`
  RAII type, and the `dispatchInterrupt` body change all live in Phase 7.
- **No entry-point assertions for `vmsmalloc` / `vmsfree`.** Phase 7.
- **No `make<T>` static_asserts (DEC-025).** Phase 7.
- **No DEC-028 convention-internal comment.** Phase 7.
- **No ARMv8 / RISC-V backend implementations.** The portable interface
  (`arch::setCurrentCpuLocalBase` / `getCurrentCpuLocalBase`) is in place so
  future ports add their backend without touching call sites. Only the AMD64
  backend (via GSBase) ships in Phase 4.5.
- **No general-purpose per-CPU API for kernel-wide consumers beyond what
  Phase 7 needs.** `CpuLocal` is designed to be extended (future scheduler
  fields, etc.), but Phase 4.5 doesn't add new consumers — only `getLogicalProcessorID`
  (rewritten), `cpuLocal()` (introduced), and the field declarations are added.
- **No new tests of the per-CPU mechanism beyond confirming the boot sequence
  works.** Phase 7's interrupt-context test plus the existing
  Phase 3 boot smoke regression coverage is sufficient.

## Consumer Contract

### Architecture-portable interface (`kernel/include/arch/cpu_local_base.h`, new)

```cpp
namespace arch {
    // Sets the current CPU's per-CPU base register to `ptr`. Architecture-specific
    // backend (AMD64 → GSBase via wrgsbase; ARMv8 → TPIDR_EL1; RISC-V → tp).
    // Called once per CPU during boot/SMP-init.
    void setCurrentCpuLocalBase(void* ptr) noexcept;

    // Reads the current CPU's per-CPU base register. Cheap — single
    // register-read instruction on every supported architecture.
    void* getCurrentCpuLocalBase() noexcept;
}  // namespace arch
```

### AMD64 backend (`kernel/arch/amd64/cpu_local_base.cpp`, new)

```cpp
namespace arch {
    void setCurrentCpuLocalBase(void* ptr) noexcept {
        asm volatile("wrgsbase %0" : : "r"(reinterpret_cast<uint64_t>(ptr)));
    }
    void* getCurrentCpuLocalBase() noexcept {
        uint64_t v;
        asm volatile("rdgsbase %0" : "=r"(v));
        return reinterpret_cast<void*>(v);
    }
}
```

### `InterruptContextDepths` struct (`kernel/include/interrupts/InterruptContextDepths.h`, new — struct only in 4.5)

```cpp
namespace kernel::interrupts {

    struct alignas(arch::cacheLineSize) InterruptContextDepths {
        uint32_t irq;   // external IRQ (vector >= 32)
        uint32_t nmi;   // vector 2
        uint32_t ud;    // vector 6 (#UD undefined opcode)
        uint32_t df;    // vector 8 (#DF double fault)
        uint32_t gp;    // vector 13 (#GP general protection)
        uint32_t mc;    // vector 18 (#MC machine check)
        uint32_t pf;    // vector 14 (#PF page fault)
        // Phase 4.5 ships just the struct (for embedding in CpuLocal). Phase 7
        // extends this header with InterruptKind, kindForVector,
        // InterruptContextGuard, and the dispatchInterrupt integration.
    };
    static_assert(sizeof(InterruptContextDepths) == arch::cacheLineSize);
    static_assert(alignof(InterruptContextDepths) == arch::cacheLineSize);

}  // namespace kernel::interrupts
```

### `kernel::CpuLocal` struct (`kernel/include/cpu_local.h`, new)

```cpp
namespace kernel {

    struct alignas(arch::cacheLineSize) CpuLocal {
        arch::ProcessorID logicalID;
        // implicit padding to next cache line
        kernel::interrupts::InterruptContextDepths interruptDepths;
        kernel::mm::vmsmalloc::Magazine magazines[kernel::mm::vmsmalloc::kNumSizeClasses];
        // Future fields (e.g., scheduler's preempt_count, current_thread)
        // land here.
    };
    // Size envelope: 64 (logicalID + padding) + 64 (interruptDepths) + 8 × 64
    // (magazines) = 640 B; one page hosts the struct with room for future
    // growth.

    inline constexpr size_t kCpuLocalBytes =
        divideAndRoundUp(sizeof(CpuLocal), arch::smallPageSize) * arch::smallPageSize;
    inline constexpr size_t kCpuLocalPages = kCpuLocalBytes / arch::smallPageSize;
    static_assert(kCpuLocalBytes % arch::smallPageSize == 0);
    static_assert(kCpuLocalPages >= 1);

    // Readiness flag (file-scope, defined in cpu_local.cpp). Initialized false;
    // set true at the end of VMSubstrate init.
    extern bool cpuLocalReady;

    // Cheap typed accessor. Assertion-gated.
    inline CpuLocal& cpuLocal() noexcept {
        assert(cpuLocalReady, "cpuLocal() called before VMSubstrate init");
        return *static_cast<CpuLocal*>(arch::getCurrentCpuLocalBase());
    }

}  // namespace kernel
```

### `getLogicalProcessorID` rewrite (`kernel/arch/amd64/smp/smp.cpp:27–31`)

```cpp
ProcessorID getLogicalProcessorID() {
    return kernel::cpuLocal().logicalID;  // assertion gate per cpuLocal()
}
```

`setLogicalProcessorID` is removed. Its declaration is dropped from
`kernel/include/arch/amd64/smp.h`. The logicalID field is written into each
arena-resident `CpuLocal` at arena-creation time during VMSubstrate init (see
"VMSubstrate init integration" below), then immutable.

### VMSubstrate init integration (extends Phase 3's createArena)

Phase 3's `createArena(i)` already allocates and maps a per-CPU page on
`NUMAPolicy::domainFor(i)` between the occupancy buffer and the allocatable
region. Phase 4.5 adds two steps at the end of `createArena(i)`:

1. **Zero the CpuLocal page**: `memset(cpuLocalPageFor(i), 0, kCpuLocalBytes);`.
2. **Write the logicalID**: `static_cast<CpuLocal*>(cpuLocalPageFor(i))->logicalID = i;`.

At the end of VMSubstrate's `init()` (after the `for (i = 0; i < cpuCount; i++)
createArena(i)` loop completes):

3. **Point BSP's GSBase**: `arch::setCurrentCpuLocalBase(cpuLocalPageFor(0));`.
4. **Flip the readiness flag**: `kernel::cpuLocalReady = true;`.

After step 4, all `cpuLocal()` and `getLogicalProcessorID()` callers on the
BSP work normally.

### AP bootstrap integration

Each AP's bootstrap routine (per the init-registry DSL's `ap_routine`)
**prepends one instruction**: `arch::setCurrentCpuLocalBase(cpuLocalPageFor(myApLogicalID))`.
The AP's logical ID is obtained from the existing SMP-bringup parameter
mechanism (today's `setLogicalProcessorID` call site already has this value;
Phase 4.5 swaps the body to `setCurrentCpuLocalBase`). `cpuLocalReady` is
already `true` on the AP because the BSP set it during VMSubstrate init.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| Phase 3 — VMSubstrate arena layout with per-CPU page | The per-CPU page reservation (`kCpuLocalPages` per P3-DEC-004 amended) is where each CPU's `CpuLocal` lives. Phase 4.5 consumes the `cpuLocalPageFor(i)` accessor introduced by Phase 3. | Yes — Phase 3 must land. |
| Phase 2 — `kernel::mm::vmsmalloc::Magazine` type, `kNumSizeClasses` | The `Magazine magazines[kNumSizeClasses]` field of `CpuLocal` needs the Magazine struct definition. | Yes — Phase 2 must land. |
| `kernel/arch/amd64/smp/smp.cpp:18–31` — `setLogicalProcessorID` / `getLogicalProcessorID` | The sole pre-existing consumer of the GSBase scheme. Phase 4.5 rewrites `getLogicalProcessorID` and removes `setLogicalProcessorID`. | Refactored in Phase 4.5. |
| `kernel/include/arch/amd64/smp.h` | Drop `setLogicalProcessorID` declaration. | Refactored in Phase 4.5. |
| `arch::ProcessorID` (typedef in `kernel/include/arch/amd64/amd64.h:90`) | The `logicalID` field type. | Yes — live. |
| `arch::cacheLineSize` | The `CpuLocal` struct's alignas requirement. | Yes — live. |
| Per-CPU storage discipline | The CR4.FSGSBASE bit must be enabled for `wrgsbase` / `rdgsbase` to work. CroCOS already enables it (CLAUDE.md: "qemu64 with fsgsbase support"). The pre-existing `setLogicalProcessorID` uses `wrgsbase` already, so the prerequisite holds. | Yes — live. |
| SMP bringup path (BSP early init + AP bootstrap routines) | Phase 4.5 hooks `setCurrentCpuLocalBase` into both. | Refactored in Phase 4.5. |
| `kernel::assert` | Used by the `cpuLocal()` readiness assertion. | Yes — live. |

## Invariants

- **`cpuLocalReady` is monotonic** — initialized `false` at boot, set `true`
  exactly once during VMSubstrate init, never written again. Single writer
  (BSP at init time); any number of readers afterward. No atomic needed at
  the language level because the write-once-then-read-many pattern lets
  release/acquire ordering happen through other synchronization (the BSP's
  init returns; APs read it only after their bootstrap, by which point the
  BSP's init has completed).
- **Every CPU's GSBase points at its own arena-resident `CpuLocal` after
  the BSP's VMSubstrate-init completes (for the BSP) or after the AP's
  bootstrap-routine first instruction (for each AP).** Before these points,
  GSBase is undefined; the assertion in `cpuLocal()` is intentionally the
  guard.
- **`CpuLocal::logicalID == i` for the CpuLocal page at `cpuLocalPageFor(i)`.**
  Written exactly once during `createArena(i)` and never modified.
- **No false sharing across CPUs.** Each CPU's CpuLocal lives in a distinct
  arena's metadata page (one or more pages, all NUMA-placed on the CPU's home
  domain). Within the struct, `InterruptContextDepths` and `Magazine` are
  `alignas(cacheLineSize)`; logicalID has implicit padding to the next cache
  line.
- **Architecture portability.** All call sites use `arch::setCurrentCpuLocalBase`
  / `getCurrentCpuLocalBase`; no `rdgsbase` / `wrgsbase` / `IA32_GS_BASE`
  appears outside `kernel/arch/amd64/cpu_local_base.cpp`.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `cpuLocal()` called before `cpuLocalReady == true` (debug build) | Assertion fires; caller is in pre-VMSubstrate-init code and must replace with literal `BSP_LOGICAL_ID = 0`. | No (caller bug) |
| `cpuLocal()` called in release build with stale/zero GSBase | Reads garbage from VA 0 (or wherever the bootloader left GSBase). Crashes downstream. Phase 4.5 is debug-build-only safe; release relies on caller discipline established during the debug-build migration. | No (caller bug) |
| AP fires an interrupt before its bootstrap-routine calls `setCurrentCpuLocalBase` | `dispatchInterrupt` (Phase 7) reads `cpuLocal().interruptDepths` from an undefined GSBase; crashes. Discipline: `setCurrentCpuLocalBase` is the FIRST instruction of the AP's `ap_routine`. | No (caller bug) |
| `PageAllocator::allocateSmallPage(i)` fails during `createArena(i)`'s CpuLocal page setup | Panic (DEC-012). VMSubstrate init can't continue. | No |
| Stray direct read of GSBase outside the AMD64 backend file | Compile-time grep audit catches it (step 1 of implementation). | No (review-time fix) |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P45-ITEM-001 | Resolved 2026-05-27 (plain `bool`) | | | Should `cpuLocalReady` be `Atomic<bool>` or plain `bool`? | Resolved: plain `bool`. Single-writer (BSP at init), read by APs only after BSP-init has returned. The SMP-bringup happens-before edge (APs don't run until BSP launches them, which is after BSP-init completes) provides the ordering without needing atomicity. Atomic would add a fence on every `cpuLocal()` call for no correctness benefit. |
| P45-ITEM-002 | Resolved 2026-05-27 (named constant) | | | Should pre-VMSubstrate call sites use a `BSP_LOGICAL_ID` constant or literal `0`? | Resolved: named constant. Declare `constexpr arch::ProcessorID BSP_LOGICAL_ID = 0;` in `kernel/include/arch/amd64/smp.h`. Improves readability and grep-ability for the migration audit. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P45-DEC-001 | Settled | **`CpuLocal` lives in the per-CPU arena metadata page introduced by Phase 3 (P3-DEC-004 amended).** No separate `PageAllocator::allocateSmallPage` call for CpuLocal; the existing arena layout already provides NUMA-local placement. `cpuLocalPageFor(i)` returns the page VA. | Reuses Phase 3's existing per-CPU NUMA-placed storage. CpuLocal becomes available during VMSubstrate init (the natural point) without inventing a separate allocation lifecycle. Simpler than the alternative considered (allocate CpuLocal via PageAllocator directly during SMP init). |
| P45-DEC-002 | Settled | **`cpuLocalReady` flag gates `cpuLocal()` / `getLogicalProcessorID()`.** Plain `bool`, initialized `false` in BSS, set `true` as the last statement of VMSubstrate's `init()`. Both accessors `assert(cpuLocalReady)` at entry. Pre-init callers are caller bugs; the fix is replacing the call with literal `BSP_LOGICAL_ID = 0`. | Per user direction 2026-05-27. Forces migration discipline rather than papering over with a BSS-bootstrap struct. Single-writer-then-many-reader pattern (BSP writes at init; APs and BSP read post-init) doesn't need atomicity — the SMP-bringup happens-before edge provides ordering. The assertion is intentionally loud; once the migration is settled (no early callers fire it across enough boots), it can be gated behind `#ifdef CROCOS_DEBUG_CPULOCAL_INIT` or removed. |
| P45-DEC-003 | Settled | **Architecture-portable abstraction: `arch::setCurrentCpuLocalBase` / `getCurrentCpuLocalBase`.** AMD64 backend in `kernel/arch/amd64/cpu_local_base.cpp` uses `wrgsbase` / `rdgsbase`. ARMv8 (`TPIDR_EL1`) and RISC-V (`tp`) backends are designed to slot in by adding a single source file per architecture; the portable interface and `kernel::CpuLocal` struct are unchanged. | Standard kernel idiom (Linux `__this_cpu_*`, FreeBSD `PCPU_GET`). Splitting the arch-specific accessor from the portable struct definition means future ports add their backend without touching call sites. |
| P45-DEC-004 | Settled | **`InterruptContextDepths` struct lives in `kernel/include/interrupts/InterruptContextDepths.h` and ships with just the struct definition in Phase 4.5** (no consumer logic). The struct is a field of `kernel::CpuLocal`. Phase 7 extends the same header with the `InterruptKind` enum, `kindForVector` mapping, and `InterruptContextGuard` RAII type. | Avoids a forward-declaration / circular-include problem: `CpuLocal` needs the struct definition; the consumer logic (guard, dispatchInterrupt integration) belongs naturally to Phase 7's assertion-paths scope. Splitting the header between phases is mechanical and incurs no overhead. |
| P45-DEC-005 | Settled | **The pre-VMSubstrate `BSP_LOGICAL_ID = 0` migration is forcing, not optional.** Any caller that fires the `cpuLocalReady` assertion is rewritten to use the constant. The audit happens at step 1 of Phase 4.5 implementation (grep for `getLogicalProcessorID` and `setLogicalProcessorID` across the tree). | Per user direction 2026-05-27. The assertion is the migration trigger; the migration is mechanical (literal value substitution); the result is a clean codebase where `cpuLocal()` and friends have a meaningful precondition. |

## Hazards

- **CR4.FSGSBASE must be enabled** for `wrgsbase` / `rdgsbase`. CroCOS already
  enables it (QEMU `qemu64,+fsgsbase` per CLAUDE.md; the pre-existing
  `setLogicalProcessorID` uses `wrgsbase`). If a future build configuration
  disables FSGSBASE, the AMD64 backend faults with #UD; alternative is
  `wrmsr(IA32_GS_BASE, ptr)` (slower but works on every AMD64 CPU).
- **AP bootstrap must call `setCurrentCpuLocalBase` BEFORE any other
  instruction that reads cpuLocal or fires an interrupt.** Watch for any
  IDT unmasking, MMIO access, or kernel-thread setup that runs before the
  `setCurrentCpuLocalBase` call.
- **The `cpuLocalReady` assertion is a development-only safety net.** Once
  the pre-VMSubstrate call-site audit is complete and stable, gate behind
  `#ifdef CROCOS_DEBUG_CPULOCAL_INIT` (or remove) to save one bool load per
  `cpuLocal()` call. Migration trigger: enough successful boots without the
  assertion firing.
- **GSBase semantic change** — pre-Phase-4.5: low 8 bits hold `ProcessorID`.
  Post-Phase-4.5: full pointer to `CpuLocal`. Step 1 grep for `rdgsbase` /
  `wrgsbase` / `IA32_GS_BASE` ensures no stray direct readers remain.
- **`CpuLocal` lifetime is the kernel's lifetime** — once created, the
  arena page hosting the struct is pinned. No teardown / migration / hot-plug
  story (parent-spec Non-Goals).
- **ARMv8 / RISC-V backends are unimplemented in Phase 4.5.** The portable
  interface is shaped to accept them; the AMD64 backend is the only one
  shipped. If a future port starts, the new backend goes in
  `kernel/arch/<arch>/cpu_local_base.cpp` with no changes elsewhere.

## Verification Targets

| Property | Method |
|---|---|
| `cpuLocal()` and `getLogicalProcessorID()` work correctly after VMSubstrate init on the BSP | Boot smoke: `make run` boots successfully; `getLogicalProcessorID()` returns 0 from the BSP post-init |
| `cpuLocal().logicalID == i` for each CPU `i` post-bootstrap | Add a debug-only post-init scan logging each CPU's `cpuLocal().logicalID`; assert equality with the iteration index |
| `make run_numa` / `make run_numa_hmat` boot; each CPU's CpuLocal lives on its home NUMA domain | Boot smoke + klog of `cpuLocalPageFor(i)` VAs; manual verification |
| `naiveTest` continues to pass after the smp.cpp / arena-layout / SMP-bringup changes | `cmake --build cmake-build-debug --target run`; runtime within ±5% of baseline |
| `cpuLocalReady` assertion fires when called before VMSubstrate init (debug build) | Negative test: insert a `cpuLocal()` call at a pre-init code path; confirm the harness catches the assertion-failure exception |
| Grep audit: no stray `rdgsbase` / `wrgsbase` / `IA32_GS_BASE` outside `kernel/arch/amd64/cpu_local_base.cpp` | Code review during merge |
| Grep audit: every previous `getLogicalProcessorID()` call site has been audited and (if pre-VMSubstrate) replaced with `BSP_LOGICAL_ID = 0` | Code review during merge |
| `setLogicalProcessorID` removed from both `smp.cpp` and `smp.h` | Code review during merge |

## Testing Approach

- **Boot smoke regression** — `make run`, `make run_numa`, `make run_numa_hmat`
  all boot. Klog the BSP's `cpuLocal().logicalID` (should be 0) and one AP's
  `cpuLocal().logicalID` (should be > 0).
- **Debug-only post-init scan** — for each CPU `i`, the BSP reads
  `static_cast<CpuLocal*>(cpuLocalPageFor(i))->logicalID` and asserts equality.
- **Negative test for the readiness flag** — inserted manually as a one-off
  during implementation, then removed. Confirm the assertion fires when
  `cpuLocal()` is called before VMSubstrate init.
- **No TSan variant.** Phase 4.5's hot path is per-CPU read-only; no
  concurrency to validate.

## Implementation Phases

1. **Confirm starting state.**
   - Phases 1–3 are merged. Phase 3's `cpuLocalPageFor(i)` accessor (or its
     pre-amendment `localCachePageFor(i)` equivalent) is in place. The per-CPU
     arena page is allocated and mapped during `createArena(i)`.
   - Grep `rdgsbase`, `wrgsbase`, `IA32_GS_BASE` to confirm `getLogicalProcessorID`
     at `kernel/arch/amd64/smp/smp.cpp:27` is the sole consumer of the GSBase
     scheme.
   - Grep every call site of `getLogicalProcessorID()` and identify any that
     run pre-`memory_management` phase. List them for the audit in step 4.

2. **Create `kernel/include/arch/cpu_local_base.h` + `kernel/arch/amd64/cpu_local_base.cpp`.**
   - Header: portable declarations of `arch::setCurrentCpuLocalBase` /
     `getCurrentCpuLocalBase`.
   - AMD64 source: `wrgsbase` / `rdgsbase` inline-asm bodies.

3. **Create `kernel/include/interrupts/InterruptContextDepths.h`.**
   - Just the struct definition + `static_assert`s for size and alignment.
   - Phase 7 extends this header with `InterruptKind`, `kindForVector`,
     `InterruptContextGuard`.

4. **Create `kernel/include/cpu_local.h` + `kernel/cpu_local.cpp`.**
   - Header: `kernel::CpuLocal` struct, `kCpuLocalBytes` / `kCpuLocalPages`
     constants, `cpuLocalReady` extern, `cpuLocal()` inline accessor with
     readiness assertion.
   - Source: `bool kernel::cpuLocalReady = false;` definition.

5. **Audit pre-VMSubstrate `getLogicalProcessorID()` call sites.**
   - For each site identified in step 1, replace with the literal
     `BSP_LOGICAL_ID = 0` (declared in `kernel/include/arch/amd64/smp.h`).
   - Add a comment at each rewritten site: `// Pre-VMSubstrate-init: BSP-only
     context; see P45-DEC-002.`

6. **Rewrite `getLogicalProcessorID` and remove `setLogicalProcessorID`.**
   - `kernel/arch/amd64/smp/smp.cpp`: replace `getLogicalProcessorID`'s body
     with `return kernel::cpuLocal().logicalID;`; delete
     `setLogicalProcessorID`.
   - `kernel/include/arch/amd64/smp.h`: delete `setLogicalProcessorID`
     declaration; add `constexpr arch::ProcessorID BSP_LOGICAL_ID = 0;`.

7. **Extend VMSubstrate's `createArena(i)` and `init()`.**
   - At the end of `createArena(i)`: zero the CpuLocal page; write
     `static_cast<CpuLocal*>(cpuLocalPageFor(i))->logicalID = i;`.
   - At the end of VMSubstrate `init()` (after the createArena loop):
     `arch::setCurrentCpuLocalBase(cpuLocalPageFor(0));` then
     `kernel::cpuLocalReady = true;`.

8. **Update AP bootstrap routines.**
   - Each AP's `ap_routine` (per the init-registry DSL): the first
     instruction is `arch::setCurrentCpuLocalBase(cpuLocalPageFor(myApLogicalID))`,
     where `myApLogicalID` comes from the existing SMP-bringup parameter
     mechanism (today's `setLogicalProcessorID(myApLogicalID)` argument).
   - Confirm the AP's IDT is unmasked AFTER the `setCurrentCpuLocalBase` call.

9. **Build, smoke-test, regression-gate.**
   - `cmake --build cmake-build-debug --target Kernel` succeeds.
   - `make run` boots through; klog shows BSP and AP CpuLocal addresses.
   - `make run_numa` / `make run_numa_hmat` boot; per-CPU CpuLocal pages
     are on the correct NUMA domain (verify via klog VA inspection).
   - `naiveTest` regression: unchanged.

10. **Audit and document.**
    - Grep audit: no `rdgsbase` / `wrgsbase` / `IA32_GS_BASE` outside
      `kernel/arch/amd64/cpu_local_base.cpp`.
    - Grep audit: no `getLogicalProcessorID` calls in pre-`memory_management`
      code paths.
    - Update `[[project_slab_abstraction_plan]]` memory: Phase 4.5 status →
      drafted / implemented.

11. **Optional follow-ups (under user latitude).**
    - Gate the `cpuLocalReady` assertion behind `#ifdef CROCOS_DEBUG_CPULOCAL_INIT`
      once the migration is stable.
    - Add ARMv8 (`TPIDR_EL1`) backend if/when an ARMv8 port begins.
    - Add RISC-V (`tp`) backend if/when a RISC-V port begins.
    - Add a debug `dumpCpuLocal()` helper for diagnostic tools.

## References

- `kernel/arch/amd64/smp/smp.cpp:18–31` — pre-Phase-4.5 `setLogicalProcessorID`
  and `getLogicalProcessorID`. Phase 4.5 rewrites these.
- `kernel/include/arch/amd64/smp.h:20–21` — pre-Phase-4.5 declarations. Phase
  4.5 removes `setLogicalProcessorID`; adds `BSP_LOGICAL_ID`.
- `kernel/include/arch/cpu_local_base.h` — **new (Phase 4.5)**: portable
  `arch::setCurrentCpuLocalBase` / `getCurrentCpuLocalBase` declarations.
- `kernel/arch/amd64/cpu_local_base.cpp` — **new (Phase 4.5)**: AMD64
  backend.
- `kernel/include/cpu_local.h` — **new (Phase 4.5)**: `kernel::CpuLocal`
  struct, `kCpuLocalBytes` / `kCpuLocalPages`, `cpuLocalReady`, `cpuLocal()`.
- `kernel/cpu_local.cpp` — **new (Phase 4.5)**: `cpuLocalReady` definition.
- `kernel/include/interrupts/InterruptContextDepths.h` — **new (Phase 4.5,
  struct only)**: the depth-counter struct. Phase 7 extends this header.
- `kernel/include/arch/amd64/amd64.h:90` — `arch::ProcessorID` typedef
  (consumed by `CpuLocal::logicalID`).
- Parent spec `specs/vmsmalloc.md`:
  - DEC-018 — NUMA placement (Phase 4.5 reuses).
  - DEC-021 — VMSubstrate init ordering (Phase 4.5 extends).
  - DEC-033 — per-CPU arena page (partially superseded 2026-05-27 by
    P45-DEC-001 + Phase-7 P7-DEC-010; the per-CPU page now hosts the unified
    `kernel::CpuLocal`).
  - DEC-046 — `allocPage` TLB invariant (Phase 4.5 inherits; relevant when
    CpuLocal pages are first written).
- Phase 3 spec `specs/vmsmalloc-phase-3.md` — `cpuLocalPageFor(i)` accessor,
  arena-layout reservation (amended for the unified CpuLocal placement).
- Phase 7 spec `specs/vmsmalloc-phase-7.md` — consumes Phase 4.5's
  `cpuLocal()` and `InterruptContextDepths` struct; adds the
  `InterruptContextGuard` + `dispatchInterrupt` integration + entry-point
  asserts.
- Memory: `[[project_slab_abstraction_plan]]` — phase plan; updated on
  Phase 4.5 completion.
