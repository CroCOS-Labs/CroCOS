//
// Created by Spencer Martin on 2/15/25.
//

#include <arch.h>
#include <CpuLocal.h>
#include <mem/PageAllocator.h>

#ifdef __x86_64__
#include "arch/amd64/amd64.h"
#endif
extern size_t archProcessorCount;

#if defined(SUPPORTS_SPINLOCK_DEADLOCK_DETECTION) && defined(DEBUG_BUILD)
#define USE_SPINLOCK_DEADLOCK_DETECTION
#endif

namespace arch{
#ifdef CROCOS_TESTING
    Atomic<bool> dummyInterruptFlag;

    void enableInterrupts() {
        dummyInterruptFlag = true;
    }

    void disableInterrupts() {
        dummyInterruptFlag = false;
    }

    bool areInterruptsEnabled() {
        return dummyInterruptFlag;
    }
#endif

#ifndef CROCOS_TESTING
    void serialOutputString(const char *str) {
#ifdef __x86_64__
        amd64::serialOutputString(str);
#endif
    }

    size_t processorCount(){
        return archProcessorCount;
    }

    // Architecture-agnostic: every supported arch wires the current CPU's
    // per-CPU base register at very early boot (BSP via initBspCpuLocal in
    // CpuLocal.cpp; APs via initApCpuLocal during their ap_routine), so
    // kernel::getLogicalProcessorID() reads the right value without an
    // arch dispatch here.
    ProcessorID getCurrentProcessorID(){
        return kernel::getLogicalProcessorID();
    }

    void SerialPrintStream::putString(const char * str){
        serialOutputString(str);
    }
#endif
    InterruptDisabler::InterruptDisabler() {
        active = true;
        wasEnabled = areInterruptsEnabled();
        // Record the prior state BEFORE masking, then actually mask. The
        // disableInterrupts() call was missing until 2026-08-01, which made
        // this a save/restore helper that disabled nothing — every call site
        // (ClockManager::compareTimerTicks, the TimerQueues mutation paths)
        // ran its critical section with interrupts live.
        disableInterrupts();
    }

    void InterruptDisabler::release() {
        if (active) {
            active = false;
            if (wasEnabled) {
                enableInterrupts();
            }
            else {
                disableInterrupts();
            }
        }
    }

    InterruptDisabler::~InterruptDisabler() {
        release();
    }

    void InterruptResetter::operator()() {
        if (state == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (state == InterruptState::DISABLED) {
            disableInterrupts();
        }
        state = InterruptState::STALE;
    }
#ifndef CROCOS_TESTING
    IteratorRange<MemMapIterator> getMemoryMap() {
#ifdef ARCH_AMD64
        return amd64::getMemoryMap();
#endif
    }
#endif

    InterruptDisablingSpinlock::InterruptDisablingSpinlock(){
        state = InterruptState::STALE;
        acquired = false;
        metadata = 0;
    }

    constexpr size_t activeMeta = 1ul << 63;

    void InterruptDisablingSpinlock::acquire() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        if (newState == InterruptState::ENABLED) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            const auto meta = metadata.load(ACQUIRE);
            if (meta & activeMeta) {
                assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
            }
#endif
            disableInterrupts();
            while (!acquired.compare_exchange_v(false, true, ACQUIRE)) {
                enableInterrupts();
                tight_spin();
                disableInterrupts();
            }
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        }
        else {
            acquirePlain();
        }
        state = newState;
    }

    void InterruptDisablingSpinlock::acquirePlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        const auto meta = metadata.load(ACQUIRE);
        if (meta & activeMeta) {
            assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
        }
