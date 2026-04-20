# HailoRT Context-Switch Orchestration (v4.23 era)

Extracted from `hailort/libhailort/src/core_op/resource_manager/resource_manager_builder.cpp`
and `hailort/libhailort/src/device_common/control.cpp` on the `master` branch.
Line numbers approximate (upstream at time of fetch, 2026-04-19).

## Top-level build flow — ResourcesManagerBuilder::build (L1418-1490)

```
create_boundary_channels(...)                     // alloc host vDMA channel IDs
fill_internal_buffers_info(...)

// Context 0: ACTIVATION
add_new_context(ACTIVATION)
fill_activation_config_recepies_for_multi_context(...)

// Context 1: BATCH_SWITCHING
activation_boundary_input_layers =
    activation_context.get_edge_layers(BOUNDARY, H2D)
add_new_context(BATCH_SWITCHING)
fill_batch_switching_context_config_recepies_for_multi_context(
    ..., activation_boundary_input_layers)

// Context 2: PRELIMINARY
add_new_context(PRELIMINARY)
fill_preliminary_config_recepies_for_multi_context(...)

// Contexts 3..N: DYNAMIC
for each dynamic_context:
    add_new_context(DYNAMIC)
    parse_and_fill_edge_layers_mapping(...)
for each dynamic_context:
    fill_context_recipes_for_multi_context(...)

resources_manager.configure()      // sends SET_NETWORK_GROUP_HEADER
                                   // + SET_CONTEXT_INFO for all contexts

// Later, on activate:
Control::enable_core_op  →  change_context_switch_status(ENABLED, ...)
```

Note the RESET is called only on *deactivate* (or for cleanup), NOT between
set_network_group_header and set_context_info. SLM-OS currently sends RESET
first; that is OK but not what HailoRT does.

## fill_activation_config_recepies_for_multi_context (L1100-1126)

Emits, in order:

1. For each output layer: `fill_boundary_output_layer` — just records the
   edge layer in context_resources. Does NOT emit actions directly.
2. For each input layer: `fill_boundary_input_layer` — same.
3. `ResetBurstCreditsTaskAction::create()`  →  BURST_CREDITS_TASK_RESET
4. For each BOUNDARY edge_layer:
   - H2D → `OpenBoundaryInputChannelAction`
   - D2H → `OpenBoundaryOutputChannelAction`

Matches SLM-OS exactly in intent.

## fill_batch_switching_context_config_recepies_for_multi_context (L1335-1351)

Calls in order (both into the same `actions` vector, then
handle_repeated_actions + write_action_list):

1. `add_lcu_actions_to_batch_switch_context` (L1281)
2. `add_edge_layers_actions_to_batch_switch_context` (L1306)

### add_lcu_actions_to_batch_switch_context (L1281-1297)

Scans the PRELIMINARY context's action list for
`EnableLcuDefault` / `EnableLcuNonDefault` / `SwitchLcuBatch`
and emits a `SwitchLcuBatchAction` for each.

### add_edge_layers_actions_to_batch_switch_context (L1306-1333)

1. `fill_batch_switching_context_edge_layers` — populates
   edge_layers on this context by copying from the FIRST dynamic
   context (ddr_output, boundary_output, inter_context_output,
   ddr_input, boundary_input).
2. For each non-BOUNDARY edge_layer: emit `DeactivateChannelAction`
   (BATCH_SWITCHING_CONTEXT=true).
3. `ResetDdrBufferingTaskAction::create()` → DDR_BUFFERING_RESET
4. `push_edge_layer_activation_actions(internal_only=true)` —
   emits ActivateDdrOutput, ActivateInterContextOutput,
   ActivateCacheOutput, ActivateDdrInput, ActivateInterContextInput,
   ActivateCacheInput (boundary is SKIPPED because internal_only=true).
5. `add_ddr_buffers_info` — emits `DdrPairInfoAction` for each
   DDR pair that needs manual credit mgmt, plus
   `StartDdrBufferingTaskAction` if any.
6. `create_change_boundary_input_batch_actions` —
   `ChangeBoundaryInputBatchAction` per boundary H2D layer, then
   `StartBurstCreditsTaskAction` (BURST_CREDITS_TASK_START).

So the FULL minimum BATCH_SWITCHING action list (single-ctx net, no
LCUs, no DDR, no inter-context, no cache — just boundary I/O) is:

```
BURST_CREDITS_TASK_START                // tail of create_change_boundary_input_batch_actions
  — after:
DDR_BUFFERING_RESET
  — after: (nothing between because push_edge_layer_activation_actions
            emits nothing when internal_only=true and no internal layers exist)
ChangeBoundaryInputBatchAction × N_boundary_H2D
```

Expanded ordering in one list:

```
DDR_BUFFERING_RESET
ChangeBoundaryInputBatchAction (per H2D boundary channel)
BURST_CREDITS_TASK_START
```

## Key insight — SLM-OS is missing ChangeBoundaryInputBatchAction

SLM-OS currently sends BATCH_SWITCHING = { DDR_BUFFERING_RESET,
BURST_CREDITS_TASK_START }. HailoRT sends
{ DDR_BUFFERING_RESET, ChangeBoundaryInputBatch × N_h2d,
  BURST_CREDITS_TASK_START }.

Action enum value for CHANGE_BOUNDARY_INPUT_BATCH =
`CONTEXT_SWITCH_DEFS__ACTION_TYPE_CHANGE_BOUNDARY_INPUT_BATCH` (40 in
the v4.23 enum). Body is
`CONTEXT_SWITCH_DEFS__change_boundary_input_batch_t` which is just
`{ uint8_t packed_vdma_channel_id; }` — a single byte body.

## Host-side boundary channel programming

`BoundaryChannel::activate` / `deactivate` are no-ops register-wise
(L72-83). They only flip `m_is_activated`. The host does NOT program
CCR_CTRL/CCR_ADDR between SET_CONTEXT_INFO(ACTIVATION) and
SET_CONTEXT_INFO(BATCH_SWITCHING) — the firmware does all channel
register programming via OpenBoundaryInput/Output actions.

## Control::enable_core_op (control.cpp L2645-2649)

```cpp
hailo_status Control::enable_core_op(Device &device, uint8_t network_group_index,
    uint16_t dynamic_batch_size, uint16_t batch_count)
{
    return Control::change_context_switch_status(device,
        CONTROL_PROTOCOL__CONTEXT_SWITCH_STATUS_ENABLED,
        network_group_index, dynamic_batch_size, batch_count);
}
```
