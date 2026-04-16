/*
 * demo_init.c - Write embedded demo scripts to filesystem at boot
 *
 * Writes /mnt/files/demo.lua and /mnt/files/demo_menu.lua at boot so
 * they can be run from the shell via `lua /mnt/files/demo.lua` or
 * `lua /mnt/files/demo_menu.lua`.
 *
 * Content is embedded in demo_scripts.S via .incbin from the source
 * files under scripts/; the demo_lua_* / demo_menu_lua_* symbols are
 * provided by that assembly file (#13). Embedding is gated on the
 * EMBED_DEMO_SCRIPTS CMake option (#14) — when off, demo_init() is a
 * no-op and no demo content is carried in the kernel image.
 */

#include "vfs.h"
#include "littlefs_slm.h"
#include "uart.h"
#include <stddef.h>

#if defined(EMBED_DEMO_SCRIPTS)

/*
 * Provided by demo_scripts.S via .incbin on scripts/demo.lua and
 * scripts/demo_menu.lua. Length of each blob is (_end - _start).
 */
extern const unsigned char demo_lua_start[];
extern const unsigned char demo_lua_end[];
extern const unsigned char demo_menu_lua_start[];
extern const unsigned char demo_menu_lua_end[];

static const char demo_auto_script[] =
    "-- SLM-OS Scripted Auto-Demo (#192)\n"
    "local P = slm.print\n"
    "local run = slm.shell_exec\n"
    "local function banner(title, n, total)\n"
    "    P(\"\")\n"
    "    P(\"============================================================\")\n"
    "    P(string.format(\"  [%d/%d] %s\", n, total, title))\n"
    "    P(\"============================================================\")\n"
    "end\n"
    "local function pause()\n"
    "    P(\"\"); P(\"  <press Enter to continue, or type 'skip' to jump ahead>\")\n"
    "    local line = slm.read_line()\n"
    "    if line == \"skip\" then return \"skip\" end\n"
    "    return \"continue\"\n"
    "end\n"
    "P(\"\")\n"
    "P(\"######################################################\")\n"
    "P(\"#             SLM-OS Capstone Live Demo              #\")\n"
    "P(\"######################################################\")\n"
    "P(\"\")\n"
    "P(\"Five features will be demonstrated in sequence:\")\n"
    "P(\"  1. Symmetric Multiprocessing\")\n"
    "P(\"  2. Preemptive Multitasking + AI Scheduling\")\n"
    "P(\"  3. AI-Driven Page Eviction\")\n"
    "P(\"  4. Model Inference\")\n"
    "P(\"  5. Live System Dashboard\")\n"
    "P(\"\")\n"
    "P(\"Version:    \" .. slm.version())\n"
    "P(\"CPUs:       \" .. slm.cpu_count() .. \" cores\")\n"
    "P(\"Scheduler:  \" .. slm.sched_policy())\n"
    "P(\"\")\n"
    "if pause() == \"skip\" then P(\"Demo aborted.\"); return end\n"
    "banner(\"Symmetric Multiprocessing\", 1, 5)\n"
    "P(\"  Launching bench smp.\"); P(\"\")\n"
    "run(\"bench smp\")\n"
    "P(\"\"); P(\"  All \" .. slm.cpu_count() .. \" cores executed the workload.\")\n"
    "if pause() == \"skip\" then return end\n"
    "banner(\"Preemptive Multitasking + AI Scheduling\", 2, 5)\n"
    "P(\"  Comparing policies side-by-side.\"); P(\"\")\n"
    "run(\"sched compare\")\n"
    "P(\"\"); P(\"  Table shows context-switch count and avg latency per policy.\")\n"
    "if pause() == \"skip\" then return end\n"
    "banner(\"AI-Driven Page Eviction\", 3, 5)\n"
    "P(\"  Current eviction policy:\")\n"
    "run(\"eviction\")\n"
    "P(\"\"); P(\"  Driving the weight pool to saturation:\"); P(\"\")\n"
    "run(\"eviction demo\")\n"
    "P(\"\"); P(\"  Each EVICT row is a live policy decision.\")\n"
    "if pause() == \"skip\" then return end\n"
    "banner(\"Model Inference\", 4, 5)\n"
    "P(\"  Loading built-in MNIST model...\")\n"
    "local mnist = slm.model_load_mnist()\n"
    "if mnist < 0 then\n"
    "    P(\"  (MNIST not available — skipping)\")\n"
    "else\n"
    "    P(string.format(\"  MNIST loaded (id=%d). Running 10 inferences:\", mnist))\n"
    "    local t0 = slm.uptime()\n"
    "    local pred = {}\n"
    "    for i = 1, 10 do pred[i] = slm.model_infer(mnist) end\n"
    "    local elapsed = slm.uptime() - t0\n"
    "    P(string.format(\"  Done: %d ms total, %d ms/inference avg.\",\n"
    "        elapsed, elapsed // 10))\n"
    "    P(\"  Predictions: \" .. table.concat(pred, \" \"))\n"
    "end\n"
    "if pause() == \"skip\" then return end\n"
    "banner(\"Live System Dashboard\", 5, 5)\n"
    "P(\"  Rendering 3 frames of `top`...\"); P(\"\")\n"
    "run(\"top -n 3 1\")\n"
    "P(\"\"); P(\"  Real-time CPU/task/memory/eviction view.\")\n"
    "P(\"\")\n"
    "P(\"============================================================\")\n"
    "P(\"   Demo complete. Thank you!\")\n"
    "P(\"============================================================\")\n"
    "P(\"\")\n"
    ;

int demo_init(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    if (!mnt) {
        uart_puts("[WARN] demo_init: /mnt/files not mounted\r\n");
        return -1;
    }

    int f = littlefs_file_open(mnt, "/demo.lua",
                               LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (f < 0) {
        uart_puts("[WARN] demo_init: failed to create /mnt/files/demo.lua\r\n");
        return -1;
    }
    littlefs_file_write(mnt, f, demo_lua_start,
                        (size_t)(demo_lua_end - demo_lua_start));
    littlefs_file_close(mnt, f);

    int fm = littlefs_file_open(mnt, "/demo_menu.lua",
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fm < 0) {
        uart_puts("[WARN] demo_init: failed to create /mnt/files/demo_menu.lua\r\n");
        /* demo.lua succeeded, so don't fail the whole init. */
        return 0;
    }
    littlefs_file_write(mnt, fm, demo_menu_lua_start,
                        (size_t)(demo_menu_lua_end - demo_menu_lua_start));
    littlefs_file_close(mnt, fm);

    /* #192: scripted auto-demo sequencer. */
    int fa = littlefs_file_open(mnt, "/demo_auto.lua",
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fa >= 0) {
        littlefs_file_write(mnt, fa, demo_auto_script,
                            sizeof(demo_auto_script) - 1);
        littlefs_file_close(mnt, fa);
    }

    return 0;
}

#else  /* !EMBED_DEMO_SCRIPTS */

/*
 * Demo scripts omitted from this build. The Lua interpreter and the
 * slm.* bindings remain fully functional; users can `lua <inline>` or
 * load scripts written to the filesystem at runtime.
 */
int demo_init(void)
{
    return 0;
}

#endif /* EMBED_DEMO_SCRIPTS */
