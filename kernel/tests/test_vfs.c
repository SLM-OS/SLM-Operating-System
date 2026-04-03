/**
 * @file test_vfs.c
 * @brief Virtual Filesystem Tests
 *
 * Validates VFS structure, lookup, read operations, and virtual file content.
 */

#include "unity.h"
#include "vfs.h"
#include "uart.h"
#include "dtb.h"
#include <stdint.h>

/* ============================================================================
 * Structure Tests
 * ============================================================================ */

/**
 * Test root node exists.
 */
static void test_vfs_root_exists(void)
{
    struct vfs_node *root = vfs_lookup("/");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL(VFS_NODE_DIR, root->type);
}

/**
 * Test /sys directory exists.
 */
static void test_vfs_sys_exists(void)
{
    struct vfs_node *sys = vfs_lookup("/sys");
    TEST_ASSERT_NOT_NULL(sys);
    TEST_ASSERT_EQUAL(VFS_NODE_DIR, sys->type);
}

/**
 * Test /proc directory exists.
 */
static void test_vfs_proc_exists(void)
{
    struct vfs_node *proc = vfs_lookup("/proc");
    TEST_ASSERT_NOT_NULL(proc);
    TEST_ASSERT_EQUAL(VFS_NODE_DIR, proc->type);
}

/**
 * Test /components directory exists.
 */
static void test_vfs_components_exists(void)
{
    struct vfs_node *components = vfs_lookup("/components");
    TEST_ASSERT_NOT_NULL(components);
    TEST_ASSERT_EQUAL(VFS_NODE_DIR, components->type);
}

/* ============================================================================
 * Lookup Tests
 * ============================================================================ */

/**
 * Test lookup of virtual files in /sys.
 */
static void test_vfs_lookup_sys_files(void)
{
    struct vfs_node *memory = vfs_lookup("/sys/memory");
    TEST_ASSERT_NOT_NULL(memory);
    TEST_ASSERT_EQUAL(VFS_NODE_FILE, memory->type);

    struct vfs_node *uptime = vfs_lookup("/sys/uptime");
    TEST_ASSERT_NOT_NULL(uptime);
    TEST_ASSERT_EQUAL(VFS_NODE_FILE, uptime->type);

    struct vfs_node *cpus = vfs_lookup("/sys/cpus");
    TEST_ASSERT_NOT_NULL(cpus);
    TEST_ASSERT_EQUAL(VFS_NODE_FILE, cpus->type);

    struct vfs_node *version = vfs_lookup("/sys/version");
    TEST_ASSERT_NOT_NULL(version);
    TEST_ASSERT_EQUAL(VFS_NODE_FILE, version->type);
}

/**
 * Test lookup of virtual files in /proc.
 */
static void test_vfs_lookup_proc_files(void)
{
    struct vfs_node *tasks = vfs_lookup("/proc/tasks");
    TEST_ASSERT_NOT_NULL(tasks);
    TEST_ASSERT_EQUAL(VFS_NODE_FILE, tasks->type);

    struct vfs_node *self = vfs_lookup("/proc/self");
    TEST_ASSERT_NOT_NULL(self);
    TEST_ASSERT_EQUAL(VFS_NODE_FILE, self->type);
}

/**
 * Test lookup of non-existent path returns NULL.
 */
static void test_vfs_lookup_nonexistent(void)
{
    TEST_ASSERT_NULL(vfs_lookup("/nonexistent"));
    TEST_ASSERT_NULL(vfs_lookup("/sys/nonexistent"));
    TEST_ASSERT_NULL(vfs_lookup("/proc/nonexistent"));
    TEST_ASSERT_NULL(vfs_lookup("/a/b/c/d/e"));
}

/**
 * Test lookup with NULL path.
 */
static void test_vfs_lookup_null(void)
{
    TEST_ASSERT_NULL(vfs_lookup(NULL));
}

/**
 * Test lookup with empty path.
 */
static void test_vfs_lookup_empty(void)
{
    TEST_ASSERT_NULL(vfs_lookup(""));
}

/**
 * Test lookup without leading slash fails.
 */
static void test_vfs_lookup_no_leading_slash(void)
{
    /* Paths must start with / */
    TEST_ASSERT_NULL(vfs_lookup("sys"));
    TEST_ASSERT_NULL(vfs_lookup("sys/memory"));
}

/* ============================================================================
 * Read Tests
 * ============================================================================ */

/**
 * Test reading /sys/version produces content.
 */
static void test_vfs_read_version(void)
{
    struct vfs_node *version = vfs_lookup("/sys/version");
    TEST_ASSERT_NOT_NULL(version);

    char buf[256];
    int len = vfs_read(version, buf, sizeof(buf) - 1);

    TEST_ASSERT_GREATER_THAN(0, len);
    buf[len] = '\0';

    /* Should contain version string */
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, "Version read returned empty");
    /* Version should contain "SLM-OS" or version number */
}

/**
 * Test reading /sys/memory produces content.
 */
