-- SLM-OS Multi-Process Demo (interactive menu)
--
-- Demonstrates multiple concurrent processes (tasks) running across the
-- platform's available CPUs. Picks scenarios from a numbered menu and
-- can dispatch any shell command directly via slm.shell_exec(). Built
-- on the slm.read_line() and slm.shell_exec() bindings (issue #151).
--
-- Usage:  lua /scripts/multiproc_demo.lua

local P = slm.print

local function header(title)
    P("")
    P("============================================================")
    P("  " .. title)
    P("============================================================")
end

local function dump_tasks()
    P(string.format("    %-4s %-20s %-10s %-4s %-4s",
        "ID", "NAME", "STATE", "CPU", "PRI"))
    P("    -------------------------------------------------")
    for _, t in ipairs(slm.tasks()) do
        P(string.format("    %-4d %-20s %-10s %-4d %-4d",
            t.id, t.name, t.state, t.cpu, t.priority))
    end
end

-- --- Scenarios ---------------------------------------------------------------

local function show_overview()
    header("System Overview")
    P("  Version:    " .. slm.version())
    P("  CPUs:       " .. slm.cpu_count() .. " cores")
    P("  This CPU:   " .. slm.cpu_id())
    P("  Scheduler:  " .. slm.sched_policy())
    local mem = slm.mem_stats()
    P(string.format("  RAM:        %d KB free / %d KB total",
        mem.free_kb, mem.total_kb))
    P(string.format("  Uptime:     %d ms", slm.uptime()))
end

local function show_tasks()
    header("Active Tasks")
    dump_tasks()
end

local function spawn_components()
    header("Spawning Concurrent Components")
    local names = {"listener", "sensor_monitor", "counter"}
    for _, name in ipairs(names) do
        local idx = slm.component_run(name)
        if idx and idx >= 0 then
            P(string.format("    spawned %-16s (component index %d)", name, idx))
        else
            P(string.format("    FAILED to spawn %s", name))
        end
        slm.yield()
    end
    slm.sleep(500)
    P("")
    P("  Task table after spawn:")
    dump_tasks()
    P("")
    P("  Active components: " .. slm.component_count())
end

local function drive_ipc()
    header("Inter-Process Communication")
    P("  Publishing to 'events':")
    for i = 1, 3 do
        local n = slm.msg_publish("events", "ping-" .. i)
        P(string.format("    events <- ping-%d   (delivered to %d)", i, n))
        slm.yield()
        slm.sleep(150)
    end
    P("")
    P("  Publishing sensor readings (>50 trips threshold):")
    for _, v in ipairs({28, 41, 73, 88, 19}) do
        local n = slm.msg_publish("/sensors/data", tostring(v))
        local tag = v > 50 and " ANOMALY" or ""
        P(string.format("    /sensors/data <- %-3d (delivered to %d)%s",
            v, n, tag))
        slm.yield()
        slm.sleep(150)
    end
end

local function run_bench(which)
    header("bench " .. which)
    local rc = slm.shell_exec("bench " .. which)
    P("  bench " .. which .. " returned " .. tostring(rc))
end

local function run_shell_command()
    P("")
    P("  Enter a shell command (e.g. 'tasks', 'cpu', 'help'):")
    P("  > ")
    local cmd = slm.read_line()
    if cmd == nil or cmd == "" then
        P("  (no command entered)")
        return
    end
    local rc = slm.shell_exec(cmd)
    P(string.format("  [exit %d]", rc))
end

-- --- Menu --------------------------------------------------------------------

local menu = {
    {key = "1", label = "System overview",                 action = show_overview},
    {key = "2", label = "Show task table",                 action = show_tasks},
    {key = "3", label = "Spawn 3 components (multi-task)", action = spawn_components},
    {key = "4", label = "Drive IPC (publish messages)",    action = drive_ipc},
    {key = "5", label = "bench smp",                       action = function() run_bench("smp") end},
    {key = "6", label = "bench stats",                     action = function() run_bench("stats") end},
    {key = "7", label = "Run an arbitrary shell command",  action = run_shell_command},
    {key = "q", label = "Quit",                            action = nil},
}

local function show_menu()
    P("")
    P("------------------------------------------------------------")
    P("  SLM-OS Multi-Process Demo")
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
P("#  SLM-OS Multi-Process Demo")
P("#  (interactive menu — uses slm.read_line / slm.shell_exec)")
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
