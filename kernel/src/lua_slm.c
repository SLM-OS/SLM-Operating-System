/**
 * lua_slm.c - Lua Integration for SLM-OS
 *
 * Provides Lua scripting support with kernel API bindings.
 * Creates the 'slm' table with functions to access kernel features.
 */

#include "lua_slm.h"
#include "build_info.h"
#include "camera.h"
#include "debug.h"
#include "timer.h"
#include "pmm.h"
#include "sched.h"
#include "task.h"
#include "shell.h"
#include "shell_internal.h"
#include "shell_session.h"
#include "vfs.h"
#include "component.h"
#include "spinlock.h"
#include "inference_device.h"
#include "slm_ffi.h"
#include "sched_policy.h"
#include "ipc.h"
#include "smp.h"
#include "string.h"
#include "latency_hist.h"
#include "rate_ewma.h"
#include "gpu_consumer.h"
#include "admin_telemetry.h"
#if defined(ENABLE_NETWORKING)
#include "shell_io_tcp.h"
#include "tcp_shell_server.h"
#include "net.h"
#include "net_http.h"
#endif
#if !defined(PLATFORM_X86_64)
#include "vmm.h"
#endif
#if defined(CONFIG_AI_SCHEDULER)
#include "ai_types.h"   /* ai_sched_action, ai_decode_action — #211 review fix */
#include "runtime_model.h"
#endif

/* Lua headers - note: these may include stdio.h from newlib */
#include "../lib/lua/src/lua.h"
#include "../lib/lua/src/lauxlib.h"
#include "../lib/lua/src/lualib.h"

/* Router-side component slots reserved for Lua states. Native components use
 * 0..COMPONENT_MAX_COUNT-1; Lua states get a disjoint fixed pool above that.
 * Size covers the console session, all TCP sessions, and the Lua task pool. */
#define LUA_TASK_MAX_CTX 16
#define LUA_MSG_COMPONENT_BASE COMPONENT_MAX_COUNT
#define LUA_MSG_COMPONENT_CAP  (MAX_TCP_SHELL_SESSIONS + 1 + LUA_TASK_MAX_CTX)
#define LUA_MSG_COMPONENT_MAX  (LUA_MSG_COMPONENT_BASE + LUA_MSG_COMPONENT_CAP)

/* ============================================================================
 * SLM-OS Kernel Bindings
 * ============================================================================ */

/**
 * slm.print(msg) - Print message to console
 */
static int l_print(lua_State *L) {
    if (!L) return 0;
    int nargs = lua_gettop(L);
    for (int i = 1; i <= nargs; i++) {
        if (i > 1) shell_printf("\t");
        if (lua_isstring(L, i)) {
            shell_printf("%s", lua_tostring(L, i));
        } else if (lua_isnil(L, i)) {
            shell_printf("nil");
        } else if (lua_isboolean(L, i)) {
            shell_printf("%s", lua_toboolean(L, i) ? "true" : "false");
        } else if (lua_isnumber(L, i)) {
            lua_Number n = lua_tonumber(L, i);
            shell_printf("%d", (int)n);
        } else {
            shell_printf("%s: %p", luaL_typename(L, i), lua_topointer(L, i));
        }
    }
    shell_printf("\n");
    return 0;
}

/**
 * slm.uptime() - Get system uptime in milliseconds
 */
static int l_uptime(lua_State *L) {
    if (!L) return 0;
    uint64_t count = timer_get_count();
    uint64_t freq = timer_get_frequency();
    uint64_t ms = count / (freq / 1000);
    lua_pushinteger(L, (lua_Integer)ms);
    return 1;
}

/*
 * slm.uptime_us() — microsecond resolution uptime.
 *
 * Pi 5's CNTPCT runs at ~54 MHz (18.5 ns per tick), giving real sub-
 * microsecond precision. Use this for benchmarks that measure work
 * shorter than a millisecond — e.g., slm.hailo.infer on a Hailo-8L
 * classifier that completes in ~0.5 ms native. slm.uptime() buckets
 * such samples into 0/1/2 ms quantized garbage.
 *
 * Returned as a Lua integer (signed 64-bit in Lua 5.4). 64-bit us
 * rolls over after ~292 thousand years — no wrap to worry about.
 */
static int l_uptime_us(lua_State *L) {
    if (!L) return 0;
    uint64_t count = timer_get_count();
    uint64_t freq = timer_get_frequency();
    /* us = count * 1e6 / freq. Compute in 64-bit to keep precision
     * for high-frequency counters. (count * 1000000) fits uint64 for
     * any realistic freq (54 MHz → ~342 years before overflow). */
    uint64_t us = (count * 1000000ULL) / freq;
    lua_pushinteger(L, (lua_Integer)us);
    return 1;
}

/**
 * slm.mem_stats() - Get memory statistics
 * Returns table: {total_kb, free_kb, used_kb}
 */
static int l_mem_stats(lua_State *L) {
    if (!L) return 0;
    size_t total_pages = pmm_get_total_pages();
    size_t free_pages = pmm_get_free_pages();
    size_t used_pages = total_pages - free_pages;

    lua_createtable(L, 0, 3);

    lua_pushinteger(L, (lua_Integer)(total_pages * 4));
    lua_setfield(L, -2, "total_kb");

    lua_pushinteger(L, (lua_Integer)(free_pages * 4));
    lua_setfield(L, -2, "free_kb");

    lua_pushinteger(L, (lua_Integer)(used_pages * 4));
    lua_setfield(L, -2, "used_kb");

    return 1;
}

/**
 * slm.tasks() - Get list of tasks
 * Returns array of tables: {{id, name, state, cpu}, ...}
 */
static int l_tasks(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);

    int idx = 1;
    for (int i = 0; i < MAX_TASKS; i++) {
        /* Iterate slots, not task IDs — IDs come from a monotonic
         * counter and can exceed MAX_TASKS (#321). */
        struct task *t = task_slot((uint32_t)i);
        if (t == NULL || t->id == 0 || t->state == TASK_TERMINATED) continue;

        lua_newtable(L);

        lua_pushinteger(L, t->id);
        lua_setfield(L, -2, "id");

        lua_pushstring(L, t->name);
        lua_setfield(L, -2, "name");

        const char *state_str;
        switch (t->state) {
            case TASK_READY: state_str = "ready"; break;
            case TASK_RUNNING: state_str = "running"; break;
            case TASK_BLOCKED: state_str = "blocked"; break;
            case TASK_TERMINATED: state_str = "terminated"; break;
            default: state_str = "unknown"; break;
        }
        lua_pushstring(L, state_str);
        lua_setfield(L, -2, "state");

        lua_pushinteger(L, t->assigned_cpu);
        lua_setfield(L, -2, "cpu");

        lua_pushinteger(L, t->priority);
        lua_setfield(L, -2, "priority");

        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/* Forward-declared: defined alongside the msg_subscribe machinery
 * further down the file (#207). */
static void lua_msg_drain(lua_State *L);

/**
 * slm.sleep(ms) - Sleep the calling Lua task for milliseconds (#319)
 *
 * Delegates to the scheduler-blocking task_sleep_ms primitive. Unlike
 * the earlier busy-wait, the task leaves the run queue entirely and
 * another ready task (or the idle task) runs until our deadline.
 *
 * msg_subscribe callbacks are drained immediately before and after
 * the sleep so any pending subscriber fires. A message published
 * mid-sleep will fire when sleep() returns (worst-case latency:
 * `ms`). Scripts that need tighter subscriber responsiveness should
 * yield more often instead of a single long sleep.
 */
static int l_sleep(lua_State *L) {
    if (!L) return 0;
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms > 0) {
        lua_msg_drain(L);
        task_sleep_ms((uint32_t)ms);
        lua_msg_drain(L);
    }
    return 0;
}

/**
 * slm.yield() - Yield CPU to scheduler
 *
 * Also drains any pending msg_subscribe callbacks (#207) so scripts that
 * yield periodically see subscriber dispatches without calling
 * slm.msg_drain() explicitly.
 */
static int l_yield(lua_State *L) {
    if (!L) return 0;
    yield();
    lua_msg_drain(L);
    return 0;
}

/**
 * slm.version() - Get SLM-OS version string
 */
static int l_version(lua_State *L) {
    if (!L) return 0;
    lua_pushstring(L, "SLM-OS " SLMOS_VERSION);
    return 1;
}

/**
 * slm.cpu_count() - Get number of online CPUs (#312)
 *
 * Returns the runtime CPU count from kernel/sched/smp.c, not the
 * compile-time MAX_CPUS ceiling. MAX_CPUS reported 8 on Pi 5 (4 cores)
 * because it is an array sizing constant, not a live count.
 */
static int l_cpu_count(lua_State *L) {
    if (!L) return 0;
    lua_pushinteger(L, (lua_Integer)cpu_count);
    return 1;
}

/**
 * slm.cpu_id() - Get current CPU's logical ID (#313)
 *
 * Delegates to the canonical cpu_id() from smp.h, which uses
 * cpu_logical_id(mpidr) to look up the logical index in the
 * platform-specific MPIDR map. Inline-asm'ing (mpidr & 0xFF)
 * previously returned Aff0, which is always 0 on Pi 5 (BCM2712
 * encodes the core ID in Aff1) and wrong on Jetson's dual-cluster
 * MPIDR layout.
 */
static int l_cpu_id(lua_State *L) {
    if (!L) return 0;
    lua_pushinteger(L, (lua_Integer)cpu_id());
    return 1;
}

/* ============================================================================
 * Component Management Bindings
 * ============================================================================ */

/**
 * slm.component_count() - Get number of registered components
 */
static int l_component_count(lua_State *L) {
    if (!L) return 0;
    lua_pushinteger(L, (lua_Integer)component_count());
    return 1;
}

/**
 * slm.component_list() - Get list of all components
 * Returns array of tables: {{name, version, type, priority, state, task_id}, ...}
 */
static int l_component_list(lua_State *L) {
    if (!L) return 0;
    uint32_t count = component_count();
    lua_createtable(L, (int)count, 0);

    int idx = 1;
    for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
        component_info_t info;
        if (component_get_info(i, &info) != 0)
            continue;

        lua_createtable(L, 0, 7);

        lua_pushstring(L, (const char *)info.name);
        lua_setfield(L, -2, "name");

        lua_pushstring(L, (const char *)info.version);
        lua_setfield(L, -2, "version");

        lua_pushstring(L, component_type_name((component_type_t)info.component_type));
        lua_setfield(L, -2, "type");

        lua_pushinteger(L, info.priority);
        lua_setfield(L, -2, "priority");

        lua_pushstring(L, component_state_name((component_state_t)info.state));
        lua_setfield(L, -2, "state");

        lua_pushinteger(L, info.task_id);
        lua_setfield(L, -2, "task_id");

        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, "index");

        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/**
 * slm.component_find(name) - Find component by name
 * Returns index or nil if not found
 */
static int l_component_find(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    int idx = component_find(name);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/**
 * slm.component_run(name) - Run a built-in component
 * Returns index or nil on failure
 */
static int l_component_run(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    int idx = component_run(name);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/**
 * slm.component_hot_swap(old_name, new_name) - Hot-swap a component
 * Returns new index or nil on failure
 */
static int l_component_hot_swap(lua_State *L) {
    if (!L) return 0;
    const char *old_name = luaL_checkstring(L, 1);
    const char *new_name = luaL_checkstring(L, 2);
    int idx = component_hot_swap(old_name, new_name);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/**
 * slm.component_hot_swap_stateful(old_name, new_name) - Stateful hot-swap
 * Exports state from old component, transfers to new.
 * Currently supports sensor_monitor (transfers alert count).
 * Returns new index or nil on failure.
 */
/* Export function registry for stateful hot-swap.
 * Components register their export callback here. */
struct state_export_entry {
    const char *name;
    component_state_export_fn fn;
};

extern int sensor_monitor_export_state(uint8_t *buf, uint32_t max_size);

static const struct state_export_entry export_registry[] = {
    { "sensor_monitor", sensor_monitor_export_state },
    { NULL, NULL }
};

static component_state_export_fn find_export_fn(const char *name)
{
    for (int i = 0; export_registry[i].name; i++) {
        const char *a = name;
        const char *b = export_registry[i].name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return export_registry[i].fn;
    }
    return NULL;
}

static int l_component_hot_swap_stateful(lua_State *L) {
    if (!L) return 0;
    const char *old_name = luaL_checkstring(L, 1);
    const char *new_name = luaL_checkstring(L, 2);

    component_state_export_fn export_fn = find_export_fn(old_name);

    int idx = component_hot_swap_stateful(old_name, new_name, export_fn);
    if (idx < 0) {
        lua_pushnil(L);
    } else {
        lua_pushinteger(L, idx);
    }
    return 1;
}

/* ============================================================================
 * Model Memory Bindings
 * ============================================================================ */

/**
 * slm.model_stats() - Get model memory pool statistics
 * Returns table: {weights={...}, workspace={...}}
 */
static int l_model_stats(lua_State *L) {
    if (!L) return 0;
    RustPoolStats w = rust_weight_pool_stats();
    RustPoolStats ws = rust_workspace_pool_stats();

    lua_createtable(L, 0, 2);

    /* weights sub-table */
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)w.total_blocks);
    lua_setfield(L, -2, "total_blocks");
    lua_pushinteger(L, (lua_Integer)w.free_blocks);
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)w.allocated_blocks);
    lua_setfield(L, -2, "allocated_blocks");
    lua_pushinteger(L, (lua_Integer)w.shared_blocks);
    lua_setfield(L, -2, "shared_blocks");
    lua_pushinteger(L, (lua_Integer)w.peak_usage);
    lua_setfield(L, -2, "peak_usage");
    lua_setfield(L, -2, "weights");

    /* workspace sub-table */
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)ws.total_blocks);
    lua_setfield(L, -2, "total_blocks");
    lua_pushinteger(L, (lua_Integer)ws.free_blocks);
    lua_setfield(L, -2, "free_blocks");
    lua_pushinteger(L, (lua_Integer)ws.allocated_blocks);
    lua_setfield(L, -2, "allocated_blocks");
    lua_pushinteger(L, (lua_Integer)ws.shared_blocks);
    lua_setfield(L, -2, "shared_blocks");
    lua_pushinteger(L, (lua_Integer)ws.peak_usage);
    lua_setfield(L, -2, "peak_usage");
    lua_setfield(L, -2, "workspace");

    return 1;
}

/* ============================================================================
 * Model Inference Bindings
 * ============================================================================ */

/**
 * slm.model_infer(index) - Run inference on a loaded model
 * Returns predicted class (integer) or -1 on error
 */
static int l_model_infer(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    uint64_t t0 = slm_get_time_ns();
    int result = rust_infer_classify((uint32_t)idx);
    uint64_t t1 = slm_get_time_ns();
    admin_telemetry_record_inference(t1 > t0 ? t1 - t0 : 0u, result >= 0);
    lua_pushinteger(L, result);
    return 1;
}

/**
 * slm.model_infer_bytes(index, bytes) - Run inference on a loaded
 * model with caller-supplied input bytes.
 *
 * `bytes` is a Lua string of fp32 values (little-endian). For MNIST
 * pass 1×1×28×28 = 784 floats = 3,136 bytes. The byte length must be
 * a multiple of 4. Internally calls `rust_infer`, which routes through
 * `engine::run_inference`; on Jetson with the v6 GPU handoff present
 * the MNIST graph short-circuits through the GPU fastpath.
 *
 * Returns two values on success: a string of fp32 logits bytes (size
 * = output_count × 4) and the argmax index. On failure returns nil +
 * a negative error code:
 *   -1 = bytes empty or length not a multiple of 4
 *   -2 = bytes longer than 32 KB
 *   -3 = page allocation failed
 *   <0 from rust_infer = inference engine error
 */
static int l_model_infer_bytes(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    size_t blen = 0;
    const char *bytes = luaL_checklstring(L, 2, &blen);

    if (blen == 0 || (blen % 4u) != 0) {
        lua_pushnil(L);
        lua_pushinteger(L, -1);
        return 2;
    }
    /* Same 32 KB cap as model_infer_file. PMM allocation tops out at
     * 8 pages with this cap; keeps the call from accidentally pinning
     * megabytes if the caller passes a huge string. */
    if (blen > 32u * 1024u) {
        lua_pushnil(L);
        lua_pushinteger(L, -2);
        return 2;
    }

    /* Lua strings have no alignment guarantee beyond `char`. The CPU
     * fallback inside engine::run_inference dereferences the input
     * as fp32 via NEON loads — under SCTLR.A=1 an unaligned f32 load
     * would fault. Copy into a page-aligned PMM buffer so rust_infer
     * (and the engine's downstream ops) sees a 4-byte-aligned input.
     * pmm_alloc_pages returns 4096-byte-aligned memory, which trivially
     * satisfies fp32 alignment. */
    size_t pages = (blen + 4095u) / 4096u;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
    if (!buf) {
        lua_pushnil(L);
        lua_pushinteger(L, -3);
        return 2;
    }
    __builtin_memcpy(buf, bytes, blen);

    /* Stack-allocated 64-fp32 output buffer. Matches CLASSIFY_OUTPUT
     * in rust_infer_classify; 256 bytes is well under any reasonable
     * shell-task stack budget. */
    uint8_t output[64 * 4] __attribute__((aligned(4)));
    __builtin_memset(output, 0, sizeof(output));

    uint64_t t0 = slm_get_time_ns();
    int n = rust_infer((uint32_t)idx,
                       (const float *)buf,
                       blen / 4u,
                       (float *)output,
                       64u);
    uint64_t t1 = slm_get_time_ns();
    admin_telemetry_record_inference(t1 > t0 ? t1 - t0 : 0u, n >= 0);
    pmm_free_pages(buf, pages);

    if (n < 0) {
        lua_pushnil(L);
        lua_pushinteger(L, n);
        return 2;
    }

    int argmax = slm_fp32_argmax(output, (uint32_t)n);
    lua_pushlstring(L, (const char *)output, (size_t)n * 4u);
    lua_pushinteger(L, argmax);
    return 2;
}