#endif
        while (!acquired.compare_exchange_v(false, true, ACQUIRE)) {
            tight_spin();
        }
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        state = InterruptState::STALE;
    }

    bool InterruptDisablingSpinlock::tryAcquire() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        disableInterrupts();
        if (!acquired.compare_exchange_v(false, true, ACQUIRE)) {
            //If we fail to acquire the lock and interrupts were originally enabled, reenable them
            if (newState == InterruptState::ENABLED) enableInterrupts();
            return false;
        }
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return true;
    }

    bool InterruptDisablingSpinlock::tryAcquirePlain() {
        if (acquired.compare_exchange_v(false, true, ACQUIRE)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
            state = InterruptState::STALE;
            return true;
        }
        return false;
    }

    void InterruptDisablingSpinlock::release() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        auto oldState = state;
        state = InterruptState::STALE;
        acquired.store(false, RELEASE);

        if (oldState == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (oldState == InterruptState::DISABLED) {
            disableInterrupts();
        }
    }

    InterruptResetter InterruptDisablingSpinlock::releasePlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        acquired.store(false, RELEASE);
        auto oldState = state;
        state = InterruptState::STALE;
        return InterruptResetter{oldState};
    }

    bool InterruptDisablingSpinlock::lock_taken() const {
        return acquired.load(ACQUIRE);
    }

    // ==================== InterruptDisablingRWSpinlock ====================

    InterruptDisablingRWSpinlock::InterruptDisablingRWSpinlock() {
        lockstate = 0;
        metadata = 0;
    }

    const uint64_t rw_write_lock_queued_bit = 1 << 1;
    const uint64_t rw_write_lock_acquired_bit = 1 << 0;
    const uint64_t rw_write_lock_mask = rw_write_lock_acquired_bit | rw_write_lock_queued_bit;
    const uint64_t rw_read_lock_count_shift = 2;

    InterruptDisablingRWSpinlock::ReaderLockGuard InterruptDisablingRWSpinlock::acquireReader() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        if (newState == InterruptState::ENABLED) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            const auto meta = metadata.load(ACQUIRE);
            if (meta & activeMeta) {
                assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
            }
#endif
            disableInterrupts();
        }
        lockstate.update_and_get_when([](uint64_t state){
            return (state & rw_write_lock_mask) == 0;
        }, [](uint64_t state){
            uint64_t count = state >> rw_read_lock_count_shift;
            return (count + 1) << rw_read_lock_count_shift;
        });

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return {newState};
    }

    void InterruptDisablingRWSpinlock::acquireReaderPlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        const auto meta = metadata.load(ACQUIRE);
        if (meta & activeMeta) {
            assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
        }
#endif
        lockstate.update_and_get_when([](uint64_t state){
            return (state & rw_write_lock_mask) == 0;
        }, [](uint64_t state){
            uint64_t count = state >> rw_read_lock_count_shift;
            return (count + 1) << rw_read_lock_count_shift;
        });

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
    }

    Optional<InterruptDisablingRWSpinlock::ReaderLockGuard> InterruptDisablingRWSpinlock::tryAcquireReader() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        disableInterrupts();

        uint64_t count = lockstate >> rw_read_lock_count_shift;
        uint64_t newCount = count + 1;
        uint64_t expectedValue = count << rw_read_lock_count_shift;

        if (!lockstate.compare_exchange(expectedValue, newCount << rw_read_lock_count_shift)) {
            if (newState == InterruptState::ENABLED) enableInterrupts();
            return {};
        }

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return {{newState}};
    }

    bool InterruptDisablingRWSpinlock::tryAcquireReaderPlain() {
        uint64_t count = lockstate >> rw_read_lock_count_shift;
        uint64_t newCount = count + 1;
        uint64_t expectedValue = count << rw_read_lock_count_shift;

        if (!lockstate.compare_exchange(expectedValue, newCount << rw_read_lock_count_shift)) {
            return false;
        }

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return true;
    }

    void InterruptDisablingRWSpinlock::releaseReader(const ReaderLockGuard& guard) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif

        // Release the lock
        lockstate.update_and_get([](uint64_t state){
            uint64_t writerState = state & rw_write_lock_mask;
            uint64_t readCount = state >> rw_read_lock_count_shift;
            return ((readCount - 1) << rw_read_lock_count_shift) | writerState;
        });

        // Restore interrupt state from the guard
        if (guard.state == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (guard.state == InterruptState::DISABLED) {
            disableInterrupts();
        }
    }

    InterruptDisablingRWSpinlock::InterruptResetter InterruptDisablingRWSpinlock::releaseReaderPlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif

        lockstate.update_and_get([](uint64_t state){
            uint64_t writerState = state & rw_write_lock_mask;
            uint64_t readCount = state >> rw_read_lock_count_shift;
            return ((readCount - 1) << rw_read_lock_count_shift) | writerState;
        });

        return {InterruptState::STALE};
    }

    InterruptDisablingRWSpinlock::WriterLockGuard InterruptDisablingRWSpinlock::acquireWriter() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        if (newState == InterruptState::ENABLED) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            const auto meta = metadata.load(ACQUIRE);
            if (meta & activeMeta) {
                assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
            }
