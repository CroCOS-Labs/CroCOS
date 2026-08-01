---
kind: leaf
status: implemented
parent: specs/rcu.md
---

# RCU Phase 2 — Kernel Veneer

> `kernel::rcu::Domain` / `ReadGuard` / `protect` / `retire`: binds Phase 1's engine to CPU
> identity, NUMA-placed slot storage, `SafePtr` freshness, and the DEC-014 context rules.

## Non-Goals

- **No algorithm.** Every grace-period decision lives in Phase 1. This layer is binding and
  policy only.
- **No IPI, no timer, no idle hook** (RCU-DEC-007). Nothing in this phase sends or handles an IPI.
- **No dynamic domain creation.** Domains are statically declared and initialized once at
  `memory_management`. Runtime create/destroy is a userspace-test-only capability of the engine.
- **No slot storage in `CpuLocal`** (RCU-DEC-014).
- **No consumer.** RadixVM is out of scope for this spec tree; Phase 2 ships with tests as its
  only caller.

## Consumer Contract

`kernel/include/rcu/RCU.h` (public) + `kernel/rcu/RCU.cpp`, `namespace kernel::rcu`.

```cpp
class Domain {
public:
    constexpr Domain() noexcept = default;   // INERT — RCU-DEC-015
    // Reserves slots, constructs the engine. drainBatchBound is the RCU-DEC-033 knob — without
    // this parameter no kernel path could ever set it, and the #PF-latency motivation for the
    // bound would be nullified in the only environment that has #PF (subagent review 2026-08-01).
    bool init(const char* name, size_t drainBatchBound = SIZE_MAX) noexcept;
    Domain(const Domain&) = delete;
    Domain& operator=(const Domain&) = delete;
};

class ReadGuard {                            // RAII, non-copyable, non-movable
public:
    explicit ReadGuard(Domain&) noexcept;
    ~ReadGuard() noexcept;
};

// Acquire-load an RCU-published pointer and wrap it for freshness (RCU-DEC-011).
template <typename T>
mm::VMSubstrate::SafePtr<T> protect(Domain&, const Atomic<T*>& src) noexcept;

// Deferred destruction. T embeds a Core::rcu::RetireHead; the member-pointer NTTP
// recovers T* from the head with no RTTI and no offsetof games.
template <typename T, Core::rcu::RetireHead T::* Head>
void retire(Domain&, T* obj, void (*deleter)(T*)) noexcept;

// Convenience: poison in debug, then VMSubstrate::destroy<T>.
template <typename T, Core::rcu::RetireHead T::* Head>
void retireDestroy(Domain&, mm::VMSubstrate::SafePtr<T>) noexcept;

void   synchronize(Domain&) noexcept;   // grace period only — RCU-DEC-031
void   barrier(Domain&) noexcept;       // caller's retirees destroyed — RCU-DEC-031
bool   tryAdvance(Domain&) noexcept;    // engine tryAdvance(slot), slot = current CPU
size_t drain(Domain&) noexcept;         // ≡ engine sweepExpired(slot), slot = current CPU
```

