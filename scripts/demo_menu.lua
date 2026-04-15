-- SLM-OS Phase 5 Demo (interactive menu)
--
-- Walks through the AI-first operating system features one scenario at
-- a time. Picks scenarios from a numbered menu and can dispatch any
-- shell command directly. Built on the slm.read_line() and
-- slm.shell_exec() bindings (#151).
--
-- Usage:  lua /scripts/demo_menu.lua

local P = slm.print

local function header(title)
    P("")
    P("============================================================")
    P("  " .. title)
    P("============================================================")
end

local function note(s) P("  " .. s) end

-- --- Scenarios ---------------------------------------------------------------

local function show_overview()
    header("System Overview")
    note("Version:    " .. slm.version())
    note("CPUs:       " .. slm.cpu_count() .. " cores")
    note("This CPU:   " .. slm.cpu_id())
    note("Scheduler:  " .. slm.sched_policy())
    local mem = slm.mem_stats()
    note(string.format("RAM:        %d KB free / %d KB total",
        mem.free_kb, mem.total_kb))
    local model = slm.model_stats()
    note(string.format("Weight Pool: %d / %d blocks allocated",
        model.weights.allocated_blocks, model.weights.total_blocks))
    note(string.format("Work Pool:   %d / %d blocks allocated",
        model.workspace.allocated_blocks, model.workspace.total_blocks))
    note(string.format("Uptime:     %d ms", slm.uptime()))
end

local function show_components()
    header("Registered Components")
    local list = slm.component_list()
    if #list == 0 then
        note("(none registered)")
        return
    end
    P(string.format("    %-4s %-20s %-12s %-10s %-8s",
        "IDX", "NAME", "TYPE", "STATE", "TASK"))
    P("    --------------------------------------------------------")
    for _, c in ipairs(list) do
        P(string.format("    %-4d %-20s %-12s %-10s %-8d",
            c.index, c.name, c.type, c.state, c.task_id))
    end
end

local function start_sensor_monitor()
    header("Start sensor_monitor")
    local idx = slm.component_run("sensor_monitor")
    if idx and idx >= 0 then
        note("sensor_monitor started (component index " .. idx .. ")")
    else
        note("ERROR: failed to start sensor_monitor")
        return
    end
    slm.yield(); slm.yield()
    note("")
    note("Driving sensor data (>50 trips threshold):")
    for _, v in ipairs({23, 31, 78, 95, 19, 62}) do
        local n = slm.msg_publish("/sensors/data", tostring(v))
        local tag = v > 50 and " ANOMALY" or ""
        note(string.format("  /sensors/data <- %-3d  (delivered to %d)%s",
            v, n, tag))
        slm.yield()
        slm.sleep(200)
    end
end

local function start_digit_classifier()
    header("Start digit_classifier")
    local idx = slm.component_run("digit_classifier")
    if idx and idx >= 0 then
        note("digit_classifier started (component index " .. idx .. ")")
        note("Watches /input/digits for inference requests.")
    else
        note("ERROR: failed to start digit_classifier")
    end
end

local function hot_swap_demo()
    header("Hot-Swap sensor_monitor")
    local existing = slm.component_find("sensor_monitor")
    if existing == nil then
        note("sensor_monitor not running — starting one first")
        slm.component_run("sensor_monitor")
        slm.yield(); slm.yield()
    end
    local new_idx = slm.component_hot_swap("sensor_monitor", "sensor_monitor")
    if new_idx and new_idx >= 0 then
        note("hot-swap succeeded (new index " .. new_idx .. ")")
    else
        note("hot-swap failed (code " .. tostring(new_idx) .. ")")
        return
    end
    slm.yield(); slm.yield()
    local n = slm.msg_publish("/sensors/data", "88")
    note(string.format("  /sensors/data <- 88  (delivered to %d)", n))
end

local function load_and_infer()
    header("MNIST Model Inference")
    local idx = slm.model_find("mnist")
    if idx == nil or idx < 0 then
        note("Loading built-in MNIST model...")
        idx = slm.model_load_mnist()
        if idx == nil or idx < 0 then
            note("ERROR: failed to load MNIST model")
            return
        end
    end
    note("Model loaded at index " .. idx)
    note("")
    note("Running inference on built-in test image:")
    local pred = slm.model_infer(idx)
    if pred ~= nil and pred >= 0 then
        note("Predicted class: " .. pred)
    else
        note("Inference failed (code " .. tostring(pred) .. ")")
    end
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

local function run_shell_command()
    P("")
    note("Enter a shell command (e.g. 'tasks', 'cpu', 'help'):")
    note("> ")
    local cmd = slm.read_line()
    if cmd == nil or cmd == "" then
        note("(no command entered)")
        return
    end
    local rc = slm.shell_exec(cmd)
    note(string.format("[exit %d]", rc))
end

-- --- Menu --------------------------------------------------------------------

local menu = {
    {key = "1", label = "System overview",                action = show_overview},
    {key = "2", label = "Show registered components",     action = show_components},
    {key = "3", label = "Start sensor_monitor + drive",   action = start_sensor_monitor},
    {key = "4", label = "Start digit_classifier",         action = start_digit_classifier},
    {key = "5", label = "Hot-swap sensor_monitor",        action = hot_swap_demo},
    {key = "6", label = "Load MNIST + run inference",     action = load_and_infer},
    {key = "7", label = "Show running tasks",             action = show_tasks},
    {key = "8", label = "Run an arbitrary shell command", action = run_shell_command},
    {key = "q", label = "Quit",                           action = nil},
}

local function show_menu()
    P("")
    P("------------------------------------------------------------")
    P("  SLM-OS Phase 5 Demo")
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
P("#  SLM-OS Phase 5 Demo")
P("#  AI-First Operating System — interactive menu")
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
