# Tutorial: Creating an SLM-OS Component

Step-by-step guide for building a new built-in component in SLM-OS. This tutorial uses the existing `sensor_monitor` and `digit_classifier` components as reference implementations.

**Prerequisites:** Familiarity with the SLM-OS build system and shell. See `docs/building.md` for build instructions.

---

## Overview

An SLM-OS component is a self-contained C function that runs as a kernel task. Components communicate through topic-based publish/subscribe messaging and can optionally invoke the Rust inference engine for AI workloads. Each component follows a standard lifecycle managed by the Rust component registry.

The component system is designed for edge/IoT scenarios: small, single-purpose services that subscribe to data topics, process input, and publish results.

---

## Step 1: Define the Entry Function

Every component starts with an entry function. The function signature is always:

```c
static void my_component_entry(void *arg)
```

The `arg` parameter carries the component index (cast from `uintptr_t`). The entry function must manage its own lifecycle by setting state transitions.

### Minimal Skeleton

```c
static void my_component_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;

    /* 1. Transition to RUNNING */
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    /* 2. Component logic goes here */
    uart_puts("[my_component] Running\r\n");

    /* 3. Transition to TERMINATING before returning */
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}
```

The cleanup callback registered by `component_run()` handles the final transition to `COMPONENT_UNLOADED` and unsubscribes from all topics after the task exits.

### Lifecycle States

| State | When Set | By Whom |
|-------|----------|---------|
| `COMPONENT_INITIALIZING` | Before task starts | `component_run()` |
| `COMPONENT_RUNNING` | Entry function begins | Entry function |
| `COMPONENT_TERMINATING` | Entry function is about to return | Entry function |
| `COMPONENT_UNLOADED` | After task exits | Cleanup callback |

---

## Step 2: Register in the Built-in Table

Add the component to the `builtin_components[]` array in `kernel/src/component_runtime.c`:

```c
static const struct builtin_component builtin_components[] = {
    /* ... existing components ... */
    {
        .name = "my_component",
        .version = "1.0",
        .description = "Brief description of purpose",
        .type = COMPONENT_TYPE_SERVICE,
        .priority = COMPONENT_PRIORITY_IDLE,
        .entry = my_component_entry,
    },
};
```

### Field Reference

| Field | Type | Notes |
|-------|------|-------|
| `name` | `const char *` | Unique name, used with `component run <name>` |
| `version` | `const char *` | Version string (e.g., "1.0") |
| `description` | `const char *` | Shown in `component builtins` output |
| `type` | `uint8_t` | `COMPONENT_TYPE_SERVICE`, `_DRIVER`, or `_APPLICATION` |
| `priority` | `uint8_t` | `COMPONENT_PRIORITY_IDLE` through `_CRITICAL` |
| `entry` | `component_entry_t` | Pointer to the entry function |

### Priority Guidance

- **`COMPONENT_PRIORITY_IDLE` (0):** For most components. Shares the CPU with the shell via round-robin scheduling.
- **`COMPONENT_PRIORITY_NORMAL` (4):** For latency-sensitive components (e.g., inference).
- **`COMPONENT_PRIORITY_HIGH` (6) / `_CRITICAL` (7):** Reserved for real-time or safety-critical tasks.

---

## Step 3: Subscribe to Topics and Receive Messages

Components communicate through the topic-based message router. The standard pattern involves subscribing to one or more topics, then polling in a loop.

### Subscribe

```c
msg_router_subscribe("/sensors/data", comp_idx);
```

### Polling Loop with Timeout

The polling loop uses `yield()` for cooperative scheduling and `pit_ticks` for timeout management. The `pit_ticks` counter increments at 100 Hz (10 ms per tick).

```c
extern volatile uint64_t pit_ticks;
uint64_t timeout_tick = pit_ticks + 3000;  /* 30 seconds at 100 Hz */

while (pit_ticks < timeout_tick) {
    char topic_buf[16];
    const char *data = msg_router_receive(comp_idx, topic_buf);

    if (data) {
        /* Process the message */
        uart_printf("[my_component] Received: %s\r\n", data);

        /* IMPORTANT: Acknowledge after processing */
        msg_router_ack(comp_idx);

        /* Reset timeout on activity */
        timeout_tick = pit_ticks + 3000;
    } else {
        yield();  /* Cooperative yield -- lets other tasks run */
    }
}
```

**Key points:**

- `msg_router_receive()` returns a pointer to the message data, or `NULL` if no message is pending. The `topic_buf` receives the topic name.
- `msg_router_ack()` must be called after processing to consume the message and free the slot.
- `yield()` is essential in the else branch. Without it, the component would busy-wait and starve other tasks.
- The timeout prevents the component from running indefinitely if no messages arrive.

### Real-World Example: sensor_monitor

The `sensor_monitor` component parses integer values from messages and checks a threshold:

```c
if (data) {
    /* Parse integer value (simple atoi) */
    int value = 0;
    for (int i = 0; data[i] >= '0' && data[i] <= '9' && i < 10; i++) {
        value = value * 10 + (data[i] - '0');
    }
    msg_router_ack(comp_idx);

    /* Threshold check */
    if (value > 50) {
        /* Publish alert */
        msg_router_publish("/alerts/threshold", "ALERT: threshold exceeded");
    }
    timeout_tick = pit_ticks + 3000;
} else {
    yield();
}
```

---

## Step 4: Publish Messages

Publishing sends a message to all subscribers of a given topic:

```c
msg_router_publish("/output/result", "class=7");
```

Published messages are delivered asynchronously. Each subscriber receives a copy in its per-component mailbox slot. If a subscriber has not acknowledged its previous message, the new message overwrites it (latest-wins semantics).

