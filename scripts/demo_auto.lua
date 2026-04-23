-- SLM-OS Scripted Auto-Demo (#192)
--
-- Walks through the five core capstone features in sequence with
-- Enter-to-advance pauses between sections. The OS drives itself so
-- the presenter can narrate without typing commands.
--
-- Usage:  lua /mnt/files/demo_auto.lua
--
-- Each section is resilient: if a subsystem isn't compiled in (e.g.,
-- DISABLE_EVICTION=ON skips the eviction narrative), the script prints a
-- note and moves on rather than failing.

local P = slm.print
local run = slm.shell_exec
local SMP_DEMO_MS = 5000
local SMP_DEMO_SLICE = 20000
local SMP_DEMO_WORKERS_PER_CPU = 3

local function banner(title, n, total)
    P("")
    P("============================================================")
    P(string.format("  [%d/%d] %s", n, total, title))
    P("============================================================")
end

local function pause()
    P("")
    P("  <press Enter to continue, or type 'skip' to jump ahead>")
    local line = slm.read_line()
    if line == "skip" then
        return "skip"
    end
    return "continue"
end

local function smp_demo_targets()
    local cpus = {}
    if slm.cpu_count() <= 1 then
        cpus[1] = 0
    else
        for cpu = 1, slm.cpu_count() - 1 do
            cpus[#cpus + 1] = cpu
        end
    end
    return cpus
end

local function csv(list)
    local out = {}
    for i, v in ipairs(list) do
        out[i] = tostring(v)
    end
    return table.concat(out, ", ")
end

local function make_smp_worker(deadline_ms, cpu, worker)
    local spin_count = SMP_DEMO_SLICE + cpu * 3000 + worker * 2000
    local yield_every = 2 + ((cpu + worker) % 3)
    local sleep_every = 5 + ((cpu * 2 + worker) % 4)
    local sleep_ms = 1 + ((cpu + worker) % 3)
    local loader, err = load(string.format([[
        return function()
            local deadline = %d
            local acc = 0
            local loops = 0
            while slm.uptime() < deadline do
                for i = 1, %d do
                    acc = (acc + i) %% 65521
                end
                loops = loops + 1
                if (loops %% %d) == 0 then
                    slm.sleep(%d)
                elseif (loops %% %d) == 0 then
                    slm.yield()
                end
            end
            if acc == -1 then slm.print("") end
        end
    ]], deadline_ms, spin_count, sleep_every, sleep_ms, yield_every), "smp_demo_worker")
    if not loader then
        return nil, err
    end
    return loader()
end

local function run_smp_demo(duration_ms)
    local ids = {}
    local cpus = smp_demo_targets()
    local deadline = slm.uptime() + duration_ms

    for _, cpu in ipairs(cpus) do
        for worker = 1, SMP_DEMO_WORKERS_PER_CPU do
            local fn = make_smp_worker(deadline, cpu, worker)
            if fn then
                local id = slm.task_create(string.format("smp-%d-%d", cpu, worker), fn)
                if id and slm.task_pin(id, cpu) then
                    ids[#ids + 1] = id
                elseif id then
                    slm.task_kill(id)
                end
            end
            slm.yield()
        end
    end

    return ids, cpus
end

-- ---------------------------------------------------------------------------

P("")
P("######################################################")
P("#             SLM-OS Capstone Live Demo              #")
P("######################################################")
P("")
P("Five features will be demonstrated in sequence:")
P("  1. Symmetric Multiprocessing")
P("  2. Preemptive Multitasking + AI Scheduling")
P("  3. AI-Driven Page Eviction")
P("  4. Model Inference")
P("  5. Live System Dashboard")
P("")
P("Version:    " .. slm.version())
P("CPUs:       " .. slm.cpu_count() .. " cores")
P("Scheduler:  " .. slm.sched_policy())
P("")

if pause() == "skip" then P("Demo aborted by user."); return end

-- [1/5] SMP -----------------------------------------------------------------
banner("Symmetric Multiprocessing", 1, 5)
P("  Launching multiple pinned Lua workers per secondary CPU.")
local ids, cpus = run_smp_demo(SMP_DEMO_MS)
P(string.format("  Workers launched: %d (%d per CPU) on CPU(s) %s.",
    #ids, SMP_DEMO_WORKERS_PER_CPU, csv(cpus)))
P("  This should leave ready tasks queued so `top` shows visible rotation.")
P("")
slm.sleep(SMP_DEMO_MS)
P("")
P("  Following with `bench smp` for the fast dispatch proof:")
P("")
run("bench smp")
P("")
P("  Observation: the pinned workers make SMP visible; `bench smp`")
P("  still confirms cross-CPU dispatch on the same build.")

if pause() == "skip" then return end

-- [2/5] Preemptive + Scheduling ---------------------------------------------
banner("Preemptive Multitasking + AI Scheduling", 2, 5)
P("  Comparing scheduler policies side-by-side via `sched compare`.")
P("")
run("sched compare")
P("")
P("  Observation: Each policy runs the same workload; the table")
P("  shows context-switch count and average round-trip latency.")

if pause() == "skip" then return end

-- [3/5] AI Page Eviction ----------------------------------------------------
banner("AI-Driven Page Eviction", 3, 5)
P("  Current eviction policy:")
run("eviction")
P("")
P("  Driving the weight pool to saturation via `eviction demo`:")
P("")
run("eviction demo")
P("")
P("  Observation: Each EVICT row is one policy decision — the AI")
P("  selected a victim and the allocation retried successfully.")

if pause() == "skip" then return end

-- [4/5] Model Inference ----------------------------------------------------
banner("Model Inference", 4, 5)
P("  Loading built-in MNIST model...")
local mnist = slm.model_load_mnist()
if mnist < 0 then
    P("  (MNIST not available in this build — skipping)")
else
    P(string.format("  MNIST loaded (model_id=%d). Running 10 inferences:", mnist))
    local t0 = slm.uptime()
    local predictions = {}
    for i = 1, 10 do
        predictions[i] = slm.model_infer(mnist)
    end
    local elapsed = slm.uptime() - t0
    P(string.format("  Completed 10 inferences in %d ms (%d ms / inference avg).",
        elapsed, elapsed // 10))
    P("  Predictions: " .. table.concat(predictions, " "))
end

if pause() == "skip" then return end

-- [5/5] Live Dashboard ----------------------------------------------------
banner("Live System Dashboard", 5, 5)
P("  Rendering `top` for 3 frames so each can be narrated...")
P("")
run("top -n 3 1")
P("")
P("  Observation: Real-time per-CPU utilization, task roster,")
P("  memory usage, eviction counters — all in one pane.")

P("")
P("============================================================")
P("   Demo complete. Thank you!")
P("============================================================")
P("")