`drainAllQuiescent` (RCU-DEC-035) has **no kernel veneer entry point in this phase**: dynamic
domain teardown is a Non-Goal here, so the teardown drain remains an engine-level capability used
by the userspace tests (and by Phase 3's scenario teardown). A future per-process-domain consumer
adds the veneer then, with the no-new-users precondition supplied by process teardown.

`ReadGuard` is the only supported way to enter a section. It binds to
`kernel::getLogicalProcessorID()` at construction and asserts in debug that the same CPU
destroys it (RCU-DEC-009).

`protect` debug-asserts the caller is inside a section on this domain.

Context rules are two-tier (RCU-DEC-012, RCU-DEC-031): `retire`, `drain`, `tryAdvance` inherit
vmsmalloc DEC-014 verbatim — forbidden in IRQ / NMI / #UD / #DF / #GP / #MC, conditionally legal
in #PF (the carve-out is load-bearing: RadixVM retires from the fault path) — debug-asserted via
`inForbiddenContext(kAllocForbiddenDepthMask)`. The blocking primitives `synchronize` and
`barrier` take the **strict** mask: forbidden in *any* interrupt context, **including #PF** — a
grace-period wait inside a fault handler spins on other CPUs' progress from a context that may
itself be blocking them. With RCU-DEC-026's packed word the strict check is simply `w != 0`.

`ReadGuard` construction and destruction are legal in any context **except NMI and #MC**
(RCU-DEC-024), asserted via `inForbiddenContext(kRcuReadSideForbiddenDepthMask)`. This narrows
the original R2, which promised legality up to and including NMI, and the narrowing belongs in
the header comment as well as here — a profiler or watchdog author reaching for RCU in an NMI
handler will otherwise find only a debug-build assertion.

Three further obligations from the parent's post-review resolutions:

- **`ReadGuard` masks maskable interrupts across the outermost transition only**, not across the
  section body, using `arch::InterruptDisabler` (RCU-DEC-025). An IRQ arriving during the
  protected traversal is harmless: the publish is complete, so a nested `readLock` correctly
  short-circuits on `nesting != 0`.
- **`retire` requires the caller to already be inside a section on this domain *or* in deleter
  context** (RCU-DEC-019 as amended by RCU-DEC-038), debug-asserted as
  `nesting > 0 || slot.inDrain || domain.teardownActive`. Deleters are bound by the RCU-DEC-039
  clause: they may touch only state reachable solely from the retired object — no assert can
  check this. Note the interaction with `synchronize`/`barrier`, which are forbidden *inside* a
  section **and inside a drain** (`!inDrain` asserted — from a deleter they would hang, not
  fail): a teardown path that retires and then synchronizes must leave the section between the
  two.
- **`protect` uses `kProtectedLinkLoad` (ACQUIRE)** (RCU-DEC-022) before wrapping in `SafePtr`.
- **`barrier` guarantees the caller's own retirees only in full** (RCU-DEC-031): it seals **and
  rotates** the calling CPU's open bag (seal-without-rotate is the I11-fatal implementation —
  RCU-DEC-036), then drives advances and sweeps until every pre-call `Sealed`/`Claimed` bag is
  destroyed. Objects still in *other* CPUs' open bags (at entry) are excluded — the ITEM-014
  residue class. The guarantee is **per-slot, not per-thread**: under a future scheduler the
  caller must hold CPU affinity from the covered `retire` through the `barrier` (RCU-DEC-040 —
  migration-off, preemption still fine). Interrupt readers arriving while either blocking
  primitive spins are explicitly permissible (I3 delays, never permits).
- **Execution-context assumptions (RCU-DEC-032)** are enforced at this layer: P2-DEC-006's
  `preemptionDisabled()` / `cpuPinned()` asserts are the forcing function that makes "engine
  calls never migrate" a checked rule when a scheduler lands, and read-side sections are
  non-preemptible by contract.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| `Core::rcu::EpochDomain` (Phase 1) | The algorithm | Yes |
| `VMSubstrate::reservePerDomainStaticBuffer(size_t, numa::DomainID)` | Slot storage, NUMA-placed, zero-filled, kernel-lifetime | Yes |
| `VMSubstrate::SafePtr<T>` / `destroy<T>` | `protect` return type; the terminal deleter | Yes |
| `kernel::getLogicalProcessorID()` (`kernel/include/CpuLocal.h:88`) | Slot identity | Yes |
| `arch::processorCount()`, `arch::CACHE_LINE_SIZE` | Slot array sizing and alignment | Yes |
| `kernel::interrupts::currentCpuInterruptDepths()` | Context asserts | Yes |
| `kernel::numa` policy | Placing each CPU's slot in its local domain | Yes |
| `kernel::timing::monoTimens()` | Debug stall detection only | No |
| `kernel/rcu/rcu.icd` → `init::` registry | Init ordering | Yes |

**Prerequisites — both landed 2026-08-01, ahead of this phase:**

