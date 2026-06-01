#pragma once
#include <stdint.h>
#include <stddef.h>

// Initialise the kernel heap. Must be called after paging_init().
void kheap_init(void);

// Allocate at least `size` bytes. Returns NULL on failure.
// Thread-safe: protected by an internal spinlock.
void* kmalloc(size_t size);

// Free a previously allocated pointer.
// Coalesces adjacent free blocks both forward and backward.
// Panics on double-free or NULL (NULL is silently ignored).
void kfree(void* ptr);

// Log heap state (block list, fragmentation stats).
void kheap_dump(void);