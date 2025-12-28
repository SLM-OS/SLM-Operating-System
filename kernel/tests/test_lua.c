/*
 * test_lua.c - Lua Integration Tests for SLM-OS
 *
 * Tests the Lua scripting engine through the lua_slm API.
 * Note: We only test through lua_slm_dostring since the kernel is compiled
 * with -mgeneral-regs-only and cannot directly use Lua's FP-using API.
 */

#include "unity.h"
#include "../include/lua_slm.h"
#include "../include/uart.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Lua State Creation Tests
 * ============================================================================ */

/*
 * Test: lua_slm_newstate creates a valid Lua state
 */
static void test_lua_newstate_basic(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);
    lua_slm_close(L);
}

/*
 * Test: Multiple Lua states can be created and closed
 */
static void test_lua_multiple_states(void)
{
    lua_State *L1 = lua_slm_newstate();
    lua_State *L2 = lua_slm_newstate();

    TEST_ASSERT_NOT_NULL(L1);
    TEST_ASSERT_NOT_NULL(L2);
    TEST_ASSERT_NOT_EQUAL(L1, L2);

    lua_slm_close(L1);
    lua_slm_close(L2);
}

/*
 * Test: Closing NULL state is safe
 */
static void test_lua_close_null(void)
{
    /* Should not crash */
    lua_slm_close(NULL);
    TEST_PASS();
}

/* ============================================================================
 * Basic Lua Execution Tests
 * ============================================================================ */

/*
 * Test: Simple arithmetic expression succeeds
 */
