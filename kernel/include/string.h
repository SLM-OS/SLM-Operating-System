/*
 * string.h - String function declarations for SLM-OS kernel
 *
 * Declares the libc-compatible string functions implemented in kernel/src/string.c.
 * These are minimal implementations for use in a freestanding environment.
 */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>

/* String operations */
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
size_t strlen(const char *s);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);

/* Memory operations */
void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
void  *memmove(void *dest, const void *src, size_t n);

/*
 * Zero `n` bytes at `p` in a way the compiler is not allowed to elide.
 *
 * Plain memset / hand-rolled loops on a stack buffer that goes out of
 * scope are dead-store-eliminated under -O2: the compiler can prove
 * nothing reads the bytes back so the write is removed. This routine
 * forces the writes through a `volatile` pointer so they survive the
 * optimiser pass, matching the semantics of C11 `memset_s` / OpenBSD
 * `explicit_bzero` / Linux's `memzero_explicit`. Use this when wiping
 * crypto state (keys, plaintext passwords, DRBG output blocks) from
 * a stack frame before the caller returns.
 */
void   secure_zero(void *p, size_t n);

/* Conversion */
int    atoi(const char *str);

#endif /* STRING_H */
