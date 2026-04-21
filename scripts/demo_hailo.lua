-- SLM-OS Hailo NPU Demo
--
-- Walks through the Hailo-8L AI HAT+ integration from Lua:
--
--   1. Probe the device via slm.hailo.status()
--   2. Load a HEF binary from the VFS via slm.hailo.load(path)
--   3. Run a single inference via slm.hailo.infer(handle, input)
--   4. Report slot accounting after the model is resident
--
-- Every call degrades safely when the Hailo backend is not registered
-- (QEMU, Pi 5 without the AI HAT+, other platforms) — status() reports
-- available=false and load()/infer() return nil. The script reports
-- what it saw and exits cleanly either way so the capstone walkthrough
-- can live alongside the other demos without platform-specific shell
-- routing.
--
-- Usage:  lua /mnt/files/demo_hailo.lua [hef-path [iterations]]
--
-- hef-path  defaults to /mnt/files/mobilenet_v1.hef — supply a different
--           path to load any other compiled HEF staged on the VFS.
-- iterations  defaults to 100 — number of bench loop passes. 0 skips the
--             benchmark and only runs a single inference. A real-world
--             capstone demo uses ≥500 for meaningful percentiles.

local P = slm.print
local yield = slm.yield

local function header(title)
    P("")
    P("============================================================")
    P("  " .. title)
    P("============================================================")
end

local function note(s) P("  " .. s) end
local function subhead(s) P("  -- " .. s) end

-- Simple argv fallback: arg[] is Lua 5.4's standard vector and works
-- under dofile / lua scripts; treat it defensively in case the host
-- shell passes no args at all.
local hef_path = (arg and arg[1]) or "/mnt/files/mobilenet_v1.hef"
local bench_iters = tonumber((arg and arg[2]) or 100) or 100

header("SLM-OS Hailo NPU Demo")
note("Target device: hailo-8L NPU (AI HAT+ on Pi 5)")
note("Script:        scripts/demo_hailo.lua")
note("HEF path:      " .. hef_path)

-- ------------------------------------------------------------------
-- Step 1: status probe
-- ------------------------------------------------------------------
header("1. Device probe — slm.hailo.status()")

local st = slm.hailo.status()
if type(st) ~= "table" then
    note("ERROR: slm.hailo.status() did not return a table — aborting")
    return
end

if not st.available then
    note("Hailo backend: NOT present on this build/platform")
    note("")
    note("This is expected on QEMU and on Pi 5 builds where the AI HAT+")
    note("is not detected at boot. The slm.hailo.* bindings still exist")
    note("so scripts remain portable, but load/infer will return nil.")
    note("Exiting demo without exercising load/infer.")
    return
end

note("Hailo backend: available")
note(string.format("  name:          %s", st.name))
note(string.format("  slots in use:  %d / %d", st.slots_in_use, st.slots_max))

-- ------------------------------------------------------------------
-- Step 2: load the HEF
-- ------------------------------------------------------------------
header("2. Load HEF — slm.hailo.load(path)")

local handle = slm.hailo.load(hef_path)
if handle == nil then
    note("Failed to load HEF from " .. hef_path)
    note("Common causes:")
    note("  - File not staged on the VFS (write it with `write " .. hef_path .. " ...`)")
    note("  - HEF header mismatch or file truncated")
    note("  - All four NPU slots already in use")
    return
end

note(string.format("Model loaded. handle = %d", handle))
yield()

local st_loaded = slm.hailo.status()
if st_loaded and st_loaded.available then
    note(string.format("  slots in use now: %d / %d",
        st_loaded.slots_in_use, st_loaded.slots_max))
end

-- ------------------------------------------------------------------
-- Step 3: run one inference
-- ------------------------------------------------------------------
header("3. Single inference — slm.hailo.infer(handle, input)")

-- slm.hailo.infer rejects any input whose byte length doesn't match the
-- model's declared input tensor. The backend exposes the size via the
-- handle, but there's no Lua getter yet — probing by doubling until
-- accept keeps the demo self-contained without hard-coding per-model
-- shapes.
local function find_input_size()
    local probe_sizes = {
        1 * 1,
        28 * 28,
        224 * 224,
        224 * 224 * 3,
        300 * 300,
        300 * 300 * 3,
    }
    for _, sz in ipairs(probe_sizes) do
        local zeros = string.rep("\0", sz)
        -- The backend returns nil for mismatched sizes but runs the model
        -- for a match. Capture both: an accepted size gives us a usable
        -- output.
        local out = slm.hailo.infer(handle, zeros)
        if out ~= nil then
            return sz, out
        end
    end
    return nil, nil
