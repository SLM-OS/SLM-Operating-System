-- SLM-OS IMX219 → MNIST Demo
--
-- End-to-end capstone walkthrough: capture one frame from the IMX219
-- camera on the Jetson Orin Nano, preprocess it through the MNIST
-- pipeline (1640×1232 RGGB → green-only → 28×28 fp32), and run
-- inference on the built-in MNIST classifier. Prints the captured
-- geometry, the 10-class logits vector, and the argmax digit
-- prediction.
--
-- The camera is not pointed at a hand-drawn digit by default, so the
-- argmax has no semantic meaning beyond "the full pipeline produced
-- a valid logits vector." The demo's purpose is to show every stage
-- — sensor power-up, CSI capture, VI DMA, MNIST preprocess, on-CPU
-- inference — running through a single Lua entry point.
--
-- Platforms without IMX219 hardware (QEMU, Pi 5, x86-64) get a clean
-- "camera not available" message and exit without running inference.
--
-- Usage:  lua-admin /mnt/files/demo_imx219_mnist.lua [camera-name]
--
-- camera-name  defaults to "imx219-0" — pass "mock" to exercise the
--              same pipeline against the embedded synthetic frame on
--              any platform.
--
-- Must be run from the admin Lua context — slm.model_load_mnist and
-- slm.model_infer_bytes are admin-only bindings (mutate the global
-- model table / run inference).

local P = slm.print

local function header(title)
    P("")
    P("============================================================")
    P("  " .. title)
    P("============================================================")
end

local function note(s) P("  " .. s) end

local cam_name = (arg and arg[1]) or "imx219-0"

header("SLM-OS IMX219 → MNIST Demo")
note("Camera:  " .. cam_name)
note("Script:  scripts/demo_imx219_mnist.lua")

-- ------------------------------------------------------------------
-- Step 1: open the camera + capture one frame
-- ------------------------------------------------------------------
header("1. Capture — slm.camera.open() + cam:capture()")

local t_open = slm.uptime()
local cam = slm.camera.open(cam_name)
if cam == nil then
    note("Camera open failed — no '" .. cam_name .. "' backend on this build.")
    note("On Jetson Orin Nano, kexec a JETSON_ORIN_NANO build first.")
    note("On other platforms try:  lua /mnt/files/demo_imx219_mnist.lua mock")
    return
end
note(string.format("camera.open: %d ms", slm.uptime() - t_open))

local t_cap = slm.uptime()
local frame_id, width, height, bayer = cam:capture()
if frame_id == nil then
    note("Capture failed — IMX219 power-up or CSI/VI bring-up returned an error.")
    note("Check kprintf output above for the specific stage that failed.")
    cam:close()
    return
end
note(string.format("capture:     %d ms  (frame_id=%d, %dx%d, bayer=%d)",
                   slm.uptime() - t_cap, frame_id, width, height, bayer))

-- ------------------------------------------------------------------
-- Step 2: MNIST preprocess (1640x1232 RGGB → 28x28 fp32)
-- ------------------------------------------------------------------
header("2. Preprocess — slm.camera.preprocess_mnist()")

local t_pre = slm.uptime()
local bytes, rc = slm.camera.preprocess_mnist(frame_id, width, height,
                                              bayer, cam_name)
if bytes == nil then
    note(string.format("preprocess_mnist failed: rc=%d", rc or 0))
    cam:close()
    return
end
note(string.format("preprocess:  %d ms  (%d bytes = %d fp32 samples)",
                   slm.uptime() - t_pre, #bytes, #bytes / 4))

-- ------------------------------------------------------------------
-- Step 3: load the built-in MNIST model
-- ------------------------------------------------------------------
header("3. Model — slm.model_load_mnist()")

local t_load = slm.uptime()
local mid = slm.model_load_mnist()
if mid == nil or mid < 0 then
    note("model_load_mnist failed — MNIST not embedded in this build.")
    cam:close()
    return
end
note(string.format("model_load:  %d ms  (model_id=%d)",
                   slm.uptime() - t_load, mid))

-- ------------------------------------------------------------------
-- Step 4: inference
-- ------------------------------------------------------------------
header("4. Inference — slm.model_infer_bytes()")

local t_inf = slm.uptime()
local logits, argmax = slm.model_infer_bytes(mid, bytes)
if logits == nil then
    note(string.format("model_infer_bytes failed: rc=%d", argmax or 0))
    cam:close()
    return
end
note(string.format("inference:   %d ms", slm.uptime() - t_inf))
note("")
note("Logits (class -> score):")
for i = 0, 9 do
    local f = string.unpack("<f", logits, 1 + i * 4)
    local marker = (i == argmax) and "  <-- argmax" or ""
    note(string.format("  %d: %8.4f%s", i, f, marker))
end
note("")
note(string.format("Predicted digit: %d", argmax))

cam:close()

-- ------------------------------------------------------------------
-- Footer
-- ------------------------------------------------------------------
header("Demo Complete")
note("Pipeline:  IMX219 sensor -> NVCSI -> VI -> kernel carveout")
note("           -> MNIST preprocess -> CPU inference -> argmax")
note("")
note("The argmax is only meaningful if the sensor is pointed at a")
note("hand-drawn digit on a uniform background. The demo's purpose")
note("is to show every stage running end-to-end through Lua.")
P("")
