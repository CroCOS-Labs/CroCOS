---
kind: leaf
status: drafting
parent: vmsmalloc.md
components: []
---

# vmsmalloc Phase 4 — Concurrent Treiber stack primitives in LibCore

> Add `libraries/Core/include/core/atomic/TreiberStack.h` defining
> `Core::TreiberStack<T, LinkageExtractor, HeadEncoding>` (basic intrusive concurrent stack with
> ABA-safe tagged head) and `Core::ChainedTreiberStack<T, HeadLinkage, ChainLinkage,
> HeadEncoding>` (chain-batching variant). `ChainedTreiberStack` exposes two push paths:
> `push(element)` extends the current top chain up to a runtime-tunable `maxChainLength` or
> starts a new singleton (used by the cross-domain singleton-free path); `pushChain(chainHead,
> depth)` publishes a pre-built chain as a new top in one CAS (matches parent-spec DEC-034
> magazine-flush semantics verbatim, ignores `maxChainLength`). `pop()` returns the whole top
> chain in a single CAS. The DEC-015 packed-tagged-head encoding lives in
> vmsmalloc as a `HeadEncoding` policy supplied in Phase 5; Core ships
> `Uint128HeadEncoding` as a `__uint128_t`-based default for consumers without a
> fixed-VA-window constraint. Memory ordering follows the named constants `kTreiberPushOrder`,
> `kTreiberPopLoadOrder`, `kTreiberPopCASOrder` pinned by DEC-042. A new TSan-instrumented test
> binary `CoreTestRunnerTSan` runs the concurrent stress alongside the existing ASan runner;
> ARMv8-on-M1 is the release gate.

## Non-Goals

<!-- What this phase explicitly does not handle. -->

- **No vmsmalloc-side consumer code.** Phase 4 ships the primitive and its tests. The
  vmsmalloc-supplied `HeadEncoding` policy (DEC-015 packed offset + 37-bit counter) and the
  `partial[d][c]` storage lives in Phase 5 / 6.
- **No magazine / per-CPU caching.** That's a vmsmalloc concept (DEC-034) implemented on top
  of these stacks in Phase 5. ChainedTreiberStack's `maxChainLength` controls the SHARED-stack
  chain bound; it is unrelated to magazine depth `currentK`.
- **No `chainDepth` semantic enforcement.** The stack maintains and reads `chainDepth` as part
  of the encoded head metadata (so push can decide extend-vs-new without walking), but it is
  the consumer's responsibility to write `chainDepth` on a node that is published as a chain
  head. `chainNext` is similarly consumer-managed via the `ChainLinkage` extractor.
- **No depth-zero / empty-chain marker.** An "empty stack" is encoded as `nullptr` pointer
  with any tag; the encoding policy defines the exact zero-value sentinel. The Treiber stack
  does not store a chain-bottom marker beyond `chainNext == nullptr` on the bottom node.