1. `inForbiddenContextForVmsmalloc()` was hoisted out of `kernel/mm/vmsmalloc.cpp`'s anonymous
   namespace into `kernel/include/interrupts/InterruptContextDepths.h` as
   `inForbiddenContext(mask)`, with `kAllocForbiddenDepthMask` and
   `kRcuReadSideForbiddenDepthMask` as the two policies (RCU-DEC-012). The depth counters were
   simultaneously narrowed to `uint8_t` and packed into one 8-byte word (RCU-DEC-026), making
   every context query one aligned load, one AND and one test.
2. **`arch::InterruptDisabler` was fixed.** Its constructor recorded `wasEnabled` but never called
   `disableInterrupts()` (`kernel/arch/arch.cpp:59-62`) — a save/restore helper that disabled
   nothing. `ReadGuard` depends on it for RCU-DEC-024, and the ITEM-008 fix would have been a
   silent no-op on top of it. Note this also silently affected `ClockManager::compareTimerTicks`
   and six `TimerQueues` mutation paths, which had been running their critical sections with
   interrupts live.

## Invariants

- **P2-I1.** Slot index `== kernel::getLogicalProcessorID()`, always. There is no other mapping.
- **P2-I2.** `slotCount == arch::processorCount()` at `init()` time, and `processorCount()` never
  changes afterwards.
- **P2-I3.** Slot storage is never freed, never remapped, and always mapped — so the framework's
  own state needs no `ensureTLBEntryFresh`. This is a property of
  `reservePerDomainStaticBuffer`, and it is why the dirty-bit mechanism does not apply to it.
- **P2-I4 (amended at implementation, 2026-08-01).** CPU *i*'s slot resides in a page placed on
  the home domain of the first CPU whose slot lands on that page. The original per-CPU form is
  **unachievable**: `EpochDomain` indexes `slots[i]` at natural stride so the array must be dense,
  while `reservePerDomainStaticBuffer` places whole pages — so page granularity (32 slots at the
  current `ReaderSlot` size) is the finest available. On every configuration CroCOS boots today
  (≤ 32 CPUs) this degenerates to one page on CPU 0's domain. The two rejected alternatives were a
  single whole-array reservation (identical below 32 CPUs, no better above) and one page per CPU
  plus a stride parameter threaded through Phase 1's hot path (exact, but 4 KiB/CPU and it reopens
  a shipped, TSan-green engine). User-confirmed.