/**
 * slm.model_infer_file(index, path) - Convenience wrapper around
 * model_infer_bytes that reads the input from VFS first.
 *
 * `path` should point at a file containing little-endian fp32 values
 * matching the model's input shape. For MNIST that's 3,136 bytes
 * (784 floats). Files are bounded at 32 KB so the bump-allocated
 * staging buffer doesn't pressure PMM.
 *
 * Returns the same (logits_string, argmax) tuple as model_infer_bytes.
 * On failure returns nil + negative rc:
 *   -1 = stat failed / not a regular file / empty
 *   -2 = file size not a multiple of 4 or > 32 KB
 *   -3 = page allocation failed
 *   -4 = read returned short
 *   <0 from rust_infer = inference engine error
 */
static int l_model_infer_file(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    const char *path = luaL_checkstring(L, 2);

    struct vfs_entry_info info;
    if (vfs_stat_path(path, &info) != 0 || info.type != 0 || info.size == 0) {
        lua_pushnil(L);
        lua_pushinteger(L, -1);
        return 2;
    }
    if ((info.size % 4u) != 0 || info.size > 32u * 1024u) {
        lua_pushnil(L);
        lua_pushinteger(L, -2);
        return 2;
    }

    size_t pages = (info.size + 4095u) / 4096u;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
    if (!buf) {
        lua_pushnil(L);
        lua_pushinteger(L, -3);
        return 2;
    }

    int rd = vfs_read_path(path, (char *)buf, info.size, 0);
    if (rd != (int)info.size) {
        pmm_free_pages(buf, pages);
        lua_pushnil(L);
        lua_pushinteger(L, -4);
        return 2;
    }

    uint8_t output[64 * 4] __attribute__((aligned(4)));
    __builtin_memset(output, 0, sizeof(output));

    uint64_t t0 = slm_get_time_ns();
    int n = rust_infer((uint32_t)idx,
                       (const float *)buf,
                       info.size / 4u,
                       (float *)output,
                       64u);
    uint64_t t1 = slm_get_time_ns();
    admin_telemetry_record_inference(t1 > t0 ? t1 - t0 : 0u, n >= 0);
    pmm_free_pages(buf, pages);

    if (n < 0) {
        lua_pushnil(L);
        lua_pushinteger(L, n);
        return 2;
    }

    int argmax = slm_fp32_argmax(output, (uint32_t)n);
    lua_pushlstring(L, (const char *)output, (size_t)n * 4u);
    lua_pushinteger(L, argmax);
    return 2;
}

/**
 * slm.model_find(name) - Find a model by name
 * Returns model index or -1 if not found
 */
static int l_model_find(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    int idx = rust_model_find(name);
    lua_pushinteger(L, idx);
    return 1;
}

/**
 * slm.model_load_mnist() - Load the built-in MNIST model
 * Returns model index or -1 on failure
 */
static int l_model_load_mnist(lua_State *L) {
    if (!L) return 0;
    int idx = rust_model_load_builtin_mnist();
    lua_pushinteger(L, idx);
    return 1;
}

/**
 * slm.gpu_run_mnist() - Dispatch the MNIST inference pipeline on
 * the Jetson GA10B GPU. Requires a v5 channel handoff to be
 * present in DRAM (set up pre-kexec by
 * scripts/gpu-kernel-mnist.c --preserve-for-kexec).
 *
 * Returns two values on success: a 40-byte string holding the 10
 * fp32 logits (little-endian) and the argmax index (0..9, the
 * predicted digit class). On failure returns nil + a negative
 * error code.
 *
 * Float values are returned as raw bytes because the kernel target
 * compiles with -mgeneral-regs-only (no fp32 in C). Decode in Lua:
 *
 *   local bytes, argmax = slm.gpu_run_mnist()
 *   for i = 0, 9 do
 *       local f = string.unpack("<f", bytes, 1 + i*4)
 *       print(string.format("logits[%d] = %.4f", i, f))
 *   end
 *
 * `string.unpack` lives in liblua (compiled with FP) so the
 * conversion happens in library code, not in the kernel C
 * compilation unit.
 */
static int l_gpu_run_mnist(lua_State *L) {
    if (!L) return 0;
    uint8_t logits_bytes[40];
    int rc = slm_gpu_run_mnist(logits_bytes);
    if (rc < 0) {
        lua_pushnil(L);
        lua_pushinteger(L, rc);
        return 2;
    }
    int argmax = slm_fp32_argmax(logits_bytes, 10u);
    lua_pushlstring(L, (const char *)logits_bytes, sizeof(logits_bytes));
    lua_pushinteger(L, argmax);
    return 2;
}

/**
 * slm.gpu_set_mnist_input(bytes) - Swap the GPU's MNIST input buffer
 * at runtime, ahead of the next slm.gpu_run_mnist() call.
 *
 * `bytes` is a Lua string of fp32 bit patterns (little-endian) — for
 * MNIST that's 1×1×28×28 = 784 floats = 3,136 bytes. The kernel
 * memcpys + cache-cleans into the v6 handoff's input_buf_phys.
 *
 * Typical Lua flow:
 *
 *   local digit_bytes = read_file_as_bytes("/mnt/files/digit_5.bin")
 *   slm.gpu_set_mnist_input(digit_bytes)
 *   local logits, argmax = slm.gpu_run_mnist()
 *   print(string.format("predicted: %d", argmax))
 *
 * Returns 0 on success or a negative error code on failure
 * (-1 = no v6 handoff, -2 = bytes too long, -3 = bad arg).
 */
static int l_gpu_set_mnist_input(lua_State *L) {
    if (!L) return 0;
    size_t len = 0;
    const char *bytes = luaL_checklstring(L, 1, &len);
    int rc = slm_gpu_set_mnist_input(bytes, len);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.gpu_set_mnist_input_fill(value_bits, n_floats) - Splat the
 * GPU's MNIST input buffer with `n_floats` copies of the fp32 bit
 * pattern `value_bits`. Designed for hardware bring-up demos where
 * sending 3 KB of explicit fp32 bytes through the serial console
 * is unreliable (NULs and long runs of repeated bytes get
 * corrupted on the test bench).
 *
 * Both args are integers (Lua doesn't have unsigned types, but the
 * binding bottoms out in uint32_t — pass the bit pattern as a
 * decimal or hex literal).
 *
 *   slm.gpu_set_mnist_input_fill(0x3F800000, 784)  -- all +1.0f
 *   slm.gpu_set_mnist_input_fill(0xBF800000, 784)  -- all -1.0f
 *   slm.gpu_set_mnist_input_fill(0, 784)            -- all  0.0f
 *
 * Returns 0 on success or a negative error code on failure.
 */
static int l_gpu_set_mnist_input_fill(lua_State *L) {
    if (!L) return 0;
    /* Lua integers are signed 64-bit. Narrow to uint32_t via C's
     * well-defined modular truncation — this preserves the bit
     * pattern of fp32 literals like 0xBF800000 that Lua sees as
     * positive 64-bit integers. */
    uint32_t value_bits = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t n_floats   = (uint32_t)luaL_checkinteger(L, 2);
    int rc = slm_gpu_set_mnist_input_fill(value_bits, n_floats);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.model_pin(index) - Pin a model to prevent LRU eviction
 * Returns 0 on success, -1 on error
 */
static int l_model_pin(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, rust_model_pin((uint32_t)idx));
    return 1;
}

/**
 * slm.model_unpin(index) - Unpin a model (allow LRU eviction)
 * Returns 0 on success, -1 on error
 */
static int l_model_unpin(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, rust_model_unpin((uint32_t)idx));
    return 1;
}

/**
 * slm.model_preload(name) - Preload a model in a background task (#64)
 * Currently only "mnist" is supported for async preload.
 * Returns 0 on success (preload started), -1 on error.
 */
static int l_model_preload(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);

    /* Delegate to shell_execute which drives the preload_task_entry
     * background task. This reuses the shell's preload infrastructure
     * without duplicating the task-spawn logic. */
    char cmd[64];
    extern int uart_snprintf(char *buf, size_t size, const char *fmt, ...);
    uart_snprintf(cmd, sizeof(cmd), "model preload %s", name);
    int rc = shell_execute(cmd);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.model_preload_wait(name [, timeout_ms]) - Wait for async preload (#64)
 * Returns model index (>=0) on success, negative on error/timeout.
 */
static int l_model_preload_wait(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    uint32_t timeout = 5000;
    if (lua_gettop(L) >= 2) {
        timeout = (uint32_t)luaL_checkinteger(L, 2);
    }
    extern int model_preload_wait(const char *name, uint32_t timeout_ms);
    int rc = model_preload_wait(name, timeout);
    lua_pushinteger(L, rc);
    return 1;
}

/* ============================================================================
 * Message Router Bindings
 * ============================================================================ */

/**
 * slm.msg_publish(topic, data) - Publish a message to a topic
 * Returns number of subscribers that received the message
 */
static int l_msg_publish(lua_State *L) {
    if (!L) return 0;
    const char *topic = luaL_checkstring(L, 1);
    const char *data = luaL_checkstring(L, 2);
    int delivered = msg_router_publish((const uint8_t *)topic, (const uint8_t *)data);
    lua_pushinteger(L, delivered);
    return 1;
}

/**
 * slm.msg_publish_priority(topic, data, priority) - Publish with priority
 * priority: 0 = normal, higher = more urgent
 * Returns number of subscribers that received the message
 */
static int l_msg_publish_priority(lua_State *L) {
    if (!L) return 0;
    const char *topic = luaL_checkstring(L, 1);
    const char *data = luaL_checkstring(L, 2);
    int prio = (int)luaL_checkinteger(L, 3);
    if (prio < 0) prio = 0;
    if (prio > 255) prio = 255;
    int delivered = msg_router_publish_priority(
        (const uint8_t *)topic, (const uint8_t *)data, (uint8_t)prio);
    lua_pushinteger(L, delivered);
    return 1;
}

/* ============================================================================
 * Lua Message Subscriptions (#207)
 * ============================================================================
 *
 * Lua scripts register callbacks against msg_router topics; messages are
 * dispatched on the shell task's stack at yield / sleep / read_line. The
 * callbacks are not truly concurrent — option (1) from the issue — but
 * match the existing single-threaded Lua model. Each lua_State gets its
 * own router mailbox component_idx so concurrent shells / Lua tasks do
 * not fight over a shared ack path.
 */
_Static_assert(LUA_MSG_COMPONENT_BASE >= COMPONENT_MAX_COUNT,
               "Lua component slots must not overlap native components");
_Static_assert(LUA_MSG_COMPONENT_MAX <= 64,
               "grow msg_router MAX_COMPONENTS before increasing Lua state slots");

/* Maximum concurrent Lua subscriptions across all active lua_States.
 * Subscriptions are lightweight (one Lua registry slot + ~20 bytes) so
 * the cap is mostly a sanity ceiling. */
#define LUA_MSG_MAX_SUBS 32

/* Topic buffer in the router — keep in sync with TOPIC_NAME_LEN in
 * runtime/src/msg_router.rs. */
#define LUA_MSG_TOPIC_LEN 16

extern const char *msg_router_receive(int component_idx, char *topic_out);
extern void msg_router_ack(int component_idx);
extern int msg_router_subscribe(const uint8_t *topic_name, int component_idx);
extern void msg_router_unsubscribe_all(int component_idx);

struct lua_msg_sub {
    int handle;                         /* Caller-visible id (1, 2, ...) */
    int ref;                            /* luaL_ref slot for the callback */
    lua_State *L;                       /* State owning ref (for cleanup) */
    int component_idx;                  /* Router mailbox for this lua_State */
    char pattern[LUA_MSG_TOPIC_LEN];    /* Subscribed topic or wildcard */
    uint8_t wildcard;                   /* 1 if pattern ends in '/*' */
    uint8_t prefix_len;                 /* Pattern length excl. trailing '*' */
    uint8_t active;
};

static struct lua_msg_sub lua_msg_subs[LUA_MSG_MAX_SUBS];
static int lua_msg_next_handle = 1;
static spinlock_t        lua_msg_subs_lock = SPINLOCK_INIT;

struct lua_state_slot {
    lua_State *L;
    int component_idx;
    uint8_t active;
};

static struct lua_state_slot lua_state_slots[LUA_MSG_COMPONENT_CAP];
static spinlock_t            lua_state_slots_lock = SPINLOCK_INIT;
static volatile int          lua_state_count = 0;

static void lua_state_count_inc(void)
{
    irq_flags_t flags = spin_lock_irqsave(&lua_state_slots_lock);
    lua_state_count++;
    spin_unlock_irqrestore(&lua_state_slots_lock, flags);
}

static int lua_state_count_dec_and_test_zero(void)
{
    int is_zero;
    irq_flags_t flags = spin_lock_irqsave(&lua_state_slots_lock);
    if (lua_state_count > 0) lua_state_count--;
    is_zero = (lua_state_count == 0);
    spin_unlock_irqrestore(&lua_state_slots_lock, flags);
    return is_zero;
}

static struct lua_state_slot *lua_state_slot_from_state(lua_State *L)
{
    if (!L) return NULL;
    return *(struct lua_state_slot **)lua_getextraspace(L);
}

static int lua_state_component_idx(lua_State *L)
{
    struct lua_state_slot *slot = lua_state_slot_from_state(L);
    return slot ? slot->component_idx : -1;
}

static struct lua_state_slot *lua_state_slot_alloc(lua_State *L)
{
    irq_flags_t flags = spin_lock_irqsave(&lua_state_slots_lock);
    for (int i = 0; i < LUA_MSG_COMPONENT_CAP; i++) {
        if (!lua_state_slots[i].active) {
            lua_state_slots[i].active = 1;
            lua_state_slots[i].L = L;
            lua_state_slots[i].component_idx = LUA_MSG_COMPONENT_BASE + i;
            spin_unlock_irqrestore(&lua_state_slots_lock, flags);
            return &lua_state_slots[i];
        }
    }
    spin_unlock_irqrestore(&lua_state_slots_lock, flags);
    return NULL;
}

static void lua_state_slot_free(struct lua_state_slot *slot)
{
    if (!slot) return;
    irq_flags_t flags = spin_lock_irqsave(&lua_state_slots_lock);
    slot->active = 0;
    slot->L = NULL;
    slot->component_idx = 0;
    spin_unlock_irqrestore(&lua_state_slots_lock, flags);
}

/* Match a Lua subscription pattern against an actual delivered topic.
 * Exact match OR (for patterns ending in "/*") prefix match up to the '*'. */
static int lua_msg_topic_matches(const struct lua_msg_sub *sub, const char *topic)
{
    if (sub->wildcard) {
        for (uint8_t i = 0; i < sub->prefix_len; i++) {
            if (topic[i] == '\0' || topic[i] != sub->pattern[i]) return 0;
        }
        return 1;
    }
    /* Exact */
    for (uint8_t i = 0; i < LUA_MSG_TOPIC_LEN; i++) {
        if (sub->pattern[i] != topic[i]) return 0;
        if (sub->pattern[i] == '\0') return 1;
    }
    return 1;
}

/* Drain pending router messages for the Lua subscriber pool and invoke
 * matching callbacks on the given state. Safe to call at any yield point;
 * any error raised by a callback is swallowed (logged to UART) so one
 * bad subscriber cannot wedge the drain loop. */
static void lua_msg_drain(lua_State *L)
{
    if (!L) return;
    int component_idx = lua_state_component_idx(L);
    if (component_idx < 0) return;

    /* The drain cap bounds work done per yield/sleep/read_line so a
     * flooded router mailbox cannot starve the main Lua thread. PR #217
     * review raised concerns about message loss under high throughput.
     * Messages are NOT lost — the router holds the next one until the
     * next drain — but we do introduce latency up to one drain cycle
     * per LUA_MSG_DRAIN_BATCH messages. 256 is tuned to be much larger
     * than realistic demo load while still cheap enough that we return
     * to the caller within a few milliseconds at worst. Scripts with
     * genuinely high subscriber fan-in should call slm.msg_drain() in
     * a tight loop themselves. */
#define LUA_MSG_DRAIN_BATCH 256
    for (int guard = 0; guard < LUA_MSG_DRAIN_BATCH; guard++) {
        int handles[LUA_MSG_MAX_SUBS];
        int handle_count = 0;
        char topic_buf[LUA_MSG_TOPIC_LEN];
        const char *data = msg_router_receive(component_idx, topic_buf);
        if (!data) break;

        irq_flags_t flags = spin_lock_irqsave(&lua_msg_subs_lock);
        for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
            struct lua_msg_sub *s = &lua_msg_subs[i];
            if (!s->active || s->L != L) continue;
            if (!lua_msg_topic_matches(s, topic_buf)) continue;
            handles[handle_count++] = s->handle;
        }
        spin_unlock_irqrestore(&lua_msg_subs_lock, flags);

        for (int i = 0; i < handle_count; i++) {
            int ref = LUA_NOREF;

            flags = spin_lock_irqsave(&lua_msg_subs_lock);
            for (int j = 0; j < LUA_MSG_MAX_SUBS; j++) {
                struct lua_msg_sub *s = &lua_msg_subs[j];
                if (!s->active || s->L != L) continue;
                if (s->handle != handles[i]) continue;
                if (!lua_msg_topic_matches(s, topic_buf)) continue;
                ref = s->ref;
                break;
            }
            spin_unlock_irqrestore(&lua_msg_subs_lock, flags);

            if (ref == LUA_NOREF) {
                continue;
            }

            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                continue;
            }
            lua_pushstring(L, topic_buf);
            lua_pushstring(L, data);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                const char *err = lua_tostring(L, -1);
                shell_printf("[lua msg_subscribe] callback error: %s\n",
                            err ? err : "(unknown)");
                lua_pop(L, 1);
            }
        }

        msg_router_ack(component_idx);
    }
}

