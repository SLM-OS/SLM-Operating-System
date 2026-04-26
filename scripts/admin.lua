-- admin.lua - SLM-OS admin & telemetry suite TUI (M6)
--
-- Seven-page operator console rendered over the current shell session
-- (UART or telnet). Pages walk the live kernel via the `slm.*` bindings
-- added in M1-M5: scheduler decision rate + latency, eviction stats,
-- GPU consumer toggles, model engine registry, telemetry feed.
--
-- Spec: docs/specs/admin-telemetry-suite.md §11.
--
-- Usage:  admin           (shell)
--         lua /scripts/admin.lua
--
-- Keys:
--   1-7    switch page
--   r      force refresh
--   q      quit
--   ?      help (overlay)
--
-- Refresh model: 1 Hz background loop using slm.sleep(1000) between
-- paints; key reads use slm.try_getc() so the loop is non-blocking.
-- Page bodies are intentionally short (≤ 18 rows of body in an 80×24
-- terminal) so a full repaint fits in one telnet packet.

local P = slm.print

local PAGES = {
    "Overview", "Tasks", "Sched", "Eviction",
    "Models", "Telemetry", "REPL",
}
local current_page = 1
local running = true
local last_refresh_ms = 0
local frame_seq = 0

-- ANSI helpers ------------------------------------------------------------

local function esc(s) P("\27[" .. s) end
local function clear_screen() P("\27[2J\27[H") end
local function home() P("\27[H") end
local function bold(s) return "\27[1m" .. s .. "\27[0m" end
local function reverse(s) return "\27[7m" .. s .. "\27[0m" end

-- Padding helpers ---------------------------------------------------------

