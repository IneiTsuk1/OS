#pragma once
#include <stdint.h>
#include <stddef.h>

// Initialise the kernel heap. Must be called after paging_init().
void kheap_init(void);

// Allocate at least `size` bytes. Returns NULL on failure.
// Returned memory is 8-byte aligned.
// Thread-safe: protected by an internal spinlock.
void* kmalloc(size_t size);

// Free a previously allocated pointer.
// Coalesces adjacent free blocks both forward and backward.
// NULL is silently ignored. Panics on double-free.
void kfree(void* ptr);

// Resize a previously allocated block.
//
// Semantics match C99 realloc:
//   krealloc(NULL,  size)  == kmalloc(size)
//   krealloc(ptr,   0)     == kfree(ptr), returns NULL
//   krealloc(ptr,   size)  — resize in-place if possible, otherwise
//                             alloc new block, copy, free old; returns
//                             NULL on OOM (original block unchanged).
//
// Fast path: if the next block is free and absorbed gives enough space,
// no copy is needed — the block is extended in-place.
void* krealloc(void* ptr, size_t new_size);

// Log heap state (block list, fragmentation stats).
void kheap_dump(void);