/**
 * slm.msg_subscribe(topic, fn) - Register a Lua callback for a topic.
 *
 * The callback is invoked as `fn(topic, data)` on the next drain point
 * (`slm.yield`, `slm.sleep`, `slm.read_line`) with pending matching
 * messages. Topics ending in "/*" match any topic with that prefix.
 *
 * Returns a subscription handle (integer >= 1) on success, nil if the
 * Lua subscription pool is full (LUA_MSG_MAX_SUBS).
 */
static int l_msg_subscribe(lua_State *L) {
    if (!L) return 0;
    const char *topic = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* Copy pattern into fixed buffer, compute wildcard metadata */
    char buf[LUA_MSG_TOPIC_LEN];
    size_t tlen = 0;
    while (topic[tlen] && tlen < LUA_MSG_TOPIC_LEN - 1) {
        buf[tlen] = topic[tlen];
        tlen++;
    }
    buf[tlen] = '\0';

    int wildcard = 0;
    uint8_t prefix_len = (uint8_t)tlen;
    if (tlen >= 2 && buf[tlen - 1] == '*') {
        wildcard = 1;
        prefix_len = (uint8_t)(tlen - 1);  /* everything before the '*' */
    }

    irq_flags_t flags = spin_lock_irqsave(&lua_msg_subs_lock);
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        if (!lua_msg_subs[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&lua_msg_subs_lock, flags);
        lua_pushnil(L);
        return 1;
    }

    /* Reference the callback (arg 2 is on top after the checks) */
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    struct lua_msg_sub *s = &lua_msg_subs[slot];
    s->handle = lua_msg_next_handle++;
    s->ref = ref;
    s->L = L;
    s->component_idx = lua_state_component_idx(L);
    for (size_t i = 0; i < LUA_MSG_TOPIC_LEN; i++) s->pattern[i] = buf[i];
    s->wildcard = (uint8_t)wildcard;
    s->prefix_len = prefix_len;
    s->active = 1;

    /* msg_router_subscribe is NOT idempotent — each call consumes a
     * fresh subscriber slot on the topic and delivers a copy of every
     * message into its own mailbox. Dedup at the Lua layer so a single
     * topic only has one router subscription no matter how many Lua
     * callbacks want it. */
    int already_subscribed_at_router = 0;
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        if (i == slot || !lua_msg_subs[i].active) continue;
        if (lua_msg_subs[i].component_idx != s->component_idx) continue;
        int eq = 1;
        for (int k = 0; k < LUA_MSG_TOPIC_LEN; k++) {
            if (lua_msg_subs[i].pattern[k] != s->pattern[k]) { eq = 0; break; }
            if (s->pattern[k] == '\0') break;
        }
        if (eq) { already_subscribed_at_router = 1; break; }
    }
    if (!already_subscribed_at_router) {
        if (msg_router_subscribe((const uint8_t *)buf, s->component_idx) != 0) {
            s->active = 0;
            spin_unlock_irqrestore(&lua_msg_subs_lock, flags);
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            lua_pushnil(L);
            return 1;
        }
    }
    spin_unlock_irqrestore(&lua_msg_subs_lock, flags);

    lua_pushinteger(L, (lua_Integer)s->handle);
    return 1;
}

/**
 * slm.msg_unsubscribe(handle) - Remove a Lua subscription.
 *
 * Returns true on success, false if the handle is unknown. The router-side
 * subscription is left in place — msg_router does not expose per-topic
 * unsubscribe — but once no Lua sub matches, the drain loop stops
 * delivering the message to Lua (and ack discards it from the mailbox).
 */
static int l_msg_unsubscribe(lua_State *L) {
    if (!L) return 0;
    lua_Integer handle = luaL_checkinteger(L, 1);
    irq_flags_t flags = spin_lock_irqsave(&lua_msg_subs_lock);
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        struct lua_msg_sub *s = &lua_msg_subs[i];
        if (s->active && s->handle == (int)handle) {
            luaL_unref(s->L, LUA_REGISTRYINDEX, s->ref);
            s->active = 0;
            spin_unlock_irqrestore(&lua_msg_subs_lock, flags);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    spin_unlock_irqrestore(&lua_msg_subs_lock, flags);
    lua_pushboolean(L, 0);
    return 1;
}

/**
 * slm.msg_drain() - Manually poll pending messages and invoke callbacks.
 *
 * Normally scripts don't need to call this — drains happen automatically
 * at slm.yield / slm.sleep / slm.read_line. Exposed for scripts that
 * compute without yielding and want to tick the subscriber pipeline.
 */
static int l_msg_drain(lua_State *L) {
    if (!L) return 0;
    lua_msg_drain(L);
    return 0;
}

/* Teardown helper — release all Lua refs belonging to this state and
 * ask the router to drop the Lua mailbox if no Lua sub remains. Called
 * from lua_slm_close. */
static void lua_msg_subs_cleanup(lua_State *L) {
    int component_idx = lua_state_component_idx(L);
    irq_flags_t flags = spin_lock_irqsave(&lua_msg_subs_lock);
    for (int i = 0; i < LUA_MSG_MAX_SUBS; i++) {
        struct lua_msg_sub *s = &lua_msg_subs[i];
        if (!s->active) continue;
        if (s->L == L) {
            luaL_unref(L, LUA_REGISTRYINDEX, s->ref);
            s->active = 0;
        }
    }
    spin_unlock_irqrestore(&lua_msg_subs_lock, flags);
    if (component_idx >= 0) {
        msg_router_unsubscribe_all(component_idx);
    }
}

/* ============================================================================
 * Scheduler Bindings
 * ============================================================================ */

/**
 * slm.sched_policy() - Get current scheduler policy name
 * Returns string
 */
static int l_sched_policy(lua_State *L) {
    if (!L) return 0;
    lua_pushstring(L, sched_get_policy());
    return 1;
}

/**
 * slm.sched_stats() - Get scheduler statistics
 * Returns table: {task_count, ready_count, context_switches, timer_ticks, policy}
 */
static int l_sched_stats(lua_State *L) {
    if (!L) return 0;
    struct sched_stats stats;
    scheduler_get_stats(&stats);

    lua_createtable(L, 0, 5);

    lua_pushinteger(L, (lua_Integer)stats.task_count);
    lua_setfield(L, -2, "task_count");

    lua_pushinteger(L, (lua_Integer)stats.ready_count);
    lua_setfield(L, -2, "ready_count");

    lua_pushinteger(L, (lua_Integer)stats.context_switches);
    lua_setfield(L, -2, "context_switches");

    lua_pushinteger(L, (lua_Integer)stats.timer_ticks);
    lua_setfield(L, -2, "timer_ticks");

    lua_pushstring(L, sched_get_policy());
    lua_setfield(L, -2, "policy");

    return 1;
}

/**
 * slm.sched_set_policy(name) - Switch scheduler policy by name
 * Returns true on success, false on failure (unknown policy name)
 */
static int l_sched_set_policy(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    const struct sched_policy_ops *p = sched_find_policy(name);
    if (!p) {
        lua_pushboolean(L, 0);
        return 1;
    }
    int rc = sched_set_policy(p);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/**
 * slm.sched_policy_list() - List available scheduler policies
 * Returns array of tables: {{name, active}, ...}
 */
static int l_sched_policy_list(lua_State *L) {
    if (!L) return 0;
    int count = sched_policy_count();
    const char *current = sched_get_policy();

    lua_createtable(L, count, 0);

    int idx = 1;
    for (int i = 0; i < count; i++) {
        const struct sched_policy_ops *p = sched_policy_get(i);
        if (!p) continue;

        lua_createtable(L, 0, 2);
        lua_pushstring(L, p->name);
        lua_setfield(L, -2, "name");
        lua_pushboolean(L, strcmp(p->name, current) == 0);
        lua_setfield(L, -2, "active");
        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/**
 * slm.ai_sched_stats() - Get AI scheduler statistics (if CONFIG_AI_SCHEDULER enabled)
 * Returns table: {decisions, fallbacks, avg_latency_ns, histogram={...}}
 * Returns nil if AI scheduler is not compiled in.
 */
static int l_ai_sched_stats(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_SCHEDULER)
    extern void sched_ai_get_stats(const char *, uint32_t *, uint32_t *,
                                   uint64_t *, const uint32_t **, int *);

    const char *policy = sched_get_policy();
    uint32_t decisions = 0, fallbacks = 0;
    uint64_t avg_lat = 0;
    const uint32_t *hist = NULL;
    int n_actions = 0;

    sched_ai_get_stats(policy, &decisions, &fallbacks, &avg_lat, &hist, &n_actions);

    lua_createtable(L, 0, 5);

    lua_pushstring(L, policy);
    lua_setfield(L, -2, "policy");

    lua_pushinteger(L, (lua_Integer)decisions);
    lua_setfield(L, -2, "decisions");

    lua_pushinteger(L, (lua_Integer)fallbacks);
    lua_setfield(L, -2, "fallbacks");

    lua_pushinteger(L, (lua_Integer)avg_lat);
    lua_setfield(L, -2, "avg_latency_ns");

    if (hist && n_actions > 0) {
        lua_createtable(L, n_actions, 0);
        for (int i = 0; i < n_actions; i++) {
            lua_pushinteger(L, (lua_Integer)hist[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "histogram");
    }
#else
    lua_pushnil(L);
#endif
    return 1;
}

/**
 * slm.sched_decision_rate() - Smoothed scheduler decision rate + percentile latencies.
 *
 * Returns table:
 *   {
 *     decisions_per_s, fallback_rate,
 *     p50_latency_ns, p90_latency_ns, p99_latency_ns,
 *     min_latency_ns, max_latency_ns, avg_latency_ns,
 *     total_decisions, total_fallbacks, total_ns,
 *     policy
 *   }
 *
 * Returns nil if AI scheduler is not compiled in or the active policy is
 * the heuristic (which doesn't populate the M1 stats — heuristic decisions
 * still happen but are not counted in the per-policy histogram).
 */
static int l_sched_decision_rate(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_SCHEDULER)
    extern int sched_ai_get_rate_stats(const char *,
                                       struct latency_hist *,
                                       uint64_t *, uint64_t *,
                                       uint64_t *, uint64_t *, uint64_t *);

    const char *policy = sched_get_policy();
    struct latency_hist hist;
    uint64_t decision_rate_q16 = 0, fallback_rate_q16 = 0;
    uint64_t total_decisions = 0, total_fallbacks = 0, total_ns = 0;

    if (sched_ai_get_rate_stats(policy, &hist,
                                &decision_rate_q16, &fallback_rate_q16,
                                &total_decisions, &total_fallbacks,
                                &total_ns) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 12);

    lua_pushstring(L, policy);
    lua_setfield(L, -2, "policy");

    /* Q16.16 → integer events/s. Sub-1/s rates report 0; that's the
     * trade-off we accepted in §8.2 of the spec for an integer-only
     * rate counter that doesn't pull libm into hot scheduler paths. */
    lua_pushinteger(L, (lua_Integer)(decision_rate_q16 >> RATE_EWMA_Q16_SHIFT));
    lua_setfield(L, -2, "decisions_per_s");

    lua_pushinteger(L, (lua_Integer)(fallback_rate_q16 >> RATE_EWMA_Q16_SHIFT));
    lua_setfield(L, -2, "fallback_rate");

    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 50));
    lua_setfield(L, -2, "p50_latency_ns");
    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 90));
    lua_setfield(L, -2, "p90_latency_ns");
    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 99));
    lua_setfield(L, -2, "p99_latency_ns");

    lua_pushinteger(L, (lua_Integer)hist.min_ns);
    lua_setfield(L, -2, "min_latency_ns");
    lua_pushinteger(L, (lua_Integer)hist.max_ns);
    lua_setfield(L, -2, "max_latency_ns");
    lua_pushinteger(L, (lua_Integer)(hist.count > 0 ? hist.sum_ns / hist.count : 0));
    lua_setfield(L, -2, "avg_latency_ns");

    lua_pushinteger(L, (lua_Integer)total_decisions);
    lua_setfield(L, -2, "total_decisions");
    lua_pushinteger(L, (lua_Integer)total_fallbacks);
    lua_setfield(L, -2, "total_fallbacks");
    lua_pushinteger(L, (lua_Integer)total_ns);
    lua_setfield(L, -2, "total_ns");
#else
    lua_pushnil(L);
#endif
    return 1;
}

/**
 * slm.latency_histogram(consumer) - Bucketed latency histogram.
 *
 * Currently only "sched" is wired; "eviction" and "inference" return nil
 * (they land in M3). Returns table:
 *   {
 *     buckets = { [low_ns_str] = count, ... },  -- only non-empty buckets
 *     total, min_ns, max_ns,
 *     p50_ns, p90_ns, p99_ns,
 *     consumer = "sched"
 *   }
 */