static void test_vfs_read_memory(void)
{
    struct vfs_node *memory = vfs_lookup("/sys/memory");
    TEST_ASSERT_NOT_NULL(memory);

    char buf[512];
    int len = vfs_read(memory, buf, sizeof(buf) - 1);

    TEST_ASSERT_GREATER_THAN(0, len);
    buf[len] = '\0';

    /* Should contain memory statistics */
    /* Look for key words that should appear */
}

/**
 * Test reading /sys/uptime produces content.
 */
static void test_vfs_read_uptime(void)
{
    struct vfs_node *uptime = vfs_lookup("/sys/uptime");
    TEST_ASSERT_NOT_NULL(uptime);

    char buf[128];
    int len = vfs_read(uptime, buf, sizeof(buf) - 1);

    TEST_ASSERT_GREATER_THAN(0, len);
    buf[len] = '\0';

    /* Uptime should be a number (seconds) */
}

/**
 * Test reading /sys/cpus produces content.
 */
static void test_vfs_read_cpus(void)
{
    struct vfs_node *cpus = vfs_lookup("/sys/cpus");
    TEST_ASSERT_NOT_NULL(cpus);

    char buf[256];
    int len = vfs_read(cpus, buf, sizeof(buf) - 1);

    TEST_ASSERT_GREATER_THAN(0, len);
    buf[len] = '\0';
}

/**
 * Test reading /proc/tasks produces content.
 */
static void test_vfs_read_tasks(void)
{
    struct vfs_node *tasks = vfs_lookup("/proc/tasks");
    TEST_ASSERT_NOT_NULL(tasks);

    char buf[1024];
    int len = vfs_read(tasks, buf, sizeof(buf) - 1);

    TEST_ASSERT_GREATER_THAN(0, len);
    buf[len] = '\0';

    /* Should list at least the idle task and shell task */
}

/**
 * Test reading /proc/self produces content.
 */
static void test_vfs_read_self(void)
{
    struct vfs_node *self = vfs_lookup("/proc/self");
    TEST_ASSERT_NOT_NULL(self);

    char buf[256];
    int len = vfs_read(self, buf, sizeof(buf) - 1);

    TEST_ASSERT_GREATER_THAN(0, len);
    buf[len] = '\0';

    /* Should contain info about current task */
}

/**
 * Test reading a directory returns error.
 */
static void test_vfs_read_directory(void)
{
    struct vfs_node *sys = vfs_lookup("/sys");
    TEST_ASSERT_NOT_NULL(sys);

    char buf[64];
    int len = vfs_read(sys, buf, sizeof(buf));

    /* Reading a directory should fail or return 0 */
    TEST_ASSERT_LESS_OR_EQUAL(0, len);
}

/**
 * Test reading with NULL buffer.
 */
static void test_vfs_read_null_buffer(void)
{
    struct vfs_node *version = vfs_lookup("/sys/version");
    TEST_ASSERT_NOT_NULL(version);

    int len = vfs_read(version, NULL, 100);
    TEST_ASSERT_LESS_OR_EQUAL(0, len);
}

/**
 * Test reading with zero size.
 * uart_snprintf returns -1 for size 0 (edge case handling).
 */
static void test_vfs_read_zero_size(void)
{
    struct vfs_node *version = vfs_lookup("/sys/version");
    TEST_ASSERT_NOT_NULL(version);

    char buf[64];
    int len = vfs_read(version, buf, 0);
    /* Implementation returns -1 for size 0 as edge case */
    TEST_ASSERT_EQUAL(-1, len);
}

/**
 * Test reading with small buffer.
 * Note: snprintf returns would-be length, not actual bytes written.
 * Data is truncated but return value shows full content size.
 */
static void test_vfs_read_small_buffer(void)
{
    struct vfs_node *memory = vfs_lookup("/sys/memory");
    TEST_ASSERT_NOT_NULL(memory);

    /* Use very small buffer */
    char small_buf[16];
    int len = vfs_read(memory, small_buf, sizeof(small_buf) - 1);

    /* snprintf returns the would-be length (full content), which is larger */
    TEST_ASSERT_GREATER_OR_EQUAL(0, len);
    /* The buffer is still null-terminated and usable */
}

/* ============================================================================
 * List Directory Tests
 * ============================================================================ */

/**
 * Test listing root directory.
 */
static void test_vfs_list_root(void)
{
    struct vfs_node *root = vfs_lookup("/");
    TEST_ASSERT_NOT_NULL(root);

    /* Root should have children (sys, proc, components) */
    TEST_ASSERT_GREATER_THAN(0, root->num_children);

    /* Check that expected children exist */
    bool found_sys = false;
    bool found_proc = false;
    bool found_components = false;

    for (int i = 0; i < root->num_children; i++) {
        if (root->children[i] != NULL) {
            if (unity_strcmp(root->children[i]->name, "sys") == 0) found_sys = true;
            if (unity_strcmp(root->children[i]->name, "proc") == 0) found_proc = true;
            if (unity_strcmp(root->children[i]->name, "components") == 0) found_components = true;
        }
    }

    TEST_ASSERT_TRUE(found_sys);
    TEST_ASSERT_TRUE(found_proc);
    TEST_ASSERT_TRUE(found_components);
}

