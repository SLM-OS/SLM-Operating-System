/**
 * lua_stubs.c - Libc Stub Implementations for Lua
 *
 * Provides minimal implementations of libc functions required by Lua
 * for operation in a freestanding kernel environment.
 *
 * We don't include most system headers because newlib defines things
 * like stdin/stdout/stderr as macros that conflict with our stubs.
 * Instead we provide minimal declarations inline.
 */

/* Safe freestanding headers */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Our kernel headers */
#include "pmm.h"
#include "timer.h"
#include "debug.h"
#include "sched.h"
#include "string.h"  /* Our kernel string functions */

/* Forward declare FILE type for our stubs */
typedef struct __slm_file {
    int fd;
} FILE;

/* Our stdio variables (not macros) */
static FILE stdin_file = {0};
static FILE stdout_file = {1};
static FILE stderr_file = {2};

FILE *__slm_stdin = &stdin_file;
FILE *__slm_stdout = &stdout_file;
FILE *__slm_stderr = &stderr_file;

/* Errno storage */
static int __slm_errno = 0;

int *__errno(void) {
    return &__slm_errno;
}

/* Define these so Lua's headers can find them */
#define stdin   __slm_stdin
#define stdout  __slm_stdout
#define stderr  __slm_stderr
#define errno   (*__errno())

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 256
#define CLOCKS_PER_SEC 1000
#define L_tmpnam 20

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

/* Signal handling constants */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

/* Time types */
typedef long time_t;
typedef long clock_t;

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

/* Locale structure */
struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

/* jmp_buf for setjmp/longjmp (we have assembly implementation)
 * Layout: x19-x28 (10), x29 (1), x30 (1), sp (1), d8-d15 (8), fpcr (1) = 22 values
 * This matches the ARM toolchain's jmp_buf size (176 bytes) */
typedef uint64_t jmp_buf[22];

/* Forward declarations for math functions (they call each other) */
double exp(double x);
double log(double x);
double fabs(double x);
double floor(double x);
double sin(double x);
double cos(double x);

/* vsnprintf from our kernel's kprintf */
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int snprintf(char *str, size_t size, const char *format, ...);

/* Helper: get milliseconds since boot */
static inline uint64_t get_ms(void) {
    uint64_t count = timer_get_count();
    uint64_t freq = timer_get_frequency();
    return count / (freq / 1000);
}

/* ============================================================================
 * Simple Heap Allocator
 * ============================================================================
 * Uses a dedicated memory region for Lua allocations.
 */

#define LUA_HEAP_SIZE   (1024 * 1024)  /* 1MB heap for Lua */
#define ALIGN_SIZE      16
#define BLOCK_MAGIC     0xABCD1234

/* Block header for tracking allocations */
struct heap_block {
    uint32_t magic;
    uint32_t size;      /* Size of user data (not including header) */
    uint32_t is_free;   /* 1 if free, 0 if allocated */
    uint32_t padding;
    struct heap_block *next;
    struct heap_block *prev;
};

static uint8_t lua_heap[LUA_HEAP_SIZE] __attribute__((aligned(16)));
static struct heap_block *heap_head = NULL;
static int heap_initialized = 0;

static void heap_init(void) {
    if (heap_initialized) return;

    heap_head = (struct heap_block *)lua_heap;
    heap_head->magic = BLOCK_MAGIC;
    heap_head->size = LUA_HEAP_SIZE - sizeof(struct heap_block);
    heap_head->is_free = 1;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_initialized = 1;
}

void *malloc(size_t size) {
    if (!heap_initialized) heap_init();
    if (size == 0) return NULL;

    /* Align size */
    size = (size + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);

    /* First-fit search */
    struct heap_block *block = heap_head;
    while (block != NULL) {
        if (block->is_free && block->size >= size) {
            /* Found a suitable block */
            /* Check for split BEFORE subtraction to avoid unsigned underflow.
             * We need room for: [allocated data] + [new header] + [new data].
             * min_split = size + sizeof(struct heap_block) + ALIGN_SIZE */
            size_t min_split_size = size + sizeof(struct heap_block) + ALIGN_SIZE;
            if (block->size >= min_split_size) {
                size_t remaining = block->size - size - sizeof(struct heap_block);
                struct heap_block *new_block = (struct heap_block *)
                    ((uint8_t *)block + sizeof(struct heap_block) + size);
                new_block->magic = BLOCK_MAGIC;
                new_block->size = remaining;
                new_block->is_free = 1;
                new_block->next = block->next;
                new_block->prev = block;
                if (block->next) block->next->prev = new_block;
                block->next = new_block;
                block->size = size;
            }

            block->is_free = 0;
            return (void *)((uint8_t *)block + sizeof(struct heap_block));
        }
        block = block->next;
    }

    return NULL;  /* Out of memory */
}