static int l_latency_histogram(lua_State *L) {
    if (!L) return 0;
    const char *consumer = luaL_checkstring(L, 1);

#if defined(CONFIG_AI_SCHEDULER)
    if (strcmp(consumer, "sched") == 0) {
        extern int sched_ai_get_rate_stats(const char *,
                                           struct latency_hist *,
                                           uint64_t *, uint64_t *,
                                           uint64_t *, uint64_t *, uint64_t *);
        const char *policy = sched_get_policy();
        struct latency_hist hist;
        if (sched_ai_get_rate_stats(policy, &hist, NULL, NULL,
                                    NULL, NULL, NULL) != 0) {
            lua_pushnil(L);
            return 1;
        }

        lua_createtable(L, 0, 7);
        lua_pushstring(L, "sched");
        lua_setfield(L, -2, "consumer");

        /* Bucket sub-table keyed by lower-bound ns (as a string for
         * Lua-friendly numeric keys outside lua_Integer range — bucket
         * 31's lower bound is ~6.9e10, which fits in lua_Integer on
         * 64-bit but we use strings for stable JSON-like rendering). */
        lua_createtable(L, 0, LATENCY_HIST_BUCKETS);
        for (uint32_t i = 0; i < LATENCY_HIST_BUCKETS; i++) {
            if (hist.buckets[i] == 0) continue;
            char key[32];
            uint64_t low = latency_hist_bucket_low_ns(i);
            /* Hand-format: avoid pulling snprintf into the hot path. */
            int kpos = 0;
            char tmp[24];
            int t = 0;
            uint64_t v = low;
            if (v == 0) tmp[t++] = '0';
            while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
            while (t > 0) key[kpos++] = tmp[--t];
            key[kpos] = '\0';
            lua_pushinteger(L, (lua_Integer)hist.buckets[i]);
            lua_setfield(L, -2, key);
        }
        lua_setfield(L, -2, "buckets");

        lua_pushinteger(L, (lua_Integer)hist.count);
        lua_setfield(L, -2, "total");
        lua_pushinteger(L, (lua_Integer)hist.min_ns);
        lua_setfield(L, -2, "min_ns");
        lua_pushinteger(L, (lua_Integer)hist.max_ns);
        lua_setfield(L, -2, "max_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 50));
        lua_setfield(L, -2, "p50_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 90));
        lua_setfield(L, -2, "p90_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 99));
        lua_setfield(L, -2, "p99_ns");

        return 1;
    }
#endif

    if (strcmp(consumer, "eviction") == 0) {
        struct latency_hist hist;
        if (admin_telemetry_get_eviction_stats(&hist, NULL, NULL,
                                               NULL, NULL, NULL) != 0) {
            lua_pushnil(L);
            return 1;
        }

        lua_createtable(L, 0, 7);
        lua_pushstring(L, "eviction");
        lua_setfield(L, -2, "consumer");

        lua_createtable(L, 0, LATENCY_HIST_BUCKETS);
        for (uint32_t i = 0; i < LATENCY_HIST_BUCKETS; i++) {
            if (hist.buckets[i] == 0) continue;
            char key[32];
            uint64_t low = latency_hist_bucket_low_ns(i);
            int kpos = 0;
            char tmp[24];
            int t = 0;
            uint64_t v = low;
            if (v == 0) tmp[t++] = '0';
            while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
            while (t > 0) key[kpos++] = tmp[--t];
            key[kpos] = '\0';
            lua_pushinteger(L, (lua_Integer)hist.buckets[i]);
            lua_setfield(L, -2, key);
        }
        lua_setfield(L, -2, "buckets");

        lua_pushinteger(L, (lua_Integer)hist.count);
        lua_setfield(L, -2, "total");
        lua_pushinteger(L, (lua_Integer)hist.min_ns);
        lua_setfield(L, -2, "min_ns");
        lua_pushinteger(L, (lua_Integer)hist.max_ns);
        lua_setfield(L, -2, "max_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 50));
        lua_setfield(L, -2, "p50_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 90));
        lua_setfield(L, -2, "p90_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 99));
        lua_setfield(L, -2, "p99_ns");
        return 1;
    }

    if (strcmp(consumer, "inference") == 0) {
        struct latency_hist hist;
        if (admin_telemetry_get_inference_stats(&hist, NULL, NULL,
                                                NULL, NULL, NULL) != 0) {
            lua_pushnil(L);
            return 1;
        }

        lua_createtable(L, 0, 7);
        lua_pushstring(L, "inference");
        lua_setfield(L, -2, "consumer");

        lua_createtable(L, 0, LATENCY_HIST_BUCKETS);
        for (uint32_t i = 0; i < LATENCY_HIST_BUCKETS; i++) {
            if (hist.buckets[i] == 0) continue;
            char key[32];
            uint64_t low = latency_hist_bucket_low_ns(i);
            int kpos = 0;
            char tmp[24];
            int t = 0;
            uint64_t v = low;
            if (v == 0) tmp[t++] = '0';
            while (v > 0) { tmp[t++] = '0' + (v % 10); v /= 10; }
            while (t > 0) key[kpos++] = tmp[--t];
            key[kpos] = '\0';
            lua_pushinteger(L, (lua_Integer)hist.buckets[i]);
            lua_setfield(L, -2, key);
        }
        lua_setfield(L, -2, "buckets");

        lua_pushinteger(L, (lua_Integer)hist.count);
        lua_setfield(L, -2, "total");
        lua_pushinteger(L, (lua_Integer)hist.min_ns);
        lua_setfield(L, -2, "min_ns");
        lua_pushinteger(L, (lua_Integer)hist.max_ns);
        lua_setfield(L, -2, "max_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 50));
        lua_setfield(L, -2, "p50_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 90));
        lua_setfield(L, -2, "p90_ns");
        lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 99));
        lua_setfield(L, -2, "p99_ns");
        return 1;
    }

    lua_pushnil(L);
    return 1;
}

/**
 * slm.eviction_decision_rate() - Smoothed eviction rate + percentile latencies.
 *
 * Same shape as `slm.sched_decision_rate()` but for the eviction
 * `select_victim` site (Rust runtime). Returns table:
 *   { decisions_per_s, fallback_rate, p50_latency_ns, p90_latency_ns,
 *     p99_latency_ns, min_latency_ns, max_latency_ns, avg_latency_ns,
 *     total_decisions, total_fallbacks, total_ns }
 */
static int l_eviction_decision_rate(lua_State *L) {
    if (!L) return 0;
    struct latency_hist hist;
    uint64_t decision_rate_q16 = 0, fallback_rate_q16 = 0;
    uint64_t total_decisions = 0, total_fallbacks = 0, total_ns = 0;

    admin_telemetry_get_eviction_stats(&hist,
                                       &decision_rate_q16, &fallback_rate_q16,
                                       &total_decisions, &total_fallbacks,
                                       &total_ns);

    lua_createtable(L, 0, 11);

    lua_pushinteger(L, (lua_Integer)(decision_rate_q16 >> RATE_EWMA_Q16_SHIFT));
    lua_setfield(L, -2, "decisions_per_s");
    lua_pushinteger(L, (lua_Integer)(fallback_rate_q16 >> RATE_EWMA_Q16_SHIFT));
    lua_setfield(L, -2, "fallback_rate");

    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 50));
    lua_setfield(L, -2, "p50_latency_ns");
    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 90));
    lua_setfield(L, -2, "p90_latency_ns");
    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 99));
    lua_setfield(L, -2, "p99_latency_ns");

    lua_pushinteger(L, (lua_Integer)hist.min_ns);
    lua_setfield(L, -2, "min_latency_ns");
    lua_pushinteger(L, (lua_Integer)hist.max_ns);
    lua_setfield(L, -2, "max_latency_ns");
    lua_pushinteger(L, (lua_Integer)(hist.count > 0 ? hist.sum_ns / hist.count : 0));
    lua_setfield(L, -2, "avg_latency_ns");

    lua_pushinteger(L, (lua_Integer)total_decisions);
    lua_setfield(L, -2, "total_decisions");
    lua_pushinteger(L, (lua_Integer)total_fallbacks);
    lua_setfield(L, -2, "total_fallbacks");
    lua_pushinteger(L, (lua_Integer)total_ns);
    lua_setfield(L, -2, "total_ns");

    return 1;
}

/**
 * slm.inference_rate() - Smoothed inference call rate + percentile latencies.
 *
 * M3 reports a single global series across all `slm.model_infer*`
 * call sites. Per-model breakdown (spec §6.2) is deferred to M4 with
 * the telemetry feed (per-topic counters give natural per-model
 * aggregation without a kernel-side fixed-size map).
 *
 * Returns table:
 *   { calls_per_s, errors_per_s, p50_ns, p99_ns, p50_latency_ns,
 *     p90_latency_ns, p99_latency_ns, min_latency_ns, max_latency_ns,
 *     avg_latency_ns, total_calls, total_errors, total_ns }
 */
static int l_inference_rate(lua_State *L) {
    if (!L) return 0;
    struct latency_hist hist;
    uint64_t calls_q16 = 0, errors_q16 = 0;
    uint64_t total_calls = 0, total_errors = 0, total_ns = 0;

    admin_telemetry_get_inference_stats(&hist,
                                        &calls_q16, &errors_q16,
                                        &total_calls, &total_errors,
                                        &total_ns);

    lua_createtable(L, 0, 11);

    lua_pushinteger(L, (lua_Integer)(calls_q16 >> RATE_EWMA_Q16_SHIFT));
    lua_setfield(L, -2, "calls_per_s");
    lua_pushinteger(L, (lua_Integer)(errors_q16 >> RATE_EWMA_Q16_SHIFT));
    lua_setfield(L, -2, "errors_per_s");

    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 50));
    lua_setfield(L, -2, "p50_latency_ns");
    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 90));
    lua_setfield(L, -2, "p90_latency_ns");
    lua_pushinteger(L, (lua_Integer)latency_hist_percentile(&hist, 99));
    lua_setfield(L, -2, "p99_latency_ns");

    lua_pushinteger(L, (lua_Integer)hist.min_ns);
    lua_setfield(L, -2, "min_latency_ns");
    lua_pushinteger(L, (lua_Integer)hist.max_ns);
    lua_setfield(L, -2, "max_latency_ns");
    lua_pushinteger(L, (lua_Integer)(hist.count > 0 ? hist.sum_ns / hist.count : 0));
    lua_setfield(L, -2, "avg_latency_ns");

    lua_pushinteger(L, (lua_Integer)total_calls);
    lua_setfield(L, -2, "total_calls");
    lua_pushinteger(L, (lua_Integer)total_errors);
    lua_setfield(L, -2, "total_errors");
    lua_pushinteger(L, (lua_Integer)total_ns);
    lua_setfield(L, -2, "total_ns");

    return 1;
}

/* ============================================================================
 * GPU consumer toggles (admin & telemetry suite, M2)
 * ============================================================================ */

/**
 * slm.gpu_use_set(consumer, enabled) - Enable or disable GPU dispatch for a consumer.
 *
 * @consumer: "sched" | "eviction" | "inference"
 * @enabled:  bool
 *
 * Returns (true) on success, or (false, "reason") on rejection. Disable
 * (enabled=false) always succeeds.
 */
static int l_gpu_use_set(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    bool enabled = lua_toboolean(L, 2);

    enum gpu_consumer c = gpu_consumer_from_name(name);
    if (c == GPU_CONSUMER_COUNT) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "unknown consumer '%s'", name);
        return 2;
    }

    const char *reason = NULL;
    int rc = gpu_consumer_set(c, enabled, &reason);
    if (rc != 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, reason ? reason : "rejected");
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/**
 * slm.gpu_use_get(consumer) - Read the current toggle state.
 *
 * Returns bool (or nil if `consumer` is unknown).
 */
static int l_gpu_use_get(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    enum gpu_consumer c = gpu_consumer_from_name(name);
    if (c == GPU_CONSUMER_COUNT) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, gpu_consumer_enabled(c));
    return 1;
}

/**
 * slm.gpu_use_status() - Tabular status of all three consumers.
 *
 * Returns table:
 *   { sched=bool, eviction=bool, inference=bool, gpu_ready=bool,
 *     last_change_ms = { sched=N, eviction=N, inference=N } }
 */
static int l_gpu_use_status(lua_State *L) {
    if (!L) return 0;
    struct gpu_consumer_status st;
    gpu_consumer_status_get(&st);

    lua_createtable(L, 0, 5);

    lua_pushboolean(L, st.sched);
    lua_setfield(L, -2, "sched");
    lua_pushboolean(L, st.eviction);
    lua_setfield(L, -2, "eviction");
    lua_pushboolean(L, st.inference);
    lua_setfield(L, -2, "inference");
    lua_pushboolean(L, st.gpu_ready);
    lua_setfield(L, -2, "gpu_ready");

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)st.sched_change_ms);
    lua_setfield(L, -2, "sched");
    lua_pushinteger(L, (lua_Integer)st.eviction_change_ms);
    lua_setfield(L, -2, "eviction");
    lua_pushinteger(L, (lua_Integer)st.inference_change_ms);
    lua_setfield(L, -2, "inference");
    lua_setfield(L, -2, "last_change_ms");

    return 1;
}

/* ============================================================================
 * Lua-defined task trampoline (#208)
 * ============================================================================
 *
 * slm.task_create(name, fn) dumps `fn` to Lua bytecode, stashes the blob in
 * a PMM-backed buffer, and spawns a kernel task whose entry point creates
 * a fresh lua_State, loads the bytecode, calls the function, and exits.
 * Isolates per-task state (no shared upvalues) at the cost of an extra
 * lua_State per task — the shared Lua heap tracks live-state count so
 * heap_reset only fires when every state is closed.
 *
 * Context pool is static; no malloc in the binding hot path.
 */

#define LUA_TASK_NAME_LEN 32

struct lua_task_ctx {
    uint8_t active;
    char name[LUA_TASK_NAME_LEN];
    uint8_t *bytecode;
    size_t bytecode_len;
    size_t bytecode_pages;
};

static struct lua_task_ctx lua_task_ctxs[LUA_TASK_MAX_CTX];

/* Accumulator for lua_dump — writes bytecode into a PMM-backed buffer as
 * lua_dump streams chunks. Tracks a current offset and a capacity; grows
 * the buffer by re-allocating a bigger region when needed. */
struct lua_dump_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t pages;
    int oom;
};

static int lua_dump_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    struct lua_dump_buf *b = (struct lua_dump_buf *)ud;
    if (b->oom) return 1;

    size_t need = b->len + sz;
    if (need > b->cap) {
        /* Double the buffer (or round up to multiple of page size). */
        size_t new_pages = b->pages * 2;
        if (new_pages < (need + 4095) / 4096) new_pages = (need + 4095) / 4096;
        if (new_pages < 1) new_pages = 1;
        uint8_t *nb = (uint8_t *)pmm_alloc_pages(new_pages);
        if (!nb) { b->oom = 1; return 1; }
        for (size_t i = 0; i < b->len; i++) nb[i] = b->data[i];
        if (b->data) pmm_free_pages(b->data, b->pages);
        b->data = nb;
        b->cap = new_pages * 4096;
        b->pages = new_pages;
    }
    const uint8_t *src = (const uint8_t *)p;
    for (size_t i = 0; i < sz; i++) b->data[b->len + i] = src[i];
    b->len += sz;
    return 0;
}

static void lua_task_entry(void *arg)
{
    struct lua_task_ctx *ctx = (struct lua_task_ctx *)arg;
    lua_State *L = lua_slm_newstate();
    if (!L) {
        shell_printf("[lua task %s] failed to create state\n", ctx->name);
        goto cleanup;
    }

    if (luaL_loadbuffer(L, (const char *)ctx->bytecode, ctx->bytecode_len,
                        ctx->name) != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        shell_printf("[lua task %s] error: %s\n",
                    ctx->name, err ? err : "(unknown)");
    }

    lua_slm_close(L);

cleanup:
    if (ctx->bytecode) pmm_free_pages(ctx->bytecode, ctx->bytecode_pages);
    ctx->active = 0;
    task_exit();
}

/**
 * slm.task_create(name, fn) - Spawn a new kernel task running a Lua function.
 *
 * `fn` is serialized to Lua bytecode via `lua_dump` and executed in a fresh
 * lua_State on the new task's stack. Returns the task id (>=1) on success
 * or nil on failure (full pool, allocation failure, bad arguments).
 *
 * The new state has no access to upvalues or globals from the caller —
 * it starts empty with the same `slm` module and standard libs as the
 * shell REPL. Scripts that need to pass data in must use the msg_router,
 * shared files, or arguments encoded into `fn`'s captured upvalues before
 * the `lua_dump` (which dumps bytecode only — upvalues are NOT captured).
 */
static int l_task_create(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    /* Grab a ctx slot before doing any allocation. */
    struct lua_task_ctx *ctx = NULL;
    for (int i = 0; i < LUA_TASK_MAX_CTX; i++) {
        if (!lua_task_ctxs[i].active) { ctx = &lua_task_ctxs[i]; break; }
    }
    if (!ctx) {
        lua_pushnil(L);
        return 1;
    }

    /* Copy name. */
    size_t nlen = 0;
    while (name[nlen] && nlen < LUA_TASK_NAME_LEN - 1) {
        ctx->name[nlen] = name[nlen];
        nlen++;
    }
    ctx->name[nlen] = '\0';

    /* Serialize the function via lua_dump. Start with 1 page (4 KB) which
     * handles any realistic short function; writer will grow as needed. */
    struct lua_dump_buf b = {0};
    b.pages = 1;
    b.cap = 4096;
    b.data = (uint8_t *)pmm_alloc_pages(1);
    if (!b.data) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushvalue(L, 2);
    int rc = lua_dump(L, lua_dump_writer, &b, /*strip=*/1);
    lua_pop(L, 1);  /* pop the function copy */

    if (rc != 0 || b.oom) {
        if (b.data) pmm_free_pages(b.data, b.pages);
        lua_pushnil(L);
        return 1;
    }

    ctx->bytecode = b.data;
    ctx->bytecode_len = b.len;
    ctx->bytecode_pages = b.pages;
    ctx->active = 1;

    struct task *t = task_create_with_priority(ctx->name, lua_task_entry,
                                               ctx, TASK_PRIORITY_NORMAL);
    if (!t) {
        pmm_free_pages(ctx->bytecode, ctx->bytecode_pages);
        ctx->active = 0;
        lua_pushnil(L);
        return 1;
    }

    scheduler_add_task(t);

    lua_pushinteger(L, (lua_Integer)t->id);
    return 1;
}

/**
 * slm.task_kill(task_id) - Terminate a task.
 *
 * Wraps scheduler_remove_task + task_destroy. Refuses to kill the
 * currently running task (would race with the scheduler on its own stack)
 * and refuses the idle task id 0. Returns true on success, false on any
 * refusal (unknown id, idle, self, terminated).
 */