#endif
            disableInterrupts();
        }

        // Signal intent to acquire writer lock
        lockstate.update_and_get_when([](uint64_t state){
            return (state & rw_write_lock_queued_bit) == 0;
        }, [](uint64_t state){
            return state | rw_write_lock_queued_bit;
        });

        // Wait for all readers to release
        lockstate.update_and_get_when([](uint64_t state){
            return state == rw_write_lock_queued_bit;
        }, [](uint64_t state){
            (void)state;
            return rw_write_lock_acquired_bit;
        });

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return {newState};
    }

    void InterruptDisablingRWSpinlock::acquireWriterPlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        const auto meta = metadata.load(ACQUIRE);
        if (meta & activeMeta) {
            assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
        }
#endif

        lockstate.update_and_get_when([](uint64_t state){
            return (state & rw_write_lock_queued_bit) == 0;
        }, [](uint64_t state){
            return state | rw_write_lock_queued_bit;
        });

        lockstate.update_and_get_when([](uint64_t state){
            return state == rw_write_lock_queued_bit;
        }, [](uint64_t state){
            (void)state;
            return rw_write_lock_acquired_bit;
        });

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
    }

    Optional<InterruptDisablingRWSpinlock::WriterLockGuard> InterruptDisablingRWSpinlock::tryAcquireWriter() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        disableInterrupts();

        if (!lockstate.compare_exchange_v(0, 1)) {
            if (newState == InterruptState::ENABLED) enableInterrupts();
            return {};
        }

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return {{newState}};
    }

    bool InterruptDisablingRWSpinlock::tryAcquireWriterPlain() {
        if (!lockstate.compare_exchange_v(0, 1)) {
            return false;
        }

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        return true;
    }

    void InterruptDisablingRWSpinlock::releaseWriter(const WriterLockGuard& guard) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        lockstate &= ~rw_write_lock_acquired_bit;

        if (guard.state == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (guard.state == InterruptState::DISABLED) {
            disableInterrupts();
        }
    }

    InterruptDisablingRWSpinlock::InterruptResetter InterruptDisablingRWSpinlock::releaseWriterPlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        lockstate &= ~rw_write_lock_acquired_bit;

        return {InterruptState::STALE};
    }

    bool InterruptDisablingRWSpinlock::writerLockTaken() const {
        return lockstate & rw_write_lock_acquired_bit;
    }

    bool InterruptDisablingRWSpinlock::readerLockTaken() const {
        return (lockstate & ~rw_write_lock_mask) != 0;
    }

    // ==================== InterruptDisablingPrioritySpinlock ====================

    InterruptDisablingPrioritySpinlock::InterruptDisablingPrioritySpinlock() {
        lockstate = 0;
        metadata = 0;
        priorityState = InterruptState::STALE;
        normalState = InterruptState::STALE;
    }

    // Lock state bits:
    // Bit 0: reader acquired (at most one reader)
    // Bit 1: writer queued
    // Bit 2: writer acquired
    const uint8_t priority_reader_acquired_bit = 1 << 0;
    const uint8_t priority_writer_queued_bit = 1 << 1;
    const uint8_t priority_writer_acquired_bit = 1 << 2;
    const uint8_t priority_writer_mask = priority_writer_queued_bit | priority_writer_acquired_bit;

    void InterruptDisablingPrioritySpinlock::acquirePriority() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        if (newState == InterruptState::ENABLED) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            const auto meta = metadata.load(ACQUIRE);
            if (meta & activeMeta) {
                assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
            }