void free(void *ptr) {
    if (ptr == NULL) return;

    struct heap_block *block = (struct heap_block *)
        ((uint8_t *)ptr - sizeof(struct heap_block));

    if (block->magic != BLOCK_MAGIC) {
        /* Corruption detected */
        return;
    }

    block->is_free = 1;

    /* Coalesce with next block if free */
    if (block->next && block->next->is_free) {
        block->size += sizeof(struct heap_block) + block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }

    /* Coalesce with previous block if free */
    if (block->prev && block->prev->is_free) {
        block->prev->size += sizeof(struct heap_block) + block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    struct heap_block *block = (struct heap_block *)
        ((uint8_t *)ptr - sizeof(struct heap_block));

    if (block->magic != BLOCK_MAGIC) {
        return NULL;
    }

    if (block->size >= size) {
        return ptr;  /* Current block is big enough */
    }

    /* Allocate new block and copy */
    void *new_ptr = malloc(size);
    if (new_ptr == NULL) return NULL;

    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/* ============================================================================
 * String Functions (some may already be in kernel, but provide stubs)
 * ============================================================================ */

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if ((char)c == '\0') return (char *)s;
    return (char *)last;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    *d = '\0';
    return dest;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dup = malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

int strcoll(const char *s1, const char *s2) {
    return strcmp(s1, s2);  /* No locale support */
}

/* strspn and strcspn are in kernel/src/string.c */

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a) return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

/* ============================================================================
 * Number Conversion
 * ============================================================================ */

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    long result = 0;
    int negative = 0;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    /* Handle sign */
    if (*s == '-') { negative = 1; s++; }
    else if (*s == '+') s++;

    /* Handle base prefix */
    if (base == 0 || base == 16) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') {
                base = 16;
                s++;
            } else if (base == 0) {
                base = 8;
            }
        } else if (base == 0) {
            base = 10;
        }
    }

    /* Parse digits */
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;

        if (digit >= base) break;

        result = result * base + digit;
        s++;
    }

    if (endptr) *endptr = (char *)s;
    return negative ? -result : result;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    double result = 0.0;
    double fraction = 0.1;
    int negative = 0;
    int in_fraction = 0;
    int exp_negative = 0;
    int exponent = 0;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;

    /* Handle sign */
    if (*s == '-') { negative = 1; s++; }
    else if (*s == '+') s++;

    /* Parse integer and fraction parts */
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            if (in_fraction) {
                result += (*s - '0') * fraction;
                fraction *= 0.1;
            } else {
                result = result * 10.0 + (*s - '0');
            }
        } else if (*s == '.' && !in_fraction) {
            in_fraction = 1;
        } else if (*s == 'e' || *s == 'E') {
            s++;
            if (*s == '-') { exp_negative = 1; s++; }
            else if (*s == '+') s++;
            while (*s >= '0' && *s <= '9') {
                exponent = exponent * 10 + (*s - '0');
                s++;
            }
            break;
        } else {
            break;
        }
        s++;
    }

    /* Apply exponent */
    while (exponent-- > 0) {
        if (exp_negative) result /= 10.0;
        else result *= 10.0;
    }

    if (endptr) *endptr = (char *)s;
    return negative ? -result : result;
}

/* atoi is in kernel/src/string.c */

long atol(const char *nptr) {
    return strtol(nptr, NULL, 10);
}

double atof(const char *nptr) {
    return strtod(nptr, NULL);
}

/* ============================================================================
 * Character Classification
 * ============================================================================ */