static int l_task_kill(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    if (task_id <= 0) {           /* 0 is idle, negative is invalid */
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Don't kill the task that's asking — task_exit is the right path for
     * self-termination and it doesn't return. */
    if (t == task_current()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    scheduler_remove_task(t);
    task_destroy(t);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * slm.task_set_priority(task_id, priority) - Change a task's priority.
 *
 * Priority must be in [0, 7] (TASK_PRIORITY_IDLE..TASK_PRIORITY_CRITICAL).
 * Returns true on success, false on argument errors.
 */
static int l_task_set_priority(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    lua_Integer prio = luaL_checkinteger(L, 2);

    if (task_id < 0 || prio < 0 || prio > 7) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    task_set_priority(t, (uint8_t)prio);
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * slm.task_pin(task_id, cpu) - Pin a task to a specific CPU.
 *
 * Wraps sched_set_task_affinity. Pass -1 (or `nil`) as the second argument
 * to clear affinity. Returns true on success, false on argument errors.
 */
static int l_task_pin(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    lua_Integer cpu = luaL_checkinteger(L, 2);

    if (task_id < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Negative -> clear affinity. Positive -> must be a valid cpu id. */
    uint32_t target;
    if (cpu < 0) {
        target = CPU_AFFINITY_ANY;
    } else if (cpu >= (lua_Integer)cpu_count) {
        lua_pushboolean(L, 0);
        return 1;
    } else {
        target = (uint32_t)cpu;
    }

    int rc = sched_set_task_affinity(t, target);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/**
 * slm.task_migrate(task_id, target_cpu) - Move a task to a specific CPU
 *
 * Wraps sched_migrate_task. Returns true on success, false if the task
 * cannot be migrated (unknown id, running, affinity conflict, target out
 * of range). Scripts can poll slm.tasks() to see the CPU assignment
 * change after the migrate.
 */
static int l_task_migrate(lua_State *L) {
    if (!L) return 0;
    lua_Integer task_id = luaL_checkinteger(L, 1);
    lua_Integer target_cpu = luaL_checkinteger(L, 2);

    /* Task IDs are a monotonic uint32_t counter (see next_task_id in
     * kernel/sched/task.c) — NOT bounded by MAX_TASKS, which only
     * sizes the task_table array. Just reject negative values and
     * out-of-range CPUs. task_get() returns NULL for unknown ids.
     * Note: lua_Integer is 32-bit signed here (LUA_32BITS=1) so we
     * cannot compare against UINT32_MAX — it would wrap to -1. */
    if (task_id < 0 ||
        target_cpu < 0 || target_cpu >= (lua_Integer)cpu_count) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int rc = sched_migrate_task(t, (uint32_t)target_cpu);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/**
 * slm.ai_sched_decision(task_id) - Get the last AI scheduler decision for a task.
 *
 * Returns a table: {core, priority_adj, preempt, raw}. Returns nil when
 *   - CONFIG_AI_SCHEDULER is off
 *   - the task id is unknown or terminated
 *   - the AI policy has not run on this task yet (last_ai_action == -1)
 *
 * priority_adj is 0/1/2 (none/boost/reduce). preempt is 0/1. raw is the
 * packed action index used inside the AI scheduler for histogram keys.
 */
static int l_ai_sched_decision(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_SCHEDULER)
    lua_Integer task_id = luaL_checkinteger(L, 1);
    if (task_id < 0) {
        lua_pushnil(L);
        return 1;
    }

    struct task *t = task_get((uint32_t)task_id);
    if (!t || t->state == TASK_TERMINATED || t->last_ai_action < 0) {
        lua_pushnil(L);
        return 1;
    }

    /* Decode via the canonical ai_decode_action (ai_types.h). Originally
     * inlined here to keep the Lua library isolated from AI headers, but
     * the PR #217 review flagged that as a silent-break hazard if the
     * encoding ever changes — ai_types.h is plain C with no heavy deps
     * so the include is free. */
    struct ai_sched_action act;
    ai_decode_action(t->last_ai_action, &act);

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)act.core_assignment);
    lua_setfield(L, -2, "core");

    lua_pushinteger(L, (lua_Integer)act.priority_adj);
    lua_setfield(L, -2, "priority_adj");

    lua_pushinteger(L, (lua_Integer)act.preempt);
    lua_setfield(L, -2, "preempt");

    lua_pushinteger(L, (lua_Integer)t->last_ai_action);
    lua_setfield(L, -2, "raw");
#else
    (void)luaL_checkinteger(L, 1);  /* still validate arg shape */
    lua_pushnil(L);
#endif
    return 1;
}

/* ============================================================================
 * CPU Info Bindings
 * ============================================================================ */

/**
 * slm.cpu_info() - Get per-CPU information
 * Returns table: {online_count, total_count, current_cpu, cpus=[{id, isolated, ticks, schedules}, ...]}
 */
static int l_cpu_info(lua_State *L) {
    if (!L) return 0;
    /* Declared as pointer (into NC memory) on PLATFORM_HAS_NC_MEMORY
     * platforms, as a plain BSS array elsewhere — must extern-declare
     * in the matching shape or subscripting dereferences garbage. */
#if defined(PLATFORM_HAS_NC_MEMORY)
    extern volatile uint32_t *sched_diag_tick;
    extern volatile uint32_t *sched_diag_schedule;
#else
    extern volatile uint32_t sched_diag_tick[];
    extern volatile uint32_t sched_diag_schedule[];
#endif

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)cpus_online);
    lua_setfield(L, -2, "online_count");

    lua_pushinteger(L, (lua_Integer)cpu_count);
    lua_setfield(L, -2, "total_count");

    /* #313: delegate to the canonical cpu_id(). Inline-asm'ing
     * (mpidr & 0xFF) returned Aff0, which is always 0 on Pi 5. */
    lua_pushinteger(L, (lua_Integer)cpu_id());
    lua_setfield(L, -2, "current_cpu");

    /* Per-CPU array */
    uint32_t n = cpu_count;
    if (n > MAX_CPUS) n = MAX_CPUS;
    lua_createtable(L, (int)n, 0);
    for (uint32_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 4);

        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, "id");

        lua_pushboolean(L, sched_is_core_isolated(i));
        lua_setfield(L, -2, "isolated");

        lua_pushinteger(L, sched_diag_tick ? (lua_Integer)sched_diag_tick[i] : 0);
        lua_setfield(L, -2, "ticks");

        lua_pushinteger(L, sched_diag_schedule ? (lua_Integer)sched_diag_schedule[i] : 0);
        lua_setfield(L, -2, "schedules");

        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "cpus");

    return 1;
}

/**
 * slm.term_size() - Get current shell session terminal metadata.
 * Returns table: {cols, rows, term}
 */
static int l_term_size(lua_State *L) {
    if (!L) return 0;
    struct shell_session *s = shell_session_current();
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)(s ? s->window_cols : SHELL_DEFAULT_COLS));
    lua_setfield(L, -2, "cols");
    lua_pushinteger(L, (lua_Integer)(s ? s->window_rows : SHELL_DEFAULT_ROWS));
    lua_setfield(L, -2, "rows");
    lua_pushstring(L, (s && s->term_type[0]) ? s->term_type : "");
    lua_setfield(L, -2, "term");
    return 1;
}

/* ============================================================================
 * VMM Stats Bindings (ARM64 only — x86-64 build omits vmm.c)
 * ============================================================================ */

/**
 * slm.vmm_stats() - Get virtual memory statistics
 * Returns table: {l1_tables, l2_tables, blocks_mapped, bytes_mapped}
 * Returns nil on x86-64 (no VMM).
 */
static int l_vmm_stats(lua_State *L) {
    if (!L) return 0;
#if defined(PLATFORM_X86_64)
    lua_pushnil(L);
#else
    struct vmm_stats stats;
    vmm_get_stats(&stats);

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)stats.l1_tables);
    lua_setfield(L, -2, "l1_tables");

    lua_pushinteger(L, (lua_Integer)stats.l2_tables);
    lua_setfield(L, -2, "l2_tables");

    lua_pushinteger(L, (lua_Integer)stats.blocks_mapped);
    lua_setfield(L, -2, "blocks_mapped");

    lua_pushinteger(L, (lua_Integer)stats.bytes_mapped);
    lua_setfield(L, -2, "bytes_mapped");
#endif
    return 1;
}

/* ============================================================================
 * IPC Stats Bindings
 * ============================================================================ */

/**
 * slm.ipc_stats() - Get IPC statistics
 * Returns table: {queue_count, buffer_count, msgs_sent, msgs_recv}
 */
static int l_ipc_stats(lua_State *L) {
    if (!L) return 0;
    struct ipc_stats stats;
    ipc_get_stats(&stats);

    lua_createtable(L, 0, 4);

    lua_pushinteger(L, (lua_Integer)stats.queue_count);
    lua_setfield(L, -2, "queue_count");

    lua_pushinteger(L, (lua_Integer)stats.buffer_count);
    lua_setfield(L, -2, "buffer_count");

    lua_pushinteger(L, (lua_Integer)stats.total_msgs_sent);
    lua_setfield(L, -2, "msgs_sent");

    lua_pushinteger(L, (lua_Integer)stats.total_msgs_recv);
    lua_setfield(L, -2, "msgs_recv");

    return 1;
}

/* ============================================================================
 * Eviction Policy Bindings (gated on CONFIG_AI_EVICTION)
 * ============================================================================ */

static int eviction_blob_kind_id(const char *kind)
{
    if (!kind) return 0;
    if (strcmp(kind, "xgboost") == 0) return 1;
    if (strcmp(kind, "mlp") == 0) return 2;
    if (strcmp(kind, "cacheus_config") == 0) return 3;
    return 0;
}

static const char *eviction_blob_state_name(uint16_t state)
{
    switch (state) {
        case 0: return "empty";
        case 1: return "staged";
        case 2: return "active";
        case 3: return "rolled_back";
        default: return "unknown";
    }
}

static void lua_push_eviction_blob_meta(lua_State *L,
                                        const RustEvictionBlobMeta *meta)
{
    lua_createtable(L, 0, 5);

    lua_pushinteger(L, (lua_Integer)meta->version);
    lua_setfield(L, -2, "version");

    lua_pushinteger(L, (lua_Integer)meta->kind_id);
    lua_setfield(L, -2, "kind_id");

    lua_pushinteger(L, (lua_Integer)meta->feature_schema_version);
    lua_setfield(L, -2, "feature_schema_version");

    lua_pushinteger(L, (lua_Integer)meta->payload_len);
    lua_setfield(L, -2, "payload_len");

    lua_pushinteger(L, (lua_Integer)meta->checksum);
    lua_setfield(L, -2, "checksum");
}

#if defined(CONFIG_AI_SCHEDULER)
static int sched_model_kind_id(const char *kind)
{
    if (!kind) return 0;
    if (strcmp(kind, "mlp") == 0) return SCHED_MODEL_KIND_MLP;
    if (strcmp(kind, "ppo") == 0) return SCHED_MODEL_KIND_PPO;
    if (strcmp(kind, "config") == 0) return SCHED_MODEL_KIND_CONFIG;
    if (strcmp(kind, "thresholds") == 0) return SCHED_MODEL_KIND_THRESHOLDS;
    if (strcmp(kind, "rebalance") == 0) return SCHED_MODEL_KIND_REBALANCE;
    return 0;
}

static const char *sched_model_state_name(uint16_t state)
{
    switch (state) {
        case SCHED_MODEL_EMPTY: return "empty";
        case SCHED_MODEL_STAGED: return "staged";
        case SCHED_MODEL_ACTIVE: return "active";
        case SCHED_MODEL_ROLLED_BACK: return "rolled_back";
        default: return "unknown";
    }
}

static void lua_push_sched_model_meta(lua_State *L,
                                      const struct sched_model_meta *meta)
{
    lua_createtable(L, 0, 7);

    lua_pushinteger(L, (lua_Integer)meta->version);
    lua_setfield(L, -2, "version");

    lua_pushinteger(L, (lua_Integer)meta->schema_version);
    lua_setfield(L, -2, "schema_version");

    lua_pushinteger(L, (lua_Integer)meta->feature_version);
    lua_setfield(L, -2, "feature_version");

    lua_pushinteger(L, (lua_Integer)meta->action_version);
    lua_setfield(L, -2, "action_version");

    lua_pushinteger(L, (lua_Integer)meta->action_count);
    lua_setfield(L, -2, "action_count");

    lua_pushinteger(L, (lua_Integer)meta->payload_len);
    lua_setfield(L, -2, "payload_len");

    lua_pushinteger(L, (lua_Integer)meta->checksum);
    lua_setfield(L, -2, "checksum");
}

static void lua_push_sched_balance_config(lua_State *L,
                                          const struct sched_runtime_balance_config *cfg)
{
    lua_createtable(L, 0, 5);

    lua_pushinteger(L, (lua_Integer)cfg->enabled);
    lua_setfield(L, -2, "enabled");

    lua_pushinteger(L, (lua_Integer)cfg->min_target_ready);
    lua_setfield(L, -2, "min_target_ready");

    lua_pushinteger(L, (lua_Integer)cfg->min_active_cpus);
    lua_setfield(L, -2, "min_active_cpus");

    lua_pushinteger(L, (lua_Integer)cfg->imbalance_num);
    lua_setfield(L, -2, "imbalance_num");

    lua_pushinteger(L, (lua_Integer)cfg->imbalance_den);
    lua_setfield(L, -2, "imbalance_den");
}

static void lua_push_sched_deadline_thresholds(
    lua_State *L,
    const struct sched_runtime_deadline_thresholds *cfg)
{
    lua_createtable(L, 0, 3);

    lua_pushinteger(L, (lua_Integer)cfg->critical_ns);
    lua_setfield(L, -2, "critical_ns");

    lua_pushinteger(L, (lua_Integer)cfg->high_ns);
    lua_setfield(L, -2, "high_ns");

    lua_pushinteger(L, (lua_Integer)cfg->boost_ns);
    lua_setfield(L, -2, "boost_ns");
}

static void lua_push_sched_rebalance_config(
    lua_State *L,
    const struct sched_runtime_rebalance_config *cfg)
{
    lua_createtable(L, 0, 3);

    lua_pushinteger(L, (lua_Integer)cfg->enabled);
    lua_setfield(L, -2, "enabled");

    lua_pushinteger(L, (lua_Integer)cfg->interval_ticks);
    lua_setfield(L, -2, "interval_ticks");

    lua_pushinteger(L, (lua_Integer)cfg->imbalance_min);
    lua_setfield(L, -2, "imbalance_min");
}
#endif

/**
 * slm.eviction_policy() - Get current eviction policy name
 * Returns string, or nil if eviction is disabled.
 */
static int l_eviction_policy(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_EVICTION)
    uint8_t buf[64];
    size_t n = rust_eviction_policy_name(buf, sizeof(buf));
    if (n > 0)
        lua_pushlstring(L, (const char *)buf, n);
    else
        lua_pushnil(L);
#else
    lua_pushnil(L);
#endif
    return 1;
}

/**
 * slm.eviction_set_policy(name) - Switch eviction policy by name
 * Returns true on success, false on failure (unknown name / feature off).
 */
static int l_eviction_set_policy(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_EVICTION)
    int32_t rc = rust_eviction_policy_set((const uint8_t *)name);
    lua_pushboolean(L, rc == 0);
#else
    (void)name;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.eviction_model_status(kind) - Query runtime blob state for one kind.
 * Returns {kind, state, has_staged, has_active, has_rollback, staged,
 * active, rollback} or nil if eviction is disabled / kind invalid /
 * status unavailable.
 */
static int l_eviction_model_status(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_EVICTION)
    int kind_id = eviction_blob_kind_id(kind);
    RustEvictionBlobStatus st;
    if (kind_id == 0 || rust_eviction_blob_status((uint16_t)kind_id, &st) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 8);

    lua_pushstring(L, kind);
    lua_setfield(L, -2, "kind");

    lua_pushstring(L, eviction_blob_state_name(st.state));
    lua_setfield(L, -2, "state");

    lua_pushboolean(L, st.has_staged != 0);
    lua_setfield(L, -2, "has_staged");

    lua_pushboolean(L, st.has_active != 0);
    lua_setfield(L, -2, "has_active");

    lua_pushboolean(L, st.has_rollback != 0);
    lua_setfield(L, -2, "has_rollback");

    if (st.has_staged) {
        lua_push_eviction_blob_meta(L, &st.staged);
        lua_setfield(L, -2, "staged");
    }
    if (st.has_active) {
        lua_push_eviction_blob_meta(L, &st.active);
        lua_setfield(L, -2, "active");
    }
    if (st.has_rollback) {
        lua_push_eviction_blob_meta(L, &st.rollback);
        lua_setfield(L, -2, "rollback");
    }
#else
    (void)kind;
    lua_pushnil(L);
#endif
    return 1;
}

/**
 * slm.eviction_model_load(kind, path) - Read and stage a runtime blob.
 * Returns true on success, false on any error.
 */