- **No hazard pointers, no RCU, no epoch-based reclamation.** ABA safety comes from the
  tagged-head encoding alone (per DEC-015 / the consumer's policy). Reclamation is the
  consumer's responsibility — for vmsmalloc, slabs live forever (DEC-002 lazy reclamation).
  The Core default `Uint128HeadEncoding` has a 64-bit counter that wraps in ~584 years at
  10⁹ ops/sec; a 64-bit-pointer-plus-counter scheme is genuinely safe.
- **No type-erased / runtime-polymorphic stack API.** All policies are template parameters.
- **No `cmpxchg16b` polyfill.** The default `Uint128HeadEncoding` requires native lock-free
  `__atomic_compare_exchange` on a 16-byte value; a `static_assert(__atomic_is_lock_free(16,
  nullptr))` fires on platforms that lack it. (AMD64 has it since Westmere; ARMv8.0 has LDXP
  /STXP for 16-byte LL/SC; ARMv8.1 added LSE `CASP`. CroCOS targets all three.)
- **No `scavenge()` / drain-all-chains API.** Iterating the stack non-destructively is not
  needed by any Phase-4-or-earlier consumer. (Parent-spec ITEM-009 will eventually need a
  drain API on the Treiber stacks; it can be added when Phase 11 ships.)
- **No tests beyond the unit + concurrent stress suites.** Integration tests against the
  vmsmalloc magazine state machine are Phase 8.

## Consumer Contract

### Header: `libraries/Core/include/core/atomic/TreiberStack.h`

Public surface (in `namespace Core`):

```cpp
namespace Core {

    // ─── Linkage extractor concept (basic Treiber) ───────────────────────────
    //
    // The Treiber linkage is the "next chain head on the shared stack" pointer.
    // For TreiberStack<T>, every node uses this linkage; for ChainedTreiberStack
    // it is defined only on chain heads.
    template <typename T, typename Extractor>
    concept TreiberLinkageExtractor = requires(T& node, T* p) {
        { Extractor::getNext(node) } -> convertible_to<T*>;
        { Extractor::setNext(node, p) };
    };

    // ─── Head encoding policy concept ────────────────────────────────────────
    //
    // The HeadEncoding policy defines the on-the-wire head layout. It is a
    // collection of static methods (no state). The policy supplies an opaque
    // Storage type — typically uint64_t (for fixed-VA-window encodings like
    // DEC-015) or __uint128_t (the default for generic consumers). The Treiber
    // stack holds an Atomic<Storage> and routes every load/store/CAS through
    // the policy's pack / unpack / advanceTag helpers.
    //
    // Encoded "empty stack" is whatever pack(nullptr, anyTag) produces; the
    // policy must guarantee that unpack of the zero-initialized Storage value
    // yields a nullptr pointer (so default-constructed stacks are empty).
    template <typename T, typename Policy>
    concept TreiberHeadEncoding = requires(typename Policy::Storage s,
                                           typename Policy::Tag t,
                                           T* p) {
        typename Policy::Storage;
        typename Policy::Tag;
        { Policy::pack(p, t) } -> convertible_to<typename Policy::Storage>;
        { Policy::unpackPointer(s) } -> convertible_to<T*>;
        { Policy::unpackTag(s) } -> convertible_to<typename Policy::Tag>;
        { Policy::advanceTag(t) } -> convertible_to<typename Policy::Tag>;
        requires is_trivially_copyable_v<typename Policy::Storage>;
        // Zero-init invariant: unpacking a zero Storage yields nullptr.
        // (Stated as a runtime expectation; not checkable via concept.
        //  Static-asserted in TreiberStack's body via a constexpr probe.)
    };

    // ─── Hooks policy concept (extension points; default = no-ops) ───────────
    //
    // Hooks let consumers inject behavior at specific lifecycle points without
    // exposing internal state. The concept matches the existing extractor pattern
    // (static methods on a policy type) but Hooks instances may also carry state
    // (e.g., a pointer to a tuning row) — the policy is template-parameterized
    // *and* member-stored so each stack instance can bind its own hook context.
    //
    // The three hook points:
    //   - onPreTouch(topPtr): fires inside `pop()` and inside `push(element)`'s
    //     extend branch, AFTER the head acquire-load (so `topPtr` is known) but
    //     BEFORE any read of `*topPtr`'s fields. vmsmalloc supplies a hook that
    //     calls `VMSubstrate::ensureTLBEntryFresh(topPtr)` here — the popper's
    //     TLB may cache a stale VA→PA mapping for `topPtr`'s page (the chain head
    //     could have cycled through `freePage` + `allocPage` since this CPU last
    //     touched it), so the read of `topPtr->next` / `topPtr->chainDepth` could
    //     otherwise see content from the previous physical backing. See parent-spec
    //     DEC-040 (amended for lazy-freshness) for the full hazard analysis.
    //   - onCasFailure(): fires inside any push/pop CAS-loop retry that observed
    //     a head-changed failure. Per-iteration call; tuning policies use this
    //     to track contention.
    //   - onEmptyStack(): fires inside `pop()` when the loaded head's pointer is
    //     null (the stack is empty for this CPU's view). Tuning policies use this
    //     as a starvation signal.
    //
    // Default `NoopHooks` has zero-size members and zero-overhead methods —
    // suitable for Core consumers without freshness or tuning needs.
    template <typename T, typename H>
    concept TreiberHooks = requires(H& h, T* p) {
        { h.onPreTouch(p) };
        { h.onCasFailure() };
        { h.onEmptyStack() };
    };

    struct NoopHooks {
        template <typename T> void onPreTouch(T*) const noexcept {}
        void onCasFailure() const noexcept {}
        void onEmptyStack() const noexcept {}
    };

    // ─── Default head encoding for generic consumers ─────────────────────────
    //
    // 128-bit head: low 64 bits = pointer; high 64 bits = monotonically
    // advancing tag. Push-only counter advance (matches DEC-015 rule).
    template <typename T>
    struct Uint128HeadEncoding {
        struct alignas(16) Storage {
            uintptr_t ptr;
            uint64_t  tag;
        };
        using Tag = uint64_t;

        static constexpr Storage pack(T* p, Tag t) {
            return { reinterpret_cast<uintptr_t>(p), t };
        }
        static constexpr T* unpackPointer(Storage s) {
            return reinterpret_cast<T*>(s.ptr);
        }
        static constexpr Tag unpackTag(Storage s) { return s.tag; }
        static constexpr Tag advanceTag(Tag t)   { return t + 1; }

        static_assert(sizeof(Storage) == 16);
        static_assert(__atomic_is_lock_free(16, nullptr),
                      "Uint128HeadEncoding requires native 16-byte lock-free CAS "
                      "(cmpxchg16b on AMD64; LDXP/STXP or CASP on ARMv8). "
                      "Supply a custom HeadEncoding policy if your target lacks it.");
    };

    // ─── Memory-ordering constants (DEC-042 pinned) ──────────────────────────
    //
    // Every CAS site in this file references these by name. Any downgrade
    // (e.g. push CAS to RELAXED) is a one-point edit and an audit trigger.
    // Do NOT collapse kTreiberPopLoadOrder to RELAXED — x86 TSO would tolerate
    // it but ARMv8/RISC-V would silently break (the exact failure mode DEC-042
    // is portably correct against).
    inline constexpr MemoryOrder kTreiberPushOrder       = RELEASE;
    inline constexpr MemoryOrder kTreiberPushLoadOrder   = RELAXED;
    inline constexpr MemoryOrder kTreiberPopLoadOrder    = ACQUIRE;
    inline constexpr MemoryOrder kTreiberPopCASOrder     = ACQUIRE;
    inline constexpr MemoryOrder kTreiberCASFailureOrder = RELAXED;
    inline constexpr MemoryOrder kChainLinkOrder         = RELAXED;

    // ─── Basic Treiber stack ─────────────────────────────────────────────────
    //
    // Concurrent intrusive stack. push(node) pushes a single node; pop()
    // returns the top node (or nullptr if empty).
    //
    // Memory model: see DEC-042. Push CAS is RELEASE; pop initial load is
    // ACQUIRE (load-bearing on weak architectures).
    template <typename T, typename LinkageExtractor, typename HeadEncoding>
    requires TreiberLinkageExtractor<T, LinkageExtractor>
          && TreiberHeadEncoding<T, HeadEncoding>
    class TreiberStack {
        using Storage = typename HeadEncoding::Storage;
        alignas(64) Atomic<Storage> head{};   // zero-init == empty per policy contract

    public:
        TreiberStack() = default;
        TreiberStack(const TreiberStack&) = delete;
        TreiberStack& operator=(const TreiberStack&) = delete;

        // push(node): single-element push. Performs one CAS loop on the head.
        // node->next is written inside the loop (consumer's prior store, if any, is
        // overwritten). On success, every prior write to *node is published to any
        // future pop().
        void push(T& node);

        // pop(): single-element pop. Returns the popped node, or nullptr if empty.
        // Reads node->next inside the loop after an ACQUIRE-load of head.
        T* pop();

        // Snapshot accessor: lockless peek. Returns nullptr if empty; the returned
        // pointer may be stale by the time the caller dereferences it. Safe only
        // for diagnostic use (e.g. asserting empty at teardown).
        T* peek() const;

        [[nodiscard]] bool empty() const { return peek() == nullptr; }
    };

    // ─── Chained Treiber stack ───────────────────────────────────────────────
    //
    // Like TreiberStack, but every shared-stack head bears an intra-chain linkage
    // (`ChainLinkage::getNext` / `setNext`) and an explicit `chainDepth` (consumer
    // field on T, read via `ChainLinkage::getChainDepth` / `setChainDepth`).
    //
    // push(element) semantics:
    //   - If the top chain currently has fewer than maxChainLength elements,
    //     extend it by making `element` the new head (element->chainNext = oldTop;
    //     element->next = oldTop->next; element->chainDepth = oldTop->chainDepth + 1)
    //     and CAS-publishing the new head. The old top remains structurally reachable
    //     via element's chainNext.
    //   - Otherwise (stack empty, or top chain at maxChainLength), push `element`
    //     as a new singleton chain (element->chainNext = nullptr; element->next =
    //     oldTop; element->chainDepth = 1) and CAS-publish.
    //
    // pushChain(chainHead, depth) semantics:
    //   - Caller passes a pre-built chain: `chainHead` is the chain's head, the
    //     chain walks via ChainLinkage::getChainNext through `depth - 1` more
    //     elements terminating with chainNext == nullptr at the bottom. The caller
    //     has set ChainLinkage::setChainDepth(chainHead, depth) and the chainNext
    //     linkage on every element.
    //   - One CAS publishes the chain as the new top, **always as a new chain**
    //     — never as an extension of the existing top. `maxChainLength` is ignored.
    //     (Extending the existing top would require a walk to the incoming chain's
    //     bottom; the caller is expected to have already chosen `depth` as the
    //     amortization unit, so respect it.)
    //   - Push CAS sets chainHead->next = oldTop and advances the tag.
    //   - This is the magazine-flush path per parent-spec DEC-034.
    //
    // pop() semantics:
    //   - One CAS atomically detaches the entire top chain. Returns a
    //     PoppedChain { head: T*, depth: uint32_t }, or { nullptr, 0 } if empty.
    //   - The caller is the sole owner of the popped chain head and all chainNext-
    //     reachable nodes; the stack will not touch them again.
    //   - Walking the chain (head, head->chainNext, …) is the caller's job. Any
    //     freshness sweep (e.g. vmsmalloc's DEC-040 ensureTLBEntryFresh walk) is
    //     similarly the caller's job.
    //
    // maxChainLength is mutable at runtime (relaxed-atomic uint32). Producers
    // read it inside their push loop; a setter call by a tuning policy is visible
    // on the next push. Reducing it does not affect existing in-stack chains —
    // they retain their oversize depth until popped; only new pushes are bounded.
    //
    // Concurrency on the chain-extend path: when push reads the existing top's
    // chainNext / chainDepth / next fields, it does so speculatively. Those fields
    // are immutable while the top sits on the stack (only push can write them, and
    // a successful push replaces the top atomically), so the speculative read sees
    // a consistent snapshot or the CAS fails. See Invariants below.
    template <typename T,
              typename HeadLinkage,
              typename ChainLinkage,
              typename HeadEncoding,
              typename Hooks = NoopHooks>
    requires TreiberLinkageExtractor<T, HeadLinkage>
          && TreiberHeadEncoding<T, HeadEncoding>
          && TreiberHooks<T, Hooks>
    class ChainedTreiberStack {
        using Storage = typename HeadEncoding::Storage;
        alignas(64) Atomic<Storage>  head{};
        alignas(64) Atomic<uint32_t> maxChainLength;
        [[no_unique_address]] Hooks  hooks{};   // zero-size for NoopHooks

    public:
        struct PoppedChain { T* head; uint32_t depth; };

        explicit ChainedTreiberStack(uint32_t initialMaxChainLength)
            requires std::is_default_constructible_v<Hooks>
            : maxChainLength(initialMaxChainLength) {}

        ChainedTreiberStack(uint32_t initialMaxChainLength, Hooks h)
            : maxChainLength(initialMaxChainLength), hooks(move(h)) {}

        ChainedTreiberStack(const ChainedTreiberStack&) = delete;
        ChainedTreiberStack& operator=(const ChainedTreiberStack&) = delete;

        // Push a single element. May extend the existing top chain or push a new
        // singleton; either way, exactly one CAS publishes the new head.
        void push(T& element);

        // Push a pre-built chain as a new top chain (single CAS, no extend). The
        // caller has already set chainNext linkage through the chain bottom and
        // chainDepth = depth on chainHead. maxChainLength is not consulted.
        void pushChain(T& chainHead, uint32_t depth);

        // Atomically detach the entire top chain. Returns {nullptr, 0} on empty.
        PoppedChain pop();

        // Runtime knob. Cheap relaxed read on every push; cheap relaxed store
        // by a tuning thread. New value visible on next push.
        void     setMaxChainLength(uint32_t k) {
            maxChainLength.store(k, RELAXED);
        }
        uint32_t getMaxChainLength() const {
            return maxChainLength.load(RELAXED);
        }

        // Diagnostic peek; returns {nullptr, 0} on empty.
        PoppedChain peek() const;

        [[nodiscard]] bool empty() const { return peek().head == nullptr; }
    };

    // ─── ChainLinkage concept (extra methods over TreiberLinkageExtractor) ───
    //
    // ChainedTreiberStack's ChainLinkage parameter must additionally supply
    // chainDepth read/write and the chainNext (intra-chain) linkage.
    template <typename T, typename Extractor>
    concept ChainedTreiberChainLinkage = requires(T& node, T* p, uint32_t d) {
        // chainNext linkage (analogous to TreiberLinkageExtractor)
        { Extractor::getChainNext(node) } -> convertible_to<T*>;
        { Extractor::setChainNext(node, p) };
        // chainDepth field accessors
        { Extractor::getChainDepth(node) } -> convertible_to<uint32_t>;
        { Extractor::setChainDepth(node, d) };
    };

}  // namespace Core
```

### `TreiberStack::push` algorithm

```cpp
void push(T& node) {
    Storage old = head.load(kTreiberPushLoadOrder);  // RELAXED per DEC-042 #1
    Storage neu;
    do {
        T* oldPtr = HeadEncoding::unpackPointer(old);
        LinkageExtractor::setNext(node, oldPtr);
        auto newTag = HeadEncoding::advanceTag(HeadEncoding::unpackTag(old));
        neu = HeadEncoding::pack(&node, newTag);
    } while (!head.compare_exchange(old, neu,
                                    kTreiberPushOrder,           // RELEASE
                                    kTreiberCASFailureOrder));   // RELAXED
}
```

### `TreiberStack::pop` algorithm

```cpp
T* pop() {
    Storage old = head.load(kTreiberPopLoadOrder);  // ACQUIRE per DEC-042 #2
    while (true) {
        T* oldPtr = HeadEncoding::unpackPointer(old);
        if (oldPtr == nullptr) return nullptr;
        // Speculative read of next; CAS will reject if head has changed.
        T* nextPtr = LinkageExtractor::getNext(*oldPtr);
        auto oldTag = HeadEncoding::unpackTag(old);     // pop carries tag (DEC-015)
        Storage neu = HeadEncoding::pack(nextPtr, oldTag);
        if (head.compare_exchange(old, neu,
                                  kTreiberPopCASOrder,        // ACQUIRE
                                  kTreiberCASFailureOrder)) { // RELAXED; loop re-reads with ACQUIRE
            return oldPtr;
        }
        // On failure: re-load with ACQUIRE for the next attempt.
        old = head.load(kTreiberPopLoadOrder);
    }
}
```

### `ChainedTreiberStack::push` algorithm

```cpp
void push(T& element) {
    Storage old = head.load(kTreiberPushLoadOrder);
    uint32_t kmax = maxChainLength.load(RELAXED);
    Storage neu;
    while (true) {
        T* topPtr = HeadEncoding::unpackPointer(old);
        if (topPtr == nullptr) {
            // New singleton chain on empty stack.
            ChainLinkage::setChainNext(element, nullptr);
            ChainLinkage::setChainDepth(element, 1);
            HeadLinkage::setNext(element, nullptr);
        } else {
            // Hook fires before any read of *topPtr — vmsmalloc's hook calls
            // ensureTLBEntryFresh(topPtr) here (parent-spec DEC-040, amended).
            hooks.onPreTouch(topPtr);
            if (ChainLinkage::getChainDepth(*topPtr) >= kmax) {
                // New singleton chain (top at max length).
                ChainLinkage::setChainNext(element, nullptr);
                ChainLinkage::setChainDepth(element, 1);
                HeadLinkage::setNext(element, topPtr);
            } else {
                // Extend the existing top chain.
                ChainLinkage::setChainNext(element, topPtr);
                ChainLinkage::setChainDepth(element,
                    ChainLinkage::getChainDepth(*topPtr) + 1);
                HeadLinkage::setNext(element, HeadLinkage::getNext(*topPtr));
            }
        }
        auto newTag = HeadEncoding::advanceTag(HeadEncoding::unpackTag(old));
        neu = HeadEncoding::pack(&element, newTag);
        if (head.compare_exchange(old, neu,
                                  kTreiberPushOrder,
                                  kTreiberCASFailureOrder)) {
            return;
        }
        hooks.onCasFailure();
        // `old` already updated by compare_exchange on failure.
    }
}
```

### `ChainedTreiberStack::pushChain` algorithm

```cpp
void pushChain(T& chainHead, uint32_t depth) {
    // Caller invariants (debug-asserted):
    //   - ChainLinkage::getChainDepth(chainHead) == depth
    //   - Walking ChainLinkage::getChainNext from chainHead exactly depth-1 steps
    //     reaches a node whose getChainNext() == nullptr
    //   - depth >= 1
    // The push CAS sets chainHead->next and advances the tag. The chainNext
    // walk and chainDepth fields were published by the caller's prior writes;
    // the push CAS's RELEASE ordering propagates them.
    //
    // No `onPreTouch` hook fires here — pushChain doesn't read `*oldPtr`'s fields,
    // it only writes chainHead->next = oldPtr. The caller's chainHead is local.
    Storage old = head.load(kTreiberPushLoadOrder);
    Storage neu;
    while (true) {
        T* oldPtr = HeadEncoding::unpackPointer(old);
        HeadLinkage::setNext(chainHead, oldPtr);
        auto newTag = HeadEncoding::advanceTag(HeadEncoding::unpackTag(old));
        neu = HeadEncoding::pack(&chainHead, newTag);
        if (head.compare_exchange(old, neu,
                                  kTreiberPushOrder,
                                  kTreiberCASFailureOrder)) {
            return;
        }
        hooks.onCasFailure();
    }
}
```

### `ChainedTreiberStack::pop` algorithm

```cpp
PoppedChain pop() {
    Storage old = head.load(kTreiberPopLoadOrder);
    while (true) {
        T* topPtr = HeadEncoding::unpackPointer(old);
        if (topPtr == nullptr) {
            hooks.onEmptyStack();
            return { nullptr, 0 };
        }
        // Hook fires before any read of *topPtr — vmsmalloc's hook calls
        // ensureTLBEntryFresh(topPtr) here (parent-spec DEC-040, amended).
        // Without this call, the subsequent reads of getChainDepth(*topPtr)
        // and getNext(*topPtr) would go through this CPU's potentially-stale
        // TLB and see content from a previous physical backing if the page
        // has cycled since this CPU last touched it. The acquire-load on head
        // synchronizes value visibility through the current PTE, but does NOT
        // refresh this CPU's TLB mapping.
        hooks.onPreTouch(topPtr);
        uint32_t depth = ChainLinkage::getChainDepth(*topPtr);
        T* nextStackHead = HeadLinkage::getNext(*topPtr);
        auto oldTag = HeadEncoding::unpackTag(old);
        Storage neu = HeadEncoding::pack(nextStackHead, oldTag);
        if (head.compare_exchange(old, neu,
                                  kTreiberPopCASOrder,
                                  kTreiberCASFailureOrder)) {
            return { topPtr, depth };
        }
        hooks.onCasFailure();
        old = head.load(kTreiberPopLoadOrder);
    }
}
```

### Consumer-side write ordering obligation

A caller publishing a node via `push(node)` must complete every write to `*node`
(magic, sizeClass, chainNext, chainDepth, etc.) **before** the push call. The push
CAS is RELEASE, so those prior writes synchronize-with the next pop's ACQUIRE.
The Treiber stack itself does not publish any field of `*node` other than `next`
(in `TreiberStack`) and `next` + `chainNext` + `chainDepth` (in `ChainedTreiberStack`).

For `ChainedTreiberStack::push` specifically, the extend path overwrites `node`'s
`chainNext` / `chainDepth` / `next` fields inside the CAS loop, so any caller-written
values in those fields are clobbered. The caller may zero-initialize them or leave
them garbage; either is fine.

## Dependencies

| Dependency | Role | Must be stable first? |
|---|---|---|
| `libraries/Core/include/core/atomic.h` | `Atomic<T>` template, `MemoryOrder` enum, `compare_exchange`. Phase 4 uses `Atomic<Storage>` where `Storage` is `__uint128_t`-sized or `uint64_t`. The existing `Atomic` template supports arbitrary trivially-copyable sized types via `__atomic_*` intrinsics (see `_use_intrinsic_atomic_ops`), but currently only sizes 1/2/4/8. **Phase 4 must extend `_use_intrinsic_atomic_ops` to accept `sizeof(T) == 16`** so `Atomic<Uint128HeadEncoding::Storage>` works. | Yes — small change in `core/atomic.h`; verified during step 2. |
| `libraries/Core/include/core/utility.h` | `move`, `forward`, `tight_spin`, `conditional_t`, friends. | Yes — live. |
| `libraries/Core/include/core/TypeTraits.h` | `is_trivially_copyable_v`, `convertible_to`, `is_pointer_v`. The `TreiberHeadEncoding` concept needs them. | Yes — live. |
| `tests/harness/TestHarness.h` and `tests/TestMain.cpp` | Phase-4 unit + stress tests register through the existing harness. | Yes — live. |
| `tests/core/CMakeLists.txt` | Phase 4 adds `TreiberStackTest.cpp` to `CoreTests`'s sources and stands up a parallel `CoreTestRunnerTSan` executable that links the same sources with `-fsanitize=thread -fsanitize=undefined` instead of ASan/leak. The two binaries cannot share build artifacts (sanitizers are mutually exclusive); CMakeLists wiring per Phase 1's `LibAllocTestRunnerTSan` precedent. | Yes — live; mirror the Phase-1 TSan binary pattern. |
| C++ standard library `<thread>`, `<atomic>`, `<chrono>` for the host-side test harness | Concurrent stress tests spawn `std::thread`s. Test code only — the kernel build is unaffected. | Yes — host stdlib. |
| Apple Clang TSan / ARMv8 LSE atomics on the M1 dev machine | Default on macOS 14+ for Apple Silicon. TSan supports LDXP/STXP and CASP on ARMv8. | Yes — verify in step 1. |

**Phase ordering relative to Phases 1–3.** Phase 4 has no code dependency on Phases 1–3.
It can be implemented in parallel and merged in any order relative to them. Phase 5 (the
fast path) is the first phase that consumes both Phase 3's `Magazine` / `TreiberHead` storage
and Phase 4's stack primitives — Phase 5 cannot start until Phase 4 is in.

## Invariants

<!-- Conditions that must hold at all times in Phase 4's output. -->

- **Empty-stack representation.** A zero-initialized `Atomic<Storage>` decodes to an empty
  stack via `HeadEncoding::unpackPointer({}) == nullptr`. Verified by a per-encoding probe in
  Phase 4's test file (`Uint128HeadEncoding`'s probe is a `static_assert`; vmsmalloc's Phase-5
  policy will assert the same).