static void test_lua_arithmetic(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    int result = lua_slm_dostring(L, "x = 1 + 2 + 3");
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: String concatenation works
 */
static void test_lua_strings(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    int result = lua_slm_dostring(L, "s = 'Hello' .. ' ' .. 'World'");
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: Table creation and access
 */
static void test_lua_tables(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    int result = lua_slm_dostring(L, "t = {a=1, b=2, c=3}; sum = t.a + t.b + t.c");
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: Function definition and call
 */
static void test_lua_functions(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "function add(a, b) return a + b end\n"
        "result = add(10, 20)";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: Loops work correctly
 */
static void test_lua_loops(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "sum = 0\n"
        "for i = 1, 10 do sum = sum + i end\n"
        "assert(sum == 55, 'sum should be 55')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/* ============================================================================
 * Error Handling Tests
 * ============================================================================ */

/*
 * Test: Syntax error returns non-zero
 */
static void test_lua_syntax_error(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    /* Missing 'end' keyword */
    int result = lua_slm_dostring(L, "if true then x = 1");
    TEST_ASSERT_NOT_EQUAL(0, result);

    lua_slm_close(L);
}

/*
 * Test: Runtime error returns non-zero
 */
static void test_lua_runtime_error(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    /* Call nil value */
    int result = lua_slm_dostring(L, "foo()");
    TEST_ASSERT_NOT_EQUAL(0, result);

    lua_slm_close(L);
}

/*
 * Test: Error in pcall is caught
 */
static void test_lua_pcall_error(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "function failing() error('test error') end\n"
        "ok, err = pcall(failing)\n"
        "assert(ok == false, 'pcall should return false')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/* ============================================================================
 * SLM-OS Bindings Tests
 * ============================================================================ */

/*
 * Test: slm module exists and has expected functions
 */
static void test_slm_module_exists(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "assert(type(slm) == 'table', 'slm should be a table')\n"
        "assert(type(slm.uptime) == 'function', 'slm.uptime should be function')\n"
        "assert(type(slm.mem_stats) == 'function', 'slm.mem_stats should be function')\n"
        "assert(type(slm.tasks) == 'function', 'slm.tasks should be function')\n"
        "assert(type(slm.version) == 'function', 'slm.version should be function')\n"
        "assert(type(slm.cpu_count) == 'function', 'slm.cpu_count should be function')\n"
        "assert(type(slm.cpu_id) == 'function', 'slm.cpu_id should be function')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: slm.uptime returns positive value
 */
static void test_slm_uptime(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "t = slm.uptime()\n"
        "assert(type(t) == 'number', 'uptime should return number')\n"
        "assert(t >= 0, 'uptime should be >= 0')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: slm.mem_stats returns valid table
 */
static void test_slm_mem_stats(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "stats = slm.mem_stats()\n"
        "assert(type(stats) == 'table', 'mem_stats should return table')\n"
        "assert(stats.total_kb > 0, 'total_kb should be > 0')\n"
        "assert(stats.free_kb >= 0, 'free_kb should be >= 0')\n"
        "assert(stats.used_kb >= 0, 'used_kb should be >= 0')\n"
        "assert(stats.total_kb == stats.free_kb + stats.used_kb, 'total = free + used')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: slm.tasks returns array of task tables
 */
static void test_slm_tasks(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "tasks = slm.tasks()\n"
        "assert(type(tasks) == 'table', 'tasks should return table')\n"
        "assert(#tasks > 0, 'should have at least one task')\n"
        "t = tasks[1]\n"
        "assert(t.id ~= nil, 'task should have id')\n"
        "assert(t.name ~= nil, 'task should have name')\n"
        "assert(t.state ~= nil, 'task should have state')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: slm.version returns non-empty string
 */
static void test_slm_version(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "ver = slm.version()\n"
        "assert(type(ver) == 'string', 'version should return string')\n"
        "assert(#ver > 0, 'version should not be empty')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: slm.cpu_count returns positive value
 */
static void test_slm_cpu_count(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "cpus = slm.cpu_count()\n"
        "assert(type(cpus) == 'number', 'cpu_count should return number')\n"
        "assert(cpus > 0, 'should have at least one CPU')\n"
        "assert(cpus <= 64, 'sanity check: <= 64 CPUs')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: slm.cpu_id returns valid value
 */
static void test_slm_cpu_id(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "cpu = slm.cpu_id()\n"
        "assert(type(cpu) == 'number', 'cpu_id should return number')\n"
        "assert(cpu >= 0, 'cpu_id should be >= 0')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/* ============================================================================
 * Standard Library Tests
 * ============================================================================ */

/*
 * Test: string library is available
 */
static void test_lua_string_lib(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "s = string.upper('hello')\n"
        "assert(s == 'HELLO', 'string.upper failed')\n"
        "len = string.len('test')\n"
        "assert(len == 4, 'string.len failed')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: table library is available
 */
static void test_lua_table_lib(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "t = {3, 1, 4, 1, 5}\n"
        "table.sort(t)\n"
        "assert(t[1] == 1, 'first element should be 1')\n"
        "assert(t[5] == 5, 'last element should be 5')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: math library is available
 */
static void test_lua_math_lib(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "m = math.max(1, 5, 3)\n"
        "assert(m == 5, 'math.max failed')\n"
        "a = math.abs(-42)\n"
        "assert(a == 42, 'math.abs failed')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/*
 * Test: Complex script with multiple features
 */
static void test_lua_complex_script(void)
{
    lua_State *L = lua_slm_newstate();
    TEST_ASSERT_NOT_NULL(L);

    const char *code =
        "-- Define a class-like table\n"
        "Counter = {}\n"
        "function Counter.new(start)\n"
        "    local self = {value = start or 0}\n"
        "    setmetatable(self, {__index = Counter})\n"
        "    return self\n"
        "end\n"
        "function Counter:increment()\n"
        "    self.value = self.value + 1\n"
        "end\n"
        "function Counter:get()\n"
        "    return self.value\n"
        "end\n"
        "\n"
        "-- Test it\n"
        "c = Counter.new(10)\n"
        "for i = 1, 5 do c:increment() end\n"
        "assert(c:get() == 15, 'Counter should be 15')";

    int result = lua_slm_dostring(L, code);
    TEST_ASSERT_EQUAL_INT(0, result);

    lua_slm_close(L);
}

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_lua(void)
{
    UNITY_BEGIN();

    /* State management */
    RUN_TEST(test_lua_newstate_basic);
    RUN_TEST(test_lua_multiple_states);
    RUN_TEST(test_lua_close_null);

    /* Basic execution */
    RUN_TEST(test_lua_arithmetic);
    RUN_TEST(test_lua_strings);
    RUN_TEST(test_lua_tables);
    RUN_TEST(test_lua_functions);
    RUN_TEST(test_lua_loops);

    /* Error handling */
    RUN_TEST(test_lua_syntax_error);
    RUN_TEST(test_lua_runtime_error);
    RUN_TEST(test_lua_pcall_error);

    /* SLM-OS bindings */
    RUN_TEST(test_slm_module_exists);
    RUN_TEST(test_slm_uptime);
    RUN_TEST(test_slm_mem_stats);
    RUN_TEST(test_slm_tasks);
    RUN_TEST(test_slm_version);
    RUN_TEST(test_slm_cpu_count);
    RUN_TEST(test_slm_cpu_id);

    /* Standard libraries */
    RUN_TEST(test_lua_string_lib);
    RUN_TEST(test_lua_table_lib);
    RUN_TEST(test_lua_math_lib);

    /* Complex integration */
    RUN_TEST(test_lua_complex_script);

    return UNITY_END();
}
