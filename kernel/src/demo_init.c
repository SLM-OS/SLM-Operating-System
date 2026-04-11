/*
 * demo_init.c - Write embedded demo script to filesystem at boot
 *
 * Writes /mnt/files/demo.lua which can be run via `lua demo.lua`
 * from the shell. The script exercises the industrial IoT demo scenario.
 */

#include "vfs.h"
#include "littlefs_slm.h"
#include "uart.h"

static const char demo_script[] =
    "-- SLM-OS Industrial IoT Demo\n"
    "local P = slm.print\n"
    "local sleep = slm.sleep\n"
    "local yield = slm.yield\n"
    "\n"
    "P('')\n"
    "P('============================================================')\n"
    "P('  SLM-OS Industrial IoT Demo')\n"
    "P('  Small Language Model Operating System')\n"
    "P('============================================================')\n"
    "P('')\n"
    "sleep(1000)\n"
    "\n"
    "-- System overview\n"
    "P('--- System Overview ---')\n"
    "P('  Version:   ' .. slm.version())\n"
    "P('  CPUs:      ' .. slm.cpu_count() .. ' cores')\n"
    "P('  Scheduler: ' .. slm.sched_policy())\n"
    "local mem = slm.mem_stats()\n"
    "P(string.format('  Memory:    %d KB free / %d KB total', mem.free_kb, mem.total_kb))\n"
    "P('')\n"
    "sleep(1000)\n"
    "\n"
    "-- Start sensor monitor\n"
    "P('--- Starting Sensor Monitor ---')\n"
    "local idx = slm.component_run('sensor_monitor')\n"
    "if idx >= 0 then\n"
    "    P('  sensor_monitor started (idx=' .. idx .. ')')\n"
    "else\n"
    "    P('  ERROR: Failed to start sensor_monitor')\n"
    "end\n"
    "yield(); yield()\n"
    "P('  Components active: ' .. slm.component_count())\n"
    "P('')\n"
    "sleep(1000)\n"
    "\n"
    "-- Simulate normal sensor data\n"
    "P('--- Sensor Data Stream ---')\n"
    "P('  Publishing normal readings (below threshold 50):')\n"
    "for _, v in ipairs({23, 31, 42}) do\n"
    "    local n = slm.msg_publish('/sensors/data', tostring(v))\n"
    "    P(string.format('    Reading: %d  -> %d subscriber(s)', v, n))\n"
    "    yield(); yield(); sleep(300)\n"
    "end\n"
    "P('')\n"
    "\n"
    "-- Simulate anomalies\n"
    "P('  Injecting anomalies (above threshold 50):')\n"
    "for _, v in ipairs({78, 95}) do\n"
    "    local n = slm.msg_publish('/sensors/data', tostring(v))\n"
    "    P(string.format('    ANOMALY: %d  -> %d subscriber(s)', v, n))\n"
    "    yield(); yield(); sleep(300)\n"
    "end\n"
    "P('')\n"
    "sleep(1000)\n"
    "\n"
    "-- Hot-swap\n"
    "P('--- Live Component Hot-Swap ---')\n"
    "P('  Swapping sensor_monitor with new instance...')\n"
    "local new_idx = slm.component_hot_swap('sensor_monitor', 'sensor_monitor')\n"
    "if new_idx >= 0 then\n"
    "    P('  Hot-swap OK (new idx=' .. new_idx .. ')')\n"
    "else\n"
    "    P('  Hot-swap failed')\n"
    "end\n"
    "yield(); yield()\n"
    "\n"
    "-- Verify new instance works\n"
    "P('  Verifying new instance:')\n"
    "local n = slm.msg_publish('/sensors/data', '88')\n"
    "P(string.format('    Reading: 88  -> %d subscriber(s)', n))\n"
    "yield(); yield()\n"
    "P('')\n"
    "sleep(1000)\n"
    "\n"
    "-- Summary\n"
    "P('============================================================')\n"
    "P('  Demo Complete')\n"
    "P('')\n"
    "P('  Demonstrated:')\n"
    "P('    - Component lifecycle (start, hot-swap)')\n"
    "P('    - Publish/subscribe message routing')\n"
    "P('    - Threshold-based anomaly detection')\n"
    "P('    - Zero-downtime component replacement')\n"
    "P('    - ' .. slm.cpu_count() .. '-core scheduling')\n"
    "P('============================================================')\n"
    "P('')\n";

int demo_init(void)
{
    const char *subpath = NULL;
    struct lfs_mount *mnt = (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
    if (!mnt) {
        return -1;
    }

    int f = littlefs_file_open(mnt, "/demo.lua", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (f < 0) {
        return -1;
    }

    /* sizeof includes the null terminator, subtract 1 for file content */
    littlefs_file_write(mnt, f, demo_script, sizeof(demo_script) - 1);
    littlefs_file_close(mnt, f);

    return 0;
}