- **ABA safety (basic TreiberStack).** A `pop` CAS that succeeds against a head value `(P, T)`
  has observed that the head has not been any value other than `(P, T)` since the loop's
  `head.load`. Because `push` advances the tag on every success, "head returned to `(P, T)`
  after one or more pushes" requires the tag to have cycled — which, for `Uint128HeadEncoding`,
  takes 2⁶⁴ successful pushes. For vmsmalloc's 37-bit-tag policy in Phase 5, see parent-spec
  DEC-015's wraparound analysis.
- **ABA safety (ChainedTreiberStack).** Same reasoning; the push that extends an existing top
  chain still bumps the tag, so any "head returned to identical (P, T)" sequence still requires
  a full tag cycle.
- **Chain-head field immutability while top.** Once an element is the top of the stack's chain
  (i.e., it sits at the head pointer), its `chainNext`, `chainDepth`, and `next` fields are
  not modified by any party until the element is removed from being the top. Justification:
  - `chainNext`, `chainDepth`, and `next` are written only by `push` (consumer-side initial
    writes happen before the push call; stack-side writes happen inside the push CAS loop on
    the pushed element, never on the existing top).
  - `pop` does not write any field of the popped node; it only reads `chainDepth` and `next`.
  - A successful push that extends the top writes the new element's fields (linking new →
    old via `chainNext`); the old top's fields are untouched by that push.
  - A successful push that creates a new singleton writes the new element's fields and links
    it to the old top via `next`; the old top's fields are untouched.
  - This immutability is what makes the extend-path's speculative reads of
    `getChainNext(top)` / `getChainDepth(top)` / `getNext(top)` safe — they may see a stale
    snapshot, but the CAS on the head will reject the push if the top changed.
