/*
 * Boot-test filter and repeat count: parser and active state.
 * See test_filter.h and docs/testing.md "Running selected tests".
 */

#include "test_filter.h"

test_filter_t       g_test_filter;
test_filter_stats_t g_test_filter_stats[TEST_FILTER_MAX_NAMES];
uint32_t            g_test_filter_pass;

static bool is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int test_filter_parse_names(test_filter_t *f, const char *csv, size_t len)
{
    if (f == NULL || csv == NULL || len == 0) {
        return TEST_FILTER_ERR_EMPTY;
    }

    uint32_t count = 0;
    size_t i = 0;
    while (i <= len) {
        size_t start = i;
        while (i < len && csv[i] != ',') {
            if (!is_name_char(csv[i])) {
                return TEST_FILTER_ERR_CHAR;
            }
            i++;
        }
        size_t n = i - start;
        if (n == 0) {
            return TEST_FILTER_ERR_EMPTY;       /* "a,,b", ",a" or "a," */
        }
        if (n >= TEST_FILTER_NAME_MAX) {
            return TEST_FILTER_ERR_TOO_LONG;
        }
        if (count >= TEST_FILTER_MAX_NAMES) {
            return TEST_FILTER_ERR_TOO_MANY;
        }
        for (size_t k = 0; k < n; k++) {
            f->names[count][k] = csv[start + k];
        }
        f->names[count][n] = '\0';
        count++;
        i++;                                    /* skip the comma (or end) */
    }
    f->count = count;
    return TEST_FILTER_OK;
}

int test_filter_parse_repeat(const char *s, size_t len, uint32_t *out)
{
    if (s == NULL || out == NULL || len == 0 || len > 7) {
        return TEST_FILTER_ERR_REPEAT;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return TEST_FILTER_ERR_REPEAT;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    if (v == 0 || v > TEST_FILTER_REPEAT_MAX) {
        return TEST_FILTER_ERR_REPEAT;
    }
    *out = v;
    return TEST_FILTER_OK;
}

const char *test_filter_find_kv(const char *cmdline, const char *key,
                                size_t *out_len)
{
    if (cmdline == NULL || key == NULL || out_len == NULL) {
        return NULL;
    }
    size_t klen = 0;
    while (key[klen] != '\0') {
        klen++;
    }

    const char *p = cmdline;
    while (*p != '\0') {
        while (is_space(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *tok = p;
        while (*p != '\0' && !is_space(*p)) {
            p++;
        }
        size_t tlen = (size_t)(p - tok);
        if (tlen > klen && tok[klen] == '=') {
            bool eq = true;
            for (size_t i = 0; i < klen; i++) {
                if (tok[i] != key[i]) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                *out_len = tlen - klen - 1;
                return tok + klen + 1;
            }
        }
    }
    return NULL;
}

int test_filter_from_cmdline(test_filter_t *f, const char *cmdline)
{
    size_t nlen = 0;
    const char *names = test_filter_find_kv(cmdline, TEST_FILTER_KEY_NAMES,
                                            &nlen);
    if (names == NULL) {
        return 0;
    }

    test_filter_t tmp = { 0 };
    int rc = test_filter_parse_names(&tmp, names, nlen);
    if (rc != TEST_FILTER_OK) {
        return rc;
    }

    tmp.repeat = 1;
    size_t rlen = 0;
    const char *rep = test_filter_find_kv(cmdline, TEST_FILTER_KEY_REPEAT,
                                          &rlen);
    if (rep != NULL) {
        rc = test_filter_parse_repeat(rep, rlen, &tmp.repeat);
        if (rc != TEST_FILTER_OK) {
            return rc;
        }
    }

    tmp.active = true;
    tmp.source = "cmdline";
    *f = tmp;
    return 1;
}

int test_filter_index_of(const test_filter_t *f, const char *name)
{
    if (f == NULL || name == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < f->count; i++) {
        const char *a = f->names[i];
        const char *b = name;
        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return (int)i;
        }
    }
    return -1;
}

/* Empty index for images built without the generated table
 * (scripts/gen-test-index.py, boot-test images only). */
__attribute__((weak)) const char *test_index_next_suite(const char *test_name,
                                                        uint32_t *cursor)
{
    (void)test_name;
    (void)cursor;
    return NULL;
}

__attribute__((weak)) uint32_t test_index_size(void)
{
    return 0;
}
