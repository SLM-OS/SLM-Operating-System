# Component Development Guide

How to create and run built-in components in SLM-OS.

**Status:** Phase 5 M5

---

## Overview

SLM-OS components are built-in C functions registered in the `builtin_components[]` table in `kernel/src/component_runtime.c`. Each component runs as a kernel task with its own lifecycle, managed by the Rust component registry. Components communicate via the topic-based message router and can optionally invoke the inference engine for AI workloads.

Components are designed for edge/IoT scenarios: small, single-purpose services that subscribe to data topics, process input (with or without ML inference), and publish results.

---

## Registration

Every built-in component is described by a `builtin_component` struct:

```c
struct builtin_component {
    const char *name;           // Unique component name
    const char *version;        // Version string (e.g., "1.0")
    const char *description;    // Human-readable description
    uint8_t type;               // COMPONENT_TYPE_SERVICE, _DRIVER, or _APPLICATION
    uint8_t priority;           // COMPONENT_PRIORITY_IDLE through _CRITICAL
    component_entry_t entry;    // Entry function: void (*)(void *arg)
};
```

To add a new component, append an entry to the `builtin_components[]` array in `component_runtime.c`:

```c
static const struct builtin_component builtin_components[] = {
    // ... existing components ...
    {
        .name = "my_component",
        .version = "1.0",
        .description = "Brief description of what this component does",
        .type = COMPONENT_TYPE_SERVICE,
        .priority = COMPONENT_PRIORITY_IDLE,
        .entry = my_component_entry,
    },
};
```

When `component run my_component` is invoked from the shell, the runtime:
1. Registers the component metadata in the Rust component registry
2. Creates a kernel task with the entry function
3. Pins the task to CPU 0 (for shared-memory IPC visibility)
4. Links the task ID back to the component slot
5. Sets up a cleanup callback that marks the component as Unloaded when the task exits

---

## Entry Function Pattern

Every component entry function has the signature:

```c
void my_component_entry(void *arg)
```

The `arg` parameter is the component index (cast from `uintptr_t`). The entry function must:

1. **Set state to Running** at the start
2. **Do work** (subscribe to topics, process messages, run inference)
3. **Set state to Terminating** before returning

Minimal skeleton:

```c
static void my_component_entry(void *arg)
{
    int comp_idx = (int)(uintptr_t)arg;
    component_set_state((uint32_t)comp_idx, COMPONENT_RUNNING);

    // ... component logic ...

    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
}
```

The cleanup callback registered by `component_run()` handles unsubscribing from all topics and setting the final Unloaded state after the task exits.

---

## Message Routing

Components communicate through a topic-based publish/subscribe message router. The typical pattern involves three steps: subscribe, receive (poll), and publish.

### Subscribe to a Topic

```c
msg_router_subscribe("/sensors/data", comp_idx);
```

### Polling Loop

Components poll for messages using `yield()` to avoid busy-waiting. A `pit_ticks`-based timeout prevents the component from running indefinitely if no messages arrive:

```c
extern volatile uint64_t pit_ticks;
uint64_t timeout_tick = pit_ticks + 3000;  /* 30s at 100 Hz */

while (pit_ticks < timeout_tick) {
    char topic_buf[16];
    const char *data = msg_router_receive(comp_idx, topic_buf);

    if (data) {
        /* Process the message */
        msg_router_ack(comp_idx);

        /* Optionally publish a result */
        msg_router_publish("/output/result", "processed");

        /* Reset timeout on activity */
        timeout_tick = pit_ticks + 3000;
    } else {
        yield();  /* Cooperative yield — lets other tasks run */
    }
}
```

### API Summary

| Function | Purpose |
|----------|---------|
| `msg_router_subscribe(topic, idx)` | Subscribe component `idx` to `topic` |
| `msg_router_receive(idx, topic_out)` | Check for pending message (returns data pointer or NULL) |
| `msg_router_ack(idx)` | Acknowledge and consume the current message |
| `msg_router_publish(topic, data)` | Publish a message to all subscribers of `topic` |
| `msg_router_unsubscribe_all(idx)` | Remove all subscriptions for component `idx` |

