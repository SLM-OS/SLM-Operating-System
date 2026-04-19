-- SLM-OS Industrial IoT Demo (linear walkthrough)
--
-- Simulates an industrial monitoring scenario with sensor data flowing
-- through a component-based pipeline. Exercises the major runtime
-- subsystems in one scripted sequence:
--
--   1. System overview (memory, CPUs, scheduler)
--   2. Component lifecycle (start, monitor, hot-swap)
--   3. Publish/subscribe message routing
--   4. Threshold-based anomaly detection
--   5. Live hot-swap of the monitoring component
--   6. AI model inference (MNIST)
--
-- This is the embedded /mnt/files/demo.lua script written by demo_init.c
-- at boot. The interactive menu version is /mnt/files/demo_menu.lua;
-- the 5-step scripted sequencer is /mnt/files/demo_auto.lua.
--
-- Usage:  lua /mnt/files/demo.lua

local P = slm.print
local sleep = slm.sleep
local yield = slm.yield

local function pause(ms)
    sleep(ms or 1500)
end

local function header(title)
    P("")
    P("------------------------------------------------------------")
    P("  " .. title)
    P("------------------------------------------------------------")
end

local function step(n, desc)
    P("")
    P(string.format("  [Step %d] %s", n, desc))
    P("")
end

-- Capture start time so the final "demo time" line is meaningful.
local t_start = slm.uptime()

-- Register a stub subscriber for /alerts/threshold so the msg_router
-- does not log "Topic '/alerts/threshold' not found" once per anomaly.
-- The sensor_monitor component publishes alerts to this topic; without
-- a subscriber, every alert emits a warning that clutters the demo
-- output. The stub callback just counts alerts for the closing summary.
local alert_count = 0
if slm.msg_subscribe then
    slm.msg_subscribe("/alerts/threshold", function(_topic, data)
        alert_count = alert_count + 1
        P(string.format("    [alert-consumer] received: %s", data))
    end)
end

-- ================================================================

P("")
P("============================================================")
P("  SLM-OS Industrial IoT Demo")
P("  Small Language Model Operating System")
P("============================================================")
P("")
P("  Simulating an industrial monitoring scenario where sensor")
P("  data flows through AI-capable components with live hot-swap.")
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
P(string.format("  RAM:         %d KB free / %d KB total (%d%% used)",
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
if idx and idx >= 0 then
    P("  sensor_monitor started (component index " .. idx .. ")")
else
    P("  NOTE: sensor_monitor could not be started (already running or registry full)")
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
    yield()
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
-- Handle both nil (binding returns nil on failure) and negative integer.
if new_idx and new_idx >= 0 then
    P("  Hot-swap successful (new index " .. new_idx .. ")")
else
    P("  Hot-swap failed (binding returned " .. tostring(new_idx) .. ")")
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
-- Step 5: AI Model Inference
-- ================================================================

header("On-Device Model Inference")
step(5, "Loading MNIST and running a classification")

local mnist = slm.model_load_mnist()
if mnist and mnist >= 0 then
    P(string.format("  MNIST loaded (model_id=%d)", mnist))
    local pred = slm.model_infer(mnist)
    P(string.format("  Predicted class (zero input): %d", pred))
else
    P("  MNIST not available in this build — skipping inference")
end

pause()

-- ================================================================
-- Step 6: Final Status
-- ================================================================

header("Final System Status")

local running, terminated = 0, 0
for _, t in ipairs(slm.tasks()) do
    -- slm.tasks() emits lowercase state strings ("running", "ready",
    -- "blocked", "terminated"). Match accordingly.
    if t.state == "running" or t.state == "ready" then
        running = running + 1
    elseif t.state == "terminated" then
        terminated = terminated + 1
    end
end
P(string.format("  Tasks:       %d active, %d completed", running, terminated))
P("  Components:  " .. slm.component_count() .. " registered")

local mem2 = slm.mem_stats()
P(string.format("  Memory:      %d KB used during demo",
    math.max(0, mem.free_kb - mem2.free_kb)))
P(string.format("  Alerts seen: %d", alert_count))
P(string.format("  Demo time:   %d ms", slm.uptime() - t_start))

P("")
P("============================================================")
P("  Demo Complete")
P("")
P("  SLM-OS demonstrated:")
P(string.format("    - Multi-core scheduling (%d CPUs)", slm.cpu_count()))
P("    - Component lifecycle management")
P("    - Publish/subscribe message routing")
P("    - Threshold-based anomaly detection")
P("    - Zero-downtime component hot-swap")
P("    - AI inference pipeline (MNIST)")
P("============================================================")
P("")