#endif
            disableInterrupts();
        }

        // Wait while writer is queued or acquired (writer priority)
        while (true) {
            uint8_t state = lockstate.load(ACQUIRE);
            if ((state & priority_writer_mask) == 0 && (state & priority_reader_acquired_bit) == 0) {
                if (lockstate.compare_exchange_v(state, state | priority_reader_acquired_bit, ACQUIRE)) {
                    break;
                }
            }
            tight_spin();
        }

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        priorityState = newState;
    }

    void InterruptDisablingPrioritySpinlock::acquirePriorityPlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        const auto meta = metadata.load(ACQUIRE);
        if (meta & activeMeta) {
            assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
        }
#endif

        while (true) {
            uint8_t state = lockstate.load(ACQUIRE);
            if ((state & priority_writer_mask) == 0 && (state & priority_reader_acquired_bit) == 0) {
                if (lockstate.compare_exchange_v(state, state | priority_reader_acquired_bit, ACQUIRE)) {
                    break;
                }
            }
            tight_spin();
        }

        priorityState = InterruptState::STALE;
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
    }

    bool InterruptDisablingPrioritySpinlock::tryAcquirePriority() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        disableInterrupts();

        uint8_t state = lockstate.load(ACQUIRE);
        if ((state & priority_writer_mask) == 0 && (state & priority_reader_acquired_bit) == 0) {
            if (lockstate.compare_exchange_v(state, state | priority_reader_acquired_bit, ACQUIRE)) {
                priorityState = newState;
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
                metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
                return true;
            }
        }

        if (newState == InterruptState::ENABLED) enableInterrupts();
        return false;
    }

    bool InterruptDisablingPrioritySpinlock::tryAcquirePriorityPlain() {
        uint8_t state = lockstate.load(ACQUIRE);
        if ((state & priority_writer_mask) == 0 && (state & priority_reader_acquired_bit) == 0) {
            if (lockstate.compare_exchange_v(state, state | priority_reader_acquired_bit, ACQUIRE)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
                metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
                return true;
            }
        }
        return false;
    }

    void InterruptDisablingPrioritySpinlock::releasePriority() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        auto oldReaderState = priorityState;
        priorityState = InterruptState::STALE;
        lockstate &= ~priority_reader_acquired_bit;

        if (oldReaderState == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (oldReaderState == InterruptState::DISABLED) {
            disableInterrupts();
        }
    }

    InterruptResetter InterruptDisablingPrioritySpinlock::releasePriorityPlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        auto oldReaderState = priorityState;
        priorityState = InterruptState::STALE;
        lockstate &= ~priority_reader_acquired_bit;
        return InterruptResetter{oldReaderState};
    }

    void InterruptDisablingPrioritySpinlock::acquire() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        if (newState == InterruptState::ENABLED) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
            const auto meta = metadata.load(ACQUIRE);
            if (meta & activeMeta) {
                assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
            }