end

local in_size, out_str = find_input_size()
if in_size == nil then
    note("ERROR: could not determine input size from standard probe set")
    note("The model's input tensor shape may be unusual; edit")
    note("demo_hailo.lua's probe list to include the expected byte count.")
    return
end

note(string.format("Input bytes accepted: %d", in_size))
note(string.format("Output bytes:         %d", #out_str))

-- Print a short hexdump of the first bytes of the output so the reviewer
-- sees real NPU output (not just a length count) without flooding the
-- serial console on classifier outputs with 1000-class logits.
local preview = {}
local preview_len = math.min(#out_str, 16)
for i = 1, preview_len do
    table.insert(preview, string.format("%02x", string.byte(out_str, i)))
end
note("Output[0..15]:        " .. table.concat(preview, " "))

-- Also report argmax + its value when the output looks like a 1-D
-- classifier. Decoding INT8 logits as signed bytes matches Hailo's on-
-- device quantization (asymmetric zero-point is ignored here — this is
-- a rough argmax, not a calibrated probability).
if #out_str >= 2 then
    local best_idx, best_val = 0, -128
    for i = 1, #out_str do
        local b = string.byte(out_str, i)
        local s = (b >= 128) and (b - 256) or b  -- INT8 decode
        if s > best_val then best_val = s; best_idx = i - 1 end
    end
    note(string.format("Argmax (INT8):        class=%d value=%d",
        best_idx, best_val))
end

-- ------------------------------------------------------------------
-- Step 4: benchmark loop — live FPS / latency percentiles
-- ------------------------------------------------------------------
if bench_iters > 0 then
    header(string.format("4. Benchmark — %d inferences", bench_iters))
    note("Running back-to-back inferences with slm.hailo.infer()")
    note("Rolling stats printed every 10 iterations.")
    note("")

    local input = string.rep("\0", in_size)
    local samples = {}
    local t_start = slm.uptime()
    local t_last_print = t_start
    local last_printed_i = 0
    local errors = 0
    local print_every = math.max(1, math.floor(bench_iters / 10))

    for i = 1, bench_iters do
        local t0 = slm.uptime()
        local out = slm.hailo.infer(handle, input)
        local t1 = slm.uptime()
        if out == nil then
            errors = errors + 1
        else
            samples[#samples + 1] = t1 - t0
        end
        if i % print_every == 0 or i == bench_iters then
            local window_ms = math.max(1, t1 - t_last_print)
            local window_iters = i - last_printed_i
            local fps = math.floor((window_iters * 1000) / window_ms)
            P(string.format("    [%4d/%d]  window: %d iters in %d ms  (%d FPS)",
                i, bench_iters, window_iters, window_ms, fps))
            t_last_print = t1
            last_printed_i = i
        end
    end

    local t_end = slm.uptime()
    local total_ms = t_end - t_start
    note("")
    subhead("Benchmark summary:")
    note(string.format("  Total:         %d iterations in %d ms",
        bench_iters, total_ms))
    note(string.format("  Successful:    %d  (errors: %d)",
        #samples, errors))
    if total_ms > 0 then
        note(string.format("  Throughput:    %d FPS",
            math.floor((#samples * 1000) / total_ms)))
    end
    if #samples > 0 then
        table.sort(samples)
        local sum = 0
        for _, v in ipairs(samples) do sum = sum + v end
        local n = #samples
        local p50 = samples[math.max(1, math.floor(n * 0.50))]
        local p95 = samples[math.max(1, math.floor(n * 0.95))]
        local p99 = samples[math.max(1, math.floor(n * 0.99))]
        note(string.format("  Latency (ms):  min=%d p50=%d p95=%d p99=%d max=%d avg=%d",
            samples[1], p50, p95, p99, samples[n],
            math.floor(sum / n)))
    end
end

-- ------------------------------------------------------------------
-- Step 5: closing summary
-- ------------------------------------------------------------------
header("Demo Complete")
note("Hailo NPU path exercised from Lua:")
note("  - slm.hailo.status()  — device probe")
note("  - slm.hailo.load()    — HEF staging + firmware context-switch")
note("  - slm.hailo.infer()   — boundary DMA in, NPU compute, DMA out")
note("")
note("For narration: this path is end-to-end userspace Lua on a bare-metal")
note("kernel — no Linux, no HailoRT driver. The firmware protocol (ACTIVATION,")
note("PRELIMINARY, DYNAMIC context-switch sequence) runs from SLM-OS's")
note("hailo_cs_builder / hailo_cs_translator modules.")
