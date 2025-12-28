/**
 * slm_lua_stubs.h - Libc Stubs for Lua in Freestanding Environment
 *
 * Provides minimal libc functions that Lua requires.
 * Included via LUA_USER_H in slm_luaconf.h.
 */

#ifndef SLM_LUA_STUBS_H
#define SLM_LUA_STUBS_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * setjmp/longjmp for Error Handling
 * ============================================================================
 * Lua uses these for pcall/error recovery. We provide minimal implementations.
 */

/* Jump buffer - must save callee-saved registers for AArch64 */
typedef struct {
    uint64_t regs[22];  /* x19-x29, x30 (LR), SP, FPSR, FPCR, d8-d15 */
} jmp_buf[1];

/* Save context - returns 0 on initial call */
int setjmp(jmp_buf env);

/* Restore context - never returns, appears to return val from setjmp */
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* ============================================================================
 * Memory Allocation
 * ============================================================================
 * Lua uses a custom allocator function. We use a simple heap implementation.
 */

/* Standard allocation functions */
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);

/* ============================================================================
 * Additional String Functions
 * ============================================================================
 * Some functions Lua expects that we may not have.
 */

/* Find last occurrence of character in string */
char *strrchr(const char *s, int c);

/* Concatenate strings */
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);

/* Find substring */
char *strstr(const char *haystack, const char *needle);

/* Duplicate string (allocates memory) */
char *strdup(const char *s);

/* ============================================================================
 * Number Conversion
 * ============================================================================
 */

/* String to long */
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

/* String to double (returns 0.0 if no FPU support) */
double strtod(const char *nptr, char **endptr);

/* Long to string */
char *ltoa(long value, char *str, int base);

/* ============================================================================
 * Character Classification (simplified)
 * ============================================================================
 */

int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int iscntrl(int c);
int ispunct(int c);
int isxdigit(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

/* ============================================================================
 * Locale Stubs (do nothing)
 * ============================================================================
 */

struct lconv {
    char *decimal_point;
    char *thousands_sep;
};

struct lconv *localeconv(void);
char *setlocale(int category, const char *locale);

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

/* ============================================================================
 * Time Stubs
 * ============================================================================
 * Note: time_t and clock_t are provided by the ARM toolchain's sys/types.h
 * which is included via Lua's signal.h inclusion. We don't redefine them.
 */

#include <sys/types.h>  /* For time_t, clock_t */

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t *tloc);
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
time_t mktime(struct tm *tm);
double difftime(time_t time1, time_t time0);
clock_t clock(void);

#define CLOCKS_PER_SEC 1000

/* ============================================================================
 * Assertions and Abort
 * ============================================================================
 */

void abort(void) __attribute__((noreturn));

#ifndef NDEBUG
#define assert(expr) ((void)((expr) || (abort(), 0)))
#else
#define assert(expr) ((void)0)
#endif

/* ============================================================================
 * File I/O Stubs (Lua libraries need these declarations)
 * ============================================================================
 */

typedef struct _FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Most file operations will fail or do nothing - Lua's io library disabled */

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 256

#define L_tmpnam 20

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fflush(FILE *stream);
int fgetc(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int ungetc(int c, FILE *stream);
int fprintf(FILE *stream, const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int sscanf(const char *str, const char *format, ...);
int getc(FILE *stream);
int putc(int c, FILE *stream);
int puts(const char *s);
int getchar(void);
int putchar(int c);
void setbuf(FILE *stream, char *buf);
int setvbuf(FILE *stream, char *buf, int mode, size_t size);
char *tmpnam(char *s);
FILE *tmpfile(void);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);
void perror(const char *s);

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* ============================================================================
 * Environment (disabled)
 * ============================================================================
 */

char *getenv(const char *name);
int system(const char *command);
void exit(int status) __attribute__((noreturn));
void _Exit(int status) __attribute__((noreturn));
int atexit(void (*function)(void));

/* ============================================================================
 * Math Functions (integer-only approximations)
 * ============================================================================
 * With LUA_32BITS, Lua uses floats but we can provide stubs.
 */

double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double pow(double x, double y);
double sqrt(double x);
double fabs(double x);
double log(double x);
double log10(double x);
double exp(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);

#define HUGE_VAL (__builtin_huge_val())

#endif /* SLM_LUA_STUBS_H */