int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int iscntrl(int c) { return (c >= 0 && c <= 31) || c == 127; }
int ispunct(int c) { return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 32 && c <= 126; }
int isgraph(int c) { return c >= 33 && c <= 126; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ============================================================================
 * Locale Stubs
 * ============================================================================ */

static struct lconv default_lconv = {
    .decimal_point = ".",
    .thousands_sep = "",
};

struct lconv *localeconv(void) {
    return &default_lconv;
}

char *setlocale(int category, const char *locale) {
    (void)category;
    (void)locale;
    return "C";  /* Always C locale */
}

/* ============================================================================
 * Time Stubs
 * ============================================================================ */

static struct tm static_tm;

time_t time(time_t *tloc) {
    /* Return seconds since boot as time */
    time_t t = (time_t)(get_ms() / 1000);
    if (tloc) *tloc = t;
    return t;
}

struct tm *localtime(const time_t *timep) {
    /* Simple implementation - just return a zeroed struct */
    (void)timep;
    memset(&static_tm, 0, sizeof(static_tm));
    static_tm.tm_mday = 1;  /* Day must be >= 1 */
    return &static_tm;
}

struct tm *gmtime(const time_t *timep) {
    return localtime(timep);  /* Same as localtime for us */
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    (void)tm;
    (void)format;
    if (max > 0) s[0] = '\0';
    return 0;
}

time_t mktime(struct tm *tm) {
    (void)tm;
    return 0;
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

clock_t clock(void) {
    return (clock_t)(get_ms());
}

/* ============================================================================
 * Math Functions (basic implementations)
 * ============================================================================ */

double floor(double x) {
    if (x >= 0) return (double)(long long)x;
    double trunc = (double)(long long)x;
    return (trunc == x) ? x : trunc - 1.0;
}

double ceil(double x) {
    if (x <= 0) return (double)(long long)x;
    double trunc = (double)(long long)x;
    return (trunc == x) ? x : trunc + 1.0;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    return x - floor(x / y) * y;
}

double fabs(double x) {
    return x < 0 ? -x : x;
}

double sqrt(double x) {
    if (x < 0) return 0.0;
    if (x == 0) return 0.0;
    double guess = x / 2.0;
    for (int i = 0; i < 20; i++) {
        guess = (guess + x / guess) / 2.0;
    }
    return guess;
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return 0.0;

    /* Handle integer powers */
    if (y == floor(y) && y >= 0) {
        double result = 1.0;
        int n = (int)y;
        while (n-- > 0) result *= x;
        return result;
    }
    if (y == floor(y) && y < 0) {
        return 1.0 / pow(x, -y);
    }

    /* For non-integer powers, use exp(y * log(x)) */
    return exp(y * log(x));
}

double exp(double x) {
    /* Taylor series: e^x = 1 + x + x^2/2! + x^3/3! + ... */
    double result = 1.0;
    double term = 1.0;
    for (int i = 1; i < 30; i++) {
        term *= x / i;
        result += term;
        if (fabs(term) < 1e-15) break;
    }
    return result;
}

double log(double x) {
    if (x <= 0) return -1e308;  /* -inf approximation */

    /* Reduce to [1, 2) range and use series */
    int exp_val = 0;
    while (x >= 2.0) { x /= 2.0; exp_val++; }
    while (x < 1.0) { x *= 2.0; exp_val--; }

    /* log(x) = log(m * 2^e) = log(m) + e*log(2) */
    /* For m in [1, 2), use log(1+y) = y - y^2/2 + y^3/3 - ... where y = m-1 */
    double y = x - 1.0;
    double result = 0.0;
    double term = y;
    for (int i = 1; i < 50; i++) {
        result += term / i;
        term *= -y;
    }
    return result + exp_val * 0.693147180559945;  /* log(2) */
}

double log10(double x) {
    return log(x) / 2.302585092994046;  /* log(10) */
}

double sin(double x) {
    /* Reduce to [-pi, pi] */
    while (x > 3.14159265358979) x -= 6.28318530717959;
    while (x < -3.14159265358979) x += 6.28318530717959;

    /* Taylor series */
    double result = 0.0;
    double term = x;
    for (int i = 0; i < 15; i++) {
        result += term;
        term *= -x * x / ((2*i + 2) * (2*i + 3));
    }
    return result;
}

double cos(double x) {
    return sin(x + 1.5707963267949);  /* cos(x) = sin(x + pi/2) */
}

double tan(double x) {
    double c = cos(x);
    if (fabs(c) < 1e-15) return 1e308;  /* inf approximation */
    return sin(x) / c;
}

double asin(double x) { (void)x; return 0.0; }  /* TODO: implement */
double acos(double x) { (void)x; return 0.0; }  /* TODO: implement */
double atan(double x) { (void)x; return 0.0; }  /* TODO: implement */
double atan2(double y, double x) { (void)y; (void)x; return 0.0; }  /* TODO */

double sinh(double x) { return (exp(x) - exp(-x)) / 2.0; }
double cosh(double x) { return (exp(x) + exp(-x)) / 2.0; }
double tanh(double x) { double e = exp(2 * x); return (e - 1) / (e + 1); }

double frexp(double x, int *exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    *exp = 0;
    while (fabs(x) >= 1.0) { x /= 2.0; (*exp)++; }
    while (fabs(x) < 0.5) { x *= 2.0; (*exp)--; }
    return x;
}

double ldexp(double x, int exp) {
    while (exp > 0) { x *= 2.0; exp--; }
    while (exp < 0) { x /= 2.0; exp++; }
    return x;
}

double modf(double x, double *iptr) {
    *iptr = (double)(long long)x;
    return x - *iptr;
}

/* ============================================================================
 * File I/O Stubs
 * ============================================================================ */

/* stdin/stdout/stderr already defined at top of file */

FILE *fopen(const char *path, const char *mode) {
    (void)path; (void)mode;
    return NULL;  /* No file support */
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    (void)path; (void)mode; (void)stream;
    return NULL;
}

int fclose(FILE *stream) {
    (void)stream;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        /* Write to UART */
        const char *s = (const char *)ptr;
        size_t total = size * nmemb;
        for (size_t i = 0; i < total; i++) {
            uart_putc(s[i]);
        }
        return nmemb;
    }
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    (void)stream; (void)offset; (void)whence;
    return -1;
}

long ftell(FILE *stream) {
    (void)stream;
    return -1;
}

void rewind(FILE *stream) {
    (void)stream;
}

int feof(FILE *stream) {
    (void)stream;
    return 1;
}

int ferror(FILE *stream) {
    (void)stream;
    return 0;
}

void clearerr(FILE *stream) {
    (void)stream;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int fgetc(FILE *stream) {
    (void)stream;
    return EOF;
}

char *fgets(char *s, int size, FILE *stream) {
    (void)s; (void)size; (void)stream;
    return NULL;
}

int fputc(int c, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        uart_putc((char)c);
        return c;
    }
    return EOF;
}

int fputs(const char *s, FILE *stream) {
    if (stream == stdout || stream == stderr) {
        while (*s) uart_putc(*s++);
        return 0;
    }
    return EOF;
}

int ungetc(int c, FILE *stream) {
    (void)c; (void)stream;
    return EOF;
}

int fprintf(FILE *stream, const char *format, ...) {
    if (stream == stdout || stream == stderr) {
        va_list args;
        va_start(args, format);
        /* Use our kernel printf */
        char buf[256];
        int len = vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        for (int i = 0; i < len && buf[i]; i++) {
            uart_putc(buf[i]);
        }
        return len;
    }
    return -1;
}

int printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    for (int i = 0; i < len && buf[i]; i++) {
        uart_putc(buf[i]);
    }
    return len;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    if (stream == stdout || stream == stderr) {
        char buf[256];
        int len = vsnprintf(buf, sizeof(buf), format, ap);
        for (int i = 0; i < len && buf[i]; i++) {
            uart_putc(buf[i]);
        }
        return len;
    }
    return -1;
}

