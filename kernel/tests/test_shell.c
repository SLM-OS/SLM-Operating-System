/*
 * test_shell.c - Shell Command Tests for SLM-OS
 *
 * Tests shell command dispatch, argument parsing, and error handling.
 * Note: Commands output to UART; these tests verify return values and behavior.
 */

#include "unity.h"
#include "../include/shell.h"
#include "../include/shell_internal.h"
#include "../include/shell_session.h"
#include "../include/component.h"
#include "../include/vfs.h"
#include "../include/task.h"
#include "../include/littlefs_slm.h"
#include "../include/string.h"
#include "../include/uart.h"
#include "../include/slm_ffi.h"
#include "../include/help.h"
#include "ai_types.h"

/* snprintf is part of the test kernel's runtime (kernel/lib) but isn't
 * pulled in by the headers above. Declare it once for all tests that
 * build command strings for shell_execute. */
extern int snprintf(char *, size_t, const char *, ...);

/* Defined in kernel/src/shell.c. Used by the help-output convention tests
 * (test_builtin_commands_grouped_and_sorted +
 * test_external_commands_categories_in_range) to walk the source-side
 * registration table and the runtime-registered external table. */
extern const shell_cmd_t builtin_commands[];
extern const int NUM_BUILTIN_COMMANDS;
extern shell_cmd_t external_commands[];
extern int num_external_commands;

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

/*
 * Clean up any components registered during tests.
 */
static void cleanup_components(void)
{
    for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
        component_unregister(i);
    }
}

/*
 * Helper to read file content via VFS API.
 * Returns bytes read, or -1 on error.
 */
static int read_file_content(const char *path, char *buf, size_t size)
{
    return vfs_read_path(path, buf, size, 0);
}

/*
 * Helper to get file size via LittleFS API.
 * Returns size in bytes, or -1 on error.
 */
static int get_file_size(const char *path)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(path, &subpath);
    if (!mnt || !subpath) return -1;

    /* Skip leading slash if present */
    if (subpath[0] == '/') subpath++;

    struct lfs_entry_info info;
    if (littlefs_stat_path(mnt, subpath, &info) < 0) return -1;

    return (int)info.size;
}

static uint32_t test_fnv1a32(const uint8_t *data, size_t len)
{
    uint32_t hash = 0x811C9DC5u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

static void write_u16_le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void write_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
    out[2] = (uint8_t)((value >> 16) & 0xFFu);
    out[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static size_t build_valid_mlp_payload(uint32_t out_weight_bits, uint8_t *out, size_t out_cap)
{
    enum {
        PAYLOAD_HEADER_LEN = 8,
        L1_IN = 27,
        L1_OUT = 64,
        L2_OUT = 32,
        L3_OUT = 16,
        OUT = 1,
        W_L1_LEN = L1_OUT * L1_IN,
        B_L1_LEN = L1_OUT,
        W_L2_LEN = L2_OUT * L1_OUT,
        B_L2_LEN = L2_OUT,
        W_L3_LEN = L3_OUT * L2_OUT,
        B_L3_LEN = L3_OUT,
        W_OUT_LEN = OUT * L3_OUT,
        B_OUT_LEN = OUT,
        FLOAT_COUNT = W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN +
                      W_L3_LEN + B_L3_LEN + W_OUT_LEN + B_OUT_LEN,
        PAYLOAD_LEN = PAYLOAD_HEADER_LEN + FLOAT_COUNT * 4
    };
    size_t cursor = PAYLOAD_HEADER_LEN;

    if (out_cap < PAYLOAD_LEN) return 0;
    memset(out, 0, PAYLOAD_LEN);

    out[0] = 'M'; out[1] = 'L'; out[2] = 'P'; out[3] = '1';
    write_u16_le(out + 4, 1);
    write_u16_le(out + 6, 0);

    write_u32_le(out + cursor, 0x3F800000u);
    cursor += W_L1_LEN * 4;
    cursor += B_L1_LEN * 4;
    write_u32_le(out + cursor, 0x3F800000u);
    cursor += W_L2_LEN * 4;
    cursor += B_L2_LEN * 4;
    write_u32_le(out + cursor, 0x3F800000u);
    cursor += W_L3_LEN * 4;
    cursor += B_L3_LEN * 4;
    write_u32_le(out + cursor, out_weight_bits);

    return PAYLOAD_LEN;
}

static int write_binary_file(const char *path, const uint8_t *data, size_t len)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(path, &subpath);
    if (!mnt || !subpath) return -1;
    int fd = littlefs_file_open(mnt, subpath, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) return -1;
    int written = littlefs_file_write(mnt, fd, data, len);
    littlefs_file_close(mnt, fd);
    return written == (int)len ? 0 : -1;
}

static size_t build_shell_test_blob(uint16_t kind_id,
                                    const uint8_t *payload,
                                    size_t payload_len,
                                    uint8_t *out,
                                    size_t out_cap)
{
    uint32_t checksum = test_fnv1a32(payload, payload_len);
    size_t total = 24 + payload_len;
    if (out_cap < total) return 0;

    out[0] = 'S'; out[1] = 'E'; out[2] = 'M'; out[3] = 'B';
    out[4] = 1; out[5] = 0;
    out[6] = (uint8_t)(kind_id & 0xFF);
    out[7] = (uint8_t)(kind_id >> 8);
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    out[12] = (uint8_t)(payload_len & 0xFF);
    out[13] = (uint8_t)((payload_len >> 8) & 0xFF);
    out[14] = (uint8_t)((payload_len >> 16) & 0xFF);
    out[15] = (uint8_t)((payload_len >> 24) & 0xFF);
    out[16] = (uint8_t)(checksum & 0xFF);
    out[17] = (uint8_t)((checksum >> 8) & 0xFF);
    out[18] = (uint8_t)((checksum >> 16) & 0xFF);
    out[19] = (uint8_t)((checksum >> 24) & 0xFF);
    out[20] = 0; out[21] = 0; out[22] = 0; out[23] = 0;
    memcpy(out + 24, payload, payload_len);
    return total;
}

#ifdef CONFIG_AI_SCHEDULER
static size_t build_sched_mlp_payload(uint32_t out_weight_bits,
                                      uint8_t *out,
                                      size_t out_cap)
{
    enum {
        PAYLOAD_HEADER_LEN = 12,
        W0 = AI_MLP_LAYER0_OUT * AI_MLP_LAYER0_IN,
        B0 = AI_MLP_LAYER0_OUT,
        W1 = AI_MLP_LAYER1_OUT * AI_MLP_LAYER1_IN,
        B1 = AI_MLP_LAYER1_OUT,
        W2 = AI_MLP_LAYER2_OUT * AI_MLP_LAYER2_IN,
        B2 = AI_MLP_LAYER2_OUT,
        W3 = AI_SCHED_N_ACTIONS * AI_MLP_LAYER3_IN,
        B3 = AI_SCHED_N_ACTIONS,
        FLOATS = W0 + B0 + W1 + B1 + W2 + B2 + W3 + B3,
        TOTAL = PAYLOAD_HEADER_LEN + FLOATS * 4
    };
    size_t cursor = 0;
    size_t idx = 0;

    if (out_cap < TOTAL) return 0;
    memset(out, 0, TOTAL);
    out[0] = 'S'; out[1] = 'M'; out[2] = 'L'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = (uint8_t)(AI_SCHED_N_ACTIONS & 0xFF);
    out[11] = (uint8_t)(AI_SCHED_N_ACTIONS >> 8);

    cursor = PAYLOAD_HEADER_LEN;
#define WRITE_U32_LE(bits)                                                   \
    do {                                                                    \
        uint32_t bits_ = (bits);                                            \
        out[cursor + 0] = (uint8_t)(bits_ & 0xFF);                          \
        out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);                   \
        out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);                  \
        out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);                  \
        cursor += 4;                                                        \
    } while (0)

    for (idx = 0; idx < FLOATS; idx++) {
        uint32_t bits = 0u;
        if (idx == 0) bits = 0x3F800000u;
        if (idx == W0 + B0) bits = 0x3F800000u;
        if (idx == W0 + B0 + W1 + B1) bits = 0x3F800000u;
        if (idx == W0 + B0 + W1 + B1 + W2 + B2) bits = out_weight_bits;
        WRITE_U32_LE(bits);
    }
#undef WRITE_U32_LE

    return cursor;
}

static size_t build_sched_config_payload(uint32_t enabled,
                                         uint32_t min_target_ready,
                                         uint32_t min_active_cpus,
                                         uint32_t imbalance_num,
                                         uint32_t imbalance_den,
                                         uint8_t *out,
                                         size_t out_cap)
{
    size_t cursor = 0;

    if (out_cap < 32u) return 0;
    memset(out, 0, 32u);
    out[0] = 'S'; out[1] = 'C'; out[2] = 'F'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    cursor = 12;
#define WRITE_U32_LE(bits)                                                   \
    do {                                                                    \
        uint32_t bits_ = (bits);                                            \
        out[cursor + 0] = (uint8_t)(bits_ & 0xFF);                          \
        out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);                   \
        out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);                  \
        out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);                  \
        cursor += 4;                                                        \
    } while (0)
    WRITE_U32_LE(enabled);
    WRITE_U32_LE(min_target_ready);
    WRITE_U32_LE(min_active_cpus);
    WRITE_U32_LE(imbalance_num);
    WRITE_U32_LE(imbalance_den);
#undef WRITE_U32_LE
    return cursor;
}

static size_t build_sched_thresholds_payload(uint64_t critical_ns,
                                             uint64_t high_ns,
                                             uint64_t boost_ns,
                                             uint8_t *out,
                                             size_t out_cap)
{
    size_t cursor = 0;

    if (out_cap < 36u) return 0;
    memset(out, 0, 36u);
    out[0] = 'S'; out[1] = 'T'; out[2] = 'H'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    cursor = 12;
#define WRITE_THRESH_U64(bits)                                               \
    do {                                                                     \
        uint64_t bits_ = (bits);                                             \
        out[cursor + 0] = (uint8_t)(bits_ & 0xFF);                           \
        out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);                    \
        out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);                   \
        out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);                   \
        out[cursor + 4] = (uint8_t)((bits_ >> 32) & 0xFF);                   \
        out[cursor + 5] = (uint8_t)((bits_ >> 40) & 0xFF);                   \
        out[cursor + 6] = (uint8_t)((bits_ >> 48) & 0xFF);                   \
        out[cursor + 7] = (uint8_t)((bits_ >> 56) & 0xFF);                   \
        cursor += 8;                                                         \
    } while (0)
    WRITE_THRESH_U64(critical_ns);
    WRITE_THRESH_U64(high_ns);
    WRITE_THRESH_U64(boost_ns);
#undef WRITE_THRESH_U64
    return cursor;
}

static size_t build_sched_rebalance_payload(uint32_t enabled,
                                            uint32_t interval_ticks,
                                            uint32_t imbalance_min,
                                            uint8_t *out,
                                            size_t out_cap)
{
    size_t cursor = 0;

    if (out_cap < 24u) return 0;
    memset(out, 0, 24u);
    out[0] = 'S'; out[1] = 'R'; out[2] = 'B'; out[3] = '1';
    out[4] = 1; out[5] = 0;
    out[6] = 1; out[7] = 0;
    out[8] = 1; out[9] = 0;
    out[10] = 0; out[11] = 0;
    cursor = 12;
#define WRITE_REBAL_U32(bits)                                                \
    do {                                                                     \
        uint32_t bits_ = (bits);                                             \
        out[cursor + 0] = (uint8_t)(bits_ & 0xFF);                           \
        out[cursor + 1] = (uint8_t)((bits_ >> 8) & 0xFF);                    \
        out[cursor + 2] = (uint8_t)((bits_ >> 16) & 0xFF);                   \
        out[cursor + 3] = (uint8_t)((bits_ >> 24) & 0xFF);                   \
        cursor += 4;                                                         \
    } while (0)
    WRITE_REBAL_U32(enabled);
    WRITE_REBAL_U32(interval_ticks);
    WRITE_REBAL_U32(imbalance_min);
#undef WRITE_REBAL_U32
    return cursor;
}
#endif

/* ============================================================================
 * Command Dispatch Tests
 * ============================================================================ */

/*
 * Test: shell_execute with empty command returns 0.
 */