### Message Router API Summary

| Function | Purpose |
|----------|---------|
| `msg_router_subscribe(topic, idx)` | Subscribe component `idx` to `topic` |
| `msg_router_receive(idx, topic_out)` | Check for pending message (returns data or NULL) |
| `msg_router_ack(idx)` | Acknowledge and consume the current message |
| `msg_router_publish(topic, data)` | Publish a message to all subscribers of `topic` |
| `msg_router_unsubscribe_all(idx)` | Remove all subscriptions for component `idx` |

---

## Step 5: Using Inference from a Component

Components that need ML inference use the FFI bridge to the Rust inference engine. The model must be loaded before the component starts.

### Find the Model

```c
int model_idx = rust_model_find("mnist");
if (model_idx < 0) {
    uart_puts("[my_component] ERROR: Model not loaded\r\n");
    uart_puts("[my_component] Load with: model load /mnt/files/mnist.onnx\r\n");
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
    return;
}
```

`rust_model_find()` searches the model registry by name and returns the registry index, or -1 if not found.

### Run Inference

For integer-class results (most common for classification components):

```c
int predicted_class = rust_infer_classify((uint32_t)model_idx);
if (predicted_class >= 0) {
    uart_printf("[my_component] Predicted class: %d\r\n", predicted_class);
}
```

`rust_infer_classify()` runs the full inference pipeline with a zero-valued input buffer and returns the argmax of the output tensor as an integer. This avoids floating-point handling in kernel-mode C code.

### Real-World Example: digit_classifier

The `digit_classifier` combines subscription, inference, and publishing:

```c
if (data) {
    msg_router_ack(comp_idx);

    /* Run inference */
    int predicted_class = rust_infer_classify((uint32_t)model_idx);

    if (predicted_class >= 0) {
        /* Build result string and publish */
        char result[16];
        result[0] = 'c'; result[1] = 'l'; result[2] = 'a';
        result[3] = 's'; result[4] = 's'; result[5] = '=';
        result[6] = '0' + (char)(predicted_class % 10);
        result[7] = '\0';

        msg_router_publish("/output/class", result);
    }
    timeout_tick = pit_ticks + 3000;
} else {
    yield();
}
```

### Inference API Summary

| Function | Purpose |
|----------|---------|
| `rust_model_find(name)` | Find model by name, returns index or -1 |
| `rust_infer_classify(index)` | Run inference, return argmax class |
| `rust_infer(index, in, in_len, out, out_len)` | Run inference with custom FP32 input/output |

---

## Step 6: Build and Test

### Build

From the project root:

```bash
make kernel                  # Build for QEMU (default)
make kernel PLATFORM=RASPI5  # Build for Raspberry Pi 5
```

No separate compilation step is needed for components -- they are compiled as part of the kernel since they reside in `kernel/src/component_runtime.c`.

### Test in QEMU

```bash
make run
```

At the SLM-OS shell:

```
SLM-OS> component builtins
SLM-OS> component run my_component
SLM-OS> msg send /sensors/data 75
SLM-OS> component list
```

### Test with Inference

To test a component that uses the inference engine:

```
SLM-OS> model load /mnt/files/mnist.onnx
SLM-OS> component run digit_classifier
SLM-OS> msg send /input/digits test
SLM-OS> msg list
```

### Automated Testing

The project test suite includes component integration tests. Run all tests with:

```bash
make test
```

Component-related tests verify lifecycle management, message routing, and inference integration.

---

## Complete Example: A Custom Threshold Component

This example combines all steps into a complete component that subscribes to temperature readings, runs a simple threshold check, and publishes alerts.

```c
/*
 * Temperature Alert Component
 *
 * Subscribes to /sensors/temperature, alerts when value > 80.
 * Publishes alerts to /alerts/temperature.
 */
static void temp_alert_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    msg_router_subscribe("/sensors/temperature", comp_idx);
    uart_puts("[temp_alert] Started, watching /sensors/temperature\r\n");

    extern volatile uint64_t pit_ticks;
    uint64_t timeout_tick = pit_ticks + 6000;  /* 60s timeout */
    int alerts = 0;

    while (pit_ticks < timeout_tick) {
        char topic_buf[16];
        const char *data = msg_router_receive(comp_idx, topic_buf);

        if (data) {
            int value = 0;
            for (int i = 0; data[i] >= '0' && data[i] <= '9' && i < 10; i++)
                value = value * 10 + (data[i] - '0');

            msg_router_ack(comp_idx);

            if (value > 80) {
                msg_router_publish("/alerts/temperature", "HIGH TEMP");
                alerts++;
                uart_printf("[temp_alert] HIGH: %d (alert #%d)\r\n", value, alerts);
            }

            timeout_tick = pit_ticks + 6000;
        } else {
            yield();
        }
    }

    uart_printf("[temp_alert] Exiting (%d alerts)\r\n", alerts);
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}
```

Register in the built-in table:

```c
{
    .name = "temp_alert",
    .version = "1.0",
    .description = "Temperature threshold alerting",
    .type = COMPONENT_TYPE_SERVICE,
    .priority = COMPONENT_PRIORITY_IDLE,
    .entry = temp_alert_entry,
},
```

---

## Source Files

| File | Purpose |
|------|---------|
| `kernel/src/component_runtime.c` | Built-in component table and entry functions |
| `kernel/include/component.h` | Component types, states, priorities, API |
| `kernel/include/slm_ffi.h` | FFI declarations for inference and model loader |
| `runtime/src/component/` | Rust component registry (register, find, lifecycle) |
| `runtime/src/msg_router.rs` | Rust message router (subscribe, publish, receive) |

---

*Last updated: April 2026*
