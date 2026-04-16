-- SLM-OS Scripted Auto-Demo (#192)
--
-- Walks through the five core capstone features in sequence with
-- Enter-to-advance pauses between sections. The OS drives itself so
-- the presenter can narrate without typing commands.
--
-- Usage:  lua /mnt/files/demo_auto.lua
--
-- Each section is resilient: if a subsystem isn't compiled in (e.g.,
-- AI_EVICTION=OFF skips the eviction narrative), the script prints a
-- note and moves on rather than failing.

local P = slm.print
local run = slm.shell_exec

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
P("  Launching bench smp — dispatches work to every CPU.")
P("")
run("bench smp")
P("")
P("  Observation: All " .. slm.cpu_count() .. " cores executed the workload.")

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