static int l_eviction_model_load(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
    const char *path = luaL_checkstring(L, 2);
#if defined(CONFIG_AI_EVICTION)
    int kind_id = eviction_blob_kind_id(kind);
    if (kind_id == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0 || info.type != 0 || info.size == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    size_t pages_needed = (info.size + 4095) / 4096;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        pmm_free_pages(buf, pages_needed);
        lua_pushboolean(L, 0);
        return 1;
    }

    int rc = rust_eviction_blob_stage((uint16_t)kind_id, buf, (size_t)bytes_read);
    pmm_free_pages(buf, pages_needed);
    lua_pushboolean(L, rc == 0);
#else
    (void)kind;
    (void)path;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.eviction_model_activate(kind) - Promote staged runtime blob.
 */
static int l_eviction_model_activate(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_EVICTION)
    int kind_id = eviction_blob_kind_id(kind);
    lua_pushboolean(L, kind_id != 0 &&
                       rust_eviction_blob_activate((uint16_t)kind_id) == 0);
#else
    (void)kind;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.eviction_model_rollback(kind) - Roll back to prior runtime blob.
 */
static int l_eviction_model_rollback(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_EVICTION)
    int kind_id = eviction_blob_kind_id(kind);
    lua_pushboolean(L, kind_id != 0 &&
                       rust_eviction_blob_rollback((uint16_t)kind_id) == 0);
#else
    (void)kind;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.eviction_model_clear(kind) - Clear staged/active/rollback slots.
 */
static int l_eviction_model_clear(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_EVICTION)
    int kind_id = eviction_blob_kind_id(kind);
    lua_pushboolean(L, kind_id != 0 &&
                       rust_eviction_blob_clear((uint16_t)kind_id) == 0);
#else
    (void)kind;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.sched_model_status(kind) - Query runtime scheduler blob state.
 * Returns {kind, state, has_staged, has_active, has_rollback, staged,
 * active, rollback} or nil if unavailable / kind invalid.
 */
static int l_sched_model_status(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_SCHEDULER)
    int kind_id = sched_model_kind_id(kind);
    struct sched_model_status st;
    struct sched_runtime_balance_config cfg;
    struct sched_runtime_deadline_thresholds thresholds;
    struct sched_runtime_rebalance_config rebalance;
    if (kind_id == 0 || sched_model_status((uint16_t)kind_id, &st) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 8);

    lua_pushstring(L, kind);
    lua_setfield(L, -2, "kind");

    lua_pushstring(L, sched_model_state_name(st.state));
    lua_setfield(L, -2, "state");

    lua_pushboolean(L, st.has_staged != 0);
    lua_setfield(L, -2, "has_staged");

    lua_pushboolean(L, st.has_active != 0);
    lua_setfield(L, -2, "has_active");

    lua_pushboolean(L, st.has_rollback != 0);
    lua_setfield(L, -2, "has_rollback");

    if (st.has_staged) {
        lua_push_sched_model_meta(L, &st.staged);
        lua_setfield(L, -2, "staged");
    }
    if (st.has_active) {
        lua_push_sched_model_meta(L, &st.active);
        lua_setfield(L, -2, "active");
    }
    if (st.has_rollback) {
        lua_push_sched_model_meta(L, &st.rollback);
        lua_setfield(L, -2, "rollback");
    }
    if (kind_id == SCHED_MODEL_KIND_CONFIG &&
        sched_runtime_balance_config_snapshot(&cfg) == 0) {
        lua_push_sched_balance_config(L, &cfg);
        lua_setfield(L, -2, "config");
    }
    if (kind_id == SCHED_MODEL_KIND_THRESHOLDS &&
        sched_runtime_deadline_thresholds_snapshot(&thresholds) == 0) {
        lua_push_sched_deadline_thresholds(L, &thresholds);
        lua_setfield(L, -2, "thresholds");
    }
    if (kind_id == SCHED_MODEL_KIND_REBALANCE &&
        sched_runtime_rebalance_config_snapshot(&rebalance) == 0) {
        lua_push_sched_rebalance_config(L, &rebalance);
        lua_setfield(L, -2, "rebalance");
    }
#else
    (void)kind;
    lua_pushnil(L);
#endif
    return 1;
}

/**
 * slm.sched_model_load(kind, path) - Read and stage a scheduler runtime blob.
 */
static int l_sched_model_load(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
    const char *path = luaL_checkstring(L, 2);
#if defined(CONFIG_AI_SCHEDULER)
    int kind_id = sched_model_kind_id(kind);
    if (kind_id == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0 || info.type != 0 || info.size == 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    size_t pages_needed = (info.size + 4095) / 4096;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        pmm_free_pages(buf, pages_needed);
        lua_pushboolean(L, 0);
        return 1;
    }

    int rc = sched_model_stage_blob((uint16_t)kind_id, buf, (size_t)bytes_read);
    pmm_free_pages(buf, pages_needed);
    lua_pushboolean(L, rc == 0);
#else
    (void)kind;
    (void)path;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.sched_model_activate(kind) - Promote staged scheduler runtime blob.
 */
static int l_sched_model_activate(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_SCHEDULER)
    int kind_id = sched_model_kind_id(kind);
    lua_pushboolean(L, kind_id != 0 &&
                       sched_model_activate((uint16_t)kind_id) == 0);
#else
    (void)kind;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.sched_model_rollback(kind) - Roll back to prior scheduler runtime blob.
 */
static int l_sched_model_rollback(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_SCHEDULER)
    int kind_id = sched_model_kind_id(kind);
    lua_pushboolean(L, kind_id != 0 &&
                       sched_model_rollback((uint16_t)kind_id) == 0);
#else
    (void)kind;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.sched_model_clear(kind) - Clear scheduler staged/active/rollback slots.
 */
static int l_sched_model_clear(lua_State *L) {
    if (!L) return 0;
    const char *kind = luaL_checkstring(L, 1);
#if defined(CONFIG_AI_SCHEDULER)
    int kind_id = sched_model_kind_id(kind);
    lua_pushboolean(L, kind_id != 0 &&
                       sched_model_clear((uint16_t)kind_id) == 0);
#else
    (void)kind;
    lua_pushboolean(L, 0);
#endif
    return 1;
}

/**
 * slm.eviction_stats() - Get eviction statistics
 * Returns table with {policy, enabled, weight_evictions, workspace_evictions,
 *                     weight_allocated/total, workspace_allocated/total,
 *                     snapshot_candidates, expert_weights_bp}
 * Returns nil if eviction feature is disabled.
 */
static int l_eviction_stats(lua_State *L) {
    if (!L) return 0;
#if defined(CONFIG_AI_EVICTION)
    RustEvictionStats stats;
    int32_t rc = rust_eviction_get_stats(&stats);
    if (rc != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 10);

    lua_pushboolean(L, stats.feature_enabled);
    lua_setfield(L, -2, "enabled");

    /* Policy name */
    uint8_t pbuf[64];
    size_t pn = rust_eviction_policy_name(pbuf, sizeof(pbuf));
    if (pn > 0)
        lua_pushlstring(L, (const char *)pbuf, pn);
    else
        lua_pushstring(L, "unknown");
    lua_setfield(L, -2, "policy");

    lua_pushinteger(L, (lua_Integer)stats.weight_evictions);
    lua_setfield(L, -2, "weight_evictions");

    lua_pushinteger(L, (lua_Integer)stats.workspace_evictions);
    lua_setfield(L, -2, "workspace_evictions");

    lua_pushinteger(L, (lua_Integer)stats.weight_allocated);
    lua_setfield(L, -2, "weight_allocated");

    lua_pushinteger(L, (lua_Integer)stats.weight_total);
    lua_setfield(L, -2, "weight_total");

    lua_pushinteger(L, (lua_Integer)stats.workspace_allocated);
    lua_setfield(L, -2, "workspace_allocated");

    lua_pushinteger(L, (lua_Integer)stats.workspace_total);
    lua_setfield(L, -2, "workspace_total");

    lua_pushinteger(L, (lua_Integer)stats.snapshot_candidates);
    lua_setfield(L, -2, "snapshot_candidates");

    /* CACHEUS expert weights (basis points: 0..10000) */
    if (stats.cacheus_expert_count > 0) {
        uint32_t n = stats.cacheus_expert_count;
        if (n > 5) n = 5;
        lua_createtable(L, (int)n, 0);
        for (uint32_t i = 0; i < n; i++) {
            lua_pushinteger(L, (lua_Integer)stats.expert_weights_bp[i]);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_setfield(L, -2, "expert_weights_bp");
    }
#else
    lua_pushnil(L);
#endif
    return 1;
}

/* ============================================================================
 * Extended Model Bindings
 * ============================================================================ */

/**
 * slm.model_list() - List all loaded models
 * Returns array of tables: {{index, name, format, params, weight_size, nodes}, ...}
 */
static int l_model_list(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);

    int idx = 1;
    /* Registry has 8 slots; skip invalid ones. Same loop bound as the
     * `model list` shell handler in shell_exec.c. */
    for (uint32_t i = 0; i < 8; i++) {
        RustModelInfo info;
        if (rust_model_get_info(i, &info) != 0) continue;

        lua_createtable(L, 0, 6);

        lua_pushinteger(L, (lua_Integer)i);
        lua_setfield(L, -2, "index");

        lua_pushstring(L, (const char *)info.name);
        lua_setfield(L, -2, "name");

        const char *fmt;
        switch (info.format) {
            case 0: fmt = "GGUF"; break;
            case 1: fmt = "ONNX"; break;
            case 2: fmt = "Raw"; break;
            default: fmt = "unknown"; break;
        }
        lua_pushstring(L, fmt);
        lua_setfield(L, -2, "format");

        lua_pushinteger(L, (lua_Integer)info.param_count);
        lua_setfield(L, -2, "params");

        lua_pushinteger(L, (lua_Integer)info.weight_size);
        lua_setfield(L, -2, "weight_size");

        lua_pushinteger(L, (lua_Integer)info.node_count);
        lua_setfield(L, -2, "nodes");

        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

/**
 * slm.model_info(index) - Get detailed info for one model
 * Returns table or nil if index invalid.
 */
static int l_model_info(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);

    RustModelInfo info;
    if (rust_model_get_info((uint32_t)idx, &info) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 9);

    lua_pushinteger(L, (lua_Integer)idx);
    lua_setfield(L, -2, "index");

    lua_pushstring(L, (const char *)info.name);
    lua_setfield(L, -2, "name");

    const char *fmt;
    switch (info.format) {
        case 0: fmt = "GGUF"; break;
        case 1: fmt = "ONNX"; break;
        case 2: fmt = "Raw"; break;
        default: fmt = "unknown"; break;
    }
    lua_pushstring(L, fmt);
    lua_setfield(L, -2, "format");

    lua_pushinteger(L, (lua_Integer)info.param_count);
    lua_setfield(L, -2, "params");

    lua_pushinteger(L, (lua_Integer)info.weight_size);
    lua_setfield(L, -2, "weight_size");

    lua_pushinteger(L, (lua_Integer)info.workspace_size);
    lua_setfield(L, -2, "workspace_size");

    lua_pushinteger(L, (lua_Integer)info.node_count);
    lua_setfield(L, -2, "nodes");

    lua_pushinteger(L, (lua_Integer)info.input_count);
    lua_setfield(L, -2, "inputs");

    lua_pushinteger(L, (lua_Integer)info.output_count);
    lua_setfield(L, -2, "outputs");

    return 1;
}

/**
 * slm.model_bench(index, iterations) - Run inference benchmark
 * Results are printed to UART. Returns 0 on success, -1 on error.
 */
static int l_model_bench(lua_State *L) {
    if (!L) return 0;
    int idx = (int)luaL_checkinteger(L, 1);
    int iters = (int)luaL_optinteger(L, 2, 10);
    if (iters < 1) iters = 1;
    int rc = rust_infer_bench((uint32_t)idx, (uint32_t)iters);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.model_load(path [, name]) - Load an ONNX model from the VFS.
 *
 * Mirrors the `model load <path>` shell command (kernel/src/shell_sys.c)
 * so scripts can exercise the eviction demo with different models. If
 * `name` is omitted the model name is derived from the filename
 * (last path component, extension stripped).
 *
 * Returns the registry index (>=0) on success, -1 on any failure
 * (path too long, file not found, not a file, empty, out of memory,
 * ONNX parse error). The file buffer is freed before return regardless
 * of success — rust_model_load copies into its own registry storage.
 */
static int l_model_load(lua_State *L) {
    if (!L) return 0;
    const char *path = luaL_checkstring(L, 1);

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        lua_pushinteger(L, -1);
        return 1;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0 || info.type != 0 || info.size == 0) {
        lua_pushinteger(L, -1);
        return 1;
    }

    size_t pages_needed = (info.size + 4095) / 4096;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) {
        lua_pushinteger(L, -1);
        return 1;
    }

    int bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        pmm_free_pages(buf, pages_needed);
        lua_pushinteger(L, -1);
        return 1;
    }

    /* Derive model name: explicit arg 2 wins; otherwise use the
     * last path component, extension stripped (matches shell's
     * `model load` behavior). */
    char derived[32];
    const char *name = luaL_optstring(L, 2, NULL);
    if (!name) {
        const char *base = resolved;
        for (const char *p = resolved; *p; p++)
            if (*p == '/') base = p + 1;
        size_t n = 0;
        for (const char *p = base; *p && *p != '.' && n < 31; p++)
            derived[n++] = *p;
        derived[n] = '\0';
        name = derived;
    }

    int result = rust_model_load(name, buf, (size_t)bytes_read);
    pmm_free_pages(buf, pages_needed);

    lua_pushinteger(L, result);
    return 1;
}

/**
 * slm.infer_stats() - Get inference performance statistics
 * Returns table: {total, total_ns, min_ns, max_ns, last_ns, errors}
 * Returns nil on error (e.g., stats unavailable).
 */
static int l_infer_stats(lua_State *L) {
    if (!L) return 0;
    RustInferStats stats;
    int rc = rust_infer_stats(&stats);
    if (rc != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 6);

    lua_pushinteger(L, (lua_Integer)stats.total_inferences);
    lua_setfield(L, -2, "total");

    lua_pushinteger(L, (lua_Integer)stats.total_time_ns);
    lua_setfield(L, -2, "total_ns");

    lua_pushinteger(L, (lua_Integer)stats.min_time_ns);
    lua_setfield(L, -2, "min_ns");

    lua_pushinteger(L, (lua_Integer)stats.max_time_ns);
    lua_setfield(L, -2, "max_ns");

    lua_pushinteger(L, (lua_Integer)stats.last_time_ns);
    lua_setfield(L, -2, "last_ns");

    lua_pushinteger(L, (lua_Integer)stats.errors);
    lua_setfield(L, -2, "errors");

    return 1;
}

/**
 * slm.gpu_status() - Get GPU subsystem status
 * Returns table: {available, name, device, compute_ready, unified_memory, memory_size}
 */
static int l_gpu_status(lua_State *L) {
    if (!L) return 0;

    int avail = slm_gpu_available();

    lua_createtable(L, 0, 6);

    lua_pushboolean(L, avail);
    lua_setfield(L, -2, "available");

    if (avail) {
        RustGpuInfo info;
        if (slm_gpu_get_info(&info) == 0) {
            lua_pushstring(L, (const char *)info.name);
            lua_setfield(L, -2, "name");

            lua_pushstring(L, (const char *)info.device);
            lua_setfield(L, -2, "device");

            lua_pushboolean(L, info.compute_ready);
            lua_setfield(L, -2, "compute_ready");

            lua_pushboolean(L, info.unified_memory);
            lua_setfield(L, -2, "unified_memory");

            lua_pushinteger(L, (lua_Integer)info.memory_size);
            lua_setfield(L, -2, "memory_size");
        }
    }

    return 1;
}

/* ============================================================================
 * Shell Integration Bindings
 * ============================================================================ */

/**
 * slm.read_line() - Read one line from the shell's UART input.
 *
 * Blocks until the user presses Enter (or Ctrl+C, which yields an empty line).
 * Returns the line as a string with the trailing newline stripped.
 *
 * Used by interactive Lua scripts (menus, prompts) so they can read user
 * input without each script reimplementing line editing.
 */
static int l_read_line(lua_State *L) {
    if (!L) return 0;
    /* Drain pending msg_subscribe callbacks before blocking on UART (#207)
     * so scripts that wait at a menu prompt still see their subscribers. */
    lua_msg_drain(L);
    char buf[SHELL_MAX_LINE];
    int n = shell_read_line(buf, (int)sizeof(buf));
    if (n < 0) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, buf, (size_t)n);
    }
    return 1;
}

/**
 * slm.try_getc() - Read one input character without blocking.
 *
 * Returns a one-byte Lua string when input is available, or nil when the
 * current shell session has no pending character.
 */
static int l_try_getc(lua_State *L) {
    if (!L) return 0;
    lua_msg_drain(L);
    int ch = shell_try_getc();
    if (ch < 0) {
        lua_pushnil(L);
    } else {
        char c = (char)ch;
        lua_pushlstring(L, &c, 1);
    }
    return 1;
}

/**
 * slm.shell_exec(cmd) - Run a shell command string and return its exit code.
 *
 * Dispatches `cmd` through the same parser the shell REPL uses, so anything
 * the user could type (e.g. "bench smp", "tasks", "run test") works from
 * Lua. Output goes to UART as it normally would. Returns the command's
 * integer return value, or -1 if the command was not found / line too long.
 *
 * Validates the command length at the binding boundary so a too-long input
 * raises a clean Lua error (with arg position) instead of falling through
 * to shell_execute's UART warning.
 */
static int l_shell_exec(lua_State *L) {
    if (!L) return 0;
    size_t len;
    const char *cmd = luaL_checklstring(L, 1, &len);
    luaL_argcheck(L, len < SHELL_MAX_LINE, 1,
                  "command exceeds SHELL_MAX_LINE");
    int rc = shell_execute(cmd);
    lua_pushinteger(L, rc);
    return 1;
}

#if defined(ENABLE_NETWORKING)
/* ============================================================================
 * slm.telnetd_* — Phase 3 Lua bindings for the TCP shell daemon
 *
 * Flat namespace (slm.telnetd_start, slm.telnetd_stop, ...) to match
 * the project's existing slm.component_* / slm.msg_* / slm.sched_*
 * pattern. The plan (§3.4) shows a nested slm.telnetd.{start,stop,...}
 * form; flattening is a deliberate deviation to keep Lua convention
 * consistent across the bindings.
 * ============================================================================ */

/**
 * slm.telnetd_start([port]) — bring the listener up.
 * Returns 0 on success, negative on failure.
 */