- **No spurious node sharing.** A node may be in at most one Treiber stack instance at a time.
  Pushing a node that is already in some stack is a consumer-side bug and is not detected by
  the primitive. (Detection would require a back-pointer field; out of scope.)
- **No-op shrink of `maxChainLength`.** Calling `setMaxChainLength(k)` where `k < current
  depth of top chain` is safe — existing oversize chains stay on the stack; only future
  pushes are bounded by the new value. No truncation, no walk-and-split.
- **Push CAS RELEASE / pop initial-load ACQUIRE are load-bearing on ARMv8.** A change that
  downgrades either to RELAXED will be caught by the TSan suite on the M1 dev machine. The
  named ordering constants make any such downgrade a one-point edit.
- **`__atomic_is_lock_free(16, nullptr) == true` on every supported target.** Static-asserted
  in `Uint128HeadEncoding`'s body. (vmsmalloc's 64-bit policy is unaffected — its lock-freedom
  is already guaranteed by `_use_intrinsic_atomic_ops` for sizeof==8.)

## Failure Modes

| Failure | Defined Behavior | Recoverable |
|---|---|---|
| Caller pushes a node already in another `TreiberStack` instance | Not detected. Caller bug; the node's `next` field is silently overwritten by the new push. | No (caller bug) |
| Caller pushes a node already in *this* stack | Not detected. The push will read `getNext(node)` indirectly (as `oldHead`), then overwrite `node.next` with `oldHead`, producing a cycle on the stack. Eventually a pop returns `node` and the freshly-popped state is corrupted. Caller bug; out of scope. | No (caller bug) |
| Caller reads/writes a popped chain's fields after returning it from `ChainedTreiberStack::pop` | Safe — the caller owns the chain exclusively after pop returns. | N/A |
| Caller calls `push` on a TreiberStack from inside a signal handler interrupting another `push` on the same stack from the same thread | Undefined. Treiber stacks are not signal-safe — the CAS loop's local state may be corrupted by re-entry. CroCOS kernel callers are protected by DEC-014 / DEC-030 (no IRQ context, no preemption mid-call). Userspace tests do not test from signal handlers. | No (caller bug) |
| `Uint128HeadEncoding` instantiated on a target where `__atomic_is_lock_free(16, …) == false` | Compile-time `static_assert` failure with a directive to supply a custom policy. | No |
| `maxChainLength == 0` set on a `ChainedTreiberStack` | Every `push(element)` goes via the "new singleton" branch; chain extension never happens. The stack degenerates to a basic Treiber stack with per-element pop. `pushChain` is unaffected (it ignores `maxChainLength`). Not a bug; possibly useful for testing. | N/A |
| `pushChain(chainHead, depth)` called with `depth == 0` | Debug-asserted (`depth >= 1`). In release, the chain is published with `chainDepth == 0`, which violates ChainedTreiberStack's "every chain on the stack has depth ≥ 1" invariant; subsequent `pop()` returns `{chainHead, 0}` and the consumer's walk loop terminates immediately, leaking the chain's remaining elements. Caller bug. | No (caller bug) |
| `pushChain(chainHead, depth)` called with `depth` mismatching the actual chain length (e.g., `depth = 5` but walking chainNext finds only 3 elements before nullptr, or 7 elements) | Not detected by the primitive. The `pop()` consumer will walk `depth - 1` chainNext steps; under-count loses elements (they remain reachable but undiscoverable until the chain is re-pushed); over-count walks past the chain bottom into uninitialized memory. Debug builds may add a chain-validation walk before the push CAS as a follow-up. | No (caller bug) |
| `pushChain` called with a `chainHead` whose chainNext walk includes a cycle | Not detected. `pop()` consumer walks `depth - 1` steps and exits regardless of cycle; if `depth` is finite the consumer terminates, but element accounting is wrong. Caller bug. | No (caller bug) |
| Concurrent reader inside a popper observes a stale `chainNext` value (e.g., a node from a prior chain incarnation) | Cannot happen: the popper exclusively owns the chain after a successful pop CAS, and the chainNext field of every chain element was published with at most RELAXED ordering but synchronized-with via the popper's ACQUIRE-load of the head (DEC-042 #3). The chain element values are stable from the popper's perspective. | N/A |
| Push extend path observes stale `chainDepth == k - 1` and pushes with depth `k`, but the actual top has changed (now depth `k - 5`) | The push CAS fails (head storage changed); the loop re-loads and recomputes. The transient wrong-depth `neu` value is never published. | N/A |
| `getNext(*topPtr)` reads a stale value during extend path | Same as above — CAS rejects, loop retries. | N/A |
| Caller pushes a node whose `next` field is uninitialized | Safe — push writes `next` inside the CAS loop, overwriting any prior content. | N/A |
| `Atomic<Storage>` not 16-byte aligned at runtime (e.g., consumer embeds the stack in a packed struct) | Undefined on AMD64 (`cmpxchg16b` requires 16-byte alignment). `alignas(16)` on `Uint128HeadEncoding::Storage` enforces it for declarations of `Atomic<Storage>` members; `TreiberStack` adds `alignas(64)` for cache-line isolation, which subsumes 16-byte alignment. Misalignment is a consumer-introduced bug (custom `HeadEncoding::Storage` without `alignas`); the implementation cannot detect it at compile time generically. | No (consumer bug) |

