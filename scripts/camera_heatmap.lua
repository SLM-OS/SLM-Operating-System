-- camera_heatmap.lua — show what the model actually sees.
--
-- Captures one frame, runs it through the MNIST preprocess pipeline,
-- and renders the resulting 28x28 fp32 tensor as an ASCII heatmap.
-- Then (under lua-admin) runs the MNIST classifier and prints the
-- top-3 logits side-by-side with the image.
--
-- Purpose: when the MNIST demo returns the same class for every
-- scene, the heatmap is the fastest way to tell whether the bug is
-- in capture/preprocess (image is mostly uniform regardless of
-- scene) or whether the model is just out-of-distribution on real
-- photos (image looks correct, but logit magnitudes are tiny and
-- argmax collapses to whichever class the network biases toward
-- with near-uniform input).
--
-- Usage:  lua-admin /mnt/files/camera_heatmap.lua [camera-name]
--   or:   lua       /mnt/files/camera_heatmap.lua [camera-name]
--
-- Under plain `lua` the heatmap renders but inference is skipped
-- (model_load_mnist / model_infer_bytes are admin-only).

local P = slm.print
local cam_name = (arg and arg[1]) or "imx219-0"

-- ------------------------------------------------------------------
-- Capture + preprocess
-- ------------------------------------------------------------------
local cam = slm.camera.open(cam_name)
if cam == nil then
    P("camera.open failed for '" .. cam_name .. "'")
    return
end

local t0 = slm.uptime()
local frame_id, w, h, bayer = cam:capture()
local t_cap = slm.uptime() - t0
if frame_id == nil then
    P("cam:capture failed")
    cam:close()
    return
end

t0 = slm.uptime()
local bytes, rc = slm.camera.preprocess_mnist(frame_id, w, h, bayer, cam_name)
local t_pre = slm.uptime() - t0
if bytes == nil then
    P(string.format("preprocess_mnist failed: rc=%d", rc or 0))
    cam:close()
    return
end

-- ------------------------------------------------------------------
-- Decode the 784 fp32 samples + collect range stats
-- ------------------------------------------------------------------
local N = 28 * 28
local px = {}
local pmin, pmax, psum = math.huge, -math.huge, 0.0
for i = 0, N - 1 do
    local v = string.unpack("<f", bytes, 1 + i * 4)
    px[i + 1] = v
    if v < pmin then pmin = v end
    if v > pmax then pmax = v end
    psum = psum + v
end
local pmean = psum / N

P(string.format("Camera: %s   capture: %dms   preprocess: %dms",
                cam_name, t_cap, t_pre))
P(string.format("Pixel range: min=%.4f  max=%.4f  mean=%.4f",
                pmin, pmax, pmean))
P("")

-- ------------------------------------------------------------------
-- Render heatmap. Each pixel is doubled horizontally so the 28x28
-- image isn't squashed by the terminal's ~2:1 character cell
-- aspect ratio. Normalise to the data's own (min, max) so a tightly
-- clustered input still shows internal structure rather than a flat
-- grey square.
-- ------------------------------------------------------------------
local ramp = " .:-=+*#%@"  -- 10 levels, dark to bright
local span = pmax - pmin
if span < 1e-6 then span = 1.0 end  -- guard against a perfectly flat frame

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

cam:close()

-- ------------------------------------------------------------------
-- Optional MNIST inference (admin-only bindings)
-- ------------------------------------------------------------------
if slm.model_load_mnist == nil or slm.model_infer_bytes == nil then
    P("")
    P("(Run under `lua-admin` to also see MNIST logits.)")
    return
end

local mid = slm.model_load_mnist()
if mid == nil or mid < 0 then return end
local logits, argmax = slm.model_infer_bytes(mid, bytes)
if logits == nil then return end

local scored = {}
for i = 0, 9 do
    scored[i + 1] = { i, string.unpack("<f", logits, 1 + i * 4) }
end
table.sort(scored, function(a, b) return a[2] > b[2] end)

P("")
P(string.format("Inference: argmax=%d", argmax))
P(string.format("  top-3:  %d=%.4f   %d=%.4f   %d=%.4f",
                scored[1][1], scored[1][2],
                scored[2][1], scored[2][2],
                scored[3][1], scored[3][2]))
P(string.format("  range:  max=%.4f  min=%.4f  spread=%.4f",
                scored[1][2], scored[10][2], scored[1][2] - scored[10][2]))
P("")
P("If the heatmap clearly shows your subject but spread is small")
P("(< ~1.0), the bug is OOD input — MNIST is trained on centered")
P("black-on-white handwritten digits and won't classify photos.")
