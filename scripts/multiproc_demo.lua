-- SLM-OS Multi-Process Demo (interactive menu)
--
-- Demonstrates multiple concurrent processes (tasks) running across the
-- platform's available CPUs. Picks scenarios from a numbered menu and
-- can dispatch any shell command directly via slm.shell_exec(). Built
-- on the slm.read_line() and slm.shell_exec() bindings (issue #151).
--
-- Usage:  lua /mnt/files/multiproc_demo.lua
--
-- Embedded into the kernel image via .incbin and written to the
-- LittleFS ramdisk at boot by demo_init() in kernel/src/demo_init.c.

local P = slm.print
local SMP_DEMO_MS = 8000
local SMP_DEMO_SLICE = 20000
local SMP_DEMO_WORKERS_PER_CPU = 3
local IPC_RENDER_MS = 100
local IPC_MIN_PUBLISH_MS = 500
local IPC_MAX_PUBLISH_MS = 2000

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

local function csv(list)
    local out = {}
    for i, v in ipairs(list) do
        out[i] = tostring(v)
    end
    return table.concat(out, ", ")
end

local function wait_for_key(message)
    P("")
    P(message or "Press any key to return")
    while true do
        local ch = slm.try_getc()
        if ch ~= nil then
            break
        end
        slm.sleep(50)
    end
end

local function admin_only_message(title, reason)
    header(title)
    P("  This demo action requires lua-admin.")
    if reason then
        P("  Missing binding: " .. reason)
    end
    P("")
    P("  Re-run the demo with:")
    P("    lua-admin /mnt/files/multiproc_demo.lua")
    wait_for_key("Press any key to return to the menu")
end

local function require_binding(title, field)
    if slm[field] == nil then
        admin_only_message(title, "slm." .. field)
        return false
    end
    return true
end

local function collect_task_rows()
    local rows = {}
    rows[#rows + 1] = string.format("  %-4s %-20s %-10s %-4s %-4s",
        "ID", "NAME", "STATE", "CPU", "PRI")
    rows[#rows + 1] = "  ----------------------------------------------------"
    for _, t in ipairs(slm.tasks()) do
        rows[#rows + 1] = string.format("  %-4d %-20s %-10s %-4d %-4d",
            t.id, t.name, t.state, t.cpu, t.priority)
    end
    return rows
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

local function launch_smp_workers(duration_ms)
    local cpus = smp_demo_targets()
    local deadline = slm.uptime() + duration_ms
    local ids = {}

    for _, cpu in ipairs(cpus) do
        for worker = 1, SMP_DEMO_WORKERS_PER_CPU do
            local fn, err = make_smp_worker(deadline, cpu, worker)
            if not fn then
                P(string.format("    FAILED to build worker %d for CPU %d: %s",
                    worker, cpu, tostring(err)))
            else
                local id = slm.task_create(string.format("smp-%d-%d", cpu, worker), fn)
                if not id then
                    P(string.format("    FAILED to spawn worker %d for CPU %d", worker, cpu))
                elseif not slm.task_pin(id, cpu) then
                    slm.task_kill(id)
                    P(string.format("    FAILED to pin worker %d to CPU %d", id, cpu))
                else
                    ids[#ids + 1] = id
                    P(string.format("    worker %-3d pinned to CPU %d (slot %d)",
                        id, cpu, worker))
                end
            end
            slm.yield()
        end
    end

    return ids, cpus
end

-- --- Scenarios ---------------------------------------------------------------

local function show_overview()
    header("System Overview")
    P("  Version:    " .. slm.version())
    P("  CPUs:       " .. slm.cpu_count() .. " cores")
    P("  This CPU:   " .. slm.cpu_id())
    P("  Scheduler:  " .. slm.sched_policy())
    P("  Eviction:   " .. tostring(slm.eviction_policy() or "disabled"))
    local mem = slm.mem_stats()
    P(string.format("  RAM:        %d KB free / %d KB total",
        mem.free_kb, mem.total_kb))
    P(string.format("  Uptime:     %d ms", slm.uptime()))
    wait_for_key("Press any key to return to the menu")
end

local function show_tasks()
    while true do
        local term = slm.term_size()
        local max_rows = 18
        if term and term.rows and term.rows > 8 then
            max_rows = term.rows - 6
        end

        local rows = collect_task_rows()
        local lines = {
            "\027[H\027[2JSLM-OS Task Table",
            "Press q to return to the menu",
            string.format("uptime=%d ms  tasks=%d", slm.uptime(), #slm.tasks()),
            "",
        }
        for i = 1, math.min(#rows, max_rows) do
            lines[#lines + 1] = rows[i]
        end
        if #rows > max_rows then
            lines[#lines + 1] = string.format("  ... %d more tasks not shown", #rows - max_rows)
        end

        P(table.concat(lines, "\n"))

        local waited = 0
        while waited < 1000 do
            local ch = slm.try_getc()
            if ch == "q" or ch == "Q" or ch == " " then
                P("")
                return
            end
            slm.sleep(100)
            waited = waited + 100
        end
    end
end

local function spawn_components()
    if not require_binding("Spawning Concurrent Components", "component_run") then
        return
    end
    header("Spawning Concurrent Components")
    local names = {"listener", "sensor_monitor", "counter"}
    P("  Demo value:")
    P("    listener       subscribes to 'events'")
    P("    sensor_monitor subscribes to '/sensors/data'")
    P("    counter        runs independent periodic work")
    P("  This shows built-in components becoming separate kernel tasks.")
    P("")
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
    wait_for_key("Press any key to return to the menu")
end

local function pad_right(s, width)
    s = tostring(s or "")
    if #s >= width then
        return s:sub(1, width)
    end
    return s .. string.rep(" ", width - #s)
end

local function draw_ipc_dashboard(logs, consumers, now_ms, stats)
    local term = slm.term_size()
    local total_w = 120
    if term and term.cols and term.cols > 40 then
        total_w = term.cols
    end
    local left_w = math.floor(total_w * 0.42)
    if left_w < 36 then left_w = 36 end
    if left_w > 52 then left_w = 52 end
    local right_w = total_w - left_w - 3
    if right_w < 32 then right_w = 32 end
    local lines = {}

    lines[#lines + 1] = "\027[H\027[2JSLM-OS IPC Dashboard"
    lines[#lines + 1] = "Press space or q to stop"
    lines[#lines + 1] = string.format(
        "topics=%d  published=%d  delivered=%d  callbacks=%d  pending=%d",
        stats.topic_count,
        stats.published,
        stats.delivered,
        stats.callbacks,
        stats.pending)
    lines[#lines + 1] = string.format(
        "uptime=%d ms  last_publish=%d ms",
        now_ms, stats.last_publish_ms)
    lines[#lines + 1] = pad_right("Publish Stream", left_w) .. " | " ..
        pad_right("Consumers", right_w)
    lines[#lines + 1] = string.rep("-", left_w) .. "-+-" .. string.rep("-", right_w)

    local rows = math.max(#logs, #consumers + 1)
    for row = 1, rows do
        local left = ""
        local entry = logs[row]
        if entry then
            left = string.format("%6d  %-10s => %-18s",
                entry.at_ms, entry.topic, entry.payload)
        end

        local right = ""
        if row == 1 then
            right = string.format("     %-8s %7s %-8s %-10s %s",
                "name", "count", "topic", "last msg", "subs")
        else
            local c = consumers[row - 1]
            if c then
                if c.divider then
                    right = ""
                else
                    right = string.format("%s %-8s %7d  %-8s %-10s  %s",
                        c.arrow and "=>" or "  ",
                        pad_right(c.name, 8),
                        c.count,
                        pad_right(c.last_topic, 8),
                        pad_right(c.last_msg, 10),
                        c.subs_display)
                end
            end
        end
        lines[#lines + 1] = pad_right(left, left_w) .. " | " .. pad_right(right, right_w)
    end

    P(table.concat(lines, "\n"))
end

local function run_ipc_dashboard()
    local topics = {
        "alpha", "bravo", "charlie", "delta", "echo",
        "foxtrot", "golf", "hotel", "india", "juliet", "zulu",
    }
    local consumer_defs = {
        {name = "cons-a", topics = {"alpha", "bravo", "charlie", "delta"}},
        {name = "cons-b", topics = {"charlie", "delta", "echo", "foxtrot"}},
        {name = "cons-c", topics = {"echo", "foxtrot", "golf", "hotel"}},
        {name = "cons-d", topics = {"golf", "hotel", "india", "juliet"}},
        {name = "",       topics = {}},
        {name = "zulu",   topics = {"zulu"}},
    }
    local consumers = {}
    local handles = {}
    local logs = {}
    local log_cap = 14
    local stats = {
        last_publish_ms = 0,
        topic_count = #topics,
        published = 0,
        delivered = 0,
        callbacks = 0,
        pending = 0,
    }
    local topic_consumers = {}
    local topic_bag = {}

    for i, def in ipairs(consumer_defs) do
        consumers[i] = {
            name = def.name,
            count = 0,
            last_topic = "-",
            last_msg = "-",
            arrow = false,
            divider = def.name == "",
            subs_display = table.concat(def.topics, ","),
        }
        for _, topic in ipairs(def.topics) do
            if not topic_consumers[topic] then
                topic_consumers[topic] = 0
            end
            topic_consumers[topic] = topic_consumers[topic] + 1
        end
    end

    local function refill_topic_bag()
        topic_bag = {}
        for _, topic in ipairs(topics) do
            topic_bag[#topic_bag + 1] = topic
        end
        for i = #topic_bag, 2, -1 do
            local j = math.random(i)
            topic_bag[i], topic_bag[j] = topic_bag[j], topic_bag[i]
        end
    end

    local function push_log(topic, payload)
        table.insert(logs, 1, {
            at_ms = slm.uptime(),
            topic = topic,
            payload = payload,
        })
        if #logs > log_cap then
            table.remove(logs)
        end
    end

    for idx, def in ipairs(consumer_defs) do
        if consumers[idx].divider then
            goto continue_consumer
        end
        for _, topic in ipairs(def.topics) do
            local handle = slm.msg_subscribe(topic, function(rx_topic, data)
                consumers[idx].count = consumers[idx].count + 1
                consumers[idx].last_topic = rx_topic
                consumers[idx].last_msg = data
                stats.callbacks = stats.callbacks + 1
                for _, c in ipairs(consumers) do
                    c.arrow = false
                end
                consumers[idx].arrow = true
            end)
            if handle then
                handles[#handles + 1] = handle
            end
        end
        ::continue_consumer::
    end

    math.randomseed(slm.uptime())
    refill_topic_bag()
    local next_publish_ms = slm.uptime()

    while true do
        local now_ms = slm.uptime()
        if now_ms >= next_publish_ms then
            if #topic_bag == 0 then
                refill_topic_bag()
            end
            local topic = table.remove(topic_bag)
            local payload = string.format("msg-%03d", math.random(1000))
            local delivered = topic_consumers[topic] or 0
            slm.msg_publish(topic, payload)
            push_log(topic, string.format("%s (%d)", payload, delivered))
            stats.published = stats.published + 1
            stats.delivered = stats.delivered + delivered
            stats.last_publish_ms = now_ms
            next_publish_ms = now_ms + math.random(IPC_MIN_PUBLISH_MS, IPC_MAX_PUBLISH_MS)
        end

        -- Force subscriber callbacks through before rendering so the
        -- right pane stays in sync with the publish log.
        slm.msg_drain()
        if stats.callbacks > stats.delivered then
            stats.callbacks = stats.delivered
        end
        stats.pending = stats.delivered - stats.callbacks

        draw_ipc_dashboard(logs, consumers, now_ms, stats)

        local ch = slm.try_getc()
        if ch == " " or ch == "q" or ch == "Q" then
            break
        end

        slm.sleep(IPC_RENDER_MS)
    end

    for _, handle in ipairs(handles) do
        slm.msg_unsubscribe(handle)
    end

    P("")
    P("IPC dashboard stopped.")
    wait_for_key("Press any key to return to the menu")
end

local function run_smp_workers()
    if not require_binding("Pinned SMP Workers", "task_create") then
        return
    end
    if not require_binding("Pinned SMP Workers", "task_pin") then
        return
    end
    header("Pinned SMP Workers")
    P(string.format("  Launching %d Lua workers per secondary CPU for %d seconds.",
        SMP_DEMO_WORKERS_PER_CPU, SMP_DEMO_MS // 1000))
    P("  They stay runnable, yield cooperatively, and then exit on their own.")
    P("")

    local ids, cpus = launch_smp_workers(SMP_DEMO_MS)
    if #ids == 0 then
        P("  No workers launched.")
        return
    end

    P("")
    P("  Target CPUs:  " .. csv(cpus))
    P("  Worker IDs:   " .. csv(ids))
    P("  Use `top` from another shell now; these tasks will exit on their own.")
    P("")
    P("  Task table after spawn:")
    dump_tasks()

    local start = slm.uptime()
    while slm.uptime() - start < SMP_DEMO_MS do
        slm.sleep(250)
    end

    slm.sleep(100)
    P("")
    P("  Worker window finished.")
    P("  Task table after completion:")
    dump_tasks()
    wait_for_key("Press any key to return to the menu")
end

-- --- Menu --------------------------------------------------------------------

local menu = {
    {key = "1", label = "System overview",                 action = show_overview},
    {key = "2", label = "Live task table",                 action = show_tasks},
    {key = "3", label = "Spawn 3 built-in components*",    action = spawn_components},
    {key = "4", label = "IPC dashboard (live pub/sub)",    action = run_ipc_dashboard},
    {key = "5", label = "Pinned SMP workers x3 (8s)*",     action = run_smp_workers},
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
if slm.component_run == nil or slm.task_create == nil or slm.task_pin == nil then
    P("#  * marked options require lua-admin")
    P("#    launch with: lua-admin /mnt/files/multiproc_demo.lua")
    P("############################################################")
end

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
