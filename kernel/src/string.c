/*
 * string.c - Minimal string functions for freestanding environment
 *
 * Provides libc string functions needed by LittleFS.
 * These are bare-minimum implementations for kernel use.
 */

#include "string.h"
#include <stddef.h>

/* Copy string from src to dest */
char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != '\0') {
        ;
    }
    return dest;
}

/* Copy at most n characters from src to dest */
char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

/* Find first occurrence of c in s */
char *strchr(const char *s, int c)
{
    while (*s != '\0') {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

/* Calculate length of initial segment matching accept */
size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    const char *a;

    while (*s != '\0') {
        for (a = accept; *a != '\0'; a++) {
            if (*s == *a) {
                break;
            }
        }
        if (*a == '\0') {
            return count;
        }
        count++;
        s++;
    }
    return count;
}

/* Calculate length of initial segment not matching reject */
size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    const char *r;

    while (*s != '\0') {
        for (r = reject; *r != '\0'; r++) {
            if (*s == *r) {
                return count;
            }
        }
        count++;
        s++;
    }
    return count;
}

/* Compare two strings */
int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/* Compare at most n characters of two strings */
int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0) {
        return 0;
    }
    while (n > 1 && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/* Calculate string length */
size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++) {
        len++;
    }
    return len;
}

/* Copy memory */
void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/* Set memory */
void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

/*
 * Optimisation-resistant memset(0). The volatile-qualified pointer is
 * the load-bearing piece — it forbids the compiler from eliminating
 * the writes even when the target object is dead after the call.
 * See string.h for the rationale (crypto-state wipe).
 */
void secure_zero(void *p, size_t n)
{
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) {
        *q++ = 0u;
    }
}

/* Compare memory */
int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = s1;
    const unsigned char *p2 = s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

/* Move memory (handles overlapping regions) */
void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

/* Convert string to integer.
 *
 * Deprecated for new code: this silently overflows on large inputs and can't
 * distinguish "0" from a parse failure. Use shell_parse_uint() (overflow-
 * checked, positive integers) from shell_internal.h where numeric range
 * matters. Retained here because third-party code under kernel/lib/lwip
 * expects the standard libc name. */
int atoi(const char *str)
{
    int result = 0;
    int sign = 1;

    /* Skip whitespace */
    while (*str == ' ' || *str == '\t' || *str == '\n' ||
           *str == '\r' || *str == '\f' || *str == '\v') {
        str++;
    }

    /* Handle optional sign */
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    /* Convert digits */
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }

    return sign * result;
}
