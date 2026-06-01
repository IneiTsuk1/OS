#pragma once
/* stdlib.h — malloc/free and basic utilities. */
#include <stdint.h>
#include "string.h"   /* size_t */

void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void  free(void* ptr);

/* Integer/string conversions */
int   atoi(const char* s);
char* itoa(int n, char* buf, int base);  /* buf must be >= 12 bytes */

/* Absolute value */
static inline int abs(int n) { return n < 0 ? -n : n; }