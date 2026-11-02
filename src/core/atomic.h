#pragma once

#include <atomic>

#include <cstdint>
#include <type_traits>
#include <coreinit/atomic.h>

// Espresso needs a cache workaround for exclusive accesses; Cafe OS implements it in its atomic primitives.
// https://fail0verflow.com/blog/2014/console-hacking-2013-omake/
// Keep compiler-generated loads/stores, but use the OS for read-modify-write operations.
template <typename T> class Atomic32 {
    static_assert(std::is_integral<T>::value && sizeof(T) <= 4, "Atomic32 requires an integral value up to 32 bits");
    alignas(4) uint32_t value = 0;

public:
    Atomic32() = default;
    constexpr Atomic32(T initial): value(static_cast<uint32_t>(initial)) {}
    Atomic32(const Atomic32&) = delete;
    Atomic32 &operator=(const Atomic32&) = delete;

    T load() const {
        return static_cast<T>(__atomic_load_n(&value, __ATOMIC_SEQ_CST));
    }

    void store(T next) {
        __atomic_store_n(&value, static_cast<uint32_t>(next), __ATOMIC_SEQ_CST);
    }

    T exchange(T next) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        uint32_t previous = OSSwapAtomic(&value, static_cast<uint32_t>(next));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return static_cast<T>(previous);
    }

    bool compare_exchange_strong(T &expected, T next) {
        uint32_t previous;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        bool changed = OSCompareAndSwapAtomicEx(&value, static_cast<uint32_t>(expected),
            static_cast<uint32_t>(next), &previous);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!changed) expected = static_cast<T>(previous);
        return changed;
    }

    bool compare_exchange_weak(T &expected, T next) {
        return compare_exchange_strong(expected, next);
    }
};
