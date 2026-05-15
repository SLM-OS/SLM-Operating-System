-- camera_flatfield.lua — flat-field calibration + per-mode MNIST sweep.
--
-- Workflow:
--   1. Point the camera at a uniformly-lit blank background. Run:
--        lua-admin /mnt/files/camera_flatfield.lua capture
--      This grabs one frame, stores its per-block green-channel sums
--      as the flat-field reference, and prints the gradient: 28×28
--      reference grid + min/max/center/mid-edge anchor values + the
--      "average variation by distance from center" curve.
--
--   2. Replace the background with the test digit. Run:
--        lua-admin /mnt/files/camera_flatfield.lua sweep [expected]
--      This captures one frame and runs MNIST inference under each
--      normalization mode (none, center anchor, max anchor, mid-edge
--      anchor) so you can see which one best matches `expected`.
--
--   3. Drop the reference and start over with a different scene:
--        lua-admin /mnt/files/camera_flatfield.lua clear
--
-- All three operations leave the imx219 capture session open so the
-- second + later captures don't re-burn an RCE channel slot.

local P = slm.print
local cmd = arg and arg[1] or "capture"

-- ------------------------------------------------------------------
-- Helpers
-- ------------------------------------------------------------------
local function dist_from_center(i, j)
    local di = i - 13.5
    local dj = j - 13.5
    return math.sqrt(di * di + dj * dj)
end

