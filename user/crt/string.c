#include "string.h"

size_t strlen(const char* s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n)
{
    while (n && *a && *b && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* strcpy(char* dst, const char* src)
{
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n)
{
    char* d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

char* strchr(const char* s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : 0;
}

void* memcpy(void* dst, const void* src, size_t n)
{
    unsigned char*       d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void* memset(void* dst, int val, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    while (n--) *d++ = (unsigned char)val;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}