# HailoRT v4.23.0 — boundary `host_buffer_info` wire values

Answers for how `CONTROL_PROTOCOL__host_buffer_info_t` is populated for
`OPEN_BOUNDARY_INPUT_CHANNEL` / `OPEN_BOUNDARY_OUTPUT_CHANNEL` context-switch
actions. Pulled from HailoRT tag `v4.23.0`.

## Call chain

1. `resource_manager_builder.cpp` (v4.23.0 lines 104/108 input, 174/178 output)
   calls `LayerInfoUtils::get_layer_transfer_size(layer_info)` to get
   **bytes per frame** (from `HailoRTCommon::get_periph_frame_size(hw_shape, format)`
   for non-NMS layers), then
   `resources_manager.get_boundary_buffer_info(*vdma_channel, transfer_size)`.

2. `resource_manager.cpp:1068-1080` — `ResourcesManager::get_boundary_buffer_info`.
   DESC_BOUNDARY_CHANNEL path delegates to
   `vdma::VdmaEdgeLayer::get_host_buffer_info(Type::SCATTER_GATHER,
       desc_list.dma_address(), desc_list.desc_page_size(),
       desc_list.count(), transfer_size)`.

3. `vdma_edge_layer.cpp:63-76` (verbatim below) — the actual wire-field
   assignments.

```cpp
CONTROL_PROTOCOL__host_buffer_info_t VdmaEdgeLayer::get_host_buffer_info(Type type, uint64_t dma_address,
    uint16_t desc_page_size, uint32_t desc_count, uint32_t transfer_size)
{
    CONTROL_PROTOCOL__host_buffer_info_t buffer_info{};
    buffer_info.buffer_type = static_cast<uint8_t>((type == vdma::VdmaEdgeLayer::Type::SCATTER_GATHER) ?
        CONTROL_PROTOCOL__HOST_BUFFER_TYPE_EXTERNAL_DESC :
        CONTROL_PROTOCOL__HOST_BUFFER_TYPE_CCB);
    buffer_info.dma_address = dma_address;
    buffer_info.desc_page_size = desc_page_size;
    buffer_info.total_desc_count = desc_count;
    buffer_info.bytes_in_pattern = transfer_size;   // <-- always transfer_size

    return buffer_info;
}
```

## Takeaway

For **both** BOUNDARY H2D and D2H:
- `bytes_in_pattern = transfer_size` (bytes-per-frame for the layer,
  from `get_periph_frame_size`)
- `total_desc_count = desc_list.count()` (number of entries in the SG
  descriptor list programmed for the channel)
- `desc_page_size = desc_list.desc_page_size()` (typically 512 or 4096
  for boundary scatter-gather)
- `buffer_type = CONTROL_PROTOCOL__HOST_BUFFER_TYPE_EXTERNAL_DESC` (SG)
- `dma_address = desc_list.dma_address()` (IOVA of the descriptor-list
  page, 64 KB-aligned)

Neither field is allowed to be zero on the wire for a live boundary
channel; both are computed from real hardware state. `bytes_in_pattern=0`
would mean a zero-byte frame, and `total_desc_count=0` would mean no
descriptors programmed — both are degenerate.

## Comment referenced at line 705

Actually at `resource_manager_builder.cpp:633-636` in v4.23.0 (cache
layers, not boundary): "buffer_info isn't correct — total_desc_count and
bytes_in_pattern are calculated according to cache size, not transfer
size." That comment is about the cache-layer code path patching
`buffer_info` after the fact; it does *not* apply to boundary channels,
which use `transfer_size` straight through.