---

## Using Inference

Components that need ML inference use the FFI bridge to the Rust inference engine. The typical pattern:

1. **Find the model** by name (must be loaded beforehand via the shell)
2. **Run inference** using `rust_infer_classify()` to get an integer class result

```c
/* Find the model — returns registry index or -1 */
int model_idx = rust_model_find("mnist");
if (model_idx < 0) {
    uart_puts("[my_component] Model not loaded\r\n");
    component_set_state((uint32_t)comp_idx, COMPONENT_TERMINATING);
    return;
}

/* Run inference with zero input — returns argmax class index */
int predicted_class = rust_infer_classify((uint32_t)model_idx);
if (predicted_class >= 0) {
    uart_printf("[my_component] Predicted class: %d\r\n", predicted_class);
}
```

`rust_infer_classify(model_index)` runs the full inference pipeline (operator graph execution) with a zero-valued input buffer and returns the argmax of the output tensor as an integer. This avoids floating-point handling in kernel-mode C code. The model must be loaded first via `model load <path>` in the shell.

For components that need raw float output (e.g., probability distributions), use `rust_infer()` instead. See `docs/ffi.md` for the full inference FFI.

---

## Example Components

### sensor_monitor (Rule-Based Threshold Monitoring)

A service component that demonstrates message routing without ML inference.

- **Subscribes to:** `/sensors/data`
- **Publishes to:** `/alerts/threshold`
- **Logic:** Parses integer value from message. If value > 50, publishes an alert. Otherwise logs the value as normal.
- **Timeout:** 30 seconds of inactivity

This component is useful as a baseline that validates the component lifecycle and message routing independently of the inference engine.

### digit_classifier (MNIST Inference)

An application component that demonstrates the full AI inference pipeline.

- **Subscribes to:** `/input/digits`
- **Publishes to:** `/output/class`
- **Logic:** On each message, calls `rust_infer_classify()` on the MNIST model and publishes the predicted digit class.
- **Prerequisites:** MNIST model must be loaded via `model load /mnt/files/mnist.onnx` before starting this component.
- **Timeout:** 30 seconds of inactivity

---

## Shell Commands

### Component Management

```
component builtins                  List available built-in components
component run <name>                Run a built-in component as a kernel task
component list                      List all registered (running) components
component status <name|idx>         Show component details
component swap <old> <new>          Hot-swap: replace old with new (preserves subscriptions)
```

### Message Router

```
msg send <topic> <data>             Publish a message to a topic
msg list                            List all topics and their subscribers
msg subscribe <topic> <idx>         Subscribe a component to a topic
```

---

## Demo

The `scripts/demo.lua` script provides a guided walkthrough of the Phase 5 capabilities. Run it from the SLM-OS shell:

```
SLM-OS> lua scripts/demo.lua
```

For a hands-on interactive demo:

```
SLM-OS> model load /mnt/files/mnist.onnx
SLM-OS> component run sensor_monitor
SLM-OS> component run digit_classifier
SLM-OS> msg send /sensors/data 75
SLM-OS> msg send /sensors/data 30
SLM-OS> msg send /input/digits test
SLM-OS> msg list
SLM-OS> component list
```

The sensor monitor will publish a threshold alert for value 75 (exceeds threshold of 50) and log value 30 as normal. The digit classifier will run MNIST inference and publish the predicted class to `/output/class`.

---

## Source Files

| File | Purpose |
|------|---------|
| `kernel/src/component_runtime.c` | Built-in component table, entry functions, `component_run()` |
| `kernel/include/component.h` | Component types, states, priorities |
| `runtime/src/component/` | Rust component registry (register, find, lifecycle) |
| `runtime/src/msg_router.rs` | Rust message router (subscribe, publish, receive) |
| `kernel/include/slm_ffi.h` | C declarations for Rust FFI (inference, model loader) |

---

*Last updated: April 2026*
