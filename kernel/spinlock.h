#pragma once
#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_acquire(spinlock_t* lock)
{
    while (__sync_lock_test_and_set(&lock->locked, 1))
        __asm__ volatile ("pause");
}

static inline void spinlock_release(spinlock_t* lock)
{
    __sync_lock_release(&lock->locked);
}