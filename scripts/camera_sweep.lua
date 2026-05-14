-- camera_sweep.lua — sweep IMX219 gain + exposure to find a good
-- MNIST setting for the current scene.
--
-- For each (analog, digital, exposure) combo, the script:
--   1. Programmes the runtime registers via slm.camera.imx219_set_*
--   2. Captures one frame
--   3. Runs MNIST preprocess + inference
--   4. Prints a one-line summary: pixel range, argmax, top-1
--      margin over second-best (the larger, the more confident
--      the classifier is)
--
-- The user reads the table, decides which combo looks best, and
-- either applies it from the REPL with imx219_set_gain/exposure
-- or edits the IMX219 driver defaults to ship them.
--
-- Usage:  lua-admin /mnt/files/camera_sweep.lua [expected-digit]
--
-- expected-digit  optional 0..9. If given, marks each row with
--                 MATCH or (expected N) so the right answer is
--                 obvious at a glance.

local P = slm.print
local expected = tonumber(arg and arg[1])

-- ------------------------------------------------------------------
-- Trial table. Two regimes worth comparing: an EXPOSURE sweep at
-- the shipping low-gain default (4× analog, 1× digital — empirically
-- best for MNIST on the Jetson IMX219 + center-crop pipeline),
-- followed by a couple of high-gain entries for contrast. Re-run
-- with a custom grid if you need to drill in further.
-- ------------------------------------------------------------------
local grid = {
    -- analog, digital, exposure(lines)
    -- Independent A × D sweep at fixed E=1600.
    -- A encodes via 256/(256-A): 192=4×, 224=8×, 232=10.67× (max).
    -- D is fixed-point: 0x0100=1.0×, 0x0200=2.0×, 0x0400=4.0×.
    { 192, 0x0100, 0x0640 },   -- A=4×   D=1×    (baseline)
    { 192, 0x0200, 0x0640 },   -- A=4×   D=2×
    { 192, 0x0400, 0x0640 },   -- A=4×   D=4×
    { 232, 0x0100, 0x0640 },   -- A=11×  D=1×
    { 232, 0x0200, 0x0640 },   -- A=11×  D=2×    (prior shipping default)
    { 232, 0x0400, 0x0640 },   -- A=11×  D=4×
}

-- ------------------------------------------------------------------
-- Per-trial work
-- ------------------------------------------------------------------
local function trial(idx, analog, digital, exp_lines)
    slm.camera.imx219_set_gain(analog, digital)
    slm.camera.imx219_set_exposure(exp_lines)

    local cam = slm.camera.open("imx219-0")
    if cam == nil then
        P(string.format("[%d] camera open failed", idx))
        return
    end

    local frame_id, w, h, bayer = cam:capture()
    if frame_id == nil then
        P(string.format("[%d] capture failed", idx))
        cam:close()
        return
    end

    local bytes, rc = slm.camera.preprocess_mnist(frame_id, w, h, bayer,
                                                  "imx219-0")
    if bytes == nil then
        P(string.format("[%d] preprocess failed rc=%d", idx, rc or 0))
        cam:close()
        return
    end

    -- Pixel range across the 28×28 input
    local pmin, pmax = math.huge, -math.huge
    for j = 0, 783 do
        local v = string.unpack("<f", bytes, 1 + j * 4)
        if v < pmin then pmin = v end
        if v > pmax then pmax = v end
    end

    -- Inference
    local mid = slm.model_load_mnist()
    if mid == nil or mid < 0 then
        P(string.format("[%d] model_load_mnist failed (mid=%s)",
                        idx, tostring(mid)))
        cam:close()
        return
    end
    local logits, argmax = slm.model_infer_bytes(mid, bytes)
    if logits == nil then
        P(string.format("[%d] infer failed", idx))
        cam:close()
        return
    end

    -- Top-1 margin = top - second-top. A clear winner has margin
    -- > ~0.3; ambiguous predictions (close runners-up) read small.
    local top, second = -math.huge, -math.huge
    for j = 0, 9 do
        local v = string.unpack("<f", logits, 1 + j * 4)
        if v > top then
            second = top
            top = v
        elseif v > second then
            second = v
        end
    end
    local margin = top - second

    local mark = ""
    if expected then
        if argmax == expected then
            mark = "  MATCH"
        else
            mark = string.format("  (expected %d)", expected)
        end
    end

    P(string.format("[%d] A=%3d D=0x%04x E=%4d  px=[%.3f..%.3f] "
                    .. "argmax=%d margin=%.3f%s",
                    idx, analog, digital, exp_lines,
                    pmin, pmax, argmax, margin, mark))

    cam:close()
end

-- ------------------------------------------------------------------
-- Run
-- ------------------------------------------------------------------
P("IMX219 gain/exposure sweep — " .. #grid .. " trials")
if expected then
    P(string.format("Expected digit: %d", expected))
end
P("")
P("     A=analog gain  D=digital gain (fixed-point)  E=exposure lines")
P("     px[min..max]   = preprocess-output pixel range (closer to [0,1]")
P("                      = closer to MNIST's training distribution)")
P("     margin         = top logit − second-top  (larger = more confident)")
P("")

-- Remember the operator's starting values so the sweep doesn't
-- silently leave the sensor stuck on the last grid entry's setting.
local start_a, start_d = slm.camera.imx219_get_gain()
local start_e = slm.camera.imx219_get_exposure()

for i, t in ipairs(grid) do
    trial(i, t[1], t[2], t[3])
end

-- Restore the gain/exposure that were in effect before the sweep.
slm.camera.imx219_set_gain(start_a, start_d)
slm.camera.imx219_set_exposure(start_e)

P("")
P(string.format("Sweep complete. Gain/exposure restored to "
                .. "(A=%d D=0x%04x E=%d).",
                start_a, start_d, start_e))
P("To apply a specific row from the REPL:")
P("  slm.camera.imx219_set_gain(<analog>, <digital>)")
P("  slm.camera.imx219_set_exposure(<lines>)")
P("then run /mnt/files/camera_heatmap.lua to see the resulting image.")