static void test_shell_empty_command(void)
{
    int ret = shell_execute("");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: shell_execute with whitespace-only returns 0.
 */
static void test_shell_whitespace_only(void)
{
    int ret = shell_execute("   ");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: Unknown command returns -1.
 */
static void test_shell_unknown_command(void)
{
    int ret = shell_execute("nonexistent_command");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Regression test: shell_execute with string longer than SHELL_MAX_LINE
 * must return -1 without crashing (was a stack buffer overflow).
 */
static void test_shell_cmd_too_long(void)
{
    char long_cmd[SHELL_MAX_LINE + 64];
    extern void *memset(void *s, int c, size_t n);
    memset(long_cmd, 'a', SHELL_MAX_LINE + 32);
    long_cmd[SHELL_MAX_LINE + 32] = '\0';

    int ret = shell_execute(long_cmd);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Regression test: shell_execute at exactly SHELL_MAX_LINE-1 still works.
 */
static void test_shell_cmd_at_max_length(void)
{
    char cmd[SHELL_MAX_LINE];
    extern void *memset(void *s, int c, size_t n);
    memset(cmd, ' ', SHELL_MAX_LINE - 1);
    cmd[SHELL_MAX_LINE - 1] = '\0';

    /* All spaces — should parse as empty command, return 0 */
    int ret = shell_execute(cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'uptime' command executes successfully (minimal output).
 */
static void test_shell_cmd_uptime(void)
{
    int ret = shell_execute("uptime");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'bench' command with each subcommand.
 */
static void test_shell_cmd_bench_no_args(void)
{
    int ret = shell_execute("bench");
    TEST_ASSERT_EQUAL_INT(1, ret);  /* Missing subcommand → error */
}

static void test_shell_cmd_bench_context(void)
{
    int ret = shell_execute("bench context");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_irq(void)
{
    int ret = shell_execute("bench irq");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_ipc(void)
{
    int ret = shell_execute("bench ipc");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_stats(void)
{
    int ret = shell_execute("bench stats");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_all(void)
{
    int ret = shell_execute("bench all");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_bench_invalid(void)
{
    int ret = shell_execute("bench foobar");
    TEST_ASSERT_EQUAL_INT(1, ret);  /* Unknown subcommand → error */
}

/*
 * Test: 'clear' command executes successfully (minimal output).
 */
static void test_shell_cmd_clear(void)
{
    int ret = shell_execute("clear");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'top -n 1' runs a single frame and exits cleanly. Regression
 * for #191 — the command must not hang without 'q' and must accept -n.
 */
static void test_shell_cmd_top_one_iter(void)
{
    int ret = shell_execute("top -n 1");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'top -n 1 5' accepts both iteration count and refresh interval.
 * Refresh isn't exercised here since we only run one frame, but the
 * parser must not error.
 */
static void test_shell_cmd_top_refresh_arg(void)
{
    int ret = shell_execute("top -n 1 5");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'top' rejects a zero-second refresh interval.
 */
static void test_shell_cmd_top_zero_refresh(void)
{
    int ret = shell_execute("top 0");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * Test: 'top -n' without a value errors out.
 */
static void test_shell_cmd_top_missing_count(void)
{
    int ret = shell_execute("top -n");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * Tests for #195: `sched trace` subcommands.
 * The trace system exists independent of recorded events — start/stop/clear
 * and dumping must succeed even with zero captured events.
 */
static void test_shell_cmd_sched_trace_lifecycle(void)
{
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace start"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace per-cpu"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace stop"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace clear"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched trace"));
}

/*
 * Regression for #193: `sched compare` runs the context-switch
 * microbenchmark under each registered policy and prints a table.
 * We don't assert on the numbers (they vary run-to-run) — just that
 * the command returns 0 and doesn't panic.
 */
static void test_shell_cmd_sched_compare(void)
{
    int ret = shell_execute("sched compare");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Regression for #194: `eviction demo` fills the weight pool to
 * capacity, drives at least one eviction, and cleans up. When
 * AI_EVICTION is off the policy isn't invoked but the command still
 * runs and returns 0.
 */
static void test_shell_cmd_eviction_demo(void)
{
    int ret = shell_execute("eviction demo");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Regression for #196: `bench context` now renders a latency histogram
 * after the average/rating output. The histogram records every
 * context-switch round-trip, buckets them logarithmically, and prints
 * p50/p95/p99 percentiles. Running bench context twice in a row
 * verifies that the histogram reinitializes cleanly (no carry-over
 * from the previous run).
 */
static void test_shell_cmd_bench_context_histogram_reinit(void)
{
    TEST_ASSERT_EQUAL_INT(0, shell_execute("bench context"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("bench context"));
}

/*
 * Regression for #117: `bench eviction` runs the workload replay
 * comparison and returns 0 without panicking. The numbers vary
 * run-to-run so we pin only the return code.
 */
static void test_shell_cmd_bench_eviction(void)
{
    int ret = shell_execute("bench eviction");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Regression for #112: `eviction features` lists the 27-element
 * feature vector. Returns 0 whether AI_EVICTION is on or off.
 */
static void test_shell_cmd_eviction_features(void)
{
    int ret = shell_execute("eviction features");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_eviction_model_status(void)
{
    int ret = shell_execute("eviction model status");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_eviction_model_lifecycle(void)
{
    static const char *path = "/mnt/files/test-eviction-mlp.blob";
    static uint8_t payload[18000];
    static uint8_t blob[18100];
    size_t payload_len = build_valid_mlp_payload(0x41200000u, payload, sizeof(payload));
    size_t blob_len = build_shell_test_blob(2, payload, payload_len, blob, sizeof(blob));

    TEST_ASSERT_TRUE(payload_len > 0);
    TEST_ASSERT_TRUE(blob_len > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path, blob, blob_len));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("eviction model clear mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("eviction model load mlp /mnt/files/test-eviction-mlp.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("eviction model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("eviction model activate mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("eviction model status"));
    TEST_ASSERT_NOT_EQUAL(0, shell_execute("eviction model rollback mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("eviction model clear mlp"));
    shell_execute("rm /mnt/files/test-eviction-mlp.blob");
}

static void test_shell_cmd_sched_model_lifecycle(void)
{
#ifndef CONFIG_AI_SCHEDULER
    TEST_IGNORE_MESSAGE("CONFIG_AI_SCHEDULER not enabled");
#else
    static const char *path_a = "/mnt/files/test-sched-mlp-a.blob";
    static const char *path_b = "/mnt/files/test-sched-mlp-b.blob";
    static const char *path_c = "/mnt/files/test-sched-ppo-a.blob";
    static const char *path_d = "/mnt/files/test-sched-ppo-b.blob";
    static const char *path_e = "/mnt/files/test-sched-config-a.blob";
    static const char *path_f = "/mnt/files/test-sched-config-b.blob";
    static uint8_t payload_a[530000];
    static uint8_t blob_a[530100];
    static uint8_t payload_b[530000];
    static uint8_t blob_b[530100];
    static uint8_t payload_c[530000];
    static uint8_t blob_c[530100];
    static uint8_t payload_d[530000];
    static uint8_t blob_d[530100];
    static uint8_t payload_e[64];
    static uint8_t blob_e[128];
    static uint8_t payload_f[64];
    static uint8_t blob_f[128];
    size_t payload_len_a = build_sched_mlp_payload(0x40000000u, payload_a, sizeof(payload_a));
    size_t payload_len_b = build_sched_mlp_payload(0x40400000u, payload_b, sizeof(payload_b));
    size_t payload_len_c = build_sched_mlp_payload(0x3f000000u, payload_c, sizeof(payload_c));
    size_t payload_len_d = build_sched_mlp_payload(0x40800000u, payload_d, sizeof(payload_d));
    size_t payload_len_e = build_sched_config_payload(0u, 2u, 2u, 3u, 2u,
                                                      payload_e, sizeof(payload_e));
    size_t payload_len_f = build_sched_config_payload(1u, 1u, 2u, 1u, 1u,
                                                      payload_f, sizeof(payload_f));
    size_t blob_len_a;
    size_t blob_len_b;
    size_t blob_len_c;
    size_t blob_len_d;
    size_t blob_len_e;
    size_t blob_len_f;

    TEST_ASSERT_TRUE(payload_len_a > 0);
    TEST_ASSERT_TRUE(payload_len_b > 0);
    TEST_ASSERT_TRUE(payload_len_c > 0);
    TEST_ASSERT_TRUE(payload_len_d > 0);
    TEST_ASSERT_TRUE(payload_len_e > 0);
    TEST_ASSERT_TRUE(payload_len_f > 0);
    blob_len_a = build_shell_test_blob(0x1001u, payload_a, payload_len_a, blob_a, sizeof(blob_a));
    blob_len_b = build_shell_test_blob(0x1001u, payload_b, payload_len_b, blob_b, sizeof(blob_b));
    blob_len_c = build_shell_test_blob(0x1002u, payload_c, payload_len_c, blob_c, sizeof(blob_c));
    blob_len_d = build_shell_test_blob(0x1002u, payload_d, payload_len_d, blob_d, sizeof(blob_d));
    blob_len_e = build_shell_test_blob(0x1003u, payload_e, payload_len_e, blob_e, sizeof(blob_e));
    blob_len_f = build_shell_test_blob(0x1003u, payload_f, payload_len_f, blob_f, sizeof(blob_f));
    TEST_ASSERT_TRUE(blob_len_a > 0);
    TEST_ASSERT_TRUE(blob_len_b > 0);
    TEST_ASSERT_TRUE(blob_len_c > 0);
    TEST_ASSERT_TRUE(blob_len_d > 0);
    TEST_ASSERT_TRUE(blob_len_e > 0);
    TEST_ASSERT_TRUE(blob_len_f > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_a, blob_a, blob_len_a));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_b, blob_b, blob_len_b));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_c, blob_c, blob_len_c));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_d, blob_d, blob_len_d));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_e, blob_e, blob_len_e));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_f, blob_f, blob_len_f));

    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load mlp /mnt/files/test-sched-mlp-a.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load mlp /mnt/files/test-sched-mlp-b.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model rollback mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear mlp"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear ppo"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load ppo /mnt/files/test-sched-ppo-a.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate ppo"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load ppo /mnt/files/test-sched-ppo-b.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate ppo"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model rollback ppo"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear ppo"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear config"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load config /mnt/files/test-sched-config-a.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate config"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load config /mnt/files/test-sched-config-b.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate config"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model rollback config"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear config"));
    shell_execute("rm /mnt/files/test-sched-mlp-a.blob");
    shell_execute("rm /mnt/files/test-sched-mlp-b.blob");
    shell_execute("rm /mnt/files/test-sched-ppo-a.blob");
    shell_execute("rm /mnt/files/test-sched-ppo-b.blob");
    shell_execute("rm /mnt/files/test-sched-config-a.blob");
    shell_execute("rm /mnt/files/test-sched-config-b.blob");
#endif
}

static void test_shell_cmd_sched_thresholds_lifecycle(void)
{
#ifdef CONFIG_AI_SCHEDULER
    static const char *path_a = "/mnt/files/test-sched-thresholds-a.blob";
    static const char *path_b = "/mnt/files/test-sched-thresholds-b.blob";
    uint8_t payload_a[64];
    uint8_t payload_b[64];
    uint8_t blob_a[128];
    uint8_t blob_b[128];
    size_t payload_len_a = build_sched_thresholds_payload(10u * 1000000u,
                                                          50u * 1000000u,
                                                          100u * 1000000u,
                                                          payload_a, sizeof(payload_a));
    size_t payload_len_b = build_sched_thresholds_payload(20u * 1000000u,
                                                          80u * 1000000u,
                                                          150u * 1000000u,
                                                          payload_b, sizeof(payload_b));
    size_t blob_len_a;
    size_t blob_len_b;

    TEST_ASSERT_TRUE(payload_len_a > 0);
    TEST_ASSERT_TRUE(payload_len_b > 0);
    blob_len_a = build_shell_test_blob(0x1004u, payload_a, payload_len_a, blob_a, sizeof(blob_a));
    blob_len_b = build_shell_test_blob(0x1004u, payload_b, payload_len_b, blob_b, sizeof(blob_b));
    TEST_ASSERT_TRUE(blob_len_a > 0);
    TEST_ASSERT_TRUE(blob_len_b > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_a, blob_a, blob_len_a));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_b, blob_b, blob_len_b));

    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear thresholds"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load thresholds /mnt/files/test-sched-thresholds-a.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate thresholds"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load thresholds /mnt/files/test-sched-thresholds-b.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate thresholds"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model rollback thresholds"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear thresholds"));
    shell_execute("rm /mnt/files/test-sched-thresholds-a.blob");
    shell_execute("rm /mnt/files/test-sched-thresholds-b.blob");
#endif
}

static void test_shell_cmd_sched_rebalance_lifecycle(void)
{
#ifdef CONFIG_AI_SCHEDULER
    static const char *path_a = "/mnt/files/test-sched-rebalance-a.blob";
    static const char *path_b = "/mnt/files/test-sched-rebalance-b.blob";
    uint8_t payload_a[64];
    uint8_t payload_b[64];
    uint8_t blob_a[128];
    uint8_t blob_b[128];
    size_t payload_len_a = build_sched_rebalance_payload(1u, 7u, 2u,
                                                         payload_a, sizeof(payload_a));
    size_t payload_len_b = build_sched_rebalance_payload(0u, 13u, 4u,
                                                         payload_b, sizeof(payload_b));
    size_t blob_len_a;
    size_t blob_len_b;

    TEST_ASSERT_TRUE(payload_len_a > 0);
    TEST_ASSERT_TRUE(payload_len_b > 0);
    blob_len_a = build_shell_test_blob(0x1005u, payload_a, payload_len_a, blob_a, sizeof(blob_a));
    blob_len_b = build_shell_test_blob(0x1005u, payload_b, payload_len_b, blob_b, sizeof(blob_b));
    TEST_ASSERT_TRUE(blob_len_a > 0);
    TEST_ASSERT_TRUE(blob_len_b > 0);
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_a, blob_a, blob_len_a));
    TEST_ASSERT_EQUAL_INT(0, write_binary_file(path_b, blob_b, blob_len_b));

    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear rebalance"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load rebalance /mnt/files/test-sched-rebalance-a.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate rebalance"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model load rebalance /mnt/files/test-sched-rebalance-b.blob"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model activate rebalance"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model rollback rebalance"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model status"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("sched model clear rebalance"));
    shell_execute("rm /mnt/files/test-sched-rebalance-a.blob");
    shell_execute("rm /mnt/files/test-sched-rebalance-b.blob");
#endif
}

/*
 * Regression for #37: model pin/unpin lifecycle. Load a model, pin it,
 * verify list shows it as pinned, unpin, verify. Also tests the
 * `model load mnist` built-in shortcut.
 */
static void test_shell_cmd_model_pin_lifecycle(void)
{
    TEST_ASSERT_EQUAL_INT(0, shell_execute("model load mnist"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("model pin 0"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("model list"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("model unpin 0"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("model unload 0"));
}

/*
 * Regression for #64: `model preload mnist` spawns a background task
 * to load the model. After yielding a few times, the model should
 * appear in the registry. `model preload-status` should report 'done'.
 */
static void test_shell_cmd_model_preload(void)
{
    /* Unload if already loaded from previous test. */
    int existing = rust_model_find("mnist");
    if (existing >= 0) {
        shell_execute("model unload mnist");
    }

    TEST_ASSERT_EQUAL_INT(0, shell_execute("model preload mnist"));

    /* Sleep to let the background task run. yield() alone may not
     * give enough scheduling cycles for the preload to complete. */
    extern void sleep_ms(uint32_t ms);
    sleep_ms(200);

    /* Model should be loaded now. */
    int idx = rust_model_find("mnist");
    TEST_ASSERT_TRUE(idx >= 0);

    /* Status should be 'done'. */
    TEST_ASSERT_EQUAL_INT(0, shell_execute("model preload-status"));

    /* Clean up. */
    shell_execute("model unload mnist");
}

/*
 * Test the preload-wait path: start a preload, then immediately call
 * `model preload-wait mnist` which should block until the background
 * task completes and return the model index.
 */
static void test_shell_cmd_model_preload_wait(void)
{
    int existing = rust_model_find("mnist");
    if (existing >= 0) {
        shell_execute("model unload mnist");
    }

    TEST_ASSERT_EQUAL_INT(0, shell_execute("model preload mnist"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("model preload-wait mnist 5000"));

    int idx = rust_model_find("mnist");
    TEST_ASSERT_TRUE(idx >= 0);

    shell_execute("model unload mnist");
}

/*
 * Test preload-wait when no preload is in-flight — should return
 * "not found" error without hanging.
 */
static void test_shell_cmd_model_preload_wait_not_inflight(void)
{
    int existing = rust_model_find("nonexistent");
    TEST_ASSERT_TRUE(existing < 0);
    /* preload-wait for a model that was never preloaded should fail. */
    int ret = shell_execute("model preload-wait nonexistent 100");
    TEST_ASSERT_NOT_EQUAL(0, ret);
}

/*
 * Test that /mnt/files/preload.conf exists at boot and contains
 * "mnist" (written by demo_init). This validates the config file
 * creation path. The actual boot preload is gated on !ENABLE_BOOT_TESTS
 * so it doesn't run during testing, but the file must be present.
 */
static void test_boot_preload_conf_exists(void)
{
    char buf[256] = {0};
    int bytes = vfs_read_path("/mnt/files/preload.conf", buf, sizeof(buf) - 1, 0);
    TEST_ASSERT_TRUE(bytes > 0);
    buf[bytes] = '\0';
    /* Should contain "mnist" somewhere. */
    extern char *strstr(const char *haystack, const char *needle);
    TEST_ASSERT_NOT_NULL(strstr(buf, "mnist"));
}

/* ============================================================================
 * VFS Command Tests (ls, cat) - Error Cases
 * ============================================================================ */

/*
 * Test: 'ls /nonexistent' returns error.
 */
static void test_shell_cmd_ls_nonexistent(void)
{
    int ret = shell_execute("ls /nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'cat' with no args shows usage (returns error).
 */
static void test_shell_cmd_cat_no_args(void)
{
    int ret = shell_execute("cat");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'cat /nonexistent' returns error.
 */
static void test_shell_cmd_cat_nonexistent(void)
{
    int ret = shell_execute("cat /nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'cat /sys' on directory returns error.
 */
static void test_shell_cmd_cat_directory(void)
{
    int ret = shell_execute("cat /sys");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * VFS Command Tests (ls, cat) - Success Cases
 * ============================================================================ */

/*
 * Test: 'ls /' lists root directory.
 */
static void test_shell_cmd_ls_root(void)
{
    int ret = shell_execute("ls /");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'ls /sys' lists sys directory.
 */
static void test_shell_cmd_ls_sys(void)
{
    int ret = shell_execute("ls /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'cat /sys/memory' reads memory info file.
 */
static void test_shell_cmd_cat_sys_memory(void)
{
    int ret = shell_execute("cat /sys/memory");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'cat /sys/cpus' reads CPU info file.
 */
static void test_shell_cmd_cat_sys_cpus(void)
{
    int ret = shell_execute("cat /sys/cpus");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * Verbose Status Command Tests
 * ============================================================================ */

/*
 * Test: 'help' lists available commands.
 */
static void test_shell_cmd_help(void)
{
    int ret = shell_execute("help");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'mem' shows memory statistics.
 */
static void test_shell_cmd_mem(void)
{
    int ret = shell_execute("mem");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'tasks' shows task list.
 */
static void test_shell_cmd_tasks(void)
{
    int ret = shell_execute("tasks");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'cpu' shows CPU information.
 */
static void test_shell_cmd_cpu(void)
{
    int ret = shell_execute("cpu");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'vmm' shows virtual memory mappings.
 */
static void test_shell_cmd_vmm(void)
{
    int ret = shell_execute("vmm");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'ipc' shows IPC statistics.
 */
static void test_shell_cmd_ipc(void)
{
    int ret = shell_execute("ipc");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'model' shows model memory info.
 */
static void test_shell_cmd_model(void)
{
    int ret = shell_execute("model");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'dtb' shows device tree info.
 */
static void test_shell_cmd_dtb(void)
{
    int ret = shell_execute("dtb");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * peek/poke Command Tests
 *
 * peek is read-only, poke writes a 32-bit word. Both accept a raw
 * physical address so they can probe identity-mapped MMIO. We use a
 * stack-allocated buffer as the test target — its address is a valid
 * C pointer that both commands dereference identically to MMIO. The
 * tests exercise argument parsing (missing / malformed / uppercase /
 * 0x-prefix) and round-trip write→read through the shell dispatcher.
 * ============================================================================ */

/* Build a stack-free hex string rendering of a pointer for shell_execute. */
static void hex_ptr(char *buf, size_t bufsz, const void *p)
{
    snprintf(buf, bufsz, "0x%lx", (unsigned long)(uintptr_t)p);
}

static void test_shell_cmd_peek_missing_args(void)
{
    int ret = shell_execute("peek");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_shell_cmd_peek_bad_hex(void)
{
    int ret = shell_execute("peek 0xZZZZ");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_shell_cmd_poke_missing_args(void)
{
    int r1 = shell_execute("poke");
    TEST_ASSERT_EQUAL_INT(-1, r1);
    int r2 = shell_execute("poke 0x1000");
    TEST_ASSERT_EQUAL_INT(-1, r2);
}

static void test_shell_cmd_poke_bad_hex_addr(void)
{
    int ret = shell_execute("poke notahex 0x1234");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_shell_cmd_poke_bad_hex_value(void)
{
    int ret = shell_execute("poke 0x1000 xyz");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_shell_cmd_poke_writes_word(void)
{
    /* Target: a dword-aligned heap-ish slot. Static so the storage
     * outlives this call frame even if the shell defers work. */
    static volatile uint32_t target;
    target = 0xDEADBEEFu;

    char cmd[96];
    char addr[32];
    hex_ptr(addr, sizeof(addr), (const void *)&target);
    snprintf(cmd, sizeof(cmd), "poke %s 0xCAFEBABE", addr);

    int ret = shell_execute(cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(0xCAFEBABEu, target);
}

static void test_shell_cmd_peek_reads_back_after_poke(void)
{
    static volatile uint32_t target;
    target = 0;

    char cmd[96];
    char addr[32];
    hex_ptr(addr, sizeof(addr), (const void *)&target);

    /* First poke, then peek (which only reports to UART; we assert
     * the memory state directly as a proxy for the read path). */
    snprintf(cmd, sizeof(cmd), "poke %s 0x11223344", addr);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(cmd));
    TEST_ASSERT_EQUAL_UINT32(0x11223344u, target);

    snprintf(cmd, sizeof(cmd), "peek %s", addr);
    TEST_ASSERT_EQUAL_INT(0, shell_execute(cmd));
    /* Memory unchanged by the read */
    TEST_ASSERT_EQUAL_UINT32(0x11223344u, target);
}

static void test_shell_cmd_poke_rejects_unaligned_addr(void)
{
    /* 0x1001 is not 4-byte aligned. Must be rejected before the write. */
    int ret = shell_execute("poke 0x1001 0xDEADBEEF");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_shell_cmd_poke_rejects_hex_too_long(void)
{
    /* 17 hex digits exceeds the uint64_t accumulator capacity —
     * the parser must reject rather than silently truncate. */
    int ret = shell_execute("poke 0x11111111111111111 0x1");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

static void test_shell_cmd_poke_accepts_uppercase_and_no_prefix(void)
{
    static volatile uint32_t target;
    target = 0;

    char cmd[96];
    char addr[32];
    /* Bare hex (no 0x prefix) with uppercase digits */
    snprintf(addr, sizeof(addr), "%lX", (unsigned long)(uintptr_t)&target);
    snprintf(cmd, sizeof(cmd), "poke %s ABCDEF01", addr);

    int ret = shell_execute(cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(0xABCDEF01u, target);
}

/* ============================================================================
 * timdiag Command Tests
 *
 * timdiag is the timer/interrupt delivery diagnostic added to investigate
 * hardware timer preemption on Pi 5 and Jetson. On QEMU, the GIC is a
 * single-security-state GICv2 where these probes are harmless — the command
 * should run to completion without crashing.
 * ============================================================================ */

#if !defined(PLATFORM_X86_64)
/*
 * Test: 'timdiag' with no args runs the safe diagnostic path.
 * On QEMU this dumps timer state + WFI test skip message.
 * Verifies that the diagnostic does not crash on the non-hardware path.
 */
static void test_shell_cmd_timdiag_no_args(void)
{
    int ret = shell_execute("timdiag");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'timdiag fiq' passes the explicit argument through.
 * On QEMU GICv2, this still skips the GICv3 FIQ test path, so it
 * should run to completion without crashing. The argument handling
 * only matters on Jetson GICv3.
 */
static void test_shell_cmd_timdiag_fiq_arg(void)
{
    int ret = shell_execute("timdiag fiq");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'timdiag' called repeatedly does not accumulate state.
 * Previous runs should not affect subsequent runs (idempotent).
 */
static void test_shell_cmd_timdiag_idempotent(void)
{
    int ret;
    for (int i = 0; i < 3; i++) {
        ret = shell_execute("timdiag");
        TEST_ASSERT_EQUAL_INT(0, ret);
    }
}

/*
 * Test: Unknown extra argument still runs the default safe path.
 */
static void test_shell_cmd_timdiag_unknown_arg(void)
{
    int ret = shell_execute("timdiag unknown");
    TEST_ASSERT_EQUAL_INT(0, ret);
}
#endif /* !PLATFORM_X86_64 */

/* ============================================================================
 * Component Command Tests
 * ============================================================================ */

/*
 * Test: 'component' with no args shows help (returns 0).
 */
static void test_shell_cmd_component_help(void)
{
    int ret = shell_execute("component");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'component list' shows empty list.
 */
static void test_shell_cmd_component_list_empty(void)
{
    cleanup_components();
    int ret = shell_execute("component list");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: 'component register' with missing args returns error.
 */
static void test_shell_cmd_component_register_missing_args(void)
{
    int ret = shell_execute("component register");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("component register mycomp");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("component register mycomp 1.0.0");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component register' with valid args succeeds.
 */
static void test_shell_cmd_component_register_valid(void)
{
    cleanup_components();

    int ret = shell_execute("component register test-svc 1.0.0 service");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify it was registered */
    int idx = component_find("test-svc");
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    cleanup_components();
}

/*
 * Test: 'component register' with invalid type returns error.
 */
static void test_shell_cmd_component_register_invalid_type(void)
{
    int ret = shell_execute("component register mycomp 1.0.0 invalid_type");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component unregister' with missing args returns error.
 */
static void test_shell_cmd_component_unregister_missing_args(void)
{
    int ret = shell_execute("component unregister");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component unregister' with invalid index returns error.
 */
static void test_shell_cmd_component_unregister_invalid(void)
{
    cleanup_components();
    int ret = shell_execute("component unregister 0");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component unregister' with valid index succeeds.
 */
static void test_shell_cmd_component_unregister_valid(void)
{
    cleanup_components();

    shell_execute("component register to-remove 1.0.0 service");
    TEST_ASSERT_EQUAL_UINT32(1, component_count());

    int ret = shell_execute("component unregister 0");
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(0, component_count());

    cleanup_components();
}

/*
 * Test: 'component status' with missing args returns error.
 */
static void test_shell_cmd_component_status_missing_args(void)
{
    int ret = shell_execute("component status");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component status' by index.
 */
static void test_shell_cmd_component_status_by_index(void)
{
    cleanup_components();

    shell_execute("component register status-test 1.0.0 driver");

    int ret = shell_execute("component status 0");
    TEST_ASSERT_EQUAL_INT(0, ret);

    cleanup_components();
}

/*
 * Test: 'component status' with nonexistent name returns error.
 */
static void test_shell_cmd_component_status_not_found(void)
{
    cleanup_components();
    int ret = shell_execute("component status nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'component' with unknown subcommand returns error.
 */
static void test_shell_cmd_component_unknown_subcmd(void)
{
    int ret = shell_execute("component foobar");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * Run Command Tests
 * ============================================================================ */

/*
 * Test: 'run' with unknown program returns error.
 */
static void test_shell_cmd_run_unknown_program(void)
{
    int ret = shell_execute("run nonexistent_program");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * Kill Command Tests
 * ============================================================================ */

/*
 * Test: 'kill' with no args shows usage.
 */
static void test_shell_cmd_kill_no_args(void)
{
    int ret = shell_execute("kill");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'kill' with invalid PID format returns error.
 */
static void test_shell_cmd_kill_invalid_pid(void)
{
    int ret = shell_execute("kill abc");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: 'kill' with nonexistent PID returns error.
 */
static void test_shell_cmd_kill_nonexistent_pid(void)
{
    int ret = shell_execute("kill 9999");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * Argument Parsing Tests
 * ============================================================================ */

/*
 * Test: Commands handle extra whitespace.
 */
static void test_shell_extra_whitespace(void)
{
    int ret = shell_execute("  clear  ");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("\tclear\t");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: Commands handle arguments with spaces between.
 */
static void test_shell_args_with_spaces(void)
{
    cleanup_components();

    /* Register with multiple args separated by spaces */
    int ret = shell_execute("component   register   spaced-comp   1.0.0   service");
    TEST_ASSERT_EQUAL_INT(0, ret);

    int idx = component_find("spaced-comp");
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    cleanup_components();
}

/* ============================================================================
 * Command Registration Tests
 * ============================================================================ */

/* Custom command handler for testing */
static int test_custom_cmd_called = 0;

static int custom_cmd_handler(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    test_custom_cmd_called = 1;
    return 42;  /* Distinctive return value */
}

/*
 * Test: External command registration and execution.
 */
static void test_shell_register_external_command(void)
{
    test_custom_cmd_called = 0;

    /* Test-fixture commands follow the `t_` prefix convention so the
     * help-coverage regression test (test_every_command_has_help_entry)
     * skips them without a per-name allow-list. See the comment in
     * that test for the rationale. */
    shell_cmd_t cmd = {
        .name = "t_testcmd",
        .handler = custom_cmd_handler,
        .help = "Test command",
        .category = SHELL_CAT_SHELL,  /* test fixture; category irrelevant */
    };

    int ret = shell_register_command(&cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Execute the custom command */
    ret = shell_execute("t_testcmd");
    TEST_ASSERT_EQUAL_INT(42, ret);
    TEST_ASSERT_EQUAL_INT(1, test_custom_cmd_called);
}

/* ============================================================================
 * Working Directory and Path Resolution Tests
 * ============================================================================ */

/*
 * Test: pwd command returns current directory.
 */
static void test_shell_cmd_pwd(void)
{
    /* First cd to root to ensure known state */
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd to root directory.
 */
static void test_shell_cmd_cd_root(void)
{
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd with no argument goes to root.
 */
static void test_shell_cmd_cd_no_arg(void)
{
    /* First cd somewhere else */
    shell_execute("cd /sys");

    /* cd with no arg should go to root */
    int ret = shell_execute("cd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify we're at root by listing */
    ret = shell_execute("ls");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd to valid directory works.
 */
static void test_shell_cmd_cd_valid_dir(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* pwd should now be /sys */
    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset to root */
    shell_execute("cd /");
}

/*
 * Test: cd to nonexistent directory fails.
 */
static void test_shell_cmd_cd_nonexistent(void)
{
    int ret = shell_execute("cd /nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: cd to file fails (not a directory).
 */
static void test_shell_cmd_cd_file(void)
{
    int ret = shell_execute("cd /sys/memory");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: cd with .. goes to parent directory.
 */
static void test_shell_cmd_cd_dotdot(void)
{
    /* Go to /sys first */
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cd .. should go back to / */
    ret = shell_execute("cd ..");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify at root */
    ret = shell_execute("ls /");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd .. at root stays at root.
 */
static void test_shell_cmd_cd_dotdot_at_root(void)
{
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cd .. at root should stay at root */
    ret = shell_execute("cd ..");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd with . stays in current directory.
 */
static void test_shell_cmd_cd_dot(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cd . should stay in /sys */
    ret = shell_execute("cd .");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: ls with no argument uses cwd.
 */
static void test_shell_cmd_ls_cwd(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls with no arg should list /sys */
    ret = shell_execute("ls");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: ls with relative path.
 */
static void test_shell_cmd_ls_relative(void)
{
    int ret = shell_execute("cd /");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls sys should work as relative path */
    ret = shell_execute("ls sys");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: ls with . path.
 */
static void test_shell_cmd_ls_dot(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls . should list current directory */
    ret = shell_execute("ls .");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: ls with .. path.
 */
static void test_shell_cmd_ls_dotdot(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* ls .. should list parent (root) */
    ret = shell_execute("ls ..");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: cat with relative path.
 */
static void test_shell_cmd_cat_relative(void)
{
    int ret = shell_execute("cd /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cat memory should work as relative path */
    ret = shell_execute("cat memory");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Path resolution with complex path (multiple ..).
 */
static void test_shell_path_complex(void)
{
    /* cd to a path with multiple .. */
    int ret = shell_execute("cd /sys/../proc/../sys");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Should be in /sys */
    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Path resolution with trailing slashes.
 */
static void test_shell_path_trailing_slash(void)
{
    int ret = shell_execute("cd /sys/");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Path resolution with double slashes.
 */
static void test_shell_path_double_slash(void)
{
    int ret = shell_execute("ls //sys//");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: cd to mount point subdirectory.
 */
static void test_shell_cmd_cd_mount_subdir(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("pwd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* List should work */
    ret = shell_execute("ls");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: Relative path in mounted filesystem.
 */
static void test_shell_mount_relative_path(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* cat hello.txt should work */
    ret = shell_execute("cat hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/*
 * Test: df with relative path (cwd in mount).
 */
static void test_shell_cmd_df_cwd(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* df with no arg should show current filesystem */
    ret = shell_execute("df");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Reset */
    shell_execute("cd /");
}

/* ============================================================================
 * New Filesystem Commands Tests (cp, touch, stat, tree, wc, hexdump, grep, find)
 * ============================================================================ */

/*
 * Test: touch creates empty file - VERIFIES FILE SIZE IS 0.
 */
static void test_shell_cmd_touch(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("touch testtouch.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify file exists AND is empty (0 bytes) */
    int size = get_file_size("/mnt/files/testtouch.tmp");
    TEST_ASSERT_EQUAL_INT(0, size);

    /* Cleanup */
    shell_execute("rm testtouch.tmp");
    shell_execute("cd /");
}

/*
 * Test: touch on existing file doesn't truncate.
 */
static void test_shell_cmd_touch_existing(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create file with content */
    ret = shell_execute("write touch_exist.tmp Hello123");
    TEST_ASSERT_EQUAL_INT(0, ret);

    int orig_size = get_file_size("/mnt/files/touch_exist.tmp");
    TEST_ASSERT_TRUE(orig_size > 0);

    /* Touch should NOT truncate */
    ret = shell_execute("touch touch_exist.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify size unchanged */
    int new_size = get_file_size("/mnt/files/touch_exist.tmp");
    TEST_ASSERT_EQUAL_INT(orig_size, new_size);

    /* Cleanup */
    shell_execute("rm touch_exist.tmp");
    shell_execute("cd /");
}

/*
 * Test: cp copies a file - VERIFIES CONTENT IS IDENTICAL.
 */
static void test_shell_cmd_cp(void)
{
    const char *test_content = "Hello from cp test!";
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create source file */
    ret = shell_execute("write cpsrc.tmp Hello from cp test!");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Copy it */
    ret = shell_execute("cp cpsrc.tmp cpdst.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify destination has same content */
    char buf[64] = {0};
    int bytes = read_file_content("/mnt/files/cpdst.tmp", buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(bytes > 0);
    TEST_ASSERT_EQUAL_STRING(test_content, buf);

    /* Verify sizes match */
    int src_size = get_file_size("/mnt/files/cpsrc.tmp");
    int dst_size = get_file_size("/mnt/files/cpdst.tmp");
    TEST_ASSERT_EQUAL_INT(src_size, dst_size);

    /* Cleanup */
    shell_execute("rm cpsrc.tmp");
    shell_execute("rm cpdst.tmp");
    shell_execute("cd /");
}

/*
 * Test: cp copies binary content correctly.
 */
static void test_shell_cmd_cp_binary(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Write bytes including special chars via shell */
    ret = shell_execute("write cpbin.tmp ABCDEFGHIJ1234567890");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Copy */
    ret = shell_execute("cp cpbin.tmp cpbin2.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Read both and compare */
    char buf1[64] = {0}, buf2[64] = {0};
    int bytes1 = read_file_content("/mnt/files/cpbin.tmp", buf1, sizeof(buf1) - 1);
    int bytes2 = read_file_content("/mnt/files/cpbin2.tmp", buf2, sizeof(buf2) - 1);

    TEST_ASSERT_EQUAL_INT(bytes1, bytes2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf1, buf2, (size_t)bytes1));

    /* Cleanup */
    shell_execute("rm cpbin.tmp");
    shell_execute("rm cpbin2.tmp");
    shell_execute("cd /");
}

/*
 * Test: cp with source not found fails.
 */
static void test_shell_cmd_cp_not_found(void)
{
    int ret = shell_execute("cp /mnt/files/nonexistent.tmp /mnt/files/dst.tmp");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: cp with missing args fails.
 */
static void test_shell_cmd_cp_missing_args(void)
{
    int ret = shell_execute("cp");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("cp /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: stat shows file information - VERIFIES SIZE.
 */
static void test_shell_cmd_stat_file(void)
{
    /* hello.txt exists with known content */
    int ret = shell_execute("stat /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify we can get its size (should be non-zero) */
    int size = get_file_size("/mnt/files/hello.txt");
    TEST_ASSERT_TRUE(size > 0);
}

/*
 * Test: stat shows directory information.
 */
static void test_shell_cmd_stat_dir(void)
{
    int ret = shell_execute("stat /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: standard /mnt/files subdirectories exist for tooling contracts.
 */
static void test_shell_cmd_stat_standard_storage_dirs(void)
{
    struct vfs_entry_info info;

    TEST_ASSERT_EQUAL_INT(0, vfs_stat_path("/mnt/files/policies", &info));
    TEST_ASSERT_EQUAL_UINT8(1, info.type);

    TEST_ASSERT_EQUAL_INT(0, vfs_stat_path("/mnt/files/models", &info));
    TEST_ASSERT_EQUAL_UINT8(1, info.type);

    TEST_ASSERT_EQUAL_INT(0, vfs_stat_path("/mnt/files/autoload", &info));
    TEST_ASSERT_EQUAL_UINT8(1, info.type);
}

/*
 * Test: stat on nonexistent fails.
 */
static void test_shell_cmd_stat_not_found(void)
{
    int ret = shell_execute("stat /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: stat on virtual file.
 */
static void test_shell_cmd_stat_virtual(void)
{
    int ret = shell_execute("stat /sys/memory");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: stat with missing args.
 */
static void test_shell_cmd_stat_missing_args(void)
{
    int ret = shell_execute("stat");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: tree lists directory recursively.
 */
static void test_shell_cmd_tree(void)
{
    int ret = shell_execute("tree /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: tree with subdirectory structure.
 */
static void test_shell_cmd_tree_subdir(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create subdirectory with file */
    ret = shell_execute("mkdir tree_test_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("write tree_test_dir/nested.txt nested content");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Tree should show subdirectory and its contents */
    ret = shell_execute("tree /mnt/files/tree_test_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Cleanup */
    shell_execute("rm tree_test_dir/nested.txt");
    shell_execute("rm tree_test_dir");
    shell_execute("cd /");
}

/*
 * Test: tree with depth limit.
 */
static void test_shell_cmd_tree_depth(void)
{
    int ret = shell_execute("tree /mnt/files 2");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: tree on virtual directory.
 */
static void test_shell_cmd_tree_virtual(void)
{
    int ret = shell_execute("tree /sys");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: tree on root.
 */
static void test_shell_cmd_tree_root(void)
{
    int ret = shell_execute("tree / 2");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: wc counts lines, words, bytes - VERIFIES COMMAND RUNS.
 */
static void test_shell_cmd_wc(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create file with known content */
    ret = shell_execute("write wc_test.tmp line1 word2 word3");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("wc wc_test.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Cleanup */
    shell_execute("rm wc_test.tmp");
    shell_execute("cd /");
}

/*
 * Test: wc on known file verifies size.
 */
static void test_shell_cmd_wc_known_content(void)
{
    /* hello.txt has "Hello from LittleFS!" = 20 bytes, 1 line, 3 words */
    int ret = shell_execute("wc /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: wc on nonexistent fails.
 */
static void test_shell_cmd_wc_not_found(void)
{
    int ret = shell_execute("wc /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: wc missing args.
 */
static void test_shell_cmd_wc_missing_args(void)
{
    int ret = shell_execute("wc");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: hexdump shows hex output.
 */
static void test_shell_cmd_hexdump(void)
{
    int ret = shell_execute("hexdump /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: hexdump with offset and length.
 */
static void test_shell_cmd_hexdump_offset(void)
{
    int ret = shell_execute("hexdump /mnt/files/hello.txt 0 16");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: hexdump with offset in middle of file.
 */
static void test_shell_cmd_hexdump_middle(void)
{
    int ret = shell_execute("hexdump /mnt/files/hello.txt 6 10");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: hexdump on nonexistent fails.
 */
static void test_shell_cmd_hexdump_not_found(void)
{
    int ret = shell_execute("hexdump /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: hexdump missing args.
 */
static void test_shell_cmd_hexdump_missing_args(void)
{
    int ret = shell_execute("hexdump");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: grep finds pattern in file.
 */
static void test_shell_cmd_grep(void)
{
    /* The hello.txt file contains "Hello from LittleFS!" */
    int ret = shell_execute("grep Hello /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: grep finds pattern in middle of line.
 */
static void test_shell_cmd_grep_middle(void)
{
    /* Search for "from" in "Hello from LittleFS!" */
    int ret = shell_execute("grep from /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: grep case sensitivity.
 */
static void test_shell_cmd_grep_case(void)
{
    /* "hello" (lowercase) should NOT match "Hello" */
    int ret = shell_execute("grep hello /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);  /* Returns 0 but shows "0 matches" */
}

/*
 * Test: grep with no match.
 */
static void test_shell_cmd_grep_no_match(void)
{
    int ret = shell_execute("grep NOTFOUND /mnt/files/hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);  /* Returns 0, prints "no matches" */
}

/*
 * Test: grep on nonexistent fails.
 */
static void test_shell_cmd_grep_not_found(void)
{
    int ret = shell_execute("grep pattern /mnt/files/nonexistent");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: grep missing args.
 */
static void test_shell_cmd_grep_missing_args(void)
{
    int ret = shell_execute("grep");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("grep pattern");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: grep with multiline file.
 */
static void test_shell_cmd_grep_multiline(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* readme.txt has multiple lines */
    ret = shell_execute("grep SLM-OS /mnt/files/readme.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    shell_execute("cd /");
}

/*
 * Test: find locates files by exact pattern.
 */
static void test_shell_cmd_find(void)
{
    int ret = shell_execute("find /mnt/files hello.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find with * wildcard.
 */
static void test_shell_cmd_find_wildcard(void)
{
    int ret = shell_execute("find /mnt/files *.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find with leading wildcard.
 */
static void test_shell_cmd_find_leading_wildcard(void)
{
    int ret = shell_execute("find /mnt/files *lo.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find with no matches.
 */
static void test_shell_cmd_find_no_match(void)
{
    int ret = shell_execute("find /mnt/files *.xyz");
    TEST_ASSERT_EQUAL_INT(0, ret);  /* Returns 0, prints "no files found" */
}

/*
 * Test: find with question mark wildcard.
 */
static void test_shell_cmd_find_question(void)
{
    int ret = shell_execute("find /mnt/files hell?.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: find in subdirectory.
 */
static void test_shell_cmd_find_subdir(void)
{
    int ret = shell_execute("cd /mnt/files");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Create subdirectory with file */
    ret = shell_execute("mkdir find_test_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("write find_test_dir/target.txt found me");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Find should locate it */
    ret = shell_execute("find /mnt/files target.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Also find with wildcard */
    ret = shell_execute("find /mnt/files *.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Cleanup */
    shell_execute("rm find_test_dir/target.txt");
    shell_execute("rm find_test_dir");
    shell_execute("cd /");
}

/*
 * Test: find missing args.
 */
static void test_shell_cmd_find_missing_args(void)
{
    int ret = shell_execute("find");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    ret = shell_execute("find /mnt/files");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: find on non-mount path returns error.
 * (find requires a mounted filesystem, not virtual directories)
 */
static void test_shell_cmd_find_nonmount(void)
{
    int ret = shell_execute("find /sys mem*");
    TEST_ASSERT_EQUAL_INT(-1, ret);  /* Expected: not a mounted filesystem */
}

/* ============================================================================
 * Help System Tests
 * ============================================================================ */

/*
 * Test: help with no argument lists all commands.
 */
static void test_shell_cmd_help_list(void)
{
    int ret = shell_execute("help");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: help with valid command shows detailed help.
 */
static void test_shell_cmd_help_valid(void)
{
    int ret = shell_execute("help cp");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("help ls");
    TEST_ASSERT_EQUAL_INT(0, ret);

    ret = shell_execute("help grep");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: help with unknown command returns error.
 */
static void test_shell_cmd_help_unknown(void)
{
    int ret = shell_execute("help nonexistent_command");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Convention test: builtin_commands[] must be grouped by category and
 * alphabetized within each group. This pins the source-side rule
 * documented in the comment block over `builtin_commands[]` in
 * kernel/src/shell.c.
 *
 * Why source-invariant (not just trust the runtime sort)?
 *   - cmd_help re-sorts at runtime, so even a jumbled source produces
 *     correct output. But the convention has a second purpose: a
 *     human reading the registration list sees the same ordering as
 *     the help output. This test catches drift where someone adds a
 *     command to the wrong category block, or out of alphabetical
 *     order, before that drift becomes a maintenance hazard.
 *   - The runtime sort uses the same comparator, so by construction
 *     "source is grouped+sorted" implies "runtime output is too".
 */
static void test_builtin_commands_grouped_and_sorted(void)
{
    /* Track which categories we've already seen close (i.e. the run
     * of entries with that category has ended and a new category
     * started). Re-encountering a closed category means entries are
     * not contiguous. */
    bool seen_closed[SHELL_CAT_COUNT] = {false};
    shell_cmd_category_t prev_cat = (shell_cmd_category_t)-1;
    const char *prev_name_in_cat = NULL;

    for (int i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
        const shell_cmd_t *cmd = &builtin_commands[i];

        /* Category must be in valid range. */
        TEST_ASSERT_MESSAGE((unsigned)cmd->category < SHELL_CAT_COUNT,
                            "category out of range");

        if ((shell_cmd_category_t)cmd->category != prev_cat) {
            /* Starting a new category. Must not have seen its
             * "closed" mark — that would mean an interleaved entry. */
            TEST_ASSERT_MESSAGE(!seen_closed[cmd->category],
                "category run not contiguous in builtin_commands[] — "
                "every category's entries must be in one block");

            /* Mark the previous category closed (if there was one). */
            if ((int)prev_cat >= 0) {
                seen_closed[prev_cat] = true;
            }
            prev_cat = (shell_cmd_category_t)cmd->category;
            prev_name_in_cat = NULL;
        }

        /* Within a category, names must be in strict ascending order. */
        if (prev_name_in_cat != NULL) {
            int cmp = unity_strcmp(prev_name_in_cat, cmd->name);
            TEST_ASSERT_MESSAGE(cmp < 0,
                "entries within a category must be alphabetized");
        }
        prev_name_in_cat = cmd->name;
    }
}

/*
 * Convention test (externals): every shell_register_command() entry
 * must have a `.category` value in the valid range.
 *
 * Why range-only (not grouping + alphabetization)?
 *   - external_commands[] is a single flat array filled by
 *     shell_register_command() calls scattered across many .c files
 *     (lua_shell.c, net_shell.c, kernel_cmd.c, hailo_shell.c, ...).
 *     Source-array provenance is lost at flatten time, so the
 *     "grouped by category, alphabetized within" rule that applies
 *     within each per-file array is not observable here.
 *   - The thing this test does catch is the partial-init foot-gun:
 *     a `shell_cmd_t` literal that omits `.category` zero-initialises
 *     it to SHELL_CAT_SHELL. The struct-level comment in shell.h
 *     warns about it; this test fires when the warning is missed and
 *     a forgotten field ends up out of range (e.g. set to SHELL_CAT_COUNT
 *     by a typo).
 */
static void test_external_commands_categories_in_range(void)
{
    for (int i = 0; i < num_external_commands; i++) {
        const shell_cmd_t *cmd = &external_commands[i];
        TEST_ASSERT_MESSAGE((unsigned)cmd->category < SHELL_CAT_COUNT,
                            "external command category out of range — "
                            "every shell_register_command() entry must "
                            "set .category to a SHELL_CAT_* enum value");
    }
}

/*
 * Test: help files exist in /mnt/files/help/ directory.
 */
static void test_shell_help_files_exist(void)
{
    /* Verify some help files exist */
    char buf[64];

    int bytes = vfs_read_path("/mnt/files/help/cp.txt", buf, sizeof(buf), 0);
    TEST_ASSERT_TRUE(bytes > 0);

    bytes = vfs_read_path("/mnt/files/help/ls.txt", buf, sizeof(buf), 0);
    TEST_ASSERT_TRUE(bytes > 0);

    bytes = vfs_read_path("/mnt/files/help/help.txt", buf, sizeof(buf), 0);
    TEST_ASSERT_TRUE(bytes > 0);
}

/*
 * Test: help file contains expected content (non-empty and reasonable size).
 */
static void test_shell_help_file_content(void)
{
    char buf[256];

    /* Read cp help file */
    int bytes = vfs_read_path("/mnt/files/help/cp.txt", buf, sizeof(buf) - 1, 0);
    TEST_ASSERT_TRUE(bytes > 50);  /* Should have substantial help text */

    /* Read help help file */
    bytes = vfs_read_path("/mnt/files/help/help.txt", buf, sizeof(buf) - 1, 0);
    TEST_ASSERT_TRUE(bytes > 50);  /* Should have substantial help text */
}

/*
 * Coverage test: every registered command has a help_entries[] entry.
 *
 * Walks both builtin_commands[] (compiled-in) and external_commands[]
 * (runtime-registered by lua/net/hailo/kernel/...) and asserts
 * help_exists(name) returns 1 for each. Catches the failure mode where
 * a new shell command is added without a matching HELP_TEXT entry —
 * the user-visible symptom is `help <cmd>` printing "No help available
 * for '<cmd>'" instead of the actual help. Pre-test, this had drifted
 * to 27 missing entries (admin/bench/sched/eviction/imx219/telemetry/
 * top/peek/poke/sleep/msg + the platform-specific diagnostics).
 *
 * Hailo / GPU / etc. external commands are only registered when their
 * subsystem inits during boot, so the external_commands[] walk only
 * verifies what's actually on this build's surface — fine, since
 * help_exists() is a static lookup table not affected by which
 * commands are live.
 */
static void test_every_command_has_help_entry(void)
{
    for (int i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
        const shell_cmd_t *cmd = &builtin_commands[i];
        if (!help_exists(cmd->name)) {
            uart_printf("  [FAIL] no help entry for built-in '%s'\r\n",
                        cmd->name);
        }
        TEST_ASSERT_MESSAGE(help_exists(cmd->name),
            "every built-in command must have a HELP_TEXT entry "
            "in kernel/src/help.c");
    }
    for (int i = 0; i < num_external_commands; i++) {
        const shell_cmd_t *cmd = &external_commands[i];
        /* Skip in-test fixture commands registered by other tests via
         * the `t_` prefix convention. Fixtures are transient and the
         * "every command has a help entry" rule only applies to
         * production registrations. New test fixtures must use a `t_`
         * prefix so this skip works without per-name maintenance —
         * see test_shell_register_external_command and the entries in
         * test_shell_session.c (t_nested_inner / t_nested_outer). */
        if (strncmp(cmd->name, "t_", 2) == 0) {
            continue;
        }
        if (!help_exists(cmd->name)) {
            uart_printf("  [FAIL] no help entry for external '%s'\r\n",
                        cmd->name);
        }
        TEST_ASSERT_MESSAGE(help_exists(cmd->name),
            "every shell_register_command() entry must have a "
            "HELP_TEXT entry in kernel/src/help.c");
    }
}

/*
 * Test: ls /mnt/files/help shows help files.
 */
static void test_shell_help_dir_listing(void)
{
    int ret = shell_execute("ls /mnt/files/help");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/* ============================================================================
 * Write/Modify Command Tests
 *
 * Tests for shell commands that create, modify, or remove files:
 * write, mkdir, rm, mv, append, truncate.
 * ============================================================================ */

/*
 * Test: write command creates a file with content.
 */
static void test_shell_cmd_write(void)
{
    int ret = shell_execute("write /mnt/files/write_test.tmp hello world");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Verify file was created by catting it */
    ret = shell_execute("cat /mnt/files/write_test.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/write_test.tmp");
}

/*
 * Test: write with no arguments returns error.
 */
static void test_shell_cmd_write_no_args(void)
{
    int ret = shell_execute("write");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: write translates \n / \t / \\ escapes into real bytes.
 */
static void test_shell_cmd_write_escapes(void)
{
    const char *path = "/mnt/files/write_esc.tmp";
    int ret = shell_execute("write /mnt/files/write_esc.tmp line1\\nline2\\there\\\\done");
    TEST_ASSERT_EQUAL_INT(0, ret);

    char buf[64];
    int n = read_file_content(path, buf, sizeof(buf) - 1);
    TEST_ASSERT_GREATER_THAN(0, n);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("line1\nline2\there\\done", buf);

    shell_execute("rm /mnt/files/write_esc.tmp");
}

/*
 * Test: write \xNN translates to a single byte.
 */
static void test_shell_cmd_write_hex_escape(void)
{
    const char *path = "/mnt/files/write_hex.tmp";
    /* \x41 = 'A', \x42 = 'B' */
    int ret = shell_execute("write /mnt/files/write_hex.tmp \\x41\\x42C");
    TEST_ASSERT_EQUAL_INT(0, ret);

    char buf[16];
    int n = read_file_content(path, buf, sizeof(buf) - 1);
    TEST_ASSERT_EQUAL_INT(3, n);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("ABC", buf);

    shell_execute("rm /mnt/files/write_hex.tmp");
}

/*
 * Test: write accepts content larger than the old 512-byte limit.
 */
static void test_shell_cmd_write_large_content(void)
{
    const char *path = "/mnt/files/write_big.tmp";
    /* Build a command line that is below SHELL_MAX_LINE but above 512 bytes
     * of content (the previous internal cap). */
    char cmd[SHELL_MAX_LINE];
    extern void *memset(void *s, int c, size_t n);
    int prefix = 0;
    const char *header = "write /mnt/files/write_big.tmp ";
    while (header[prefix]) { cmd[prefix] = header[prefix]; prefix++; }
    int payload_len = SHELL_MAX_LINE - prefix - 1;
    if (payload_len > 800) payload_len = 800;  /* well above old 512 cap */
    memset(cmd + prefix, 'a', payload_len);
    cmd[prefix + payload_len] = '\0';

    int ret = shell_execute(cmd);
    TEST_ASSERT_EQUAL_INT(0, ret);

    int sz = get_file_size(path);
    TEST_ASSERT_EQUAL_INT(payload_len, sz);

    shell_execute("rm /mnt/files/write_big.tmp");
}

/*
 * Test: put writes binary bytes decoded from hex.
 */
static void test_shell_cmd_put(void)
{
    const char *path = "/mnt/files/put_test.bin";
    int ret = shell_execute("put /mnt/files/put_test.bin 000102ff4142");
    TEST_ASSERT_EQUAL_INT(0, ret);

    uint8_t buf[8];
    int n = read_file_content(path, (char *)buf, sizeof(buf));
    uint8_t expected[] = {0x00, 0x01, 0x02, 0xff, 0x41, 0x42};
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, sizeof(expected));

    shell_execute("rm /mnt/files/put_test.bin");
}

/*
 * Test: put -a appends another binary chunk.
 */
static void test_shell_cmd_put_append(void)
{
    const char *path = "/mnt/files/put_append.bin";
    int ret = shell_execute("put /mnt/files/put_append.bin aabb");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("put -a /mnt/files/put_append.bin ccdd");
    TEST_ASSERT_EQUAL_INT(0, ret);

    uint8_t buf[8];
    int n = read_file_content(path, (char *)buf, sizeof(buf));
    uint8_t expected[] = {0xaa, 0xbb, 0xcc, 0xdd};
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, sizeof(expected));

    shell_execute("rm /mnt/files/put_append.bin");
}

/*
 * Test: put rejects malformed hex.
 */
static void test_shell_cmd_put_invalid_hex(void)
{
    int ret = shell_execute("put /mnt/files/put_bad.bin 0xz1");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: put rejects missing arguments.
 */
static void test_shell_cmd_put_no_args(void)
{
    int ret = shell_execute("put");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: xput framed upload lifecycle writes exact bytes.
 */
static void test_shell_cmd_xput_lifecycle(void)
{
    const char *path = "/mnt/files/xput_test.bin";
    int ret = shell_execute("xput begin /mnt/files/xput_test.bin 4");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput chunk 0 aabb");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput chunk 2 ccdd");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput finish");
    TEST_ASSERT_EQUAL_INT(0, ret);

    uint8_t buf[8];
    int n = read_file_content(path, (char *)buf, sizeof(buf));
    uint8_t expected[] = {0xaa, 0xbb, 0xcc, 0xdd};
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, sizeof(expected));

    shell_execute("rm /mnt/files/xput_test.bin");
}

/*
 * Test: xput rejects offset mismatch.
 */
static void test_shell_cmd_xput_offset_mismatch(void)
{
    int ret = shell_execute("xput begin /mnt/files/xput_bad.bin 4");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput chunk 1 aabb");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    ret = shell_execute("xput abort");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: xput finish rejects incomplete upload.
 */
static void test_shell_cmd_xput_incomplete_finish(void)
{
    int ret = shell_execute("xput begin /mnt/files/xput_short.bin 4");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput chunk 0 aabb");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput finish");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    ret = shell_execute("xput abort");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

/*
 * Test: xput state is scoped to the current shell session.
 */
static void test_shell_cmd_xput_isolated_per_session(void)
{
    struct task *t = task_current();
    struct shell_session *orig = shell_session_current();
    struct shell_session *a = shell_session_alloc();
    struct shell_session *b = shell_session_alloc();
    const char *path_a = "/mnt/files/xput_session_a.bin";
    const char *path_b = "/mnt/files/xput_session_b.bin";
    uint8_t buf[8];
    uint8_t expect_a[] = {0xaa, 0xbb, 0xcc, 0xdd};
    uint8_t expect_b[] = {0x11, 0x22, 0x33, 0x44};
    int n;

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    shell_session_bind(t, a);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("xput begin /mnt/files/xput_session_a.bin 4"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("xput chunk 0 aabb"));

    shell_session_bind(t, b);
    TEST_ASSERT_EQUAL_INT(-1, shell_execute("xput finish"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("xput begin /mnt/files/xput_session_b.bin 4"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("xput chunk 0 11223344"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("xput finish"));

    shell_session_bind(t, a);
    TEST_ASSERT_EQUAL_INT(0, shell_execute("xput chunk 2 ccdd"));
    TEST_ASSERT_EQUAL_INT(0, shell_execute("xput finish"));

    n = read_file_content(path_a, (char *)buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)sizeof(expect_a), n);
    TEST_ASSERT_EQUAL_MEMORY(expect_a, buf, sizeof(expect_a));
    n = read_file_content(path_b, (char *)buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)sizeof(expect_b), n);
    TEST_ASSERT_EQUAL_MEMORY(expect_b, buf, sizeof(expect_b));

    shell_execute("rm /mnt/files/xput_session_a.bin");
    shell_execute("rm /mnt/files/xput_session_b.bin");
    shell_session_bind(t, orig);
    shell_session_free(a);
    shell_session_free(b);
}

/*
 * Test: xput resume picks up an existing partial file without
 * truncating it. Models the TCP-disconnect-then-reconnect path —
 * `shell_xput_session_close_for` is the teardown that
 * shell_session_free runs when a TCP shell session closes, which is
 * what discards the in-memory xput state. After that runs, the disk
 * still has the bytes; `xput resume` must see them, report the
 * correct received offset, and let `xput chunk <received>` continue
 * the upload from there. Pre-fix, the only path forward was
 * `xput begin` which LFS_O_TRUNCs the file and starts over.
 */
static void test_shell_cmd_xput_resume_after_disconnect(void)
{
    const char *path = "/mnt/files/xput_resume.bin";

    /* Phase 1: begin + first chunk, then simulate TCP close. */
    int ret = shell_execute("xput begin /mnt/files/xput_resume.bin 4");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput chunk 0 aabb");
    TEST_ASSERT_EQUAL_INT(0, ret);

    shell_xput_session_close_for(shell_session_current());

    /* Status should be inactive — the in-memory session is gone. */
    ret = shell_execute("xput status");
    TEST_ASSERT_EQUAL_INT(0, ret);   /* status returns 0 even when inactive */

    /* Phase 2: resume from disk. */
    ret = shell_execute("xput resume /mnt/files/xput_resume.bin 4");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Phase 3: continue from offset 2. */
    ret = shell_execute("xput chunk 2 ccdd");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput finish");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Verify the full file is correct — first half from begin/chunk0,
     * second half from resume/chunk2. If begin had truncated, we would
     * see {0xcc, 0xdd, ...} instead. */
    uint8_t buf[8];
    int n = read_file_content(path, (char *)buf, sizeof(buf));
    uint8_t expected[] = {0xaa, 0xbb, 0xcc, 0xdd};
    TEST_ASSERT_EQUAL_INT((int)sizeof(expected), n);
    TEST_ASSERT_EQUAL_MEMORY(expected, buf, sizeof(expected));

    shell_execute("rm /mnt/files/xput_resume.bin");
}

/*
 * Test: xput resume rejects a request for a file that does not exist.
 * Calling code must fall back to `xput begin` in that case (which
 * the slm-put.py begin_or_resume_framed helper does).
 */
static void test_shell_cmd_xput_resume_missing_file(void)
{
    int ret = shell_execute("xput resume /mnt/files/xput_no_such.bin 100");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: xput resume rejects a request whose declared total is smaller
 * than what's already on disk — the on-disk file would no longer fit
 * the protocol, and silently truncating in resume would defeat the
 * point. Caller can `xput begin` (which truncates) if they really
 * want to overwrite.
 */
static void test_shell_cmd_xput_resume_existing_too_large(void)
{
    /* Set up a 4-byte file. */
    int ret = shell_execute("xput begin /mnt/files/xput_resume_big.bin 4");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput chunk 0 aabbccdd");
    TEST_ASSERT_EQUAL_INT(0, ret);
    ret = shell_execute("xput finish");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Resume with a smaller total — should be rejected. */
    ret = shell_execute("xput resume /mnt/files/xput_resume_big.bin 2");
    TEST_ASSERT_EQUAL_INT(-1, ret);

    shell_execute("rm /mnt/files/xput_resume_big.bin");
}

/*
 * Test: mkdir creates a directory.
 */
static void test_shell_cmd_mkdir(void)
{
    int ret = shell_execute("mkdir /mnt/files/test_mkdir_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Verify dir exists by listing it */
    ret = shell_execute("ls /mnt/files/test_mkdir_dir");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/test_mkdir_dir");
}

/*
 * Test: mkdir with no arguments returns error.
 */
static void test_shell_cmd_mkdir_no_args(void)
{
    int ret = shell_execute("mkdir");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: rm removes a file.
 */
static void test_shell_cmd_rm(void)
{
    shell_execute("write /mnt/files/rm_test.tmp data");
    int ret = shell_execute("rm /mnt/files/rm_test.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Verify file is gone */
    ret = shell_execute("cat /mnt/files/rm_test.tmp");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: rm on nonexistent file returns error.
 */
static void test_shell_cmd_rm_nonexistent(void)
{
    int ret = shell_execute("rm /mnt/files/does_not_exist_12345");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: mv renames a file.
 */
static void test_shell_cmd_mv(void)
{
    shell_execute("write /mnt/files/mv_src.tmp move me");
    int ret = shell_execute("mv /mnt/files/mv_src.tmp /mnt/files/mv_dst.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    /* Source should be gone */
    ret = shell_execute("cat /mnt/files/mv_src.tmp");
    TEST_ASSERT_EQUAL_INT(-1, ret);
    /* Dest should exist */
    ret = shell_execute("cat /mnt/files/mv_dst.tmp");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/mv_dst.tmp");
}

/*
 * Test: mv with missing arguments returns error.
 */
static void test_shell_cmd_mv_missing_args(void)
{
    int ret = shell_execute("mv");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: append adds content to an existing file.
 */
static void test_shell_cmd_append(void)
{
    shell_execute("write /mnt/files/append_test.tmp first");
    int ret = shell_execute("append /mnt/files/append_test.tmp second");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/append_test.tmp");
}

/*
 * Test: append with no arguments returns error.
 */
static void test_shell_cmd_append_no_args(void)
{
    int ret = shell_execute("append");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/*
 * Test: truncate shortens a file to a given size.
 */
static void test_shell_cmd_truncate(void)
{
    shell_execute("write /mnt/files/trunc_test.tmp some long content here");
    int ret = shell_execute("truncate /mnt/files/trunc_test.tmp 4");
    TEST_ASSERT_EQUAL_INT(0, ret);
    shell_execute("rm /mnt/files/trunc_test.tmp");
}

/*
 * Test: truncate with no arguments returns error.
 */
static void test_shell_cmd_truncate_no_args(void)
{
    int ret = shell_execute("truncate");
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

/* ============================================================================
 * sleep Command Validation (CORE-M1)
 *
 * cmd_sleep now uses shell_parse_uint instead of atoi, so it rejects
 * non-numeric input, negative durations, and "0". The happy-path sleeps
 * for real, so that is not exercised here.
 * ============================================================================ */

static void test_shell_cmd_sleep_rejects_non_numeric(void)
{
    /* "abc" is not a valid uint — parse fails. */
    int ret = shell_execute("sleep abc");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_rejects_zero(void)
{
    /* "0" parses but is rejected (duration must be > 0). */
    int ret = shell_execute("sleep 0");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_rejects_negative(void)
{
    /* shell_parse_uint rejects strings with non-digit chars including '-'. */
    int ret = shell_execute("sleep -5");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_rejects_trailing_garbage(void)
{
    /* "10x" has trailing garbage — shell_parse_uint rejects. atoi would
     * silently return 10 and sleep for 10ms. */
    int ret = shell_execute("sleep 10x");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

static void test_shell_cmd_sleep_missing_args(void)
{
    int ret = shell_execute("sleep");
    TEST_ASSERT_EQUAL_INT(1, ret);
}

/* ============================================================================
 * Path Resolution Unit Tests (CORE-H4)
 *
 * Direct tests for shell_resolve_path() covering canonicalization of "..",
 * ".", empty strings, double slashes, trailing slashes, and overflow.
 * ============================================================================ */

/* Restore the console session's cwd after each test in this group. */
static void resolve_path_setup(char *saved_cwd)
{
    struct shell_session *sess = shell_session_console();
    strcpy(saved_cwd, sess->cwd);
    strcpy(sess->cwd, "/");
}

static void resolve_path_teardown(const char *saved_cwd)
{
    struct shell_session *sess = shell_session_console();
    strcpy(sess->cwd, saved_cwd);
}

static void test_shell_resolve_path_absolute_simple(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_dotdot(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/../bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_dot(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/./bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_dotdot_at_root(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    /* ".." at root stays at root, then "etc" is appended. */
    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/../etc", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/etc", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_double_slash(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo//bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_trailing_slash(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("/foo/bar/", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_empty(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);
    strcpy(shell_session_console()->cwd, "/sys");

    /* Empty path resolves to current working directory. */
    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/sys", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_relative(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);
    strcpy(shell_session_console()->cwd, "/foo");

    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(0, shell_resolve_path("bar", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/foo/bar", out);

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_overflow(void)
{
    char saved_cwd[VFS_MAX_PATH];
    resolve_path_setup(saved_cwd);

    /* Tiny output buffer forces the length check to fail. */
    char out[4];
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path("/foo/bar", out, sizeof(out)));

    resolve_path_teardown(saved_cwd);
}

static void test_shell_resolve_path_rejects_null(void)
{
    char out[VFS_MAX_PATH];
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path("/foo", NULL, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, shell_resolve_path("/foo", out, 1));
}

/* ============================================================================
 * Shared String Function Regression Tests (CORE-L1)
 *
 * Previously, 6 files had their own static copies of strcmp/strlen/strcpy.
 * Now they all use the shared functions from string.c via string.h.
 * ============================================================================ */

/* Regression: shared string functions from string.h work correctly */
static void test_string_strcmp_basic(void)
{
    TEST_ASSERT_EQUAL_INT(0, strcmp("hello", "hello"));
    TEST_ASSERT_TRUE(strcmp("abc", "abd") < 0);
    TEST_ASSERT_TRUE(strcmp("abd", "abc") > 0);
    TEST_ASSERT_TRUE(strcmp("", "a") < 0);
    TEST_ASSERT_EQUAL_INT(0, strcmp("", ""));
}

static void test_string_strlen_basic(void)
{
    TEST_ASSERT_EQUAL_INT(0, strlen(""));
    TEST_ASSERT_EQUAL_INT(5, strlen("hello"));
    TEST_ASSERT_EQUAL_INT(1, strlen("x"));
}

static void test_string_strcpy_basic(void)
{
    char buf[32];
    strcpy(buf, "test");
    TEST_ASSERT_EQUAL_STRING("test", buf);
    strcpy(buf, "");
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_string_strncpy_basic(void)
{
    char buf[8];
    extern void *memset(void *s, int c, size_t n);
    memset(buf, 'X', sizeof(buf));
    strncpy(buf, "hi", sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("hi", buf);
    /* strncpy should zero-pad remaining bytes */
    TEST_ASSERT_EQUAL_INT(0, buf[3]);
}

/* ============================================================================
 * kprintf Format Regression Tests (CORE-L3)
 *
 * The kprintf was refactored to use a common format parser.
 * These tests verify format specifiers produce correct output via
 * uart_snprintf (which writes to a buffer, testable).
 * ============================================================================ */

/* Regression: kprintf format specifiers work after refactor */
static void test_kprintf_decimal(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%d", 42);
    TEST_ASSERT_EQUAL_STRING("42", buf);
    uart_snprintf(buf, sizeof(buf), "%d", -1);
    TEST_ASSERT_EQUAL_STRING("-1", buf);
    uart_snprintf(buf, sizeof(buf), "%d", 0);
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

static void test_kprintf_hex(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%x", 0xDEAD);
    TEST_ASSERT_EQUAL_STRING("dead", buf);
    uart_snprintf(buf, sizeof(buf), "%X", 0xBEEF);
    TEST_ASSERT_EQUAL_STRING("BEEF", buf);
}

static void test_kprintf_string(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "hello %s", "world");
    TEST_ASSERT_EQUAL_STRING("hello world", buf);
    uart_snprintf(buf, sizeof(buf), "%s", "");
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_kprintf_pointer(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
    TEST_ASSERT_EQUAL_STRING("0x1234", buf);
}

static void test_kprintf_width_pad(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%8d", 42);
    TEST_ASSERT_EQUAL_STRING("      42", buf);
    uart_snprintf(buf, sizeof(buf), "%08x", 0xFF);
    TEST_ASSERT_EQUAL_STRING("000000ff", buf);
}

static void test_kprintf_long(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%lu", (unsigned long)4294967296UL);
    TEST_ASSERT_EQUAL_STRING("4294967296", buf);
}

/* Regression: %llu / %lld / %llx must format as 64-bit, not print "%l..."
 * literally. Pre-fix kprintf only consumed one 'l', so the second 'l'
 * fell into the default case and emitted "%l" + "u" -> "%lu" verbatim
 * (#netstat output bug, kprintf.c length-modifier loop). */
static void test_kprintf_long_long(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%llu", (unsigned long long)4294967296ULL);
    TEST_ASSERT_EQUAL_STRING("4294967296", buf);
    uart_snprintf(buf, sizeof(buf), "%lld", (long long)-9000000001LL);
    TEST_ASSERT_EQUAL_STRING("-9000000001", buf);
    uart_snprintf(buf, sizeof(buf), "%llx", (unsigned long long)0xDEADBEEFCAFEULL);
    TEST_ASSERT_EQUAL_STRING("deadbeefcafe", buf);
    /* The original failing pattern from the netstat report. */
    uart_snprintf(buf, sizeof(buf),
                  "RX packets: %llu  bytes: %llu",
                  (unsigned long long)42, (unsigned long long)6048);
    TEST_ASSERT_EQUAL_STRING("RX packets: 42  bytes: 6048", buf);
}

/* Edge cases on top of the basic %ll fix: width modifiers, multiple
 * %ll specifiers in one format string, mix with %d / %s. */
static void test_kprintf_long_long_edge_cases(void)
{
    char buf[128];
    /* Width on %llu — used by `top` / status displays. */
    uart_snprintf(buf, sizeof(buf), "%12llu", (unsigned long long)123ULL);
    TEST_ASSERT_EQUAL_STRING("         123", buf);
    /* Zero-pad. */
    uart_snprintf(buf, sizeof(buf), "%016llx",
                  (unsigned long long)0xCAFEBABEULL);
    TEST_ASSERT_EQUAL_STRING("00000000cafebabe", buf);
    /* Multiple %llu in one format — exercises the parser state reset. */
    uart_snprintf(buf, sizeof(buf), "%llu/%llu/%llu",
                  (unsigned long long)1ULL,
                  (unsigned long long)2ULL,
                  (unsigned long long)3ULL);
    TEST_ASSERT_EQUAL_STRING("1/2/3", buf);
    /* Mix of widths: %d (32-bit), %llu (64-bit), %s. */
    uart_snprintf(buf, sizeof(buf),
                  "[%s] count=%d total=%llu",
                  "INFO", 17, (unsigned long long)9000000000ULL);
    TEST_ASSERT_EQUAL_STRING("[INFO] count=17 total=9000000000", buf);
}

/* %z (size_t) was added alongside the %ll fix in the same length-modifier
 * loop. Pin its behaviour separately so a future regression is obvious. */
static void test_kprintf_size_t(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "%zu", (size_t)123456);
    TEST_ASSERT_EQUAL_STRING("123456", buf);
    /* size_t is 64-bit on the kernel targets; check a value that
     * doesn't fit in 32-bit unsigned. */
    uart_snprintf(buf, sizeof(buf), "%zu", (size_t)5000000000ULL);
    TEST_ASSERT_EQUAL_STRING("5000000000", buf);
}

static void test_kprintf_percent(void)
{
    char buf[64];
    uart_snprintf(buf, sizeof(buf), "100%%");
    TEST_ASSERT_EQUAL_STRING("100%", buf);
}

static void test_kprintf_mixed(void)
{
    char buf[128];
    uart_snprintf(buf, sizeof(buf), "[%s] val=%d hex=0x%X", "INFO", 42, 0xABC);
    TEST_ASSERT_EQUAL_STRING("[INFO] val=42 hex=0xABC", buf);
}

/* ============================================================================
 * Floating-point format tests
 *
 * Verify the freestanding %f / %e / %g implementations added so that
 * Lua's `string.format("%.4f", ...)` works through our snprintf stub.
 *
 * Test code is built with -mgeneral-regs-only so it can't touch FP
 * registers — that means literal `1.5` in a uart_snprintf call is
 * forbidden, since the variadic call would pass the double in V0.
 * The kprintf_float test helper takes the value as IEEE-754 bits
 * (uint64_t) and bit-casts on the FP-allowed side. The constants
 * below were generated with `printf "%016lx" $(python -c ...)`.
 * ============================================================================ */

#include "kprintf_float.h"

/* Common IEEE-754 bit patterns. Each comment shows the value.
 *
 * To derive a new constant, use:
 *
 *     python3 -c "import struct; print(hex(struct.unpack('>Q', struct.pack('>d', 1.5))[0]))"
 *
 * which gives 0x3ff8000000000000 for 1.5. The result fits straight
 * into a `0x...ULL` literal here. (Avoid printf-roundtripping —
 * different libc versions disagree on the exact rounding of some
 * decimal-literal-to-double conversions, and we want bit-exact
 * pinning across all platforms.) */
#define DBL_BITS_0_0          0x0000000000000000ULL  /* +0.0          */
#define DBL_BITS_1_0          0x3FF0000000000000ULL  /* 1.0           */
#define DBL_BITS_1_5          0x3FF8000000000000ULL  /* 1.5           */
#define DBL_BITS_NEG_2_25     0xC002000000000000ULL  /* -2.25         */
#define DBL_BITS_PI_APPROX    0x400921FB53C8D4F1ULL  /* 3.14159265    */
#define DBL_BITS_0_999        0x3FEFF7CED916872BULL  /* 0.999         */
#define DBL_BITS_1_999        0x3FFFFDF3B645A1CBULL  /* 1.999         */
#define DBL_BITS_NEG_0_005    0xBF747AE147AE147BULL  /* -0.005        */
#define DBL_BITS_3_14         0x40091EB851EB851FULL  /* 3.14          */
#define DBL_BITS_NEG_3_14     0xC0091EB851EB851FULL  /* -3.14         */
#define DBL_BITS_12345        0x40C81C8000000000ULL  /* 12345.0       */
#define DBL_BITS_0_0012345    0x3F543A22DAB9CB75ULL  /* 0.0012345     */
#define DBL_BITS_1234567      0x4132D687E0000000ULL  /* 1234567.0     */
#define DBL_BITS_0_00001      0x3EE4F8B588E368F1ULL  /* 0.00001       */
#define DBL_BITS_NAN          0x7FF8000000000000ULL  /* quiet NaN     */
#define DBL_BITS_POS_INF      0x7FF0000000000000ULL  /* +Inf          */
#define DBL_BITS_NEG_INF      0xFFF0000000000000ULL  /* -Inf          */

static void test_kprintf_float_default_precision(void)
{
    char buf[64];
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_1_5, -1, 'f');
    TEST_ASSERT_EQUAL_STRING("1.500000", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_NEG_2_25, -1, 'f');
    TEST_ASSERT_EQUAL_STRING("-2.250000", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_0_0, -1, 'f');
    TEST_ASSERT_EQUAL_STRING("0.000000", buf);
}

static void test_kprintf_float_explicit_precision(void)
{
    char buf[64];
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_PI_APPROX, 4, 'f');
    TEST_ASSERT_EQUAL_STRING("3.1416", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_1_5, 0, 'f');
    TEST_ASSERT_EQUAL_STRING("2", buf);   /* round-half-up */
    /* round-half-away-from-zero: -0.005 → -0.01 */
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_NEG_0_005, 2, 'f');
    TEST_ASSERT_EQUAL_STRING("-0.01", buf);
}

static void test_kprintf_float_rounding_carry(void)
{
    char buf[64];
    /* 0.999 with precision 2 should round up to 1.00. */
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_0_999, 2, 'f');
    TEST_ASSERT_EQUAL_STRING("1.00", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_1_999, 2, 'f');
    TEST_ASSERT_EQUAL_STRING("2.00", buf);
}

static void test_kprintf_float_specials(void)
{
    char buf[64];
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_NAN, -1, 'f');
    TEST_ASSERT_EQUAL_STRING("nan", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_NAN, -1, 'F');
    TEST_ASSERT_EQUAL_STRING("NAN", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_POS_INF, -1, 'f');
    TEST_ASSERT_EQUAL_STRING("inf", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_NEG_INF, -1, 'f');
    TEST_ASSERT_EQUAL_STRING("-inf", buf);
}

static void test_kprintf_scientific(void)
{
    char buf[64];
    /* default precision 6: 1.234500e+04 */
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_12345, -1, 'e');
    TEST_ASSERT_EQUAL_STRING("1.234500e+04", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_0_0012345, 2, 'E');
    TEST_ASSERT_EQUAL_STRING("1.23E-03", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_1_0, 0, 'e');
    TEST_ASSERT_EQUAL_STRING("1e+00", buf);
}

static void test_kprintf_g_format(void)
{
    char buf[64];
    /* %g should trim trailing zeros and the decimal when bare. */
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_1_0, -1, 'g');
    TEST_ASSERT_EQUAL_STRING("1", buf);
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_1_5, -1, 'g');
    TEST_ASSERT_EQUAL_STRING("1.5", buf);
    /* exp >= precision (6) — switches to %e. */
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_1234567, -1, 'g');
    TEST_ASSERT_EQUAL_STRING("1.23457e+06", buf);
    /* exp < -4 — switches to %e too. */
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_0_00001, -1, 'g');
    TEST_ASSERT_EQUAL_STRING("1e-05", buf);
    /* zero stays as "0", not "0.000000". */
    kprintf_float_test_format_bits(buf, sizeof(buf), DBL_BITS_0_0, -1, 'g');
    TEST_ASSERT_EQUAL_STRING("0", buf);
}

/*
 * Width / left-justify / zero-pad branches of kprintf_float_emit.
 * These bypass the va_list path but exercise the bespoke padding
 * logic at kprintf_float.c:emit_value (sign-then-zeros for negative
 * numbers, space-pad for non-finite, left-justify with trailing
 * spaces).
 */
static void test_kprintf_float_padded_zero_pad_negative(void)
{
    /* Negative number with zero-pad: sign should land first, then
     * the zeros, then digits. Width 10 — "-3.14" is 5 chars, pad 5. */
    char buf[32];
    kprintf_float_test_format_bits_padded(buf, sizeof(buf),
                                          DBL_BITS_NEG_3_14, 2, 'f',
                                          /*width=*/10, /*lj=*/0, /*zp=*/1);
    TEST_ASSERT_EQUAL_STRING("-000003.14", buf);
}

static void test_kprintf_float_padded_left_justify(void)
{
    /* Left-justify: digits first, then trailing spaces. */
    char buf[32];
    kprintf_float_test_format_bits_padded(buf, sizeof(buf),
                                          DBL_BITS_3_14, 2, 'f',
                                          /*width=*/10, /*lj=*/1, /*zp=*/0);
    TEST_ASSERT_EQUAL_STRING("3.14      ", buf);
}

static void test_kprintf_float_padded_inf_uses_space_not_zero(void)
{
    /* glibc / musl space-pad nan / inf even when zero-pad is set —
     * "0000000inf" is more confusing than informative. */
    char buf[32];
    kprintf_float_test_format_bits_padded(buf, sizeof(buf),
                                          DBL_BITS_POS_INF, -1, 'f',
                                          /*width=*/10, /*lj=*/0, /*zp=*/1);
    TEST_ASSERT_EQUAL_STRING("       inf", buf);
}

static void test_kprintf_float_padded_neg_inf_uses_space_not_zero(void)
{
    /* Negative inf with zero-pad: same — space-pad, sign stays
     * leading, no zeros emitted. */
    char buf[32];
    kprintf_float_test_format_bits_padded(buf, sizeof(buf),
                                          DBL_BITS_NEG_INF, -1, 'f',
                                          /*width=*/10, /*lj=*/0, /*zp=*/1);
    TEST_ASSERT_EQUAL_STRING("      -inf", buf);
}

static void test_kprintf_float_padded_nan_uses_space_not_zero(void)
{
    /* NaN under zero-pad: no '0' prefix. */
    char buf[32];
    kprintf_float_test_format_bits_padded(buf, sizeof(buf),
                                          DBL_BITS_NAN, -1, 'f',
                                          /*width=*/10, /*lj=*/0, /*zp=*/1);
    TEST_ASSERT_EQUAL_STRING("       nan", buf);
}

/*
 * End-to-end test exercising the va_list crossing between
 * kprintf.c (-mgeneral-regs-only) and kprintf_float.c (FP-enabled).
 * The standalone `_format_bits` tests bypass the va_list path; this
 * one routes through the real `uart_snprintf` → `fmt_vprintf` →
 * case 'f' → `kprintf_float_emit(va_list*)` chain so a regression
 * in the `(va_list *)&args` cast or in GCC's FP arg spilling under
 * `-mgeneral-regs-only` would surface here.
 */
static void test_kprintf_float_e2e_va_list_crossing(void)
{
    char buf[64];
    /* %.4f of pi-approx, identical to a real Lua-driven format. */
    kprintf_float_test_e2e_uart_snprintf(buf, sizeof(buf),
                                         DBL_BITS_PI_APPROX, 4, 'f');
    TEST_ASSERT_EQUAL_STRING("3.1416", buf);
    /* %g of 1.5 — exercises the trim path through the va_list call. */
    kprintf_float_test_e2e_uart_snprintf(buf, sizeof(buf),
                                         DBL_BITS_1_5, -1, 'g');
    TEST_ASSERT_EQUAL_STRING("1.5", buf);
    /* %e of 12345 — verifies the post-emit advance position too;
     * if the va_list weren't being advanced correctly, follow-on
     * args in real callers would silently misalign. */
    kprintf_float_test_e2e_uart_snprintf(buf, sizeof(buf),
                                         DBL_BITS_12345, -1, 'e');
    TEST_ASSERT_EQUAL_STRING("1.234500e+04", buf);
}

/* ============================================================================
 * Hailo shell command tests (PLATFORM_X86_64 omits the Hailo driver,
 * so these are gated off that build).
 *
 * The Hailo command set is registered at boot via
 * hailo_register_shell_commands(), called from shell_init(). The
 * test harness doesn't spin up shell_init, so the tests below call
 * the registrar directly — once per test-suite run. Duplicate
 * registration is a soft append; shell_execute's find_command
 * returns the first match either way.
 *
 * All tests assert return code 0 (command handled, didn't fall
 * through to "unknown command"). Command output goes to UART and
 * is not captured here — the corresponding hailo_core paths have
 * their own unit tests in test_hailo.c / test_hef_parser.c that
 * verify correctness at the function level.
 */
#if !defined(PLATFORM_X86_64)

static bool hailo_cmds_registered;

static void ensure_hailo_registered(void)
{
    if (hailo_cmds_registered) return;
    extern void hailo_register_shell_commands(void);
    hailo_register_shell_commands();
    hailo_cmds_registered = true;
}

static void test_shell_cmd_hailo_status(void)
{
    ensure_hailo_registered();
    int ret = shell_execute("hailo");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_hailo_probe(void)
{
    ensure_hailo_registered();
    /* In the test kernel hailo_platform is never installed, so
     * hailo_probe returns HAILO_ERR_NODEV and the shell reports
     * "no device". The command handler still returns 0 — the dispatch
     * worked, the device just isn't there. */
    int ret = shell_execute("hailo probe");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_hailo_boot_no_fw(void)
{
    ensure_hailo_registered();
    /* No HAILO_FW_BLOB means the weak hailo_fw_* externs are NULL
     * and the handler prints "firmware not embedded", returning 0. */
    int ret = shell_execute("hailo boot");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_hailo_load_no_args(void)
{
    ensure_hailo_registered();
    /* Missing <vfs-path> → usage printed; still returns 0. */
    int ret = shell_execute("hailo load");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_hailo_load_nonexistent(void)
{
    ensure_hailo_registered();
    /* VFS stat fails; shell prints "stat failed"; command returns 0. */
    int ret = shell_execute("hailo load /mnt/files/nosuch.hef");
    TEST_ASSERT_EQUAL_INT(0, ret);
}

static void test_shell_cmd_hailo_load_too_small(void)
{
    ensure_hailo_registered();
    /* Create a controlled 4-byte file — smaller than the 12-byte HEF
     * outer-header minimum. Using a known-size fixture rather than
     * relying on whatever /mnt/files/preload.conf happens to be
     * guards against a future preload.conf ≥ 12 bytes silently
     * changing this test's meaning. */
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx(
        "/mnt/files", &subpath);
    TEST_ASSERT_NOT_NULL(mnt);
    int f = littlefs_file_open(mnt, "/hailo_too_small.bin",
                               LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    TEST_ASSERT_TRUE(f >= 0);
    const uint8_t tiny[4] = { 0, 0, 0, 0 };
    TEST_ASSERT_TRUE(littlefs_file_write(mnt, f, tiny, sizeof(tiny)) >= 0);
    littlefs_file_close(mnt, f);

    int ret = shell_execute("hailo load /mnt/files/hailo_too_small.bin");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Clean up so repeated runs of the harness don't accumulate the
     * fixture (LittleFS may fall back to RAM-backed storage and can be
     * leak is bounded to one test-kernel run, but tightening the
     * cleanup keeps boot-test loops clean). */
    littlefs_remove(mnt, "/hailo_too_small.bin");
}

#endif /* !X86_64 */

/* ============================================================================
 * `gpu` shell command — platform-correctness regression
 *
 * On x86-64, the cross-platform built-in `gpu` cmd in shell.c is
 * intentionally guarded out so the NVIDIA driver's external
 * registration (with init/sec2/vram/regs subcommands) wins
 * find_command's lookup. On every other platform (Jetson, QEMU
 * ARM64, Pi 5) the built-in MUST be present — Jetson uses its
 * `gpu read <hex-offset>` subcommand for the integrated GA10B
 * MMIO aperture, and the other ARM64 targets get the generic
 * info display.
 *
 * test_x86_boot.c has the x86-side assertion
 * (`test_x86_gpu_cmd_not_in_builtins`); this is the cross-
 * platform mirror that fires loudly if the platform guard in
 * shell.c is widened, narrowed, or removed.
 * ============================================================================ */

/* Reference the real shell_cmd_t (from shell.h, included above) so a
 * future field-shape change doesn't compile cleanly with stale
 * offsets — the way an inline `extern const struct { ... }` re-decl
 * silently would. */
extern const shell_cmd_t builtin_commands[];
extern const int NUM_BUILTIN_COMMANDS;
extern shell_cmd_t external_commands[];
extern int num_external_commands;

/* Generic table scanner — non-static so test_x86_boot.c can reuse it
 * via an extern declaration. Takes a `const shell_cmd_t *` so it
 * accepts both the const builtin table and the non-const external
 * table (which converts implicitly). The two test TUs link into the
 * same kernel image, so a shared private header isn't needed. */
bool cmd_table_has(const shell_cmd_t *table, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(name, table[i].name) == 0)
            return true;
    }
    return false;
}

static void test_shell_gpu_cmd_platform_correctness(void)
{
    bool present = cmd_table_has(builtin_commands, NUM_BUILTIN_COMMANDS,
                                 "gpu");
#if defined(PLATFORM_X86_64)
    /* x86-64: NVIDIA driver registers a richer `gpu` externally;
     * the built-in is guarded out so it doesn't shadow the
     * dispatcher (find_command checks built-ins before externals).
     * If this fails, the platform guard around builtin_commands[]
     * was removed/widened — the NVIDIA `gpu init/sec2/vram/regs`
     * dispatcher will be unreachable from the shell. */
    TEST_ASSERT_FALSE(present);
#else
    /* Every other platform keeps the built-in: Jetson needs
     * `gpu read <hex-offset>` for the integrated GA10B MMIO
     * aperture, ARM64-QEMU/Pi 5 use the info display. Removing
     * the built-in here would break Jetson's GPU debug surface. */
    TEST_ASSERT_TRUE(present);
#endif
}

static void test_lua_shell_commands_registered_with_expected_mutation_modes(void)
{
    bool lua_found = false;
    bool lua_admin_found = false;
    for (int i = 0; i < num_external_commands; i++) {
        if (strcmp(external_commands[i].name, "lua") == 0) {
            lua_found = true;
            TEST_ASSERT_FALSE(external_commands[i].mutates);
        } else if (strcmp(external_commands[i].name, "lua-admin") == 0) {
            lua_admin_found = true;
            TEST_ASSERT_TRUE(external_commands[i].mutates);
        }
    }
    TEST_ASSERT_TRUE(lua_found);
    TEST_ASSERT_TRUE(lua_admin_found);
}

/*
 * Jetson BPMP IPC + PCIe shell commands must register on the Jetson
 * platform only. Guards in kernel/src/shell.c wrap these in
 * `#if defined(PLATFORM_JETSON_ORIN_NANO)`; removing the guard would
 * leak unresolved symbols on QEMU/Pi 5/x86-64 since cmd_hspdiag/
 * cmd_bpmp/cmd_pcietrain are compiled conditionally in shell_sys.c.
 * Fires on that mis-guard and also catches someone deleting the
 * commands accidentally.
 */
static void test_shell_jetson_bpmp_pcie_cmds_correct_platform(void)
{
    bool hspdiag_present   = cmd_table_has(builtin_commands, NUM_BUILTIN_COMMANDS, "hspdiag");
    bool bpmp_present      = cmd_table_has(builtin_commands, NUM_BUILTIN_COMMANDS, "bpmp");
    bool pcietrain_present = cmd_table_has(builtin_commands, NUM_BUILTIN_COMMANDS, "pcietrain");

#if defined(PLATFORM_JETSON_ORIN_NANO)
    TEST_ASSERT_MESSAGE(hspdiag_present,
        "hspdiag must be registered on Jetson (BPMP HSP doorbell probe)");
    TEST_ASSERT_MESSAGE(bpmp_present,
        "bpmp must be registered on Jetson (IPC smoke test)");
    TEST_ASSERT_MESSAGE(pcietrain_present,
        "pcietrain must be registered on Jetson (PCIe C8 bring-up)");
#else
    TEST_ASSERT_MESSAGE(!hspdiag_present,
        "hspdiag must NOT be registered off-Jetson — its symbol is gated");
    TEST_ASSERT_MESSAGE(!bpmp_present,
        "bpmp must NOT be registered off-Jetson — its symbol is gated");
    TEST_ASSERT_MESSAGE(!pcietrain_present,
        "pcietrain must NOT be registered off-Jetson — its symbol is gated");
#endif
}

/* ============================================================================
 * shell_io echo-negotiation tests (#581 follow-up)
 * ============================================================================ */

#include "../include/shell_io.h"

/* Mock shell_io that replays a fixed input buffer through read_char,
 * captures every byte passed to write, and reports a configurable
 * echo state. Drives the line-edit loop deterministically without a
 * real UART or TCP backend. */
struct echo_test_mock {
    struct shell_io io;
    const char     *input;
    size_t          input_pos;
    size_t          input_len;
    char            captured[256];
    size_t          captured_len;
    bool            echo;
};

static int echo_test_read_char(struct shell_io *io)
{
    struct echo_test_mock *m = (struct echo_test_mock *)io->ctx;
    if (m->input_pos >= m->input_len) return -1;
    return (unsigned char)m->input[m->input_pos++];
}

static void echo_test_write(struct shell_io *io, const char *buf, size_t len)
{
    struct echo_test_mock *m = (struct echo_test_mock *)io->ctx;
    for (size_t i = 0; i < len && m->captured_len < sizeof(m->captured); i++) {
        m->captured[m->captured_len++] = buf[i];
    }
}

static bool echo_test_is_open(struct shell_io *io) { (void)io; return true; }
static bool echo_test_echo_enabled(struct shell_io *io)
{
    struct echo_test_mock *m = (struct echo_test_mock *)io->ctx;
    return m->echo;
}

static void echo_test_init(struct echo_test_mock *m, const char *input,
                           bool with_echo_hook, bool echo)
{
    extern void *memset(void *s, int c, size_t n);
    memset(m, 0, sizeof(*m));
    m->io.read_char    = echo_test_read_char;
    m->io.write        = echo_test_write;
    m->io.is_open      = echo_test_is_open;
    m->io.echo_enabled = with_echo_hook ? echo_test_echo_enabled : NULL;
    m->io.ctx          = m;
    m->input           = input;
    m->input_len       = strlen(input);
    m->echo            = echo;
}

/* Drive shell_read_command with a bound mock session. Outputs the
 * line-read result into `*out_n`, with `out_captured`/
 * `out_captured_len` set to the mock's record of every byte the
 * line-edit loop wrote. Returns void rather than int because
 * Unity's TEST_ASSERT_NOT_NULL expands to a bare `return;` on
 * failure, which mismatches any non-void return type. */
static void echo_test_run(struct echo_test_mock *m, char *line_out,
                          int line_max, int *out_n, char *out_captured,
                          size_t out_cap_size, size_t *out_captured_len)
{
    struct shell_session *sess = shell_session_alloc();
    TEST_ASSERT_NOT_NULL(sess);
    sess->io = &m->io;
    shell_session_bind(task_current(), sess);

    *out_n = shell_read_command("$ ", line_out, line_max);

    shell_session_unbind(task_current());
    sess->io = NULL;
    shell_session_free(sess);

    size_t copy = m->captured_len < out_cap_size ? m->captured_len : out_cap_size;
    extern void *memcpy(void *d, const void *s, size_t n);
    memcpy(out_captured, m->captured, copy);
    *out_captured_len = copy;
}

/* Tiny mock for the shell_io_read_buf helper-contract tests
 * (#597). Independent of echo_test_mock so we can count read_char
 * vs read_buf invocations and prove which path the helper took. */
struct read_buf_mock {
    struct shell_io io;
    const char     *input;
    int             input_pos;
    int             input_len;
    int             read_char_calls;
    int             read_buf_calls;
};

static int read_buf_mock_read_char(struct shell_io *io)
{
    struct read_buf_mock *m = (struct read_buf_mock *)io->ctx;
    m->read_char_calls++;
    if (m->input_pos >= m->input_len) return -1;
    return (unsigned char)m->input[m->input_pos++];
}

static int read_buf_mock_read_buf(struct shell_io *io, char *dst, int max_len)
{
    struct read_buf_mock *m = (struct read_buf_mock *)io->ctx;
    m->read_buf_calls++;
    int avail = m->input_len - m->input_pos;
    if (avail <= 0) return -1;
    int n = (avail < max_len) ? avail : max_len;
    extern void *memcpy(void *d, const void *s, size_t n);
    memcpy(dst, m->input + m->input_pos, (size_t)n);
    m->input_pos += n;
    return n;
}

static void read_buf_mock_init(struct read_buf_mock *m, const char *input,
                               bool with_read_buf)
{
    extern void *memset(void *s, int c, size_t n);
    memset(m, 0, sizeof(*m));
    m->io.read_char = read_buf_mock_read_char;
    m->io.read_buf  = with_read_buf ? read_buf_mock_read_buf : NULL;
    m->io.ctx       = m;
    m->input        = input;
    m->input_len    = (int)strlen(input);
}

/*
 * Test: shell_io_read_buf uses the vtable's read_buf when set, and
 * drains all available bytes in a single call (#597 perf path).
 * A regression that re-routes through read_char would show up as
 * read_char_calls > 0, read_buf_calls != 1, or got != input_len.
 */
static void test_shell_io_read_buf_uses_batched_when_available(void)
{
    struct read_buf_mock m;
    read_buf_mock_init(&m, "hello world\n", true /*with_read_buf*/);

    char dst[32];
    int got = shell_io_read_buf(&m.io, dst, (int)sizeof(dst));

    TEST_ASSERT_EQUAL_INT(12, got);
    TEST_ASSERT_EQUAL_INT(1, m.read_buf_calls);
    TEST_ASSERT_EQUAL_INT(0, m.read_char_calls);
    TEST_ASSERT_EQUAL_INT(0, memcmp(dst, "hello world\n", 12));
}

/*
 * Test: shell_io_read_buf falls back to ONE read_char call when the
 * vtable's read_buf is NULL (UART backend etc.). Returns exactly one
 * byte even if max_len > 1, so callers don't stall waiting for a
 * second byte the user hasn't typed yet.
 */
static void test_shell_io_read_buf_falls_back_to_read_char(void)
{
    struct read_buf_mock m;
    read_buf_mock_init(&m, "hi\n", false /*no read_buf*/);

    char dst[32];
    int got = shell_io_read_buf(&m.io, dst, (int)sizeof(dst));

    TEST_ASSERT_EQUAL_INT(1, got);
    TEST_ASSERT_EQUAL_INT(1, m.read_char_calls);
    TEST_ASSERT_EQUAL_INT(0, m.read_buf_calls);
    TEST_ASSERT_EQUAL_INT('h', dst[0]);
}

/*
 * Helper contract: NULL `io` and NULL `echo_enabled` callback both
 * return true, preserving the always-echo default for backends
 * that don't opt into the hook (UART console).
 */
static void test_shell_io_echo_enabled_defaults_true(void)
{
    TEST_ASSERT_TRUE(shell_io_echo_enabled(NULL));

    struct shell_io io;
    extern void *memset(void *s, int c, size_t n);
    memset(&io, 0, sizeof(io));
    /* echo_enabled deliberately left NULL — should default to ON. */
    TEST_ASSERT_TRUE(shell_io_echo_enabled(&io));
}

/*
 * Helper contract: when `echo_enabled` is wired up, its return
 * value flows through unchanged.
 */
static void test_shell_io_echo_enabled_callback_used(void)
{
    struct echo_test_mock m;
    echo_test_init(&m, "", true /*with hook*/, false /*echo off*/);
    TEST_ASSERT_FALSE(shell_io_echo_enabled(&m.io));

    m.echo = true;
    TEST_ASSERT_TRUE(shell_io_echo_enabled(&m.io));
}

/*
 * Integration: shell_read_command's line-edit loop must NOT echo
 * input bytes when the backend reports `echo_enabled = false`. The
 * prompt is still emitted (clients still need it to know the shell
 * is ready); only the per-char data echo and the trailing CRLF are
 * suppressed. The in-memory line `buf_out` must still reflect the
 * exact input the peer sent — the negotiation only affects what's
 * sent BACK, not what's parsed.
 */
static void test_shell_read_command_skips_echo_when_disabled(void)
{
    struct echo_test_mock m;
    echo_test_init(&m, "hello\n", true /*with hook*/, false /*echo off*/);

    char line[64];
    char captured[256];
    size_t captured_len = 0;
    int n = 0;
    echo_test_run(&m, line, sizeof(line), &n,
                  captured, sizeof(captured), &captured_len);

    /* Buffer state is unchanged by the negotiation — still tracks input. */
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_STRING("hello", line);

    /* The ONLY thing written should be the prompt. No echo of "hello",
     * no trailing "\r\n". Compared as bounded memory because TEST_ASSERT
     * variants vary in null-handling. */
    TEST_ASSERT_EQUAL_UINT(2, captured_len);
    TEST_ASSERT_EQUAL_INT('$', captured[0]);
    TEST_ASSERT_EQUAL_INT(' ', captured[1]);
}

/*
 * Companion: the same input WITH echo enabled writes prompt + each
 * input char + CRLF. Locks in that the gating only fires when echo
 * is off — not by accident from some other path.
 */
static void test_shell_read_command_echoes_when_enabled(void)
{
    struct echo_test_mock m;
    echo_test_init(&m, "hi\n", true /*with hook*/, true /*echo on*/);

    char line[64];
    char captured[256];
    size_t captured_len = 0;
    int n = 0;
    echo_test_run(&m, line, sizeof(line), &n,
                  captured, sizeof(captured), &captured_len);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("hi", line);

    /* Prompt "$ ", then 'h', then 'i', then "\r\n" = 6 bytes. */
    TEST_ASSERT_EQUAL_UINT(6, captured_len);
    TEST_ASSERT_EQUAL_INT('$', captured[0]);
    TEST_ASSERT_EQUAL_INT(' ', captured[1]);
    TEST_ASSERT_EQUAL_INT('h', captured[2]);
    TEST_ASSERT_EQUAL_INT('i', captured[3]);
    TEST_ASSERT_EQUAL_INT('\r', captured[4]);
    TEST_ASSERT_EQUAL_INT('\n', captured[5]);
}

/*
 * NULL echo_enabled hook (the UART backend's configuration) defaults
 * to echo-on. Same input produces the same captured stream as the
 * explicit-echo-on case above.
 */
static void test_shell_read_command_null_hook_echoes(void)
{
    struct echo_test_mock m;
    echo_test_init(&m, "hi\n", false /*no hook*/, false /*irrelevant*/);

    char line[64];
    char captured[256];
    size_t captured_len = 0;
    int n = 0;
    echo_test_run(&m, line, sizeof(line), &n,
                  captured, sizeof(captured), &captured_len);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("hi", line);
    TEST_ASSERT_EQUAL_UINT(6, captured_len);
    TEST_ASSERT_EQUAL_INT('h', captured[2]);
    TEST_ASSERT_EQUAL_INT('i', captured[3]);
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_shell(void)
{
    UNITY_BEGIN();

    /* Cross-platform `gpu` builtin shape (x86-64 strips it; everyone
     * else keeps it for Jetson `gpu read`). */
    RUN_TEST(test_shell_gpu_cmd_platform_correctness);

    /* Jetson BPMP + PCIe shell cmds registered iff on Jetson. */
    RUN_TEST(test_shell_jetson_bpmp_pcie_cmds_correct_platform);

    /* Command dispatch tests - essential */
    RUN_TEST(test_shell_empty_command);
    RUN_TEST(test_shell_whitespace_only);
    RUN_TEST(test_shell_unknown_command);
    RUN_TEST(test_shell_cmd_too_long);
    RUN_TEST(test_shell_cmd_at_max_length);

    /* #581 follow-up: echo respects telnet WILL/DONT ECHO negotiation. */
    RUN_TEST(test_shell_io_echo_enabled_defaults_true);
    RUN_TEST(test_shell_io_echo_enabled_callback_used);
    RUN_TEST(test_shell_io_read_buf_uses_batched_when_available);
    RUN_TEST(test_shell_io_read_buf_falls_back_to_read_char);
    RUN_TEST(test_shell_read_command_skips_echo_when_disabled);
    RUN_TEST(test_shell_read_command_echoes_when_enabled);
    RUN_TEST(test_shell_read_command_null_hook_echoes);

    /* Basic commands - just verify they execute (minimal output) */
    RUN_TEST(test_shell_cmd_clear);
    RUN_TEST(test_shell_cmd_uptime);
    RUN_TEST(test_shell_cmd_top_one_iter);
    RUN_TEST(test_shell_cmd_top_refresh_arg);
    RUN_TEST(test_shell_cmd_top_zero_refresh);
    RUN_TEST(test_shell_cmd_top_missing_count);
    RUN_TEST(test_shell_cmd_sched_trace_lifecycle);
    RUN_TEST(test_shell_cmd_sched_compare);
    RUN_TEST(test_shell_cmd_eviction_demo);
    RUN_TEST(test_shell_cmd_bench_context_histogram_reinit);
    RUN_TEST(test_shell_cmd_bench_eviction);
    RUN_TEST(test_shell_cmd_eviction_features);
    RUN_TEST(test_shell_cmd_eviction_model_status);
    RUN_TEST(test_shell_cmd_eviction_model_lifecycle);
    RUN_TEST(test_shell_cmd_sched_model_lifecycle);
    RUN_TEST(test_shell_cmd_sched_thresholds_lifecycle);
    RUN_TEST(test_shell_cmd_sched_rebalance_lifecycle);
    RUN_TEST(test_shell_cmd_model_pin_lifecycle);
    RUN_TEST(test_shell_cmd_model_preload);
    RUN_TEST(test_shell_cmd_model_preload_wait);
    RUN_TEST(test_shell_cmd_model_preload_wait_not_inflight);
    RUN_TEST(test_boot_preload_conf_exists);

#if !defined(PLATFORM_X86_64)
    /* Hailo driver shell surface (dispatch + error paths) */
    RUN_TEST(test_shell_cmd_hailo_status);
    RUN_TEST(test_shell_cmd_hailo_probe);
    RUN_TEST(test_shell_cmd_hailo_boot_no_fw);
    RUN_TEST(test_shell_cmd_hailo_load_no_args);
    RUN_TEST(test_shell_cmd_hailo_load_nonexistent);
    RUN_TEST(test_shell_cmd_hailo_load_too_small);
#endif

    /* Benchmark command */
    RUN_TEST(test_shell_cmd_bench_no_args);
    RUN_TEST(test_shell_cmd_bench_context);
    RUN_TEST(test_shell_cmd_bench_irq);
    RUN_TEST(test_shell_cmd_bench_ipc);
    RUN_TEST(test_shell_cmd_bench_stats);
    RUN_TEST(test_shell_cmd_bench_all);
    RUN_TEST(test_shell_cmd_bench_invalid);

    /* VFS commands - error cases */
    RUN_TEST(test_shell_cmd_ls_nonexistent);
    RUN_TEST(test_shell_cmd_cat_no_args);
    RUN_TEST(test_shell_cmd_cat_nonexistent);
    RUN_TEST(test_shell_cmd_cat_directory);

    /* VFS commands - success cases */
    RUN_TEST(test_shell_cmd_ls_root);
    RUN_TEST(test_shell_cmd_ls_sys);
    RUN_TEST(test_shell_cmd_cat_sys_memory);
    RUN_TEST(test_shell_cmd_cat_sys_cpus);

    /* Verbose status commands */
    RUN_TEST(test_shell_cmd_help);
    RUN_TEST(test_shell_cmd_mem);
    RUN_TEST(test_shell_cmd_tasks);
    RUN_TEST(test_shell_cmd_cpu);
    RUN_TEST(test_shell_cmd_vmm);
    RUN_TEST(test_shell_cmd_ipc);
    RUN_TEST(test_shell_cmd_model);
    RUN_TEST(test_shell_cmd_dtb);

    /* peek / poke — argument parsing + round-trip */
    RUN_TEST(test_shell_cmd_peek_missing_args);
    RUN_TEST(test_shell_cmd_peek_bad_hex);
    RUN_TEST(test_shell_cmd_poke_missing_args);
    RUN_TEST(test_shell_cmd_poke_bad_hex_addr);
    RUN_TEST(test_shell_cmd_poke_bad_hex_value);
    RUN_TEST(test_shell_cmd_poke_writes_word);
    RUN_TEST(test_shell_cmd_peek_reads_back_after_poke);
    RUN_TEST(test_shell_cmd_poke_rejects_unaligned_addr);
    RUN_TEST(test_shell_cmd_poke_rejects_hex_too_long);
    RUN_TEST(test_shell_cmd_poke_accepts_uppercase_and_no_prefix);

    /* timdiag command (ARM64 only) - timer/IRQ delivery diagnostic */
#if !defined(PLATFORM_X86_64)
    RUN_TEST(test_shell_cmd_timdiag_no_args);
    RUN_TEST(test_shell_cmd_timdiag_fiq_arg);
    RUN_TEST(test_shell_cmd_timdiag_idempotent);
    RUN_TEST(test_shell_cmd_timdiag_unknown_arg);
#endif

    /* Component commands - core functionality */
    RUN_TEST(test_shell_cmd_component_help);
    RUN_TEST(test_shell_cmd_component_list_empty);
    RUN_TEST(test_shell_cmd_component_register_missing_args);
    RUN_TEST(test_shell_cmd_component_register_valid);
    RUN_TEST(test_shell_cmd_component_register_invalid_type);
    RUN_TEST(test_shell_cmd_component_unregister_missing_args);
    RUN_TEST(test_shell_cmd_component_unregister_invalid);
    RUN_TEST(test_shell_cmd_component_unregister_valid);
    RUN_TEST(test_shell_cmd_component_status_missing_args);
    RUN_TEST(test_shell_cmd_component_status_by_index);
    RUN_TEST(test_shell_cmd_component_status_not_found);
    RUN_TEST(test_shell_cmd_component_unknown_subcmd);

    /* Run command - error cases only */
    RUN_TEST(test_shell_cmd_run_unknown_program);

    /* Kill command - error cases only */
    RUN_TEST(test_shell_cmd_kill_no_args);
    RUN_TEST(test_shell_cmd_kill_invalid_pid);
    RUN_TEST(test_shell_cmd_kill_nonexistent_pid);

    /* Argument parsing */
    RUN_TEST(test_shell_extra_whitespace);
    RUN_TEST(test_shell_args_with_spaces);

    /* External command registration */
    RUN_TEST(test_shell_register_external_command);

    /* Working directory and path resolution tests */
    RUN_TEST(test_shell_cmd_pwd);
    RUN_TEST(test_shell_cmd_cd_root);
    RUN_TEST(test_shell_cmd_cd_no_arg);
    RUN_TEST(test_shell_cmd_cd_valid_dir);
    RUN_TEST(test_shell_cmd_cd_nonexistent);
    RUN_TEST(test_shell_cmd_cd_file);
    RUN_TEST(test_shell_cmd_cd_dotdot);
    RUN_TEST(test_shell_cmd_cd_dotdot_at_root);
    RUN_TEST(test_shell_cmd_cd_dot);
    RUN_TEST(test_shell_cmd_ls_cwd);
    RUN_TEST(test_shell_cmd_ls_relative);
    RUN_TEST(test_shell_cmd_ls_dot);
    RUN_TEST(test_shell_cmd_ls_dotdot);
    RUN_TEST(test_shell_cmd_cat_relative);
    RUN_TEST(test_shell_path_complex);
    RUN_TEST(test_shell_path_trailing_slash);
    RUN_TEST(test_shell_path_double_slash);
    RUN_TEST(test_shell_cmd_cd_mount_subdir);
    RUN_TEST(test_shell_mount_relative_path);
    RUN_TEST(test_shell_cmd_df_cwd);

    /* touch command tests */
    RUN_TEST(test_shell_cmd_touch);
    RUN_TEST(test_shell_cmd_touch_existing);

    /* cp command tests */
    RUN_TEST(test_shell_cmd_cp);
    RUN_TEST(test_shell_cmd_cp_binary);
    RUN_TEST(test_shell_cmd_cp_not_found);
    RUN_TEST(test_shell_cmd_cp_missing_args);

    /* stat command tests */
    RUN_TEST(test_shell_cmd_stat_file);
    RUN_TEST(test_shell_cmd_stat_dir);
    RUN_TEST(test_shell_cmd_stat_standard_storage_dirs);
    RUN_TEST(test_shell_cmd_stat_not_found);
    RUN_TEST(test_shell_cmd_stat_virtual);
    RUN_TEST(test_shell_cmd_stat_missing_args);

    /* tree command tests */
    RUN_TEST(test_shell_cmd_tree);
    RUN_TEST(test_shell_cmd_tree_subdir);
    RUN_TEST(test_shell_cmd_tree_depth);
    RUN_TEST(test_shell_cmd_tree_virtual);
    RUN_TEST(test_shell_cmd_tree_root);

    /* wc command tests */
    RUN_TEST(test_shell_cmd_wc);
    RUN_TEST(test_shell_cmd_wc_known_content);
    RUN_TEST(test_shell_cmd_wc_not_found);
    RUN_TEST(test_shell_cmd_wc_missing_args);

    /* hexdump command tests */
    RUN_TEST(test_shell_cmd_hexdump);
    RUN_TEST(test_shell_cmd_hexdump_offset);
    RUN_TEST(test_shell_cmd_hexdump_middle);
    RUN_TEST(test_shell_cmd_hexdump_not_found);
    RUN_TEST(test_shell_cmd_hexdump_missing_args);

    /* grep command tests */
    RUN_TEST(test_shell_cmd_grep);
    RUN_TEST(test_shell_cmd_grep_middle);
    RUN_TEST(test_shell_cmd_grep_case);
    RUN_TEST(test_shell_cmd_grep_no_match);
    RUN_TEST(test_shell_cmd_grep_not_found);
    RUN_TEST(test_shell_cmd_grep_missing_args);
    RUN_TEST(test_shell_cmd_grep_multiline);

    /* find command tests */
    RUN_TEST(test_shell_cmd_find);
    RUN_TEST(test_shell_cmd_find_wildcard);
    RUN_TEST(test_shell_cmd_find_leading_wildcard);
    RUN_TEST(test_shell_cmd_find_no_match);
    RUN_TEST(test_shell_cmd_find_question);
    RUN_TEST(test_shell_cmd_find_subdir);
    RUN_TEST(test_shell_cmd_find_missing_args);
    RUN_TEST(test_shell_cmd_find_nonmount);

    /* Help system tests */
    RUN_TEST(test_shell_cmd_help_list);
    RUN_TEST(test_shell_cmd_help_valid);
    RUN_TEST(test_shell_cmd_help_unknown);
    RUN_TEST(test_builtin_commands_grouped_and_sorted);
    RUN_TEST(test_external_commands_categories_in_range);
    RUN_TEST(test_shell_help_files_exist);
    RUN_TEST(test_shell_help_file_content);
    RUN_TEST(test_every_command_has_help_entry);
    RUN_TEST(test_shell_help_dir_listing);
    RUN_TEST(test_lua_shell_commands_registered_with_expected_mutation_modes);

    /* Write/modify command tests */
    RUN_TEST(test_shell_cmd_write);
    RUN_TEST(test_shell_cmd_write_no_args);
    RUN_TEST(test_shell_cmd_write_escapes);
    RUN_TEST(test_shell_cmd_write_hex_escape);
    RUN_TEST(test_shell_cmd_write_large_content);
    RUN_TEST(test_shell_cmd_put);
    RUN_TEST(test_shell_cmd_put_append);
    RUN_TEST(test_shell_cmd_put_invalid_hex);
    RUN_TEST(test_shell_cmd_put_no_args);
    RUN_TEST(test_shell_cmd_xput_lifecycle);
    RUN_TEST(test_shell_cmd_xput_offset_mismatch);
    RUN_TEST(test_shell_cmd_xput_incomplete_finish);
    RUN_TEST(test_shell_cmd_xput_isolated_per_session);
    RUN_TEST(test_shell_cmd_xput_resume_after_disconnect);
    RUN_TEST(test_shell_cmd_xput_resume_missing_file);
    RUN_TEST(test_shell_cmd_xput_resume_existing_too_large);
    RUN_TEST(test_shell_cmd_mkdir);
    RUN_TEST(test_shell_cmd_mkdir_no_args);
    RUN_TEST(test_shell_cmd_rm);
    RUN_TEST(test_shell_cmd_rm_nonexistent);
    RUN_TEST(test_shell_cmd_mv);
    RUN_TEST(test_shell_cmd_mv_missing_args);
    RUN_TEST(test_shell_cmd_append);
    RUN_TEST(test_shell_cmd_append_no_args);
    RUN_TEST(test_shell_cmd_truncate);
    RUN_TEST(test_shell_cmd_truncate_no_args);

    /* sleep command validation (CORE-M1) */
    RUN_TEST(test_shell_cmd_sleep_rejects_non_numeric);
    RUN_TEST(test_shell_cmd_sleep_rejects_zero);
    RUN_TEST(test_shell_cmd_sleep_rejects_negative);
    RUN_TEST(test_shell_cmd_sleep_rejects_trailing_garbage);
    RUN_TEST(test_shell_cmd_sleep_missing_args);

    /* Path resolution unit tests (CORE-H4) */
    RUN_TEST(test_shell_resolve_path_absolute_simple);
    RUN_TEST(test_shell_resolve_path_dotdot);
    RUN_TEST(test_shell_resolve_path_dot);
    RUN_TEST(test_shell_resolve_path_dotdot_at_root);
    RUN_TEST(test_shell_resolve_path_double_slash);
    RUN_TEST(test_shell_resolve_path_trailing_slash);
    RUN_TEST(test_shell_resolve_path_empty);
    RUN_TEST(test_shell_resolve_path_relative);
    RUN_TEST(test_shell_resolve_path_overflow);
    RUN_TEST(test_shell_resolve_path_rejects_null);

    /* String function regression tests */
    RUN_TEST(test_string_strcmp_basic);
    RUN_TEST(test_string_strlen_basic);
    RUN_TEST(test_string_strcpy_basic);
    RUN_TEST(test_string_strncpy_basic);

    /* kprintf format regression tests */
    RUN_TEST(test_kprintf_decimal);
    RUN_TEST(test_kprintf_hex);
    RUN_TEST(test_kprintf_string);
    RUN_TEST(test_kprintf_pointer);
    RUN_TEST(test_kprintf_width_pad);
    RUN_TEST(test_kprintf_long);
    RUN_TEST(test_kprintf_long_long);
    RUN_TEST(test_kprintf_long_long_edge_cases);
    RUN_TEST(test_kprintf_size_t);
    RUN_TEST(test_kprintf_percent);
    RUN_TEST(test_kprintf_mixed);

    /* Float-format coverage (added with %f / %e / %g support).
     * Width / left-justify / zero-pad coverage rides on the integer
     * specifiers' tests above — the float padding code uses the same
     * fmt_padding helper. */
    RUN_TEST(test_kprintf_float_default_precision);
    RUN_TEST(test_kprintf_float_explicit_precision);
    RUN_TEST(test_kprintf_float_rounding_carry);
    RUN_TEST(test_kprintf_float_specials);
    RUN_TEST(test_kprintf_scientific);
    RUN_TEST(test_kprintf_g_format);

    /* kprintf_float_emit padding branches + va_list crossing
     * (review #554 round-1 follow-ups). */
    RUN_TEST(test_kprintf_float_padded_zero_pad_negative);
    RUN_TEST(test_kprintf_float_padded_left_justify);
    RUN_TEST(test_kprintf_float_padded_inf_uses_space_not_zero);
    RUN_TEST(test_kprintf_float_padded_neg_inf_uses_space_not_zero);
    RUN_TEST(test_kprintf_float_padded_nan_uses_space_not_zero);
    RUN_TEST(test_kprintf_float_e2e_va_list_crossing);

    return UNITY_END();
}