## Questions

<!-- Open questions for resolution during implementation. -->

| ID | Status | Blocking | Blocked by | Question | Notes |
|---|---|---|---|---|---|
| P4-ITEM-001 | Resolved → P4-DEC-009 | | | Parent-spec DEC-034 describes magazine flush as "one Treiber CAS transfers the chain" (i.e., the magazine pre-builds a K-element chain locally and pushes it as one atomic operation). Phase 4's `ChainedTreiberStack::push(element)` is per-element with internal extend-or-create semantics. Resolved by adding `pushChain(chainHead, depth)` — see P4-DEC-009. Parent-spec DEC-034 wording is consistent: magazine flush calls `pushChain`; cross-domain singleton freer calls `push(element)`. | Resolution committed 2026-05-26 (this session). |
| P4-ITEM-002 | Resolved 2026-05-27 (per-instance) | | | Should `maxChainLength` be per-`ChainedTreiberStack` instance or per-`(domain, class)` global? | Resolved: per-instance is the Phase-4 default. The Phase-10 tuning policy sets it via `setMaxChainLength` per-stack at policy-decision time. Switch to per-global only if Phase-10's design demands it. |
| P4-ITEM-003 | Resolved 2026-05-27 (keep uint64_t) | | | Should `Uint128HeadEncoding::Tag` be `uint64_t` or `uint32_t`? | Resolved: keep `uint64_t`. No hardware supports lock-free 12-byte CAS; a `uint32_t` tag would still require 16-byte storage with 4 wasted bytes. |
| P4-ITEM-004 | Resolved 2026-05-27 (no clear-on-pop) | | | Should `TreiberStack::pop` clear the popped node's `next` field? | Resolved: no. The consumer immediately reuses or frees the node. Adding the store would mask the "pushing a node already in this stack" caller-bug detection. |
| P4-ITEM-005 | Resolved 2026-05-27 (return depth as snapshot) | | | Should `ChainedTreiberStack::peek()` return the chain depth? | Resolved: return the depth; document as a snapshot subject to concurrent invalidation. Diagnostic use only. |
| P4-ITEM-006 | Resolved 2026-05-27 (moot once `_use_intrinsic_atomic_ops` extends) | | | Does Core need a `cmpxchg16b`-style helper? | Resolved: moot. The `_use_intrinsic_atomic_ops` extension to `sizeof(T) == 16` (per Phase 4 Dependencies) is a one-line change and absorbs the 16-byte-CAS plumbing. No separate helper needed. |

## Decisions

<!-- Settled decisions specific to Phase 4. -->