int fscanf(FILE *stream, const char *format, ...) {
    (void)stream; (void)format;
    return EOF;
}

int sscanf(const char *str, const char *format, ...) {
    (void)str; (void)format;
    return 0;  /* No scanf support */
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int puts(const char *s) {
    while (*s) uart_putc(*s++);
    uart_putc('\n');
    return 0;
}

int getchar(void) {
    return uart_getc();
}

int putchar(int c) {
    uart_putc((char)c);
    return c;
}

void setbuf(FILE *stream, char *buf) {
    (void)stream; (void)buf;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;
}

char *tmpnam(char *s) {
    (void)s;
    return NULL;
}

FILE *tmpfile(void) {
    return NULL;
}

int remove(const char *pathname) {
    (void)pathname;
    return -1;
}

int rename(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -1;
}

void perror(const char *s) {
    if (s && *s) {
        uart_printf("%s: ", s);
    }
    uart_printf("error\n");
}

/* ============================================================================
 * Environment and Process Control
 * ============================================================================ */

char *getenv(const char *name) {
    (void)name;
    return NULL;
}

int system(const char *command) {
    (void)command;
    return -1;
}

void exit(int status) {
    (void)status;
    uart_printf("Lua exit called\n");
    task_exit();
    for (;;) __asm__ volatile("wfi");
}

void _Exit(int status) {
    exit(status);
}

int atexit(void (*function)(void)) {
    (void)function;
    return 0;
}

void abort(void) {
    uart_printf("Lua abort called\n");
    task_exit();
    for (;;) __asm__ volatile("wfi");
}

/* ============================================================================
 * Signal Handling (stubs)
 * ============================================================================ */

void (*signal(int sig, void (*func)(int)))(int) {
    (void)sig; (void)func;
    return SIG_DFL;
}

int raise(int sig) {
    (void)sig;
    return 0;
}

/* ============================================================================
 * Lua I/O Helper Functions
 * ============================================================================ */

void slm_lua_writestring(const char *s, size_t l) {
    for (size_t i = 0; i < l; i++) {
        uart_putc(s[i]);
    }
}

void slm_lua_writeline(void) {
    uart_putc('\n');
}

void slm_lua_writeerror(const char *s, const char *p) {
    uart_printf(s, p);
}

/* errno already defined via __errno() at top of file */

/* ============================================================================
 * Additional Stubs Required by Lua
 * ============================================================================ */

/* Integer absolute value */
int abs(int j) {
    return j < 0 ? -j : j;
}

long labs(long j) {
    return j < 0 ? -j : j;
}

/* sprintf - use vsnprintf */
int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    extern int uart_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
    int len = uart_vsnprintf(str, 4096, format, args);  /* Assume large buffer */
    va_end(args);
    return len;
}