local function dump_gradient()
    local dim = slm.camera.FLATFIELD_DIM
    if not slm.camera.flatfield_valid() then
        P("flatfield: no reference stored")
        return
    end

    -- Print the 28×28 grid as one digit per cell, scaled by max.
    local mx = 0
    local mn = 1e9
    local sum = 0
    local count = 0
    for i = 0, dim - 1 do
        for j = 0, dim - 1 do
            local v = slm.camera.flatfield_get(i, j)
            if v > mx then mx = v end
            if v < mn then mn = v end
            sum = sum + v
            count = count + 1
        end
    end
    local mean = sum / count
    local center    = slm.camera.flatfield_anchor(slm.camera.FLATFIELD_CENTER)
    local mid_edge  = slm.camera.flatfield_anchor(slm.camera.FLATFIELD_MID_EDGE)
    local mx_anchor = slm.camera.flatfield_anchor(slm.camera.FLATFIELD_MAX)

    P(string.format(
        "flatfield: 28x28 grid  min=%d max=%d mean=%.0f  center=%d "
        .. "mid_edge=%d max_anchor=%d  vignette=%.1f%%",
        mn, mx, mean, center, mid_edge, mx_anchor,
        100.0 * (1.0 - mn / mx)))

    -- Brightness map: digit 0..9 representing 0%..100% of max.
    P("")
    P("    " .. string.rep("-", dim))
    for i = 0, dim - 1 do
        local row = {}
        for j = 0, dim - 1 do
            local v = slm.camera.flatfield_get(i, j)
            local n = math.floor(9 * v / mx + 0.5)
            row[#row + 1] = tostring(n)
        end
        P(string.format("%2d |%s|", i, table.concat(row)))
    end
    P("    " .. string.rep("-", dim))

    -- Average value vs distance from centre, binned.
    P("")
    P("Mean reference vs distance from centre:")
    local bin_w = 1.5
    local bins = {}
    for i = 0, dim - 1 do
        for j = 0, dim - 1 do
            local d = dist_from_center(i, j)
            local b = math.floor(d / bin_w)
            local v = slm.camera.flatfield_get(i, j)
            bins[b] = bins[b] or {sum = 0, n = 0, dmin = 99, dmax = 0}
            bins[b].sum = bins[b].sum + v
            bins[b].n = bins[b].n + 1
            if d < bins[b].dmin then bins[b].dmin = d end
            if d > bins[b].dmax then bins[b].dmax = d end
        end
    end
    -- Sorted bin ids
    local ids = {}
    for k in pairs(bins) do ids[#ids + 1] = k end
    table.sort(ids)
    for _, k in ipairs(ids) do
        local b = bins[k]
        local mean_b = b.sum / b.n
        local pct_of_max = 100.0 * mean_b / mx
        P(string.format("  d=[%.1f..%.1f]  n=%3d  mean=%6.0f  (%.1f%% of max)",
                        b.dmin, b.dmax, b.n, mean_b, pct_of_max))
    end
end

-- Render the 28×28 fp32 tensor as an ASCII heatmap, doubled
-- horizontally so it isn't squashed by the terminal's ~2:1 cell
-- aspect. Auto-normalises to the data's own (min, max) so tightly-
-- clustered output still shows internal structure.
local function render_heatmap(bytes, label, argmax, margin, expected)
    local ramp = " .:-=+*#%@"
    local pmin, pmax = math.huge, -math.huge
    local px = {}
    for i = 0, 783 do
        local v = string.unpack("<f", bytes, 1 + i * 4)
        px[i + 1] = v
        if v < pmin then pmin = v end
        if v > pmax then pmax = v end
    end
    local span = pmax - pmin
    if span < 1e-6 then span = 1.0 end

    local mark = ""
    if expected then
        if argmax == expected then
            mark = "  MATCH"
        else
            mark = string.format("  (expected %d)", expected)
        end
    end
    P("")
    P(string.format("[%-12s] px=[%.3f..%.3f]  argmax=%d  margin=%.3f%s",
                    label, pmin, pmax, argmax, margin, mark))
    local border = "  +" .. string.rep("-", 56) .. "+"
    P(border)
    for row = 0, 27 do
        local line = "  |"
        for col = 0, 27 do
            local v = px[1 + row * 28 + col]
            local lvl = math.floor((v - pmin) / span * 9 + 0.5)
            if lvl < 0 then lvl = 0 end
            if lvl > 9 then lvl = 9 end
            local ch = string.sub(ramp, lvl + 1, lvl + 1)
            line = line .. ch .. ch
        end
        P(line .. "|")
    end
    P(border)
end

local function capture_and_preprocess(mode, mode_name)
    local cam = slm.camera.open("imx219-0")
    if cam == nil then
        P(string.format("[%s] camera open failed", mode_name))
        return nil
    end
    local frame_id, w, h, bayer = cam:capture()
    if frame_id == nil then
        P(string.format("[%s] capture failed", mode_name))
        cam:close()
        return nil
    end
    local bytes, rc = slm.camera.preprocess_mnist(frame_id, w, h, bayer,
                                                  "imx219-0", true, true,
                                                  mode)
    cam:close()
    if bytes == nil then
        P(string.format("[%s] preprocess failed rc=%d", mode_name, rc or 0))
        return nil
    end
    return bytes
end

local function classify(bytes, mode_name)
    local mid = slm.model_load_mnist()
    if mid == nil or mid < 0 then
        P(string.format("[%s] model_load_mnist failed", mode_name))
        return nil
    end
    local logits, argmax = slm.model_infer_bytes(mid, bytes)
    if logits == nil then
        P(string.format("[%s] infer failed", mode_name))
        return nil
    end
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
    return argmax, top - second
end

local function infer_under(mode, mode_name, expected)
    local bytes = capture_and_preprocess(mode, mode_name)
    if bytes == nil then return end
    local argmax, margin = classify(bytes, mode_name)
    if argmax == nil then return end

    local pmin, pmax = math.huge, -math.huge
    for j = 0, 783 do
        local v = string.unpack("<f", bytes, 1 + j * 4)
        if v < pmin then pmin = v end
        if v > pmax then pmax = v end
    end

    local mark = ""
    if expected then
        if argmax == expected then
            mark = "  MATCH"
        else
            mark = string.format("  (expected %d)", expected)
        end
    end
    P(string.format("[%-12s] px=[%.3f..%.3f]  argmax=%d  margin=%.3f%s",
                    mode_name, pmin, pmax, argmax, margin, mark))
end

local function heatmap_under(mode, mode_name, expected)
    local bytes = capture_and_preprocess(mode, mode_name)
    if bytes == nil then return end
    local argmax, margin = classify(bytes, mode_name)
    if argmax == nil then return end
    render_heatmap(bytes, mode_name, argmax, margin, expected)
end

-- ------------------------------------------------------------------
-- Dispatch
-- ------------------------------------------------------------------
if cmd == "capture" then
    local rc = slm.camera.flatfield_capture("imx219-0")
    if rc ~= 0 then
        P(string.format("flatfield_capture failed rc=%d", rc))
        return
    end
    dump_gradient()
elseif cmd == "show" then
    dump_gradient()
elseif cmd == "clear" then
    slm.camera.flatfield_clear()
    P("flatfield: cleared")
elseif cmd == "sweep" then
    local expected = tonumber(arg and arg[2])
    if not slm.camera.flatfield_valid() then
        P("flatfield: no reference stored — capture one first with")
        P("  lua-admin /mnt/files/camera_flatfield.lua capture")
        return
    end
    if expected then
        P(string.format("Per-mode MNIST sweep (expected digit: %d)", expected))
    else
        P("Per-mode MNIST sweep")
    end
    P("")
    infer_under(slm.camera.NORMALIZE_NONE,        "none",       expected)
    infer_under(slm.camera.FLATFIELD_CENTER,      "center",     expected)
    infer_under(slm.camera.FLATFIELD_MAX,         "max",        expected)
    infer_under(slm.camera.FLATFIELD_MID_EDGE,    "mid-edge",   expected)
elseif cmd == "heatmap" then
    local expected = tonumber(arg and arg[2])
    if not slm.camera.flatfield_valid() then
        P("flatfield: no reference stored — capture one first with")
        P("  lua-admin /mnt/files/camera_flatfield.lua capture")
        return
    end
    if expected then
        P(string.format("Per-mode 28x28 preprocess heatmap (expected: %d)", expected))
    else
        P("Per-mode 28x28 preprocess heatmap")
    end
    heatmap_under(slm.camera.NORMALIZE_NONE,     "none",     expected)
    heatmap_under(slm.camera.FLATFIELD_CENTER,   "center",   expected)
    heatmap_under(slm.camera.FLATFIELD_MAX,      "max",      expected)
    heatmap_under(slm.camera.FLATFIELD_MID_EDGE, "mid-edge", expected)
else
    P("usage: camera_flatfield.lua {capture | show | clear | sweep N | heatmap N}")
    P("  capture   — grab one frame, store as flat-field reference,")
    P("              dump the gradient")
    P("  show      — re-print the stored reference's gradient")
    P("  clear     — drop the stored reference")
    P("  sweep N   — one-line MNIST result per normalization mode")
    P("  heatmap N — full 28x28 ASCII heatmap per mode (slower; useful")
    P("              when sweep results disagree on what the model sees)")
    P("              N is the expected digit; omit to skip MATCH marks.")
end
