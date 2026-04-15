-- SLM-OS Multi-Process Demo
--
-- Demonstrates multiple concurrent processes (tasks) running across
-- the Pi 5's four cores. The demo runs linearly with a short pause
-- between sections so each step's output is readable. Lua does not
-- yet expose a stdin-read binding, so this script auto-advances
-- rather than waiting for ENTER (tracked in GitHub issues).
--
-- Usage:  lua /scripts/multiproc_demo.lua
-- Works on any platform; tested on Pi 5 with COOP_PREEMPT.

local P = slm.print
local sleep = slm.sleep
local yield = slm.yield

local PAUSE_MS = 2000  -- pause between sections so output is readable

local function header(title)
    P("")
    P("============================================================")
    P("  " .. title)
    P("============================================================")
end

local function step(n, desc)
    P("")
    P(string.format("  [%d] %s", n, desc))
    P("")
end

local function dump_tasks(label)
    P("  " .. label)
    P(string.format("    %-4s %-20s %-10s %-4s %-4s",
        "ID", "NAME", "STATE", "CPU", "PRI"))
    P("    -------------------------------------------------")
    local tasks = slm.tasks()
    for _, t in ipairs(tasks) do
        P(string.format("    %-4d %-20s %-10s %-4d %-4d",
            t.id, t.name, t.state, t.cpu, t.priority))
    end
end

-- ============================================================
-- Demo Start
-- ============================================================

P("")
P("############################################################")
P("#  SLM-OS Multi-Process Demo")
P("#  Multiple concurrent tasks across all CPUs")
P("############################################################")

sleep(1500)

-- ============================================================
-- Section 1: System overview
-- ============================================================

header("System Overview")

P("  Version:    " .. slm.version())
P("  CPUs:       " .. slm.cpu_count() .. " cores")
P("  This CPU:   " .. slm.cpu_id())
P("  Scheduler:  " .. slm.sched_policy())

local mem = slm.mem_stats()
P(string.format("  RAM:        %d KB free / %d KB total",
    mem.free_kb, mem.total_kb))
P(string.format("  Uptime:     %d ms", slm.uptime()))

sleep(PAUSE_MS)

-- ============================================================
-- Section 2: Baseline task snapshot
-- ============================================================

header("Baseline Tasks (before spawning)")
step(1, "Snapshot of tasks currently running")

dump_tasks("Active tasks:")

sleep(PAUSE_MS)

-- ============================================================
-- Section 3: Spawn three concurrent components
-- ============================================================

header("Spawning Concurrent Components")
step(2, "Starting listener, sensor_monitor, and counter")

P("  Each component runs as its own task and the scheduler")
P("  distributes them across the available CPUs.")
P("")

local spawned = {"listener", "sensor_monitor", "counter"}
for _, name in ipairs(spawned) do
    local idx = slm.component_run(name)
    if idx and idx >= 0 then
        P(string.format("    spawned %-16s (component index %d)", name, idx))
    else
        P(string.format("    FAILED to spawn %s", name))
    end
    yield()
end

-- Give the new tasks a moment to wake up and bind subscriptions
sleep(500)

sleep(PAUSE_MS)

-- ============================================================
-- Section 4: Task table with concurrent components
-- ============================================================

header("Concurrent Task Snapshot")
step(3, "Task table now shows the three new components")

dump_tasks("Active tasks (note CPU column distribution):")
P("")
P("  Active components: " .. slm.component_count())

sleep(PAUSE_MS)

-- ============================================================
-- Section 5: Drive IPC across the running components
-- ============================================================

header("Inter-Process Communication")
step(4, "Publishing messages to running components")

P("  listener subscribes to 'events'")
P("  sensor_monitor subscribes to '/sensors/data'")
P("")

P("  Publishing to 'events':")
for i = 1, 3 do
    local n = slm.msg_publish("events", "ping-" .. i)
    P(string.format("    events <- ping-%d   (delivered to %d)", i, n))
    yield()
    sleep(200)
end

P("")
P("  Publishing sensor readings (>50 trips threshold):")
local readings = {28, 41, 73, 88, 19}
for _, v in ipairs(readings) do
    local n = slm.msg_publish("/sensors/data", tostring(v))
    local tag = v > 50 and " ANOMALY" or ""
    P(string.format("    /sensors/data <- %-3d (delivered to %d)%s", v, n, tag))
    yield()
    sleep(200)
end

sleep(PAUSE_MS)

-- ============================================================
-- Section 6: Let counter finish, then re-snapshot
-- ============================================================

header("Letting Counter Run to Completion")
step(5, "counter ticks 10 times at 500ms — waiting ~6s")

sleep(6000)

dump_tasks("Tasks after counter completes:")

sleep(PAUSE_MS)

-- ============================================================
-- Section 7: Summary
-- ============================================================

header("Demo Summary")

local final_tasks = slm.tasks()
local running, ready, blocked = 0, 0, 0
for _, t in ipairs(final_tasks) do
    if t.state == "running" then running = running + 1
    elseif t.state == "ready" then ready = ready + 1
    elseif t.state == "blocked" then blocked = blocked + 1
    end
end

P("")
P(string.format("  Tasks:       %d running, %d ready, %d blocked",
    running, ready, blocked))
P("  Components:  " .. slm.component_count() .. " registered")

local mem2 = slm.mem_stats()
P(string.format("  RAM used:    %d KB during demo",
    mem.free_kb - mem2.free_kb))
P(string.format("  Uptime:     %d ms", slm.uptime()))

P("")
P("  Demonstrated:")
P("    - Multiple concurrent tasks scheduled across "
    .. slm.cpu_count() .. " CPUs")
P("    - Component lifecycle (spawn, run, terminate)")
P("    - Publish/subscribe IPC delivering to multiple subscribers")
P("    - Live task-table inspection via slm.tasks()")
P("")
P("############################################################")
P("#  Demo complete")
P("############################################################")
P("")