- **P2-I5.** `Domain`'s constructor touches nothing outside its own members (RCU-DEC-015).
- **P2-I6.** No `Domain` method sends an IPI or arms a timer.

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| `Domain` used before `init()` | Debug: every veneer entry asserts `Domain`'s `initialized` flag — **with P2-DEC-009's opaque storage there is no engine pointer to null-check** (final review F6); the flag is a plain `bool` member set at the end of `init()`. Release: the veneer launders storage holding no object — UB; on zeroed storage, a null slot-array deref or vacuous scans, i.e. reclamation with no reader protection. | No (init-order bug) |
| `init()` called twice | Debug: assert. Release: leaks the first slot reservation (no free API) and rebinds. | No |
| `reservePerDomainStaticBuffer` exhausted at `init()` | Panics inside VMSubstrate (existing behavior). `init()` returns `false` → `required = true` component fails → `assertNotReached`. | No |
| `retire` from forbidden context | Debug: assert naming the context. Release: deleters may reenter vmsmalloc on this CPU's magazine. | No (caller bug) |
| `ReadGuard` destroyed on a different CPU than constructed | Debug: assert. Release: two slots corrupted — one stuck active, one under-nested. | No (caller bug) |
| Section held across a debug-threshold duration | Debug: warning naming the slot and elapsed ns. Release: silent. | Yes (diagnostic only) |
| `protect` called outside a section | Debug: assert. Release: returns an unprotected pointer — a latent UAGP. | No (caller bug) |
| `synchronize` / `barrier` from interrupt context (incl. #PF) | Debug: assert via the strict mask. Release: spin inside a handler, potential self-deadlock. | No (caller bug) |
| `barrier` stalled on a bag `Claimed` by another CPU | Waits, deleter-bounded (RCU-DEC-031). The RCU-DEC-013 stall detector must name the offending bag/CPU. | Yes (diagnostic) |

## Questions

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P2-ITEM-001 | Resolved 2026-08-01 — **implemented ahead of the phase** | | | Where does the hoisted forbidden-context predicate live? | `kernel/include/interrupts/InterruptContextDepths.h`, as `inForbiddenContext(uint64_t mask)` plus the `kAllocForbiddenDepthMask` / `kRcuReadSideForbiddenDepthMask` policy constants. Landed 2026-08-01 together with RCU-DEC-026's `uint8_t` packing; `vmsmalloc.cpp`'s file-local copy now delegates to it. Verified: kernel boots on `run` / `run_numa` / `run_numa_hmat`, 655 tests + 412 Core TSan green. |
| P2-ITEM-002 | Resolved 2026-08-01 → P2-DEC-009 | | | How does the engine object get constructed given RCU-DEC-015's inert-ctor rule — placement-new into opaque `alignas` storage inside `Domain` (the vmsmalloc `PartialStackStorage` + `launder` pattern), or a pointer to engine storage carved out of the same static buffer? | Opaque storage. Keeps the Core template out of the public header, avoids a per-call indirection, matches the established in-tree pattern. |
| P2-ITEM-003 | Open | No | | Should `protect` take `const Atomic<T*>&` or a small `RcuPtr<T>` wrapper that makes the published-pointer type self-documenting and prevents non-`protect` loads? | A wrapper would catch "loaded the link without entering a section" at compile time rather than by debug assert. Costs a type. Worth deciding against real radix-node link types (parent RCU-DEC-016 defers the same way). |
| P2-ITEM-004 | Partially resolved 2026-08-01 — `ReadGuard` half implemented, `tryAdvance` half still Open | No | | Does the section-duration stall detector belong in `ReadGuard` (per-section) or in `tryAdvance` (per-blocked-advance, reporting the offending slot)? | Shipped in `ReadGuard`, because that is the form the Failure Modes table names ("warning naming the slot and elapsed ns") and the one `monoTimens` was already a dependency for. The stamp is taken **outside** the masked window per RCU-DEC-024 correction (iii). The `tryAdvance` variant remains genuinely wanted and genuinely different — it knows *which* slot blocks an advance, which the per-section form cannot — so the item stays open as an additive follow-up rather than closed. |
| P2-ITEM-005 | Deferred | No | | Should there be a `drainOnIdle()` entry point ready for the eventual idle loop? | The `hlt` loops exist (`kernel/KernelMain.cpp:82`, `kernel/arch/amd64/smp/smpboot.asm:139-142`) but are currently unreachable because `VmsmallocStress` never returns. Additive per R8; not needed for the memory bound given RCU-DEC-006. |

## Decisions

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P2-DEC-001 | Settled | **`kernel/include/rcu/` is its own small subsystem**, not a subdirectory of `mem/`. | Resolves `RCU-proposal.md` Q1. RCU is not VMM-specific — a future consumer outside MM should not reach into `mem/` for it — and `interrupts/` and `timing/` set the precedent for small cross-cutting kernel subsystems. Decided 2026-08-01. |
| P2-DEC-002 | Settled | **Slot storage via `reservePerDomainStaticBuffer`, one reservation per NUMA domain**, each CPU's slot in its local domain. | Parent RCU-DEC-014. The decisive factor is that the BSP's `CpuLocal` migrates mid-boot (GSBase is re-pointed from `bspBootstrapCpuLocal` to the arena page during `VMSubstrate::init`), which would silently zero slots placed there. Independently: no `CpuLocal` growth per domain, and domains stay test-constructible. Decided 2026-08-01. |
| P2-DEC-003 | Settled | **`protect<T>` returns `SafePtr<T>`.** | Parent RCU-DEC-011. Resolves `RCU-proposal.md` Q6 as yes, at the veneer only. A radix walk needs `ensureTLBEntryFresh` at every level anyway; folding it in gives one call site instead of two, and `SafePtr`'s non-explicit `T*` constructor makes the wrap free. Decided 2026-08-01. |
| P2-DEC-004 | Settled | **Registered as `kernel/rcu/rcu.icd`, phase `memory_management`, `depends_on = ["VMSubstrateSlab"]`, `per_cpu = false`, `required = true`.** | Needs `make<T>`/`vmsfree` and `reservePerDomainStaticBuffer`, both of which are live after `VMSubstrateSlab`. `per_cpu = false` makes it an implicit all-CPU barrier (APs spin on completion, `kernel/init.cpp:112-114`), which is what we want: slots must exist before any AP can enter a section. The recursive glob at `kernel/CMakeLists.txt:99-101` picks the file up with no CMake edit — **but the build must be re-configured** for the glob to re-evaluate. Decided 2026-08-01. |
| P2-DEC-005 | Settled | **All checks debug-only** (parent RCU-DEC-013), including context, CPU-pinning, section membership, and stall detection. | The read side must stay at one store plus one fence in release (R1). Consistent with P7-DEC-001. Decided 2026-08-01. |
| P2-DEC-006 | Settled | **`ReadGuard` asserts `preemptionDisabled()` and `cpuPinned()` despite both being vacuous `return true` stubs** (`kernel/mm/vmsmalloc.cpp:315-321`). | The DEC-030 forcing-function pattern. Free today, and when a scheduler lands the assertions become real without anyone having to remember to add them. Decided 2026-08-01. |
| P2-DEC-007 | Settled | **`retireDestroy<T>` poisons in debug then calls `VMSubstrate::destroy<T>`.** | Grace-period expiry is exactly the point at which DEC-026's `vmsfree` validation chain may safely run. Debug poison stacks with vmsmalloc's own DEC-024 `0xCC` poison, giving two independent UAGP tells. Decided 2026-08-01. |
| P2-DEC-008 | Provisional | **One domain per protected structure, declared as a namespace-scope `Domain` object**, `init()`'d from the `.icd` routine. | R6. Provisional on the count staying small — a registry becomes worthwhile only once several domains exist and something wants to iterate them (e.g. a future idle hook). Decided 2026-08-01. |
| P2-DEC-009 | Settled | **The engine is placement-new'd into opaque `alignas(64)` byte storage inside `Domain`, recovered via `launder`** — the vmsmalloc `PartialStackStorage` pattern. The size/alignment `static_assert` against `sizeof(Core::rcu::EpochDomain<KernelRcuHooks>)` lives in `RCU.cpp`, where the template is visible; `RCU.h` declares only the byte array and a storage-size constant. | Resolves P2-ITEM-002. The pointer-into-static-buffer form leaks the instantiated engine type into the public header and adds an indirection to every veneer call, buying only implementation simplicity that the established pattern (`VMSubstrateSlab.h:289-290`) has already paid for elsewhere. `constexpr Domain() = default` stays inert (RCU-DEC-015): the bytes are storage, not an object, until `init()` constructs into them. Decided 2026-08-01. |
| P2-DEC-010 | Settled — implemented | **`retire`'s deleter is a template parameter, not a runtime argument**: `retire<T, Head, Deleter>(Domain&, T*)`, and `retireDestroy<T, Head>` binds `Deleter = detail::destroyDeleter<T>`. | The signature written in this spec — `retire<T, Head>(Domain&, T*, void (*)(T*))` — is **unimplementable**. `RetireHead` has exactly one function-pointer slot and it must hold the thunk that recovers `T*` from an interior head (P1-DEC-003), so there is nowhere left to store a runtime deleter without widening `RetireHead` past RCU-DEC-016's 16 bytes. Making the deleter an NTTP keeps the thunk stateless, keeps I8 (retire never allocates), and costs a consumer needing a runtime choice exactly one dispatching deleter. The rejected alternative was a 24-byte `RetireHead` carrying thunk + erased deleter, which grows every retirable object and reopens the shipped Phase-1 header. User-confirmed 2026-08-01. |
| P2-DEC-011 | Settled — implemented | **The veneer reuses Phase 1's `CROCOS_RCU_NOEXCEPT`, and `~ReadGuard` additionally takes `CROCOS_RCU_DTOR_NOEXCEPT` (`noexcept(false)` under `CORE_LIBRARY_TESTING`). `~ReadGuard` performs the engine `readUnlock` FIRST and diagnoses afterwards.** | Two separate traps, both the same shape as Phase 1's finding #2/#3. (i) Every veneer entry point being `noexcept` would turn each of this phase's `EXPECT_ASSERT_FAILURE` targets into `std::terminate`, since `assert` throws under the harness; a destructor needs its own spelling because it is implicitly `noexcept(true)` regardless. (ii) Diagnosing before the unlock would leave the slot permanently nested when the RCU-DEC-009 cross-CPU assert fires, so every negative test would then trip the engine's quiescence check and report the wrong failure. Unlocking first also confines a genuine caller migration to the caller's bug rather than corrupting a second slot. Decided 2026-08-01. |
| P2-DEC-012 | Settled — implemented | **A single namespace-scope `kernel::rcu::kernelDomain`, `init()`'d by the `.icd` routine `kernel::rcu::initialize`.** | P2-DEC-008 keeps one-domain-per-structure as the norm; this is not a counterexample but the domain a consumer adopts before it deserves its own. It also exists for a verification reason: with no consumer shipping in this phase, it is the only thing that exercises `init()` in a real boot, and therefore the only thing that can demonstrate the RCU-DEC-015 inert constructor and the P2-DEC-004 component actually running. Decided 2026-08-01. |
| P2-DEC-013 | Settled — implemented | **`Domain` has no destructor**; the engine in opaque storage is never destroyed. | Correct for a type the kernel only ever declares at namespace scope (dynamic domain teardown is a Non-Goal), and it keeps `Domain` trivially destructible so a static one registers no `atexit` handler. Note the consequence: `~Domain` cannot carry RCU-DEC-034's quiescence assert. Tests reach that check through `DebugIntrospection::assertQuiescent` instead, which is where Phase 1 already put it. Decided 2026-08-01. |

## Hazards

- **The inert-constructor rule is easy to violate accidentally.** Any member with a non-trivial
  default constructor makes `Domain`'s constructor dynamic and it runs during `cpp_init`, before
  `VMSubstrate` exists. The `PITEventSource` boot panic is the precedent. `constexpr` on the
  constructor is the mechanical guard.
- **Slot storage is zero-filled, and zero means "no readers".** That is correct for an initialized
  empty domain and wrong for an uninitialized one — an uninitialized domain scans clean and
  reclaims freely. Only the P2-DEC-005 init assert stands between that and silent UAGP.
- **`SafePtr::operator*` and `operator->` call `ensureTLBEntryFresh` on *every* dereference**
  (`kernel/include/mem/VMSubstrate.h:87-88`). A 4-level radix walk therefore pays it at least four
  times, and it is already the flagged allocator hotspot. Correct, but the read path's cost is
  dominated by freshness, not by RCU — worth knowing before anyone optimizes the wrong fence.
- **Two `assert` macros in flight.** Kernel builds get `<kassert.h>`; test builds get the throwing
  `CORE_LIBRARY_TESTING` variant. vmsmalloc's harness had to make the mock byte-identical to Core's
  because a TU includes both and only *identical* redefinition is legal. Any new mock here inherits
  that constraint.
- **`arch::MAX_PROCESSOR_COUNT` is 256 but the real AP ceiling is 16** (`SMPStack stacks[16]`,
  `kernel/arch/amd64/smp/smp.cpp:80`, marked a temporary hack). Sizing slots off
  `processorCount()` rather than the constant avoids reserving 256 cache lines per domain, and
  avoids depending on a number that is currently fictional.
- **`.icd` glob staleness.** Adding `kernel/rcu/rcu.icd` without re-running CMake configure leaves
  the component silently unregistered — the kernel boots fine and the domain is simply never
  initialized, which per the failure table is the worst possible failure mode.

## Verification Targets

| Property | Method |
|---|---|
| P2-I1 — slot index tracks `getLogicalProcessorID()` | Harness test with `bindThreadToCpu` |
| P2-I4 — CPU *i*'s slot is in *i*'s local NUMA domain | Manual review + boot log at `init()` |
| Inert constructor (RCU-DEC-015) | Kernel boots clean on `run` / `run_numa` / `run_numa_hmat` |
| `.icd` component actually runs | Boot log line from `init()`; absence is the silent-failure mode |
| Forbidden-context asserts fire | `EXPECT_ASSERT_FAILURE` negative tests |
| Two-tier masks discriminate: `retire` legal in mock-#PF context, `synchronize`/`barrier` assert there | `EXPECT_ASSERT_FAILURE` with the settable depths mock |
| `protect` outside a section asserts | `EXPECT_ASSERT_FAILURE` |
| `ReadGuard` cross-CPU destruction asserts | `EXPECT_ASSERT_FAILURE` in the harness |
| `retireDestroy` round-trips a `make<T>` object | Harness integration test |
| Deleter runs exactly once per retired object | Harness test with a counting deleter |
| No IPI or timer use (P2-I6) | Manual review + grep |

## Testing Approach

`tests/kernel/rcu/`, following the vmsmalloc Phase-8 harness (`tests/kernel/vmsmalloc/`) closely
enough that its CMakeLists is the starting template:

- `mocks/` **first** on the include path, shadowing `<mem/VMSubstrate.h>`, `<mem/NUMA.h>`,
  `<kassert.h>`. Real `CpuLocal.h`, `InterruptContextDepths.h`, and Phase 1's `EpochDomain.h`
  compile as-is under `CROCOS_TESTING`.
- Defines: `CROCOS_TESTING`, `CORE_LIBRARY_TESTING`, `CORE_KERNEL_TESTING`,
  `CORE_LINKED_WITH_KERNEL`, `CROCOS_RCU_TEST_HARNESS`.
- Force-include `assert_support.h` on every TU (`-include .../assert_support.h`) — the throwing
  assert references `CroCOSTest`, which the mocks do not otherwise pull in.
- Reuse `kernel::test::bindThreadToCpu` (`tests/kernel/vmsmalloc/mocks/MockKernelEnv.cpp:75`).

**Two mocks must be written that do not exist yet:**

1. **`timing::monoTimens()`** — no userspace mock exists anywhere in `tests/`. A monotonic
   counter suffices; the stall detector is the only consumer.
2. **Non-zero `currentCpuInterruptDepths()`** — the existing vmsmalloc mock returns an all-zero
   struct (`MockKernelEnv.cpp:68-69`), so the forbidden-context path is never exercised. Testing
   P2-DEC-005's asserts requires a settable mock.

Sibling ASan/leak and TSan executables, mirroring `KernelVmsmallocIntegrationTestRunner[TSan]`,
with `-Wl,-undefined,dynamic_lookup` on Apple. Both registered in `run_all_tests`.

## Implementation Phases

1. **Hoist the forbidden-context predicate** out of `vmsmalloc.cpp`'s anonymous namespace
   (resolve P2-ITEM-001 first). Verify vmsmalloc still builds and boots unchanged.
2. **`Domain` + `init()`** — slot reservation, NUMA placement, engine construction into opaque
   storage (P2-DEC-009), the `drainBatchBound` parameter. Boot log line.
3. **`rcu.icd`** + CMake re-configure; confirm the component runs on `run` and `run_numa`.
4. **`ReadGuard`** with nesting, CPU binding, and the RCU-DEC-009 asserts.
5. **`protect<T>`** returning `SafePtr<T>`, with the in-section assert.
6. **`retire<T, Head>` / `retireDestroy<T>`** — the member-pointer thunk and the debug poison.
7. **`synchronize` / `barrier` / `tryAdvance` / `drain`** pass-throughs with the two-tier
   context asserts (strict mask for the blocking pair).
8. **Harness** — mocks (including the two new ones), integration tests, negative assertion tests,
   ASan + TSan runners green.

## Implementation Notes (2026-08-01)

Shipped as `kernel/include/rcu/RCU.h` + `kernel/rcu/RCU.cpp` + `kernel/rcu/rcu.icd`, with
`tests/kernel/rcu/{VeneerTest,AssertionsTest,ConcurrentTest}.cpp` and
`tests/kernel/rcu/DebugIntrospection.h`. 34/34 green under both ASan and TSan (ARMv8), zero race
reports; full suite 752 tests green; kernel boots clean on `run`, `run_numa`, `run_numa_hmat` with
the `init()` log line present in all three; `RCU.cpp` verified against
`x86_64-crocos-g++ -O2 -Werror` with every debug-only check confirmed absent from the release
object.

Five things the spec did not anticipate, beyond P2-DEC-010..013 above:

1. **`checkQuiescent` demands every bag be `Free`, not merely empty.** A bag that has ever been
   opened stays `Open` until a drain releases it, and `barrier` seals **and rotates** — so the
   freshly rotated bag is Open-and-empty. Asserting quiescence straight after a `barrier` therefore
   fails on a CORRECT build. The sanctioned sequence is `drainAllQuiescent()` then the check
   (RCU-DEC-035), which is what the tests do. Three test-logic bugs traced to this.

2. **I3's exact-match rule means a fresh reader does not block the first advance.** A reader pinned
   at the current epoch `e` is not stale, so `tryAdvance` succeeds `e → e+1`; it is the *second*
   advance the reader blocks. That pair is exactly what a grace period is, but "an active section
   blocks tryAdvance" is the wrong expectation to write.

3. **The double-retire check cannot see a bag's first node.** The already-linked marker is
   `next != nullptr`, and the first node pushed into a bag legitimately has `next == nullptr`.
   Phase 1's `RetireHead` comment claims `next` is nullptr "exactly when the node is not in a bag";
   the bag-head case is a standing exception and the check is best-effort there. Not a protocol
   bug — a hole in a debug-only check — but it should be stated where it is claimed.

4. **`Core::rcu::EpochDomain` gained `inSection(size_t)`.** `protect`'s in-section assert needs to
   read owner-only nesting, and `DebugIntrospection` is test-gated, so a debug kernel had no way to
   perform the check the spec requires. Additive, narrow, non-protocol.

5. **`PrintStream` has no `size_t` overload on the host.** On x86_64-ELF `size_t` *is* `uint64_t`
   so the kernel build is clean, but on the harness's Apple target `size_t` is `unsigned long` and
   every `klog() << someSize` is ambiguous. Log sites cast to `uint64_t` explicitly.

Two shared mocks were extended rather than duplicated (`tests/kernel/vmsmalloc/mocks/`):
`MockKernelEnv.cpp` gained settable per-thread `InterruptContextDepths` (+ `MockInterruptContext.h`
with an RAII `ScopedContext`) and `arch::test::setProcessorCount`, and
`MockVMSubstrate::reservePerDomainStaticBuffer` now rounds to whole pages exactly like the kernel's
— without which RCU's slot array sits behind an unrounded vmsmalloc buffer and is under-aligned.
New in `tests/kernel/rcu/mocks/`: `timing/timing.h` + `monoTimens` and `arch::InterruptDisabler`.

## References

- `specs/rcu.md` — parent; RCU-DEC-011 through RCU-DEC-015, RCU-DEC-031/032 (primitive split,
  strict mask, execution-context assumptions), RCU-DEC-033 (batch bound — plumbed via `init()`),
  RCU-DEC-035/037 (engine-only teardown drain), RCU-DEC-036 (barrier mechanics), RCU-DEC-038/039
  (retire assert + deleter clause), RCU-DEC-040 (affinity obligation).
- `specs/rcu-phase-1.md` — the engine this binds.
- `specs/vmsmalloc-phase-8.md` — the userspace-harness pattern being copied.
- `specs/vmsmalloc-phase-7.md` — P7-DEC-001 (debug-only asserts), DEC-014 context rules,
  DEC-030 stubs.
- `kernel/include/mem/VMSubstrate.h` — `SafePtr` (`:78-94`), `make`/`destroy` (`:96-133`),
  `reservePerDomainStaticBuffer` (`:69`).
- `kernel/general.icd` — `.icd` schema by example; `tools/gen_init_registry.py` for the grammar.