/* vsnprintf - wrapper for kernel's uart_vsnprintf */
int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    extern int uart_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
    return uart_vsnprintf(str, size, format, ap);
}

/* snprintf - uses vsnprintf */
int snprintf(char *str, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int len = vsnprintf(str, size, format, args);
    va_end(args);
    return len;
}

/* _ctype_ array for newlib's ctype macros
 * Bit flags:
 *   0x01 = upper
 *   0x02 = lower
 *   0x04 = digit
 *   0x08 = space
 *   0x10 = punct
 *   0x20 = cntrl
 *   0x40 = blank (space/tab)
 *   0x80 = xdigit
 */
const unsigned char _ctype_[257] = {
    /* -1 (EOF) */
    0,
    /* 0-31: control characters */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x28, 0x28, 0x28, 0x28, 0x28, 0x20, 0x20,  /* 0x09-0x0D are space chars */
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    /* 32: space */
    0x48,  /* space: blank + space */
    /* 33-47: punct !"#$%&'()*+,-./ */
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    /* 48-57: digits 0-9 */
    0x84, 0x84, 0x84, 0x84, 0x84, 0x84, 0x84, 0x84, 0x84, 0x84,
    /* 58-64: punct :;<=>?@ */
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    /* 65-70: uppercase A-F (also xdigit) */
    0x81, 0x81, 0x81, 0x81, 0x81, 0x81,
    /* 71-90: uppercase G-Z */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    /* 91-96: punct [\]^_` */
    0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
    /* 97-102: lowercase a-f (also xdigit) */
    0x82, 0x82, 0x82, 0x82, 0x82, 0x82,
    /* 103-122: lowercase g-z */
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    /* 123-126: punct {|}~ */
    0x10, 0x10, 0x10, 0x10,
    /* 127: DEL (control) */
    0x20,
    /* 128-255: extended ASCII (all zero for now) */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

/* memchr - search for byte in memory */
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

/* newlib reentrancy - provide the global pointer that stdio macros need.
 * struct _reent is already defined via sys/reent.h (included by string.h).
 * We provide minimal storage - only _errno is typically used by Lua. */
#include <sys/reent.h>
static struct _reent impure_data;
struct _reent *_impure_ptr = &impure_data;