static int l_telnetd_start(lua_State *L) {
    if (!L) return 0;
    int port = luaL_optinteger(L, 1, 2323);
    if (port < 1 || port > 65535) {
        return luaL_argerror(L, 1, "port must be 1..65535");
    }
    if (!net_is_up()) {
        if (net_init() != 0) {
            lua_pushinteger(L, -1);
            return 1;
        }
    }
    int rc = tcp_shell_server_start((uint16_t)port);
    lua_pushinteger(L, rc);
    return 1;
}

/**
 * slm.telnetd_stop() — stop accepting new connections.
 * Returns true if the listener was running, false if already stopped.
 */
static int l_telnetd_stop(lua_State *L) {
    if (!L) return 0;
    bool was_running = tcp_shell_server_running();
    if (was_running) {
        tcp_shell_server_stop();
    }
    lua_pushboolean(L, was_running);
    return 1;
}

/**
 * slm.telnetd_status() — returns a table with daemon state.
 *   { running, port, accepted, active, max }
 */
static int l_telnetd_status(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);

    lua_pushboolean(L, tcp_shell_server_running());
    lua_setfield(L, -2, "running");

    lua_pushinteger(L, (lua_Integer)tcp_shell_server_port());
    lua_setfield(L, -2, "port");

    lua_pushinteger(L, (lua_Integer)tcp_shell_server_accepted());
    lua_setfield(L, -2, "accepted");

    lua_pushinteger(L, (lua_Integer)shell_io_tcp_active_count());
    lua_setfield(L, -2, "active");

    lua_pushinteger(L, (lua_Integer)MAX_TCP_SHELL_SESSIONS);
    lua_setfield(L, -2, "max");

    return 1;
}

/* Closure over the lua_State so the foreach visitor can push into it. */
struct telnetd_sessions_ctx {
    lua_State *L;
    int        idx;   /* next Lua array index */
};

static bool telnetd_sessions_visitor(const struct tcp_session_info *info, void *c) {
    struct telnetd_sessions_ctx *sc = c;
    lua_State *L = sc->L;

    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)info->session_id);
    lua_setfield(L, -2, "id");

    lua_pushinteger(L, (lua_Integer)info->peer_ip);
    lua_setfield(L, -2, "peer_ip");

    lua_pushinteger(L, (lua_Integer)info->peer_port);
    lua_setfield(L, -2, "peer_port");

    lua_pushinteger(L, (lua_Integer)info->connected_at);
    lua_setfield(L, -2, "connected_at");

    lua_rawseti(L, -2, sc->idx++);
    return true;
}

/**
 * slm.telnetd_sessions() — returns an array of per-session tables.
 *   { { id=N, peer_ip=N (nbo), peer_port=N, connected_at=ms }, ... }
 */
static int l_telnetd_sessions(lua_State *L) {
    if (!L) return 0;
    lua_newtable(L);
    struct telnetd_sessions_ctx sc = { .L = L, .idx = 1 };
    shell_io_tcp_foreach(telnetd_sessions_visitor, &sc);
    return 1;
}

/**
 * slm.telnetd_kick(id) — force-disconnect one session.
 * Returns true if a matching session was found.
 */
static int l_telnetd_kick(lua_State *L) {
    if (!L) return 0;
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id < 0 || id > 0xFFFFFFFF) {
        return luaL_argerror(L, 1, "id out of range");
    }
    bool kicked = shell_io_tcp_kick((uint32_t)id);
    lua_pushboolean(L, kicked);
    return 1;
}

/**
 * slm.http_get(url, dest) — fetch one HTTP resource into the VFS.
 * Requires networking to already be initialized and usable.
 * Returns a result table on success, nil on any error.
 */
static int l_http_get(lua_State *L) {
    if (!L) return 0;
    const char *url = luaL_checkstring(L, 1);
    const char *path = luaL_checkstring(L, 2);
    const char *expected_sha256 = luaL_optstring(L, 3, NULL);
    struct net_http_get_result result;
    struct net_info info;
    char resolved[VFS_MAX_PATH];

    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        lua_pushnil(L);
        return 1;
    }
    if (!net_is_up() || net_get_info(&info) != 0 || !info.link_up ||
        (info.dhcp_enabled && info.dhcp_status == NET_DHCP_PENDING)) {
        lua_pushnil(L);
        return 1;
    }
    if (net_http_get_file(url, resolved, expected_sha256, &result) != 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 8);
    lua_pushinteger(L, (lua_Integer)result.http_status);
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, (lua_Integer)result.bytes_received);
    lua_setfield(L, -2, "bytes_received");
    lua_pushinteger(L, (lua_Integer)result.content_length);
    lua_setfield(L, -2, "content_length");
    lua_pushinteger(L, (lua_Integer)result.httpc_result);
    lua_setfield(L, -2, "httpc_result");
    lua_pushinteger(L, (lua_Integer)result.lwip_err);
    lua_setfield(L, -2, "lwip_err");
    lua_pushboolean(L, result.hash_checked != 0);
    lua_setfield(L, -2, "hash_checked");
    lua_pushboolean(L, result.hash_verified != 0);
    lua_setfield(L, -2, "hash_verified");
    lua_pushstring(L, result.sha256_hex);
    lua_setfield(L, -2, "sha256");
    lua_pushstring(L, resolved);
    lua_setfield(L, -2, "dest");
    return 1;
}
#endif /* ENABLE_NETWORKING */

/* =============================================================================
 * Hailo-8/8L NPU bindings (slm.hailo.*)
 *
 * Scripts drive the Hailo backend through the same inference_device
 * abstraction the scheduler policy uses. The nested `slm.hailo` table
 * exposes three functions:
 *
 *   slm.hailo.load(path)              Load an HEF from VFS, return handle int
 *                                     (>=0) on success, nil on failure. No
 *                                     name derivation — the handle is the
 *                                     only thing callers need.
 *   slm.hailo.infer(handle, input)    input is a Lua string used as the
 *                                     INT8 input tensor; length must equal
 *                                     the model's input_bytes. Returns the
 *                                     INT8 output as a Lua string, or nil
 *                                     on any error. No partial outputs.
 *   slm.hailo.status()                Returns a table:
 *                                       {available=bool, name=str|nil,
 *                                        slots_in_use=int, slots_max=int}
 *                                     `available` is true iff the
 *                                     "hailo-8" inference_device is
 *                                     registered AND its backend_init
 *                                     succeeded (i.e., PCIe probed +
 *                                     firmware booted on a real Pi 5;
 *                                     false on QEMU/x86).
 *
 * All paths size-check against inference_device_find("hailo-8"). When
 * the device is absent (no HAT+ / QEMU / wrong platform) every call
 * returns nil / a status table with available=false — no crashes,
 * no fallbacks. Fallback to CPU belongs in the caller script, not
 * here.
 * ========================================================================== */

/* Hailo NPU bindings are gated on platforms that compile the backend.
 * CMakeLists.txt skips inference_device_hailo.c on x86-64 (no NPU
 * available there), so referencing hailo_backend_* helpers from this
 * file would break the x86 link. The matching `#endif` closes the
 * block right after slm_hailo_lib. */
#if !defined(PLATFORM_X86_64)

/* Backend-specific helpers exposed by inference_device_hailo.h
 * (consolidated PR #355 review). */
#include "inference_device_hailo.h"

/* Hard upper bound on HEF file size the load path will accept. HEFs
 * for Hailo-8/8L top out at a few MB (MobileNetV1 ≈ 4 MB, larger
 * classifiers ≈ 16 MB); 64 MB is well above any realistic HEF and
 * well below the uint32 + size_t arithmetic danger zone. Guards
 * against (info.size + 4095) wrapping in uint32 — info.size is
 * uint32_t per struct vfs_entry_info. */
#define LUA_HAILO_HEF_MAX_BYTES (64u * 1024u * 1024u)

/* Hard upper bound on a single tensor (input or output). HEFs declare
 * per-pad shapes, the parser multiplies them into cfg.input_bytes /
 * cfg.output_bytes, and slm.hailo.infer sizes its output buffer from
 * that. 16 MB covers every realistic classifier / detector we care
 * about (MobileNetV1 input = 150 KB, segmentation outputs ≈ 4 MB)
 * without letting a malformed HEF produce a uint32_t wrap in
 * (expected_out + 4095) or a giant pmm_alloc_pages request. */
#define LUA_HAILO_TENSOR_MAX_BYTES (16u * 1024u * 1024u)

/* Hailo-8L inference tensors are INT8; matches the dtype hailo_backend_run
 * asserts before submission. */
#ifndef INF_DTYPE_INT8
#define INF_DTYPE_INT8 3
#endif

static struct inference_device *lua_hailo_dev(void)
{
    return inference_device_find("hailo-8");
}

/* slm.hailo.load(path) — see the block comment above for contract. */
static int l_hailo_load(lua_State *L)
{
    if (!L) return 0;
    const char *path = luaL_checkstring(L, 1);

    struct inference_device *dev = lua_hailo_dev();
    if (!dev) { lua_pushnil(L); return 1; }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        lua_pushnil(L); return 1;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0
     || info.type != 0 || info.size == 0
     || info.size > LUA_HAILO_HEF_MAX_BYTES) {
        lua_pushnil(L); return 1;
    }

    /* PMM page-aligned allocation matching slm.model_load's pattern.
     * HEF files on the VFS are a few KB to several MB; this is a
     * transient staging buffer that the backend copies into its own
     * slot-owned tensors, so we free it immediately after load.
     * info.size is already bounded by LUA_HAILO_HEF_MAX_BYTES above
     * so (info.size + 4095) cannot wrap in uint32_t. */
    size_t pages_needed = ((size_t)info.size + 4095) / 4096;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) { lua_pushnil(L); return 1; }

    int bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        pmm_free_pages(buf, pages_needed);
        lua_pushnil(L); return 1;
    }

    int32_t handle = -1;
    int rc = inference_load_model(dev, buf, (size_t)bytes_read, &handle);
    pmm_free_pages(buf, pages_needed);

    if (rc != 0 || handle < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, handle);
    return 1;
}

/* slm.hailo.infer(handle, input_string) — see the block comment above. */
static int l_hailo_infer(lua_State *L)
{
    if (!L) return 0;
    lua_Integer handle = luaL_checkinteger(L, 1);
    size_t in_len = 0;
    const char *in_bytes = luaL_checklstring(L, 2, &in_len);

    struct inference_device *dev = lua_hailo_dev();
    if (!dev || handle < 0 || handle > INT32_MAX) {
        lua_pushnil(L); return 1;
    }

    uint32_t expected_in = 0, expected_out = 0;
    if (hailo_backend_model_sizes((int32_t)handle,
                                  &expected_in, &expected_out) != 0) {
        lua_pushnil(L); return 1;
    }
    if (in_len != (size_t)expected_in) { lua_pushnil(L); return 1; }
    if (expected_in  == 0
     || expected_out == 0
     || expected_in  > LUA_HAILO_TENSOR_MAX_BYTES
     || expected_out > LUA_HAILO_TENSOR_MAX_BYTES) {
        lua_pushnil(L); return 1;
    }

    /* Output tensor buffer. Sized to the model's declared output_bytes;
     * PMM-allocated so we never strain the 16 KB task stack. Freed
     * before return regardless of outcome. expected_out is bounded by
     * LUA_HAILO_TENSOR_MAX_BYTES above, so (expected_out + 4095)
     * cannot wrap in uint32_t. */
    size_t out_pages = ((size_t)expected_out + 4095) / 4096;
    uint8_t *out_buf = (uint8_t *)pmm_alloc_pages(out_pages);
    if (!out_buf) { lua_pushnil(L); return 1; }

    /* shape[0] = 0 when the tensor exceeds uint16 elements. The Hailo
     * backend's run path consults n_elems for rank-1 INT8 tensors,
     * not shape[] — and a 150 KB MobileNet input (224*224*3 = 150528)
     * doesn't fit a uint16_t. Use 0 as an explicit sentinel meaning
     * "consult n_elems" rather than silently saturating at 0xFFFF
     * (which would be a plausible-but-wrong shape). */
    inference_tensor_t in_t = {
        .data    = (void *)in_bytes,   /* inference_run does not mutate in */
        .n_elems = (uint32_t)in_len,
        .dtype   = INF_DTYPE_INT8,
        .rank    = 1,
        .shape   = { (uint16_t)(in_len > 0xFFFFu ? 0u : in_len), 0, 0, 0 },
    };
    inference_tensor_t out_t = {
        .data    = out_buf,
        .n_elems = expected_out,
        .dtype   = INF_DTYPE_INT8,
        .rank    = 1,
        .shape   = { (uint16_t)(expected_out > 0xFFFFu ? 0u : expected_out), 0, 0, 0 },
    };

    int rc = inference_run(dev, (int32_t)handle, &in_t, &out_t);
    if (rc != 0) {
        pmm_free_pages(out_buf, out_pages);
        lua_pushnil(L); return 1;
    }

    lua_pushlstring(L, (const char *)out_buf, expected_out);
    pmm_free_pages(out_buf, out_pages);
    return 1;
}

/* slm.hailo.status() — see the block comment above. */
static int l_hailo_status(lua_State *L)
{
    if (!L) return 0;
    struct inference_device *dev = lua_hailo_dev();

    lua_newtable(L);
    lua_pushboolean(L, dev != NULL);
    lua_setfield(L, -2, "available");

    if (dev) {
        lua_pushstring(L, "hailo-8");
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, (lua_Integer)hailo_backend_in_use_slots());
        lua_setfield(L, -2, "slots_in_use");
        lua_pushinteger(L, (lua_Integer)hailo_backend_slots_max());
        lua_setfield(L, -2, "slots_max");
    }
    return 1;
}

/*
 * slm.hailo.unload(handle) -> bool
 *
 * Releases the NPU slot claimed by a prior slm.hailo.load(). The four-
 * slot cap means long-running scripts that cycle through models must
 * unload before loading the next one. Returns true on success, false
 * on any failure (device absent, out-of-range handle, already-free
 * slot, underlying free_model error). load()/infer()/unload() form the
 * minimum lifecycle surface a Lua app needs.
 */