local function pad_right(s, n)
    s = tostring(s)
    if #s >= n then return string.sub(s, 1, n) end
    return s .. string.rep(" ", n - #s)
end

local function fmt_int(n)
    if not n then return "?" end
    return tostring(math.floor(n))
end

local function fmt_bool(b)
    if b == true  then return "ON " end
    if b == false then return "off" end
    return "?  "
end

local function fmt_ns_short(ns)
    if not ns or ns == 0 then return "    -   " end
    if ns < 1000        then return string.format("%4d ns", ns)         end
    if ns < 1000*1000   then return string.format("%4d us", ns/1000)    end
    if ns < 1000*1000*1000 then return string.format("%4d ms", ns/1000/1000) end
    return string.format("%4d s ", ns/1000/1000/1000)
end

-- Header ------------------------------------------------------------------

local function render_header(cols)
    local title = "SLM-OS Admin"
    local up_s = string.format("uptime %ds", math.floor(slm.uptime() / 1000))
    local frame = string.format("f%d", frame_seq)
    -- Tabs: 1 Overview  2 Tasks  3 Sched ...
    local tabs = {}
    for i, name in ipairs(PAGES) do
        local entry = string.format("%d %s", i, name)
        if i == current_page then entry = reverse(entry) end
        tabs[#tabs + 1] = entry
    end
    P("+" .. string.rep("-", cols - 2) .. "+")
    P("| " .. bold(title) .. "  " .. up_s .. "  " .. frame ..
      string.rep(" ", math.max(0, cols - #title - #up_s - #frame - 8)) .. " |")
    P("| " .. table.concat(tabs, "  ") ..
      string.rep(" ", math.max(0, cols - 4 - 8 * #PAGES)) .. " |")
    P("+" .. string.rep("-", cols - 2) .. "+")
end

local function render_footer(cols)
    P("+" .. string.rep("-", cols - 2) .. "+")
    P("| q quit  r refresh  1-7 page                                                |")
end

-- Page bodies -------------------------------------------------------------

local function page_overview()
    P("")
    P(bold("System overview"))
    local mem = slm.mem_stats() or {}
    local total_kb = (mem.used_kb or 0) + (mem.free_kb or 0)
    local sched = slm.sched_policy()
    local ev    = slm.eviction_policy()
    local gpu   = slm.gpu_use_status()
    P(string.format("  cpu_count        %d", slm.cpu_count() or 0))
    P(string.format("  memory           %d KB used / %d KB total",
                    mem.used_kb or 0, total_kb))
    P(string.format("  sched policy     %s", tostring(sched or "?")))
    P(string.format("  eviction policy  %s", tostring(ev or "?")))
    P(string.format("  GPU ready        %s", fmt_bool(gpu and gpu.gpu_ready)))
    P(string.format("  GPU consumers    sched=%s  eviction=%s  inference=%s",
                    fmt_bool(gpu and gpu.sched),
                    fmt_bool(gpu and gpu.eviction),
                    fmt_bool(gpu and gpu.inference)))
    local tel = slm.telemetry_stats() or {}
    P(string.format("  telemetry        %d events published (eviction=%d, inference=%d)",
                    tel.total_published or 0,
                    (tel.eviction and tel.eviction.published) or 0,
                    (tel.inference and tel.inference.published) or 0))
end

local function page_tasks()
    P("")
    P(bold("Tasks"))
    local tasks = slm.tasks() or {}
    P(string.format("  %-4s %-20s %-10s %s", "id", "name", "state", "cpu"))
    for _, t in ipairs(tasks) do
        P(string.format("  %-4d %-20s %-10s %d",
                        t.id or 0, t.name or "?", t.state or "?", t.cpu or -1))
    end
end

local function page_sched()
    P("")
    P(bold("Scheduler"))
    local r = slm.sched_decision_rate()
    if not r then
        P("  (AI scheduler not active — only ai_mlp/ai_ppo/ai_hailo populate stats)")
        P(string.format("  active policy: %s", tostring(slm.sched_policy() or "?")))
        return
    end
    P(string.format("  policy           %s", r.policy or "?"))
    P(string.format("  decisions/s      %d", r.decisions_per_s or 0))
    P(string.format("  fallbacks/s      %d", r.fallback_rate   or 0))
    P(string.format("  p50 / p90 / p99  %s / %s / %s",
                    fmt_ns_short(r.p50_latency_ns),
                    fmt_ns_short(r.p90_latency_ns),
                    fmt_ns_short(r.p99_latency_ns)))
    P(string.format("  total decisions  %d   total fallbacks %d",
                    r.total_decisions or 0, r.total_fallbacks or 0))
    P(string.format("  total ns         %d", r.total_ns or 0))
end

local function page_eviction()
    P("")
    P(bold("Eviction"))
    local r = slm.eviction_decision_rate() or {}
    P(string.format("  decisions/s      %d", r.decisions_per_s or 0))
    P(string.format("  fallbacks/s      %d", r.fallback_rate   or 0))
    P(string.format("  p50 / p90 / p99  %s / %s / %s",
                    fmt_ns_short(r.p50_latency_ns),
                    fmt_ns_short(r.p90_latency_ns),
                    fmt_ns_short(r.p99_latency_ns)))
    P(string.format("  total decisions  %d   total fallbacks %d",
                    r.total_decisions or 0, r.total_fallbacks or 0))
end

local function page_models()
    P("")
    P(bold("Model engines"))
    local engines = slm.model_engines() or {}
    P(string.format("  %-8s %-9s %s", "name", "state", "summary"))
    for _, e in ipairs(engines) do
        P(string.format("  %-8s %-9s %s",
                        e.name or "?", e.state or "?", e.summary or ""))
    end
    P("")
    P(bold("Inference"))
    local r = slm.inference_rate() or {}
    P(string.format("  calls/s          %d", r.calls_per_s or 0))
    P(string.format("  errors/s         %d", r.errors_per_s or 0))
    P(string.format("  p50 / p99        %s / %s",
                    fmt_ns_short(r.p50_latency_ns),
                    fmt_ns_short(r.p99_latency_ns)))
    P(string.format("  total calls      %d   errors %d",
                    r.total_calls or 0, r.total_errors or 0))
end

local function page_telemetry()
    P("")
    P(bold("Telemetry feed"))
    local s = slm.telemetry_stats() or {}
    P(string.format("  %-12s published %d", "tel.evi",
                    (s.eviction and s.eviction.published) or 0))
    P(string.format("  %-12s published %d", "tel.inf",
                    (s.inference and s.inference.published) or 0))
    P(string.format("  total            %d", s.total_published or 0))
    P("")
    P("  Subscribe pattern from another shell:")
    P("    lua -e 'slm.telemetry_subscribe(\"tel.*\", function(t,d) print(t,d) end)'")
end

local function page_repl()
    P("")
    P(bold("REPL"))
    P("  Press 'q' to leave the TUI and run `lua` from the shell prompt.")
    P("  Inline expressions are easier to test from a shell session than")
    P("  inside this TUI's input loop, so the M6 REPL page is intentionally")
    P("  a launching pad rather than an embedded REPL.")
end

local PAGE_FNS = {
    page_overview, page_tasks, page_sched, page_eviction,
    page_models, page_telemetry, page_repl,
}

-- Main loop ---------------------------------------------------------------

local function paint()
    frame_seq = frame_seq + 1
    local ts = slm.term_size() or {}
    local cols = math.max(60, ts.cols or 80)

    clear_screen()
    render_header(cols)
    PAGE_FNS[current_page]()
    -- Pad body to a stable height so the footer always sits at the
    -- bottom-of-screen region. 16 rows of body + 5 rows of header/footer
    -- ~ 21 rows total; resize-tolerant in larger terms.
    render_footer(cols)
end

local function dispatch_key(c)
    if c == nil or c < 0 then return end
    if c == string.byte('q') or c == string.byte('Q') or c == 3 then
        running = false
    elseif c == string.byte('r') or c == string.byte('R') then
        -- force a repaint by zeroing the throttle
        last_refresh_ms = 0
    elseif c >= string.byte('1') and c <= string.byte('7') then
        current_page = c - string.byte('0')
        last_refresh_ms = 0
    end
end

paint()
last_refresh_ms = slm.uptime()
while running do
    -- Drain any pending keys.
    while true do
        local c = slm.try_getc()
        if not c or c < 0 then break end
        dispatch_key(c)
    end
    -- Repaint at most once per second, or immediately on a forced refresh.
    local now_ms = slm.uptime()
    if (now_ms - last_refresh_ms) >= 1000 then
        paint()
        last_refresh_ms = now_ms
    end
    slm.sleep(100)
end

clear_screen()
P("admin: bye.")