#endif
            disableInterrupts();
        }

        // Signal intent to acquire writer lock
        while (true) {
            uint8_t state = lockstate.load(ACQUIRE);
            if ((state & priority_writer_queued_bit) == 0) {
                if (lockstate.compare_exchange_v(state, state | priority_writer_queued_bit, ACQUIRE)) {
                    break;
                }
            }
            tight_spin();
        }

        // Wait for reader to release
        while ((lockstate.load(ACQUIRE) & priority_reader_acquired_bit) != 0) {
            tight_spin();
        }

        // Acquire the writer lock (set acquired bit, clear queued bit)
        lockstate |= priority_writer_acquired_bit;
        lockstate &= ~priority_writer_queued_bit;

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
        // Save state to writerState instead of returning guard
        normalState = newState;
    }

    void InterruptDisablingPrioritySpinlock::acquirePlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        const auto meta = metadata.load(ACQUIRE);
        if (meta & activeMeta) {
            assert ((meta & 0xff) != debugEarlyBootCPUID(), "Deadlock detected!");
        }
#endif

        // Signal intent to acquire writer lock
        while (true) {
            uint8_t state = lockstate.load(ACQUIRE);
            if ((state & priority_writer_queued_bit) == 0) {
                if (lockstate.compare_exchange_v(state, state | priority_writer_queued_bit, ACQUIRE)) {
                    break;
                }
            }
            tight_spin();
        }

        // Wait for reader to release
        while ((lockstate.load(ACQUIRE) & priority_reader_acquired_bit) != 0) {
            tight_spin();
        }

        // Acquire the writer lock (set acquired bit, clear queued bit)
        lockstate |= priority_writer_acquired_bit;
        lockstate &= ~priority_writer_queued_bit;

#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
    }

    bool InterruptDisablingPrioritySpinlock::tryAcquire() {
        const auto newState = areInterruptsEnabled() ? InterruptState::ENABLED : InterruptState::DISABLED;
        disableInterrupts();

        uint8_t state = lockstate.load(ACQUIRE);
        if ((state & priority_writer_mask) == 0 && (state & priority_reader_acquired_bit) == 0) {
            if (lockstate.compare_exchange_v(state, state | priority_writer_acquired_bit, ACQUIRE)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
                metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
                // Save state to writerState on success
                normalState = newState;
                return true;
            }
        }

        if (newState == InterruptState::ENABLED) enableInterrupts();
        return false;
    }

    bool InterruptDisablingPrioritySpinlock::tryAcquirePlain() {
        uint8_t state = lockstate.load(ACQUIRE);
        if ((state & priority_writer_mask) == 0 && (state & priority_reader_acquired_bit) == 0) {
            if (lockstate.compare_exchange_v(state, state | priority_writer_acquired_bit, ACQUIRE)) {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
                metadata.store(debugEarlyBootCPUID() | activeMeta);
#endif
                return true;
            }
        }
        return false;
    }

    void InterruptDisablingPrioritySpinlock::release() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        lockstate &= ~priority_writer_acquired_bit;

        // Restore interrupts from the saved writerState
        if (normalState == InterruptState::ENABLED) {
            enableInterrupts();
        }
        if (normalState == InterruptState::DISABLED) {
            disableInterrupts();
        }
        normalState = InterruptState::STALE;
    }

    arch::InterruptResetter InterruptDisablingPrioritySpinlock::releasePlain() {
#ifdef USE_SPINLOCK_DEADLOCK_DETECTION
        metadata.store(0);
#endif
        auto oldWriterState = normalState;
        normalState = InterruptState::STALE;
        lockstate &= ~priority_writer_acquired_bit;
        return InterruptResetter{oldWriterState};
    }

    bool InterruptDisablingPrioritySpinlock::priorityLockTaken() const {
        return lockstate & priority_writer_acquired_bit;
    }

    bool InterruptDisablingPrioritySpinlock::normalLockTaken() const {
        return lockstate & priority_reader_acquired_bit;
    }

    bool InterruptDisablingPrioritySpinlock::lockTaken() const {
        return (lockstate & (priority_writer_acquired_bit | priority_reader_acquired_bit)) != 0;
    }

    bool InterruptDisablingRWSpinlock::lockTaken() const {
        return readerLockTaken() || writerLockTaken();
    }


}