static int l_hailo_unload(lua_State *L)
{
    if (!L) return 0;
    lua_Integer handle = luaL_checkinteger(L, 1);

    struct inference_device *dev = lua_hailo_dev();
    if (!dev || handle < 0 || handle > INT32_MAX) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int rc = inference_free_model(dev, (int32_t)handle);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static const luaL_Reg slm_hailo_lib[] = {
    {"load",   l_hailo_load},
    {"infer",  l_hailo_infer},
    {"unload", l_hailo_unload},
    {"status", l_hailo_status},
    {NULL, NULL}
};

#endif /* !PLATFORM_X86_64 — Hailo bindings */

/* =============================================================================
 * Camera bindings (slm.camera.*)
 *
 * Surfaces the camera capture API from kernel/include/camera.h. Today only
 * the mock backend is wired in (.rodata frame embedded by CMake when
 * MOCK_CAMERA_FRAME=ON); the IMX219 driver will plug into the same surface
 * once Phase 0+ hardware work lands. See docs/jetson-camera-imx219-plan.md.
 *
 *   slm.camera.open(name)            Returns a handle table {name, capture,
 *                                    close} or nil if the backend is missing
 *                                    or the name is unknown. Method-style:
 *                                        local cam = slm.camera.open("mock")
 *                                        local f, w, h, b = cam:capture()
 *                                        cam:close()
 *
 *   slm.camera.capture(arg)          Procedural form. arg is either the camera
 *                                    name (string) or a handle table whose
 *                                    .name field is read. Returns
 *                                    (frame_id, width, height, bayer) on
 *                                    success, nil on failure. frame_id is a
 *                                    small integer that names the kernel-side
 *                                    buffer for preprocess_mnist (only 0 is
 *                                    valid today — the mock's .rodata frame).
 *
 *   slm.camera.close(arg)            Procedural form. Returns true. The mock
 *                                    backend owns no per-call resources;
 *                                    real backends will release DMA buffers
 *                                    here.
 *
 *   slm.camera.preprocess_mnist(frame_id, width, height, bayer)
 *                                    Decode the frame named by frame_id into
 *                                    3,136 bytes of fp32 in [0, 1] suitable
 *                                    for slm.model_infer_bytes. Returns the
 *                                    bytes string on success; (nil, rc<0) on
 *                                    error. rc values:
 *                                      -1 = bad frame_id (only 0 today)
 *                                      -2 = mock backend not embedded
 *                                      -3 = (w, h, bayer) don't match the
 *                                           backing frame's geometry
 *                                      <0 from camera_preprocess_mnist for
 *                                          unsupported geometry
 *
 * Frame bytes are not exposed to Lua because the 1640x1232 RAW10 frame is
 * 2.5 MB — well above the 1 MB Lua heap. preprocess_mnist therefore reads
 * the bytes through the kernel-side frame_id; future small-mode captures
 * (e.g. 640x480) could grow a string-bytes overload.
 * ========================================================================== */

/* Bound on every recognised backend name. Real names ("mock",
 * "imx219-0", future "imx219-1") are short; cap at 31 chars + NUL. */
#define LUA_CAMERA_NAME_MAX 32u

/* Resolve the first argument of a camera method to a backend name and
 * copy it into out_buf. Accepts a Lua string at idx OR a handle table
 * whose .name field is a string (so cam:capture() works through Lua's
 * a:b() == a.b(a) sugar). Copying — rather than returning a pointer
 * into the Lua VM's string heap — keeps the result valid past any
 * subsequent Lua API call without depending on the caller to leave the
 * source string anchored on the stack. Returns 0 on success, -1 on
 * any failure (wrong type, missing .name, name longer than the cap).
 * out_buf is left zero-initialised on failure. */
static int l_camera_arg_name(lua_State *L, int idx,
                             char *out_buf, size_t out_n) {
    if (!out_buf || out_n == 0u) return -1;
    out_buf[0] = '\0';

    const char *src = NULL;
    size_t src_len  = 0;
    int from_table  = 0;

    if (lua_type(L, idx) == LUA_TSTRING) {
        src = lua_tolstring(L, idx, &src_len);
    } else if (lua_type(L, idx) == LUA_TTABLE) {
        lua_getfield(L, idx, "name");
        if (lua_type(L, -1) == LUA_TSTRING) {
            src = lua_tolstring(L, -1, &src_len);
            from_table = 1;
        } else {
            lua_pop(L, 1);
            return -1;
        }
    } else {
        return -1;
    }

    int rc = 0;
    if (!src || src_len + 1u > out_n) {
        rc = -1;
    } else {
        __builtin_memcpy(out_buf, src, src_len);
        out_buf[src_len] = '\0';
    }
    if (from_table) lua_pop(L, 1);
    return rc;
}

static int l_camera_capture(lua_State *L);
static int l_camera_close(lua_State *L);

static int l_camera_open(lua_State *L) {
    if (!L) return 0;
    const char *name = luaL_checkstring(L, 1);
    struct camera_frame frame;
    if (camera_open(name, &frame) != 0) {
        lua_pushnil(L);
        return 1;
    }
    /* Build the handle table: {name=name, capture=fn, close=fn}.
     * Plain table — no metatable needed. cam:capture() works because
     * Lua passes the table as self into the function stored at .capture. */
    lua_newtable(L);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushcfunction(L, l_camera_capture);
    lua_setfield(L, -2, "capture");
    lua_pushcfunction(L, l_camera_close);
    lua_setfield(L, -2, "close");
    return 1;
}

static int l_camera_capture(lua_State *L) {
    if (!L) return 0;
    char name[LUA_CAMERA_NAME_MAX];
    if (l_camera_arg_name(L, 1, name, sizeof(name)) != 0) {
        lua_pushnil(L);
        return 1;
    }
    struct camera_frame frame;
    if (camera_open(name, &frame) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, 0);                              /* frame_id */
    lua_pushinteger(L, (lua_Integer)frame.width);
    lua_pushinteger(L, (lua_Integer)frame.height);
    lua_pushinteger(L, (lua_Integer)frame.bayer);
    return 4;
}

static int l_camera_close(lua_State *L) {
    if (!L) return 0;
    /* Mock backend has no per-handle state to release. Argument is
     * accepted (string or handle table) but unused — call the resolver
     * for its side effect of validating the input shape. */
    char name[LUA_CAMERA_NAME_MAX];
    (void)l_camera_arg_name(L, 1, name, sizeof(name));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_camera_preprocess_mnist(lua_State *L) {
    if (!L) return 0;
    lua_Integer frame_id = luaL_checkinteger(L, 1);
    lua_Integer width    = luaL_checkinteger(L, 2);
    lua_Integer height   = luaL_checkinteger(L, 3);
    lua_Integer bayer    = luaL_checkinteger(L, 4);

    if (frame_id != 0) {
        lua_pushnil(L);
        lua_pushinteger(L, -1);
        return 2;
    }
    struct camera_frame frame;
    if (camera_open("mock", &frame) != 0) {
        lua_pushnil(L);
        lua_pushinteger(L, -2);
        return 2;
    }
    if ((uint32_t)width  != frame.width
     || (uint32_t)height != frame.height
     || (uint32_t)bayer  != frame.bayer) {
        lua_pushnil(L);
        lua_pushinteger(L, -3);
        return 2;
    }

    /* 3,136 bytes on the kernel-task stack — STACK_SIZE is 64 KB
     * (kernel/include/config.h), so this is ~5% of budget. Stack
     * intentional: a PMM page would have to live across
     * lua_pushlstring, whose OOM path longjmps via LUAI_THROW
     * (kernel/lib/lua/src/ldo.c) and would skip pmm_free_pages,
     * leaking the page. Stack-resident output unwinds for free. */
    uint8_t out[CAMERA_MNIST_OUT_BYTES];
    int rc = camera_preprocess_mnist(frame.data, frame.size,
                                     frame.width, frame.height, frame.bayer,
                                     out, sizeof(out));
    if (rc != 0) {
        lua_pushnil(L);
        lua_pushinteger(L, rc);
        return 2;
    }
    lua_pushlstring(L, (const char *)out, sizeof(out));
    return 1;
}

static const luaL_Reg slm_camera_lib[] = {
    {"open",             l_camera_open},
    {"capture",          l_camera_capture},
    {"close",            l_camera_close},
    {"preprocess_mnist", l_camera_preprocess_mnist},
    {NULL, NULL}
};

/* SLM library functions */
static const luaL_Reg slm_lib_safe[] = {
    {"print", l_print},
    {"uptime", l_uptime},
    {"uptime_us", l_uptime_us},
    {"mem_stats", l_mem_stats},
    {"tasks", l_tasks},
    {"sleep", l_sleep},
    {"yield", l_yield},
    {"version", l_version},
    {"cpu_count", l_cpu_count},
    {"cpu_id", l_cpu_id},
    /* Component management */
    {"component_count", l_component_count},
    {"component_list", l_component_list},
    {"component_find", l_component_find},
    /* Model memory and inference */
    {"model_stats", l_model_stats},
    {"model_find", l_model_find},
    /* Message routing */
    {"msg_publish", l_msg_publish},
    {"msg_publish_priority", l_msg_publish_priority},
    {"msg_subscribe", l_msg_subscribe},
    {"msg_unsubscribe", l_msg_unsubscribe},
    {"msg_drain", l_msg_drain},
    /* Scheduler */
    {"sched_policy", l_sched_policy},
    {"sched_stats", l_sched_stats},
    {"sched_policy_list", l_sched_policy_list},
    {"ai_sched_stats", l_ai_sched_stats},
    {"ai_sched_decision", l_ai_sched_decision},
    /* Admin & telemetry suite (M1) */
    {"sched_decision_rate", l_sched_decision_rate},
    {"latency_histogram", l_latency_histogram},
    /* Admin & telemetry suite (M2) — read-only */
    {"gpu_use_get", l_gpu_use_get},
    {"gpu_use_status", l_gpu_use_status},
    /* Admin & telemetry suite (M3) — read-only */
    {"eviction_decision_rate", l_eviction_decision_rate},
    {"inference_rate", l_inference_rate},
    /* CPU info */
    {"cpu_info", l_cpu_info},
    {"term_size", l_term_size},
    /* Memory / VMM / IPC */
    {"vmm_stats", l_vmm_stats},
    {"ipc_stats", l_ipc_stats},
    /* Eviction */
    {"eviction_policy", l_eviction_policy},
    {"eviction_stats", l_eviction_stats},
    /* Extended model bindings */
    {"model_list", l_model_list},
    {"model_info", l_model_info},
    {"infer_stats", l_infer_stats},
    {"gpu_status", l_gpu_status},
    /* Shell integration */
    {"read_line", l_read_line},
    {"try_getc", l_try_getc},
#if defined(ENABLE_NETWORKING)
    /* TCP shell daemon (Phase 3) */
    {"telnetd_status",   l_telnetd_status},
    {"telnetd_sessions", l_telnetd_sessions},
#endif
    {NULL, NULL}
};

static const luaL_Reg slm_lib_admin[] = {
    /* Component management */
    {"component_run", l_component_run},
    {"component_hot_swap", l_component_hot_swap},
    {"component_hot_swap_stateful", l_component_hot_swap_stateful},
    /* Model memory and inference */
    {"model_infer", l_model_infer},
    {"model_infer_bytes", l_model_infer_bytes},
    {"model_infer_file", l_model_infer_file},
    {"model_load_mnist", l_model_load_mnist},
    {"model_pin", l_model_pin},
    {"model_preload", l_model_preload},
    {"model_preload_wait", l_model_preload_wait},
    {"model_unpin", l_model_unpin},
    {"model_bench", l_model_bench},
    {"model_load", l_model_load},
    /* GPU inference (M7 + M9) */
    {"gpu_run_mnist", l_gpu_run_mnist},
    {"gpu_set_mnist_input", l_gpu_set_mnist_input},
    {"gpu_set_mnist_input_fill", l_gpu_set_mnist_input_fill},
    /* Admin & telemetry suite (M2) — mutates global state */
    {"gpu_use_set", l_gpu_use_set},
    /* Scheduler / task mutation */
    {"sched_set_policy", l_sched_set_policy},
    {"task_migrate", l_task_migrate},
    {"task_create", l_task_create},
    {"task_kill", l_task_kill},
    {"task_set_priority", l_task_set_priority},
    {"task_pin", l_task_pin},
    /* Eviction mutation */
    {"eviction_set_policy", l_eviction_set_policy},
    {"eviction_model_status", l_eviction_model_status},
    {"eviction_model_load", l_eviction_model_load},
    {"eviction_model_activate", l_eviction_model_activate},
    {"eviction_model_rollback", l_eviction_model_rollback},
    {"eviction_model_clear", l_eviction_model_clear},
    /* Scheduler runtime model mutation */
    {"sched_model_status", l_sched_model_status},
    {"sched_model_load", l_sched_model_load},
    {"sched_model_activate", l_sched_model_activate},
    {"sched_model_rollback", l_sched_model_rollback},
    {"sched_model_clear", l_sched_model_clear},
    /* Shell integration */
    {"shell_exec", l_shell_exec},
#if defined(ENABLE_NETWORKING)
    /* TCP shell daemon control */
    {"telnetd_start",    l_telnetd_start},
    {"telnetd_stop",     l_telnetd_stop},
    {"telnetd_kick",     l_telnetd_kick},
    {"http_get",         l_http_get},
#endif
    {NULL, NULL}
};

/**
 * Open the SLM library
 */
static void lua_push_slm_library(lua_State *L, bool admin)
{
    luaL_newlib(L, slm_lib_safe);

    /* String constants from build_info.h. slm.VERSION stays string-shaped
     * for legacy scripts; BUILD_STAMP and BUILD_SHA are new (#360). */
    lua_pushstring(L, SLMOS_VERSION);
    lua_setfield(L, -2, "VERSION");
    lua_pushstring(L, SLMOS_BUILD_STAMP);
    lua_setfield(L, -2, "BUILD_STAMP");
    lua_pushstring(L, SLMOS_BUILD_SHA);
    lua_setfield(L, -2, "BUILD_SHA");

    /* slm.camera — read-only backend (mock-only today); fine on the safe
     * surface. Real hardware backends with side effects can move to admin
     * when they land. */
    luaL_newlib(L, slm_camera_lib);
    lua_setfield(L, -2, "camera");

    if (admin) {
        for (const luaL_Reg *r = slm_lib_admin; r->name; r++) {
            lua_pushcfunction(L, r->func);
            lua_setfield(L, -2, r->name);
        }
#if !defined(PLATFORM_X86_64)
        /* Keep the Hailo bindings on the admin surface until the
         * device/backend concurrency contract is explicitly audited.
         * x86-64 builds skip the Hailo backend entirely. */
        luaL_newlib(L, slm_hailo_lib);
        lua_setfield(L, -2, "hailo");
#endif
    }
}

/* ============================================================================
 * Lua State Management
 * ============================================================================ */

static int lua_initialized = 0;

void lua_slm_init(void) {
    if (lua_initialized) return;
    lua_initialized = 1;
    shell_printf("Lua 5.4 scripting initialized\n");
}

static lua_State *lua_slm_newstate_mode(bool admin) {
    if (!lua_initialized) {
        lua_slm_init();
    }

    /* Create Lua state with default allocator (uses our malloc) */
    lua_State *L = luaL_newstate();
    if (L == NULL) {
        shell_printf("Failed to create Lua state\n");
        return NULL;
    }
    struct lua_state_slot *slot = lua_state_slot_alloc(L);
    if (!slot) {
        shell_printf("Failed to allocate Lua state slot\n");
        lua_close(L);
        return NULL;
    }
    *(struct lua_state_slot **)lua_getextraspace(L) = slot;
    lua_state_count_inc();

    /* Open safe standard libraries */
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);

    /* Open SLM library. `slm` defaults to the concurrent-safe surface;
     * admin states explicitly opt into the global mutator bindings. */
    lua_push_slm_library(L, admin);
    lua_setglobal(L, "slm");

    /* Replace print with our version */
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    return L;
}

lua_State *lua_slm_newstate(void) {
    return lua_slm_newstate_mode(false);
}

lua_State *lua_slm_newstate_admin(void) {
    return lua_slm_newstate_mode(true);
}

void lua_slm_close(lua_State *L) {
    if (L) {
        struct lua_state_slot *slot = lua_state_slot_from_state(L);
        /* Release any Lua msg_subscribe refs owned by this state (#207)
         * before the state is closed — luaL_unref needs a live state. */
        lua_msg_subs_cleanup(L);
        *(struct lua_state_slot **)lua_getextraspace(L) = NULL;
        lua_state_slot_free(slot);
        lua_close(L);
        /* Only reset the shared Lua heap when the last state is gone.
         * With Lua-defined tasks (#208), multiple states can be live at
         * once and a reset would wipe allocations belonging to survivors. */
        if (lua_state_count_dec_and_test_zero()) {
            extern void heap_reset(void);
            heap_reset();
        }
    }
}

int lua_slm_dostring(lua_State *L, const char *script) {
    if (!L) return -1;
    int saved_top = lua_gettop(L);
    int status = luaL_dostring(L, script);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        shell_printf("Lua error: %s\n", msg ? msg : "(unknown)");
    }
    /* Restore stack to caller's view regardless of outcome: luaL_dostring
     * leaves either the results of the chunk or the error message, and
     * callers of this wrapper don't read them. */
    lua_settop(L, saved_top);
    return status;
}

int lua_slm_dofile(lua_State *L, const char *filename) {
    if (!L || !filename) return -1;
    int saved_top = lua_gettop(L);
    char resolved[VFS_MAX_PATH];

    if (shell_resolve_path(filename, resolved, sizeof(resolved)) < 0) {
        shell_printf("lua: path too long: %s\n", filename);
        return -1;
    }

    /* Sized to fit the embedded demo scripts with headroom. Lives on the
     * shell task's 64 KB stack, so keep it bounded but comfortably above
     * the current demo payload sizes. */
    char buf[32768];
    int len = vfs_read_path(resolved, buf, sizeof(buf) - 1, 0);
    if (len < 0) {
        shell_printf("lua: cannot open %s\n", resolved);
        return -1;
    }
    if (len >= (int)sizeof(buf) - 1) {
        shell_printf("lua: %s exceeds %d bytes\n", resolved, (int)sizeof(buf) - 1);
        return -1;
    }
    buf[len] = '\0';

    int status = luaL_loadbufferx(L, buf, (size_t)len, resolved, NULL);
    if (status == LUA_OK)
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        shell_printf("Lua error: %s\n", msg ? msg : "(unknown)");
    }
    lua_settop(L, saved_top);
    return status;
}

const char *lua_slm_geterror(lua_State *L) {
    if (lua_gettop(L) > 0 && lua_isstring(L, -1)) {
        return lua_tostring(L, -1);
    }
    return NULL;
}

/* ============================================================================
 * REPL (Read-Eval-Print Loop)
 * ============================================================================ */

#define REPL_BUFFER_SIZE 256

void lua_slm_repl(lua_State *L) {
    char buffer[REPL_BUFFER_SIZE];
    int pos = 0;

    shell_printf("Lua 5.4 REPL - type 'exit' to quit\n");
    shell_printf(">>> ");

    while (1) {
        bool paused = shell_mutation_pause();
        int c = shell_getc();
        shell_mutation_resume(paused);
        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            shell_printf("\n");
            buffer[pos] = '\0';

            /* Check for exit command */
            if (pos == 4 && buffer[0] == 'e' && buffer[1] == 'x' &&
                buffer[2] == 'i' && buffer[3] == 't') {
                shell_printf("Exiting Lua REPL\n");
                break;
            }

            /* Check for Ctrl+D (EOF) */
            if (pos == 0) {
                /* Empty line - just show prompt again */
                shell_printf(">>> ");
                continue;
            }

            /* Execute the line */
            lua_slm_dostring(L, buffer);

            /* Reset for next line */
            pos = 0;
            shell_printf(">>> ");
        } else if (c == 0x03) {
            /* Ctrl+C - cancel current line */
            shell_printf("^C\n>>> ");
            pos = 0;
        } else if (c == 0x04) {
            /* Ctrl+D - exit */
            shell_printf("^D\nExiting Lua REPL\n");
            break;
        } else if (c == 0x7f || c == 0x08) {
            /* Backspace */
            if (pos > 0) {
                pos--;
                shell_printf("\b \b");
            }
        } else if (c >= 32 && c < 127 && pos < REPL_BUFFER_SIZE - 1) {
            /* Printable character */
            buffer[pos++] = (char)c;
            shell_putc((char)c);
        }
    }
}