| ID | Certainty | Decision | Rationale |
|---|---|---|---|
| P4-DEC-001 | Settled | **`HeadEncoding` is a stateless static-method policy struct passed as a template parameter** (extractor pattern matching `AtomicIntrusiveLinkedList`'s `Extractor`). The policy supplies `Storage` and `Tag` typedefs and static `pack` / `unpackPointer` / `unpackTag` / `advanceTag` methods. No instance state; the policy is invoked as `HeadEncoding::pack(p, t)` etc. throughout the stack body. | Matches the established Core extractor convention; zero per-stack-instance overhead; the DEC-015 vmsmalloc policy's `vmsBase` constant lives at file scope in vmsmalloc and is captured by the policy's static methods. Stateful policy would force per-stack storage and a runtime-init step; not warranted. |
| P4-DEC-002 | Settled | **`ChainedTreiberStack` is a separate class (not a `TreiberStack` typedef) with a `volatile`-equivalent (`Atomic<uint32_t>`, RELAXED-ordered) `maxChainLength` member.** Its `push(element)` and `pop()` have chain-extension semantics distinct from `TreiberStack::push` / `pop`. | The chain-extension policy belongs at the stack level (not at the consumer level) per the user's clarification. Sharing implementation with `TreiberStack` via inheritance or composition is not worth the abstraction cost — the two algorithms differ in a handful of lines and code-duplicating is clearer than abstracting. |
| P4-DEC-003 | Settled | **`ChainedTreiberStack::push(element)` performs exactly one CAS that either (a) extends the top chain (if its depth < `maxChainLength`) by making `element` the new head with `chainNext = oldTop`, or (b) starts a new singleton chain (if stack empty or top at max length). Pop returns the entire top chain in one CAS as `{head, depth}`.** | Per user clarification (this session). Push CAS is per-element; pop CAS is per-chain. Amortization is pop-side only at the primitive level; producer-side amortization (e.g., per-CPU magazine caching) is the consumer's responsibility. Reconciliation with parent-spec DEC-034 is P4-ITEM-001. |
| P4-DEC-004 | Settled | **Memory ordering is pinned to named constants in the header**: `kTreiberPushOrder = RELEASE`, `kTreiberPushLoadOrder = RELAXED`, `kTreiberPopLoadOrder = ACQUIRE`, `kTreiberPopCASOrder = ACQUIRE`, `kTreiberCASFailureOrder = RELAXED`, `kChainLinkOrder = RELAXED`. Every CAS site references the constants by name with a `// DEC-042` comment. | Implements parent-spec DEC-042 #7's "one point of edit" requirement. Names map 1:1 to DEC-042 numbered items so a future reviewer can cross-reference instantly. |
| P4-DEC-005 | Settled | **Default head encoding is `Uint128HeadEncoding`**: 16-byte `Storage` (8-byte pointer + 8-byte tag), with `static_assert(__atomic_is_lock_free(16, nullptr))` guarding instantiation. `Storage` is `alignas(16)`. | Per user clarification (this session). 64-bit tag fully eliminates ABA on any realistic timescale; 16-byte CAS is supported by every CroCOS target. The static_assert produces a clear error directing consumers to supply a custom policy if their target lacks it. |
| P4-DEC-006 | Settled | **`maxChainLength` is stored as `Atomic<uint32_t>` with RELAXED ordering on every read/write.** Pushers read it once per push call; setters store it from a tuning policy thread. No fence needed — stale reads cap the chain at an old value, which is benign. | RELAXED is sufficient because there is no synchronization obligation tied to the value (it's a heuristic). Setting under SEQ_CST would impose unnecessary fences on every push. |
| P4-DEC-007 | Settled | **Phase 4 ships a parallel `CoreTestRunnerTSan` executable.** Same source files as `CoreTestRunner` but compiled and linked with `-fsanitize=thread -fsanitize=undefined` instead of `-fsanitize=address -fsanitize=leak`. Custom CMake target `run_core_tests_tsan` runs it. | Sanitizers are mutually exclusive; a parallel binary is the established pattern (cf. Phase 1's `LibAllocTestRunnerTSan`). Running both gives ASan's allocation hygiene coverage on the same suite that TSan validates for race-freedom. |
| P4-DEC-008 | Settled | **The push that extends the top chain reads the top's `chainNext` / `chainDepth` / `next` fields with RELAXED ordering inside the loop.** No fence is needed: those fields were published to this CPU via the ACQUIRE-load of `head` that returned the top's encoded pointer (the same synchronizes-with edge `pop` relies on). The CAS rejects the push if `head` has changed in the meantime, so any read of stale fields is caught. | This is the DEC-042 #3 reasoning applied to the chain-extend path. RELAXED on `chainNext`/`chainDepth` is sufficient because the cross-CPU happens-before edge comes from the ACQUIRE-load + the prior pusher's RELEASE-CAS. |
| P4-DEC-009 | Settled | **`ChainedTreiberStack::pushChain(chainHead, depth)` always publishes the input chain as a new top chain in one CAS — never as an extension of the existing top chain. `maxChainLength` is ignored on this path.** The caller is responsible for pre-linking `chainNext` through the chain bottom and setting `chainDepth = depth` on `chainHead` before the call. The push CAS sets `chainHead->next = oldTop` and advances the tag. | Matches parent-spec DEC-034 semantics verbatim ("one Treiber CAS transfers the chain — `m.head->next = oldHead; CAS(...)`") with no extension semantic in DEC-034 either. Extending would force a chain-bottom walk (O(depth)) on every flush, defeating the single-CAS amortization story. The caller has already chosen `depth` as the amortization unit (via magazine `currentK`); respect that choice. `push(element)` remains the only API that consults `maxChainLength`, used for the cross-domain singleton-free path (parent-spec DEC-019/DEC-034 cross-domain gate). Resolves P4-ITEM-001 with candidate (a). |
| P4-DEC-010 | Settled (added 2026-05-27) | **Add a `Hooks` template parameter to `ChainedTreiberStack` (and `TreiberStack` by symmetry, though Phase 5 only consumes the chained variant).** Default `NoopHooks` for Core consumers; vmsmalloc supplies a concrete Hooks struct that calls `VMSubstrate::ensureTLBEntryFresh(topPtr)` from `onPreTouch` and bumps `MagazineTuning::overflowCount` / `starvationCount` from `onCasFailure` / `onEmptyStack`. **Three hook points:** (a) `onPreTouch(topPtr)` — fires inside `pop()` (always) and inside `push(element)`'s extend branch, AFTER the head acquire-load but BEFORE any read of `*topPtr`. Required by parent-spec DEC-040 (amended for lazy-freshness) because the acquire-load synchronizes value visibility but NOT this CPU's TLB→PA mapping; a stale TLB entry would otherwise make the read of `topPtr->next` / `topPtr->chainDepth` return content from a previous physical backing. (b) `onCasFailure()` — fires inside every CAS-loop retry. Tuning policies use this as a contention signal. (c) `onEmptyStack()` — fires inside `pop()` when the head's pointer is null. Tuning policies use this as a starvation signal. **Hook instances are member-stored via `[[no_unique_address]] Hooks hooks{};`** so the default `NoopHooks` (empty struct) costs zero bytes per stack instance; vmsmalloc's stateful Hooks carries a `MagazineTuning*` (~8 B per stack). | The hook mechanism resolves three issues at once: (i) parent-spec DEC-040's eager-freshness sweep is replaced by lazy first-touch freshness, with the load-bearing pop-internal freshness call living inside Phase 4's `pop()` via `onPreTouch`; (ii) Phase 5's previously-external `starvationCount` / `overflowCount` bumps (the prior P5-DEC-007) move inside the primitive, giving a single point-of-truth for counter updates; (iii) the architectural pattern is reusable for future tuning policies and other Treiber-stack consumers. `[[no_unique_address]]` ensures zero-cost defaults for the basic Core case. The pre-touch hook is the most subtle: it is REQUIRED INSIDE pop because the read of `topPtr->next` is not just consumed locally — it's published as the new shared-stack head via the CAS, so a TLB-stale read would corrupt the structure visibly to all CPUs. The CAS itself can't paper over this; the read must precede the CAS and must be fresh. |

## Hazards

- **Push-CAS amortization depends on which API the consumer uses.** `ChainedTreiberStack`
  exposes two push paths: `push(element)` is per-element with extend-or-create (no
  per-K amortization), and `pushChain(chainHead, depth)` is one CAS per pre-built chain
  (full per-K amortization, matching parent-spec DEC-034). The vmsmalloc magazine flush
  path uses `pushChain`; the cross-domain singleton-free path uses `push(element)`.
  Misrouting (using `push` where `pushChain` would have been correct) degrades the
  shared-stack CAS rate from `≈ ops / K` to `≈ ops` without functional incorrectness;
  Phase 9's stress test should sample CAS counters per-path to catch routing regressions.
- **`Uint128HeadEncoding` 16-byte CAS is lock-free everywhere CroCOS runs, but the cost
  varies.** AMD64 `cmpxchg16b` is significantly more expensive than a single 8-byte CAS;
  ARMv8 LDXP/STXP can have higher contention failure rates than CASP (ARMv8.1+). The TSan
  stress suite on the M1 dev machine catches race regressions but not perf regressions —
  perf-sensitive consumers should supply a custom `HeadEncoding` with smaller storage (which
  is exactly what vmsmalloc's Phase-5 64-bit-packed policy does). The Core default is for
  consumers without a fixed-VA window; vmsmalloc is not a default-policy consumer.
- **The chain-extend speculative read pattern is correct but subtle.** Code reviewers might
  question whether `getChainNext(*topPtr)` is safe given that `topPtr` is read outside any
  exclusion. The Invariants section above pins the immutability argument; the in-source
  comment at the extend branch must cite the DEC-042 happens-before edge explicitly so the
  pattern is not "fixed" by adding spurious ACQUIRE fences.
- **TreiberStack and ChainedTreiberStack share no implementation.** Two CAS loops, two sets
  of memory-ordering wiring. A subsequent refactor that unifies them must preserve the
  named-ordering-constant property — adding indirection that hides the actual order at the
  CAS site would defeat DEC-042 #7's "one point of edit" goal.
- **TSan runner on Apple Clang and Linux Clang may differ in failure modes.** Apple Clang's
  TSan on ARMv8 occasionally reports false positives on `__atomic_*` 16-byte ops; CroCOS
  primary test environment is the M1 (per memory: `[[project_armv8_dev_tsan]]`). If the
  TSan runner reports a race we cannot explain, cross-check with the AMD64 Linux Clang
  TSan build before chasing a "real" race in the implementation.
- **Default `__atomic_is_lock_free(16, nullptr)` may return a wrong answer on older GCC
  versions targeting AMD64 without explicit `-mcx16`.** The kernel's compile flags
  (`amd64.cmake` adds `-mcx16` implicitly via `qemu64,+fsgsbase` only at QEMU runtime, not
  compile time). The host test build uses Apple Clang / Linux GCC with default flags — both
  enable `cmpxchg16b` for x86_64 by default since the platform's baseline ISA includes it
  for almost all modern CPUs. If a target build fails the `static_assert`, add `-mcx16`
  explicitly. The kernel itself does not use `Uint128HeadEncoding`; vmsmalloc's Phase-5
  64-bit policy is the in-kernel consumer.
- **`Atomic<__uint128_t-sized>` exercise extends the existing `core/atomic.h` template to
  size 16.** That extension affects every consumer that previously did `Atomic<T>` where
  `sizeof(T) == 16` and was rejected by the `_use_intrinsic_atomic_ops` check. No such
  consumer exists today (grep confirms during step 2), but a future regression could land
  silently. Add a Core-side regression test that constructs an `Atomic<struct
  alignas(16){uint64_t a, b;}>` and verifies the operations are intrinsic-backed.
- **Push-only counter advance is a load-bearing convention.** A `pop` that bumps the tag
  would invalidate parent-spec DEC-015's wraparound analysis (the analysis assumes pop
  carries the tag forward unchanged). The named ordering constants do not pin this — it's
  encoded in the algorithm itself. The in-source comment at the `pop` CAS site cites DEC-015
  explicitly to keep the discipline visible.

## Verification Targets

| Property | Method |
|---|---|
| `Uint128HeadEncoding` instantiates and packs/unpacks correctly | `static_assert` in test file: `pack(nullptr, 0) == Storage{}`, `unpackPointer(pack(p, t)) == p`, `unpackTag(pack(p, t)) == t`, `advanceTag(t) > t`. |
| `__atomic_is_lock_free(16, nullptr) == true` on host build target | The `static_assert` in `Uint128HeadEncoding` fires if not. |
| `TreiberStack::push` / `pop` correctly maintains LIFO order under single-thread driver | Sequential unit test: push 1000 ints, pop 1000, verify reverse-order recovery. |
| `TreiberStack::pop` of an empty stack returns nullptr | Sequential unit test. |
| `ChainedTreiberStack::pop` of an empty stack returns `{nullptr, 0}` | Sequential unit test. |
| `ChainedTreiberStack::push` with `maxChainLength = 1` produces only singleton chains | Sequential unit test: push 5, then 5 pops each return `{p, 1}`. |
| `ChainedTreiberStack::push` extends the top chain up to `maxChainLength` then starts a new one | Sequential unit test: push 10 with `maxChainLength = 4`; expect three pops of depths `{2, 4, 4}` (newest-first). |
| `ChainedTreiberStack::setMaxChainLength` changes future-push behavior | Sequential unit test: push to a depth-4 chain, drop max to 2, push again, expect a new singleton (not an extend). |
| `setMaxChainLength` to a smaller value does not truncate the existing top chain | Sequential unit test: build a 4-chain, set max to 1, peek/pop returns 4-chain. |
| `ChainedTreiberStack::pushChain` publishes the input chain as the new top in one CAS | Sequential unit test: build a 5-chain locally (link chainNext, set chainDepth), pushChain it; pop returns `{chainHead, 5}` and walking chainNext recovers the original 5 elements in order. |
| `pushChain` does NOT extend the existing top chain regardless of `maxChainLength` | Sequential unit test: push 3 elements (top chain depth 3); call `pushChain(newHead, 4)`; expect two separate chains on the stack (pop → `{newHead, 4}`, then pop → `{first-push-element, 3}`). |
| `pushChain` does NOT consult `maxChainLength` | Sequential unit test: set `maxChainLength = 2`; call `pushChain(head, 10)`; pop returns `{head, 10}` (the depth-10 chain landed unmolested). |
| `pushChain` and `push(element)` interoperate: push(element) after pushChain extends the chain pushed by pushChain | Sequential unit test: `pushChain(headA, 4)` then `push(elementB)` with `maxChainLength = 8`; pop returns `{elementB, 5}` and the walk recovers elementB → headA → (3 more from headA's chain) — total 5 elements. |
| Concurrent push/pop matches a serialized reference under K producers / K consumers | TSan stress test: 4 producer threads push 10K each; 4 consumer threads pop until total == 40K; verify multiset of popped values == multiset of pushed values. |
| TSan reports zero races for concurrent push/pop | `CoreTestRunnerTSan` exit code 0. |
| ABA-safety stress: a rapid push-pop-push of the same node from one thread, with another thread popping in between, does not corrupt the stack | TSan stress test mirroring the classic Treiber ABA scenario; use `Uint128HeadEncoding` with the 64-bit tag making any "tag returned" essentially impossible — the test verifies the no-corruption property, not the tag-wrap behavior. |
| Memory-ordering constants are referenced by name (not by literal `RELEASE` / `ACQUIRE`) at every CAS site | Grep audit during code review: `grep -n 'memory_order\|RELAXED\|ACQUIRE\|RELEASE' libraries/Core/include/core/atomic/TreiberStack.h` — every match should be a constant definition or a reference to a constant, not a literal at a CAS call site. |
| Existing `CoreTestRunner` (ASan / leak) continues to pass after Phase 4 lands | `cmake --build build --target run_core_tests` exit 0. |
| `Atomic<T>` for `sizeof(T) == 16` produces intrinsic-backed code (not the static_assert fallback) | Add a static_assert in test file: `_use_intrinsic_atomic_ops<Uint128HeadEncoding::Storage>`. |

## Testing Approach

- **Unit tests** (sequential, single-threaded) cover the contract surface: push/pop ordering,
  empty-stack handling, `maxChainLength` extend-vs-new logic, `setMaxChainLength` mutation
  semantics, peek behavior. These run in both `CoreTestRunner` (ASan) and `CoreTestRunnerTSan`
  (TSan) — identical results expected.
- **Concurrent stress tests** spawn `std::thread` workers. Three test families:
  1. **Producer/consumer balance.** Multiple producers push distinct integers; multiple
     consumers pop until empty. Verify the multiset of popped values equals the multiset of
     pushed values. Covers basic LIFO+chain-extend correctness under contention.
  2. **DEC-042 ordering probe.** Each pushed node carries a payload that the consumer reads
     immediately after pop. If pop's ACQUIRE-load is downgraded to RELAXED, TSan reports a
     race on the payload field on ARMv8 (test fails). Pinned by the named ordering constants;
     this is the regression sentinel.
  3. **Chain-extend stress.** `ChainedTreiberStack` with `maxChainLength = 16`; producers
     push at high rate; consumers pop chains and verify chain integrity (chainDepth matches
     walk length, chainNext terminates at nullptr at exactly the right depth).
  4. **Mixed push / pushChain stress.** `ChainedTreiberStack`; some producers call
     `push(element)` with `maxChainLength = 8`, others build local 8-chains and call
     `pushChain(head, 8)`. Consumers pop chains and verify (a) every popped element
     accounted for, (b) chain depths consistent with a chainNext walk, (c) no element
     appears in two popped chains.
- **TSan-on-ARMv8 release gate.** Per `[[project_armv8_dev_tsan]]`, TSan-clean on the M1 dev
  machine is required to merge. The TSan runner is run via `cmake --build build --target
  run_core_tests_tsan`.
- **No QEMU / in-kernel test.** Phase 4 ships userspace primitives only; in-kernel exposure
  comes in Phase 5 and is tested via Phase 8's vmsmalloc integration harness.

## Implementation Phases

<!-- Concrete ordered steps for Phase 4 itself. -->

1. **Confirm starting state.**
   - `libraries/Core/include/core/atomic.h` has `Atomic<T>`, `MemoryOrder` enum, the named
     ordering values `RELEASE` / `ACQUIRE` / `RELAXED` / `ACQ_REL` / `SEQ_CST`, and
     `compare_exchange` accepting both success and failure orderings.
   - `_use_intrinsic_atomic_ops` currently restricts to sizeof ∈ {1, 2, 4, 8}.
   - The Apple Clang toolchain on the M1 dev machine has TSan support; verify via
     `clang -fsanitize=thread -E -x c++ - < /dev/null > /dev/null` exits 0.

2. **Extend `_use_intrinsic_atomic_ops` to accept `sizeof(T) == 16`.**
   - Edit `libraries/Core/include/core/atomic.h:26` to OR in `(sizeof(T) == 16)`.
   - Audit that no other call site relies on the restriction. (grep for `_use_intrinsic_atomic_ops`
     across the tree — expected zero hits outside `core/atomic.h` itself.)
   - Add a static_assert in `tests/core/AtomicSizeTest.cpp` (new tiny file): construct an
     `Atomic<struct alignas(16){uint64_t a, b;}>`, do a `store` / `load` / `compare_exchange`,
     verify `__atomic_is_lock_free(16, nullptr)` is true on the host. (Or fold this into the
     new `TreiberStackTest.cpp`.)

3. **Create `libraries/Core/include/core/atomic/TreiberStack.h`.**
   - File header comment block citing parent-spec DEC-002 / DEC-015 / DEC-034 / DEC-041 /
     DEC-042 (mirroring the documentation style of `AtomicLinkedList.h`).
   - Concepts `TreiberLinkageExtractor`, `TreiberHeadEncoding`, `ChainedTreiberChainLinkage`.
   - `Uint128HeadEncoding<T>` with `Storage` struct (alignas 16), `Tag = uint64_t`, static
     `pack` / `unpackPointer` / `unpackTag` / `advanceTag`, plus the lock-free `static_assert`.
   - Memory-ordering constants `kTreiberPushOrder` / `kTreiberPushLoadOrder` /
     `kTreiberPopLoadOrder` / `kTreiberPopCASOrder` / `kTreiberCASFailureOrder` /
     `kChainLinkOrder` per P4-DEC-004, each annotated `// DEC-042 #N`.
   - `TreiberStack<T, LinkageExtractor, HeadEncoding>` class per the Consumer Contract above.
     Implement `push`, `pop`, `peek`, `empty`. Use the named ordering constants at every CAS site.
   - `ChainedTreiberStack<T, HeadLinkage, ChainLinkage, HeadEncoding>` class per the Consumer
     Contract. Implement `push`, `pushChain`, `pop`, `setMaxChainLength`, `getMaxChainLength`,
     `peek`, `empty`. Use the named ordering constants. `pushChain` debug-asserts
     `depth >= 1` and `ChainLinkage::getChainDepth(chainHead) == depth` on entry; the
     chain-walk validation (counting chainNext steps) is left as an optional Phase-4
     debug-only addition (P4-DEC-009 hazards table flags caller bugs as undetected).
   - In-source comments at every CAS site citing DEC-042 by item number.

4. **Add `tests/core/TreiberStackTest.cpp`.**
   - Standard `#include "../test.h"` + `<harness/TestHarness.h>` per CLAUDE.md test convention.
   - `static_assert` block verifying `Uint128HeadEncoding`'s contract (pack/unpack roundtrip,
     zero-init empty, `__atomic_is_lock_free(16, …)`).
   - A throwaway test node type `struct TestNode { TestNode* next; TestNode* chainNext;
     uint32_t chainDepth; int payload; };` plus `TestNodeLinkage` / `TestNodeChainLinkage`
     extractors that read/write those fields.
   - Sequential tests per the Verification Targets table.
   - Concurrent stress tests using `std::thread` per the Testing Approach section. Use a
     `Core::Vector<TestNode>` or similar to own the nodes' storage so the stack just stores
     pointers into a stable backing array.

5. **Update `tests/core/CMakeLists.txt` to register the new test file and TSan runner.**
   - Add `TreiberStackTest.cpp` (and optionally `AtomicSizeTest.cpp`) to `CoreTests`'s
     `target_sources`.
   - Define a parallel library `CoreTestsTSan` with the same source files but compiled with
     `-fsanitize=thread -fsanitize=undefined` (drop `-fsanitize=address -fsanitize=leak`).
   - Define `CoreTestRunnerTSan` executable mirroring `CoreTestRunner` but linking
     `CoreTestsTSan` and the matching TestHarness build, with TSan link flags.
   - Add `run_core_tests_tsan` custom target.
   - Mirror Phase 1's `LibAllocTestRunnerTSan` pattern. Verify the two binaries' object files
     land in separate build subdirectories so the sanitizer link flags don't cross-contaminate.

6. **Build and run tests.**
   - `cmake --build build --target run_core_tests` succeeds (ASan).
   - `cmake --build build --target run_core_tests_tsan` succeeds (TSan).
   - No new warnings in either build.
   - The DEC-042 ordering probe test passes on the M1 dev machine. (If running on a Linux x86
     workstation only, the test passes but does not exercise the ARMv8-only failure mode;
     note in the test runner log that the gate is met only when run on ARMv8.)

7. **Audit and document.**
   - Grep `libraries/Core/include/core/atomic/TreiberStack.h` for raw `RELEASE` / `ACQUIRE` /
     `RELAXED` outside the named-constant definitions; refactor any stragglers to use the
     constants.
   - Verify the in-source DEC-042 #N comments map 1:1 to the parent-spec DEC-042 items they
     reference.
   - Update `[[project_slab_abstraction_plan]]` in memory: Phase 4 status → "drafted /
     implemented" as appropriate.

8. **Optional follow-up (under user latitude).**
   - Add a debug-only chain-walk validator that `pushChain` runs at entry to verify
     `chainDepth` matches the actual chainNext walk length and the bottom's chainNext
     is `nullptr`. Cost: O(depth) per push in debug; zero in release. Closes the
     "caller bug undetected" rows in Failure Modes for `pushChain`.
   - Add a `drainAll()` helper that pops every chain and returns them as a `Core::Vector` of
     `PoppedChain` snapshots, for Phase 11's `scavenge()`.
   - Add coarse / full statistics templates analogous to `AtomicIntrusiveLinkedList`'s
     `StatsType` if the Phase-9 stress harness needs CAS-contention visibility; track
     `push` vs `pushChain` CAS counts separately so the Hazards-section "misrouting"
     regression is observable.

## References

- `libraries/Core/include/core/atomic.h` — `Atomic<T>`, `MemoryOrder`, `compare_exchange`.
  Phase 4 extends `_use_intrinsic_atomic_ops` to size 16.
- `libraries/Core/include/core/atomic/AtomicLinkedList.h` — style precedent (concept-based
  extractor, named memory-ordering constants, statistics policy template parameter).
- `libraries/Core/include/core/atomic/SplitBitmap.h` — style precedent for storage policy
  parameters and inline / external storage variants.
- `libraries/Core/include/core/utility.h` — `move`, `forward`, `conditional_t`, friends.
- `libraries/Core/include/core/TypeTraits.h` — `is_trivially_copyable_v`, `convertible_to`,
  `is_pointer_v`. Concept satisfaction.
- `tests/core/CMakeLists.txt` — pattern for adding new test files; Phase 4 mirrors and adds
  a parallel TSan runner.
- `tests/core/AtomicLinkedListTest.cpp`, `tests/core/AtomicBitPoolTest.cpp` — style precedents
  for concurrent stress tests with `std::thread`.
- `tests/harness/TestHarness.h` — test registration macros and `EXPECT_*` family.
- Parent spec `specs/vmsmalloc.md`:
  - DEC-002 (refined by DEC-034) — per-NUMA Treiber stack, lazy reclamation, ABA via tag.
  - DEC-015 — packed-tagged-head encoding (vmsmalloc's HeadEncoding policy, supplied in
    Phase 5; the abstract `HeadEncoding` policy shape comes from this).
  - DEC-034 (amended by DEC-041) — chained-transfer magazines; parent-spec "one CAS transfers
    the chain" semantic that P4-ITEM-001 reconciles against Phase 4's per-element push.
  - DEC-041 — head-linkage (chain head, not bottom, carries shared-stack `next`); drop
    `m.tail`. Phase 4's chain-extend invariants reflect this.
  - DEC-042 — memory-ordering policy. Phase 4 implements items #1, #2, #3, #7 verbatim;
    items #4 (bookkeeper) and #5 (tuning) are out of scope.
- Phase 1 spec `specs/vmsmalloc-phase-1.md` — `LibAllocTestRunnerTSan` precedent for the
  parallel TSan binary pattern.
- Phase 3 spec `specs/vmsmalloc-phase-3.md` — `TreiberHead` storage type (per-`(domain,
  class)`) that Phase-5 vmsmalloc will hand to ChainedTreiberStack as its head storage.
- Memory: `[[project_armv8_dev_tsan]]` — ARMv8 + TSan as the default release gate.
- Memory: `[[project_slab_abstraction_plan]]` — phase plan; updated on Phase 4 completion.
