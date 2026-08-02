//
// Created by Spencer Martin on 8/23/25.
//
#include "include/panic.h"

extern "C" void *__dso_handle;
void *__dso_handle = nullptr;

extern "C" int __cxa_atexit(void (*destructor) (void *), void *arg, void *___dso_handle){
    (void)destructor;
    (void)arg;
    (void)___dso_handle;
    return 0;
}

// Static local initialization guards (Itanium C++ ABI)
// Guard layout: byte 0 = initialized flag, byte 1 = spinlock
extern "C" {
    int __cxa_guard_acquire(uint64_t* guard) {
        auto* done = reinterpret_cast<uint8_t*>(guard);
        auto* lock = done + 1;

        if (__atomic_load_n(done, __ATOMIC_ACQUIRE))
            return 0;

        while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(done, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
                return 0;
            }
        }

        if (__atomic_load_n(done, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
            return 0;
        }

        return 1;
    }

    void __cxa_guard_release(uint64_t* guard) {
        auto* done = reinterpret_cast<uint8_t*>(guard);
        auto* lock = done + 1;
        __atomic_store_n(done, 1, __ATOMIC_RELEASE);
        __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    }

    void __cxa_guard_abort(uint64_t* guard) {
        auto* lock = reinterpret_cast<uint8_t*>(guard) + 1;
        __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
    }
}

// Stack smashing protection
// The canary value contains bytes that commonly terminate strings (null, newline, 0xFF)
// to help detect string-based buffer overflows
//
// `used` is REQUIRED under LTO and is not belt-and-braces. -fstack-protector's
// references to these two are emitted during CODEGEN, which under LTO happens
// at link time — after the IPA pass that decides what to keep. At IR level
// nothing refers to either symbol, so LTO discards both and the link then fails
// with `undefined reference to __stack_chk_guard` from every protected
// function. Measured, not theorised: that is exactly how the Release build
// failed when -flto was first added to the compile flags.
extern "C" {
    __attribute__((used)) uintptr_t __stack_chk_guard = 0x595e9fbd94fda766;

    __attribute__((used)) [[noreturn]] void __stack_chk_fail(){
        PANIC_NO_STACKTRACE("Stack smashing detected!");
    }
}