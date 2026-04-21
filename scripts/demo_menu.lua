-- SLM-OS Demo (interactive menu)
--
-- Walks through the five core features of SLM-OS:
--   * SMP (symmetric multiprocessing)
--   * Preemptive multitasking / pluggable scheduling
--   * AI-driven scheduling
--   * Page / model eviction
--   * On-device model inference
--
-- Each menu item prints a short explanation then exercises the relevant
-- slm.* bindings against the live kernel. Built on top of slm.read_line()
-- and slm.shell_exec() (#151) plus the audit work tracked in #152.
--
-- Usage:  lua /mnt/files/demo_menu.lua

local P = slm.print

local function header(title)
    P("")
    P("============================================================")
    P("  " .. title)
    P("============================================================")
end

local function note(s) P("  " .. s) end
local function subhead(s) P("  -- " .. s) end

-- ---------------------------------------------------------------------------
-- System overview
-- ---------------------------------------------------------------------------

local function show_overview()
    header("System Overview")
    note("Version:     " .. slm.version())
    note("Uptime:      " .. slm.uptime() .. " ms")
    note("This CPU:    " .. slm.cpu_id() .. " / " .. slm.cpu_count() .. " cores")
    note("Scheduler:   " .. slm.sched_policy())

    local mem = slm.mem_stats()
    note(string.format("RAM:         %d KB used / %d KB total (%d KB free)",
        mem.used_kb, mem.total_kb, mem.free_kb))

    local ipc = slm.ipc_stats()
    if ipc then
        note(string.format("IPC:         %d queues, %d msgs sent, %d received",
            ipc.queue_count, ipc.msgs_sent, ipc.msgs_recv))
    end

    local vmm = slm.vmm_stats()
    if vmm then
        note(string.format("VMM:         %d L2 tables, %d blocks, %d bytes mapped",
            vmm.l2_tables, vmm.blocks_mapped, vmm.bytes_mapped))
    end

    local model = slm.model_stats()
    note(string.format("Weight pool: %d / %d blocks allocated",
        model.weights.allocated_blocks, model.weights.total_blocks))
    note(string.format("Work pool:   %d / %d blocks allocated",
        model.workspace.allocated_blocks, model.workspace.total_blocks))
end

-- ---------------------------------------------------------------------------
-- SMP demo — per-CPU state
-- ---------------------------------------------------------------------------

local function show_smp()
    header("SMP — Symmetric Multiprocessing")
    note("SLM-OS runs a per-core scheduler. Each CPU keeps its own run")
    note("queue and idle task; work-stealing rebalances across cores.")
    note("")

    local info = slm.cpu_info()
    note(string.format("Online cores: %d / %d   Current CPU: %d",
        info.online_count, info.total_count, info.current_cpu))
    note("")
    P(string.format("    %-4s %-10s %-12s %-12s", "CPU", "ISOLATED", "TICKS", "SCHEDULES"))
    P("    ---------------------------------------------")
    for _, c in ipairs(info.cpus) do
        P(string.format("    %-4d %-10s %-12d %-12d",
            c.id,
            c.isolated and "yes" or "no",
            c.ticks, c.schedules))
    end
    note("")
    subhead("Driving all cores via `bench smp`:")
    slm.shell_exec("bench smp")
end

-- ---------------------------------------------------------------------------
-- Scheduling demo — switch policies live, show stats
-- ---------------------------------------------------------------------------

local function show_scheduling()
    header("Preemptive Scheduling")
    note("SLM-OS has a pluggable scheduler. The heuristic policy is always")
    note("available; AI policies (MLP / PPO) appear when AI_SCHED=ON.")
    note("")

    local policies = slm.sched_policy_list()
    subhead("Registered policies:")
    for _, p in ipairs(policies) do
        note(string.format("  %s%s", p.name, p.active and "  (active)" or ""))
    end
    note("")

    subhead("Scheduler statistics:")
    local s = slm.sched_stats()
    note(string.format("  tasks:           %d", s.task_count))
    note(string.format("  ready:           %d", s.ready_count))
    note(string.format("  context switches: %d", s.context_switches))
    note(string.format("  timer ticks:     %d", s.timer_ticks))
    note("")

    local ai = slm.ai_sched_stats()
    if ai then
        subhead("AI scheduler stats (policy " .. ai.policy .. "):")
        note(string.format("  decisions:       %d", ai.decisions))
        note(string.format("  fallbacks:       %d", ai.fallbacks))
        note(string.format("  avg latency:     %d ns", ai.avg_latency_ns))
        if ai.histogram then
            local nonzero = 0
            for _, v in ipairs(ai.histogram) do
                if v > 0 then nonzero = nonzero + 1 end
            end
            note(string.format("  action slots used: %d / %d",
                nonzero, #ai.histogram))
        end
    else
        subhead("AI scheduler: not compiled in (build with AI_SCHED=ON)")
    end
end

local function switch_policy()
    header("Switch Scheduler Policy")
    local policies = slm.sched_policy_list()
    note("Available policies:")
    for i, p in ipairs(policies) do
        note(string.format("  %d) %s%s", i, p.name,
            p.active and "  (active)" or ""))
    end
    note("")
    note("Enter policy name (or blank to cancel): ")
    local name = slm.read_line()
    if name == nil or name == "" then
        note("(cancelled)")
        return
    end
    local before = slm.sched_policy()
    local ok = slm.sched_set_policy(name)
    if ok then
        note(string.format("Policy: %s -> %s", before, slm.sched_policy()))
    else
        note("Failed to set policy '" .. name .. "'")
    end
end

-- ---------------------------------------------------------------------------
-- Eviction demo — policy browsing and switching
-- ---------------------------------------------------------------------------

local function show_eviction()
    header("Page / Model Eviction")
    note("When the weight or workspace pool fills, an eviction policy picks")
    note("which block to reclaim. Policies include LRU, LFU, CACHEUS,")
    note("XGBoost and int8-MLP (when AI_EVICTION_MODELS=ON).")
    note("")

    local pol = slm.eviction_policy()
    if pol == nil then
        note("Eviction feature is disabled (build with AI_EVICTION=ON).")
        return
    end

    note("Active policy: " .. pol)
    note("")

    local st = slm.eviction_stats()
    if st then
        subhead("Eviction statistics:")
        note(string.format("  weights:     %d / %d allocated, %d evicted",
            st.weight_allocated, st.weight_total, st.weight_evictions))
        note(string.format("  workspace:   %d / %d allocated, %d evicted",
            st.workspace_allocated, st.workspace_total,
            st.workspace_evictions))
        note(string.format("  candidates:  %d snapshot blocks",
            st.snapshot_candidates))
        if st.expert_weights_bp then
            local bp_parts = {}
            for _, bp in ipairs(st.expert_weights_bp) do
                table.insert(bp_parts, string.format("%.2f%%", bp / 100.0))
            end
            note("  CACHEUS:     [" .. table.concat(bp_parts, ", ") .. "]")
        end
    end
end

local function switch_eviction()
    header("Switch Eviction Policy")
    local cur = slm.eviction_policy()
    if cur == nil then
        note("Eviction feature is disabled (build with AI_EVICTION=ON).")
        return
    end
    note("Active policy: " .. cur)
    note("")
    note("Type `eviction policy` into the shell to see available names.")
    note("Common names: lru, lfu, cacheus, xgboost, mlp")
    note("")
    note("Enter policy name (blank to cancel): ")
    local name = slm.read_line()
    if name == nil or name == "" then
        note("(cancelled)")
        return
    end
    local ok = slm.eviction_set_policy(name)
    if ok then
        note(string.format("Policy: %s -> %s", cur, slm.eviction_policy()))
    else
        note("Failed to set policy '" .. name .. "'")
    end
end

-- ---------------------------------------------------------------------------
-- Inference demo — load MNIST, infer, bench, stats, GPU
-- ---------------------------------------------------------------------------

local function inference_demo()
    header("On-Device Model Inference")
    note("SLM-OS can load ONNX models and run inference in kernel space.")
    note("")

    -- Ensure MNIST is loaded
    local idx = slm.model_find("mnist")
    if idx == nil or idx < 0 then
        subhead("Loading built-in MNIST model (26 KB ONNX)...")
        idx = slm.model_load_mnist()
        if idx == nil or idx < 0 then
            note("ERROR: failed to load MNIST model")
            return
        end
    end
    note("MNIST model index: " .. idx)
    note("")

    -- Show detail
    local info = slm.model_info(idx)
    if info then
        subhead("Model details:")
        note(string.format("  name:       %s", info.name))
        note(string.format("  format:     %s", info.format))
        note(string.format("  params:     %d", info.params))
        note(string.format("  weights:    %d bytes", info.weight_size))
        note(string.format("  nodes:      %d", info.nodes))
        note(string.format("  I/O:        %d inputs / %d outputs",
            info.inputs, info.outputs))
    end
    note("")

    -- Run one inference
    subhead("Single inference (zero input):")
    local pred = slm.model_infer(idx)
    note("Predicted class: " .. pred)
    note("")

    -- Bench
    subhead("Benchmark (10 iterations):")
    slm.model_bench(idx, 10)

    -- Stats
    local s = slm.infer_stats()
    if s then
        note("")
        subhead("Cumulative inference stats:")
        note(string.format("  total:    %d inferences", s.total))
        note(string.format("  min:      %d ns", s.min_ns))
        note(string.format("  max:      %d ns", s.max_ns))
        note(string.format("  last:     %d ns", s.last_ns))
        note(string.format("  errors:   %d", s.errors))
    end

    -- GPU
    note("")
    subhead("GPU status:")
    local g = slm.gpu_status()
    if g.available then
        note("  available:     yes")
        if g.name then
            note("  driver:        " .. g.name)
            note("  device:        " .. g.device)
            note(string.format("  compute ready: %s",
                g.compute_ready and "yes" or "no (fallback to CPU)"))
            note(string.format("  unified mem:   %s",
                g.unified_memory and "yes" or "no"))
        end
    else
        note("  available:     no (CPU inference only)")
    end
end

local function list_models()
    header("Loaded Models")
    local list = slm.model_list()
    if #list == 0 then
        note("(no models loaded — try the Inference demo first)")
        return
    end
    P(string.format("    %-4s %-20s %-8s %-12s %-10s",
        "IDX", "NAME", "FORMAT", "WEIGHTS", "NODES"))
    P("    -----------------------------------------------------")
    for _, m in ipairs(list) do
        P(string.format("    %-4d %-20s %-8s %-12d %-10d",
            m.index, m.name, m.format, m.weight_size, m.nodes))
    end
end

-- ---------------------------------------------------------------------------
-- Hailo NPU demo — delegates to scripts/demo_hailo.lua
-- ---------------------------------------------------------------------------

local function hailo_demo()
    header("Hailo NPU (AI HAT+)")
    local s = slm.hailo.status()
    if not s or not s.available then
        note("Hailo backend NOT present on this build/platform.")
        note("(Expected on QEMU and on Pi 5 builds without the AI HAT+.)")
        note("Running the full demo anyway — it degrades to a status probe.")
    else
        note(string.format("Backend: %s — slots %d / %d",
            s.name, s.slots_in_use, s.slots_max))
    end
    note("")
    subhead("Delegating to /mnt/files/demo_hailo.lua:")
    -- Route through shell_exec rather than Lua's built-in dofile: SLM-OS
    -- stubs fopen to return NULL, so Lua's dofile cannot read VFS paths.
    -- The `lua` shell command uses lua_slm_dofile internally, which reads
    -- through the VFS layer and works on every platform.
    local rc = slm.shell_exec("lua /mnt/files/demo_hailo.lua")
    if rc ~= 0 then
        note(string.format("demo_hailo.lua exit code: %d", rc))
    end
end

-- ---------------------------------------------------------------------------
-- Components (hot-swap) demo — preserved from earlier demo
-- ---------------------------------------------------------------------------

local function components_demo()
    header("Components & Hot-Swap")
    local list = slm.component_list()
    if #list > 0 then
        P(string.format("    %-4s %-20s %-12s %-10s %-8s",
            "IDX", "NAME", "TYPE", "STATE", "TASK"))
        P("    --------------------------------------------------------")
        for _, c in ipairs(list) do
            P(string.format("    %-4d %-20s %-12s %-10s %-8d",
                c.index, c.name, c.type, c.state, c.task_id))
        end
    else
        note("(no components registered yet)")
    end

    note("")
    subhead("Starting sensor_monitor + driving anomaly data:")
    local idx = slm.component_run("sensor_monitor")
    if idx and idx >= 0 then
        note("sensor_monitor started (index " .. idx .. ")")
    else
        -- #318: component_run returns nil/-1 if sensor_monitor is still
        -- running from a prior invocation of this menu option (its 30s
        -- inactivity timeout has not yet expired). The prior instance
        -- still holds its /sensors/data subscription, so the rest of
        -- the demo (publish + hot-swap) exercises it cleanly — just
        -- tell the user that is what is happening instead of leaving
        -- the ugly kernel warning unexplained.
        local existing = slm.component_find("sensor_monitor")
        if existing and existing >= 0 then
            note(string.format(
                "sensor_monitor already running from a prior run (index %d) — reusing it",
                existing))
            idx = existing
        else
            note("sensor_monitor could not be started (registry full?)")
        end
    end
    slm.yield(); slm.yield()

    for _, v in ipairs({23, 78, 95, 19}) do
        local n = slm.msg_publish("/sensors/data", tostring(v))
        local tag = v > 50 and " ANOMALY" or ""
        note(string.format("  /sensors/data <- %-3d  (delivered to %d)%s",
            v, n, tag))
        slm.yield()
        slm.sleep(150)
    end

    note("")
    subhead("Hot-swapping sensor_monitor (zero-downtime):")
    local new_idx = slm.component_hot_swap("sensor_monitor", "sensor_monitor")
    if new_idx and new_idx >= 0 then
        note("hot-swap OK (new index " .. new_idx .. ")")
        local n = slm.msg_publish("/sensors/data", "88")
        note(string.format("  post-swap ping -> delivered to %d subscriber(s)", n))
    else
        note("hot-swap failed (code " .. tostring(new_idx) .. ")")
    end
end

-- ---------------------------------------------------------------------------
-- Full tour — runs everything in sequence
-- ---------------------------------------------------------------------------

local function full_tour()
    show_overview();   slm.sleep(500)
    show_smp();        slm.sleep(500)
    show_scheduling(); slm.sleep(500)
    show_eviction();   slm.sleep(500)
    inference_demo();  slm.sleep(500)
    components_demo(); slm.sleep(500)
    header("Tour Complete")
    note("Use the menu to drill into any section, or run 'help' at the")
    note("shell for the full command list.")
end

-- ---------------------------------------------------------------------------
-- Shell passthrough
-- ---------------------------------------------------------------------------

local function run_shell_command()
    P("")
    note("Enter a shell command (e.g. 'tasks', 'cpu', 'eviction stats'): ")
    local cmd = slm.read_line()
    if cmd == nil or cmd == "" then
        note("(no command entered)")
        return
    end
    local rc = slm.shell_exec(cmd)
    note(string.format("[exit %d]", rc))
end

local function show_tasks()
    header("Active Tasks")
    P(string.format("    %-4s %-20s %-10s %-4s %-4s",
        "ID", "NAME", "STATE", "CPU", "PRI"))
    P("    -------------------------------------------------")
    for _, t in ipairs(slm.tasks()) do
        P(string.format("    %-4d %-20s %-10s %-4d %-4d",
            t.id, t.name, t.state, t.cpu, t.priority))
    end
end

-- ---------------------------------------------------------------------------
-- Menu
-- ---------------------------------------------------------------------------

local menu = {
    {key = "1", label = "System overview",                action = show_overview},
    {key = "2", label = "SMP — per-core state",           action = show_smp},
    {key = "3", label = "Scheduling — policies + stats",  action = show_scheduling},
    {key = "4", label = "Switch scheduler policy",        action = switch_policy},
    {key = "5", label = "Eviction — policy + stats",      action = show_eviction},
    {key = "6", label = "Switch eviction policy",         action = switch_eviction},
    {key = "7", label = "Inference — MNIST load+bench",   action = inference_demo},
    {key = "8", label = "List loaded models",             action = list_models},
    {key = "9", label = "Components & hot-swap",          action = components_demo},
    {key = "h", label = "Hailo NPU (AI HAT+)",             action = hailo_demo},
    {key = "t", label = "Full system tour",               action = full_tour},
    {key = "p", label = "Active tasks",                   action = show_tasks},
    {key = "s", label = "Run an arbitrary shell command", action = run_shell_command},
    {key = "q", label = "Quit",                           action = nil},
}

local function show_menu()
    P("")
    P("------------------------------------------------------------")
    P("  SLM-OS Demo")
    P("------------------------------------------------------------")
    for _, item in ipairs(menu) do
        P(string.format("    %s) %s", item.key, item.label))
    end
    P("")
    P("  Choose: ")
end

local function find_action(key)
    for _, item in ipairs(menu) do
        if item.key == key then return item end
    end
    return nil
end

P("")
P("############################################################")
P("#  SLM-OS Demo — AI-First Operating System")
P("#  Interactive menu (see scripts/demo_menu.lua)")
P("############################################################")

while true do
    show_menu()
    local choice = slm.read_line()
    if choice == nil then
        P("  (input closed, exiting)")
        break
    end
    local item = find_action(choice)
    if item == nil then
        P("  Unknown choice: '" .. choice .. "'")
    elseif item.key == "q" then
        P("  Goodbye.")
        break
    else
        item.action()
    end
end
