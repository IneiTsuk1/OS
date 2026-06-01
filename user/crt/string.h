#pragma once
/* string.h — freestanding string and memory utilities. */
#include <stdint.h>

typedef unsigned int size_t;

size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strchr(const char* s, int c);

void*  memcpy(void* dst, const void* src, size_t n);
void*  memset(void* dst, int val, size_t n);
int    memcmp(const void* a, const void* b, size_t n);