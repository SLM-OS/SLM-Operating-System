-- SLM-OS Industrial IoT Demo
--
-- Demonstrates the AI-first operating system in an industrial monitoring
-- scenario. The demo exercises all major subsystems:
--
--   1. System overview (memory, CPUs, scheduler)
--   2. Component lifecycle (start, monitor, hot-swap)
--   3. Message-driven data pipeline (publish/subscribe)
--   4. Anomaly detection with threshold alerting
--   5. Live hot-swap of monitoring component
--
-- Usage: lua scripts/industrial_demo.lua
-- Platform: Runs on QEMU, Pi 5, Jetson (adapts to available hardware)

local P = slm.print
local sleep = slm.sleep
local yield = slm.yield

-- Pause between demo steps for readability
local function pause(ms)
    ms = ms or 1500
    sleep(ms)
end

-- Print a section header
local function header(title)
    P("")
    P("------------------------------------------------------------")
    P("  " .. title)
    P("------------------------------------------------------------")
end

-- Print a step marker
local function step(n, desc)
    P("")
    P(string.format("  [Step %d] %s", n, desc))
    P("")
end

-- ================================================================
-- Demo Start
-- ================================================================

P("")
P("============================================================")
P("  SLM-OS Industrial IoT Demo")
P("  Small Language Model Operating System")
P("============================================================")
P("")
P("  This demo simulates an industrial monitoring scenario")
P("  where sensor data flows through AI-driven components")
P("  with live hot-swap capability.")
P("")
pause(2000)

-- ================================================================
-- Step 1: System Overview
-- ================================================================

header("System Overview")
step(1, "Inspecting system state")

P("  OS Version:  " .. slm.version())
P("  CPUs:        " .. slm.cpu_count() .. " cores online")
P("  Current CPU: " .. slm.cpu_id())
P("  Scheduler:   " .. slm.sched_policy() .. " policy")

local mem = slm.mem_stats()
P(string.format("  RAM:         %d KB free / %d KB total (%d%%)",
    mem.free_kb, mem.total_kb,
    math.floor((mem.total_kb - mem.free_kb) * 100 / mem.total_kb)))

local model = slm.model_stats()
P(string.format("  Weight Pool: %d / %d blocks allocated",
    model.weights.allocated_blocks, model.weights.total_blocks))
P(string.format("  Work Pool:   %d / %d blocks allocated",
    model.workspace.allocated_blocks, model.workspace.total_blocks))

P(string.format("  Uptime:      %d ms", slm.uptime()))

pause()

-- ================================================================
-- Step 2: Start Sensor Monitor
-- ================================================================

header("Sensor Monitoring Pipeline")
step(2, "Starting sensor_monitor component")

P("  The sensor_monitor watches /sensors/data for threshold violations.")
P("  Values above 50 trigger alerts on /alerts/threshold.")
P("")

local idx = slm.component_run("sensor_monitor")
if idx >= 0 then
    P("  sensor_monitor started (component index " .. idx .. ")")
else
    P("  ERROR: Failed to start sensor_monitor")
end

-- Let the component initialize and subscribe
yield()
yield()

P("  Active components: " .. slm.component_count())
pause()

-- ================================================================
-- Step 3: Simulate Sensor Data
-- ================================================================

step(3, "Simulating sensor data stream")

P("  Publishing sensor readings to /sensors/data:")
P("")

-- Normal readings (below threshold)
local normal_values = {23, 31, 18, 42, 29}
for _, v in ipairs(normal_values) do
    local delivered = slm.msg_publish("/sensors/data", tostring(v))
    P(string.format("    Sensor reading: %d  (delivered to %d subscriber%s)",
        v, delivered, delivered == 1 and "" or "s"))
    yield()  -- Let sensor_monitor process the message
    yield()
    sleep(300)
end

pause(500)

-- Anomaly readings (above threshold)
P("")
P("  Injecting anomalous readings (values > 50):")
P("")

local anomaly_values = {78, 95, 62}
for _, v in ipairs(anomaly_values) do
    local delivered = slm.msg_publish("/sensors/data", tostring(v))
    P(string.format("    ANOMALY: %d  (delivered to %d subscriber%s)",
        v, delivered, delivered == 1 and "" or "s"))
    yield()
    yield()
    sleep(300)
end

pause()

-- ================================================================
-- Step 4: Hot-Swap Demonstration
-- ================================================================

header("Live Component Hot-Swap")
step(4, "Hot-swapping sensor_monitor with new instance")

P("  Performing zero-downtime swap of the monitoring component.")
P("  Subscriptions are automatically transferred to the new instance.")
P("")

local new_idx = slm.component_hot_swap("sensor_monitor", "sensor_monitor")
if new_idx >= 0 then
    P("  Hot-swap successful (new index " .. new_idx .. ")")
else
    P("  Hot-swap failed (code " .. new_idx .. ")")
end

yield()
yield()

-- Send data through the new instance to prove continuity
P("")
P("  Verifying new instance receives data:")
local delivered = slm.msg_publish("/sensors/data", "88")
P(string.format("    Sensor reading: 88  (delivered to %d subscriber%s)",
    delivered, delivered == 1 and "" or "s"))
yield()
yield()

pause()

-- ================================================================
-- Step 5: System Status Report
-- ================================================================

header("Final System Status")
step(5, "Generating status report")

-- Task summary
local tasks = slm.tasks()
local running = 0
local terminated = 0
for _, t in ipairs(tasks) do
    if t.state == "RUNNING" or t.state == "READY" then
        running = running + 1
    elseif t.state == "TERMINATED" then
        terminated = terminated + 1
    end
end
P(string.format("  Tasks:       %d active, %d completed", running, terminated))

-- Component summary
P("  Components:  " .. slm.component_count() .. " registered")

-- Memory after demo
local mem2 = slm.mem_stats()
P(string.format("  Memory:      %d KB used during demo",
    mem.free_kb - mem2.free_kb))

-- Uptime
P(string.format("  Demo time:   %d ms", slm.uptime() - mem.total_kb + mem.total_kb))

P("")
P("============================================================")
P("  Demo Complete")
P("")
P("  SLM-OS demonstrated:")
P("    - Multi-core scheduling (" .. slm.cpu_count() .. " CPUs)")
P("    - Component lifecycle management")
P("    - Publish/subscribe message routing")
P("    - Threshold-based anomaly detection")
P("    - Zero-downtime component hot-swap")
P("    - AI inference pipeline (model memory allocated)")
P("============================================================")
P("")