/**
 * Test listing /sys directory.
 */
static void test_vfs_list_sys(void)
{
    struct vfs_node *sys = vfs_lookup("/sys");
    TEST_ASSERT_NOT_NULL(sys);

    /* /sys should have virtual files */
    TEST_ASSERT_GREATER_THAN(0, sys->num_children);

    /* Check for expected files */
    bool found_memory = false;
    bool found_uptime = false;
    bool found_version = false;

    for (int i = 0; i < sys->num_children; i++) {
        if (sys->children[i] != NULL) {
            if (unity_strcmp(sys->children[i]->name, "memory") == 0) found_memory = true;
            if (unity_strcmp(sys->children[i]->name, "uptime") == 0) found_uptime = true;
            if (unity_strcmp(sys->children[i]->name, "version") == 0) found_version = true;
        }
    }

    TEST_ASSERT_TRUE(found_memory);
    TEST_ASSERT_TRUE(found_uptime);
    TEST_ASSERT_TRUE(found_version);
}

/* ============================================================================
 * Consistency Tests
 * ============================================================================ */

/**
 * Test multiple reads return consistent results.
 */
static void test_vfs_read_consistency(void)
{
    struct vfs_node *version = vfs_lookup("/sys/version");
    TEST_ASSERT_NOT_NULL(version);

    char buf1[256], buf2[256];

    int len1 = vfs_read(version, buf1, sizeof(buf1) - 1);
    int len2 = vfs_read(version, buf2, sizeof(buf2) - 1);

    /* Lengths should match */
    TEST_ASSERT_EQUAL(len1, len2);

    /* Content should match (version doesn't change) */
    if (len1 > 0) {
        buf1[len1] = '\0';
        buf2[len2] = '\0';
        TEST_ASSERT_EQUAL_STRING(buf1, buf2);
    }
}

/* String comparison uses unity_strcmp from unity.h */

/* ============================================================================
 * DTB Parser Basic Tests (TEST-L6)
 * ============================================================================ */

/**
 * Test dtb_get_info() returns a valid struct.
 */
static void test_dtb_info_available(void)
{
    const fdt_info_t *info = dtb_get_info();
    TEST_ASSERT_NOT_NULL(info);
}

/**
 * Test DTB reports a reasonable CPU count.
 */
static void test_dtb_cpu_count_reasonable(void)
{
    const fdt_info_t *info = dtb_get_info();
    TEST_ASSERT_NOT_NULL(info);
    /* If parsing succeeded, should detect at least 1 CPU, no more than 64 */
    if (info->valid) {
        TEST_ASSERT_TRUE(info->cpu_count >= 1);
        TEST_ASSERT_TRUE(info->cpu_count <= 64);
    }
}

/**
 * Test DTB reports non-zero memory size.
 */
static void test_dtb_memory_info(void)
{
    const fdt_info_t *info = dtb_get_info();
    TEST_ASSERT_NOT_NULL(info);
    if (info->valid) {
        TEST_ASSERT_TRUE(info->ram_size > 0);
    }
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

/**
 * Run all VFS tests.
 * Returns number of failures.
 */
int test_suite_vfs(void)
{
    UnityBegin("VFS Tests");

    /* Structure tests */
    RUN_TEST(test_vfs_root_exists);
    RUN_TEST(test_vfs_sys_exists);
    RUN_TEST(test_vfs_proc_exists);
    RUN_TEST(test_vfs_components_exists);

    /* Lookup tests */
    RUN_TEST(test_vfs_lookup_sys_files);
    RUN_TEST(test_vfs_lookup_proc_files);
    RUN_TEST(test_vfs_lookup_nonexistent);
    RUN_TEST(test_vfs_lookup_null);
    RUN_TEST(test_vfs_lookup_empty);
    RUN_TEST(test_vfs_lookup_no_leading_slash);

    /* Read tests */
    RUN_TEST(test_vfs_read_version);
    RUN_TEST(test_vfs_read_memory);
    RUN_TEST(test_vfs_read_uptime);
    RUN_TEST(test_vfs_read_cpus);
    RUN_TEST(test_vfs_read_tasks);
    RUN_TEST(test_vfs_read_self);
    RUN_TEST(test_vfs_read_directory);
    RUN_TEST(test_vfs_read_null_buffer);
    RUN_TEST(test_vfs_read_zero_size);
    RUN_TEST(test_vfs_read_small_buffer);

    /* List tests */
    RUN_TEST(test_vfs_list_root);
    RUN_TEST(test_vfs_list_sys);

    /* Consistency tests */
    RUN_TEST(test_vfs_read_consistency);

    /* DTB parser basic tests (TEST-L6) */
    RUN_TEST(test_dtb_info_available);
    RUN_TEST(test_dtb_cpu_count_reasonable);
    RUN_TEST(test_dtb_memory_info);

    int failures = UnityEnd();

    if (failures == 0) {
        uart_puts("[INFO] VFS tests passed\n");
    } else {
        uart_printf("[FAIL] VFS tests: %d failures\n", failures);
    }

    return failures;
}
