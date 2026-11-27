/* SPDX-License-Identifier: MIT */
#ifndef FRAMELAB_D3D12_FG_H
#define FRAMELAB_D3D12_FG_H

#include <framelab/framelab.h>
#include <d3d12.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameLabD3D12Fg FrameLabD3D12Fg;
typedef void (FRAMELAB_CALL *FrameLabD3D12InitReportFn)(void* user, const char* operation, uint32_t ngx_result);

typedef struct FrameLabD3D12FgConfig {
    uint32_t struct_size, abi_version;
    uint64_t device_id, queue_id;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12Fence* completion_fence;
    FrameLabExtent extent;
    const char* project_id;
    const char* engine_version;
    const wchar_t* ngx_runtime_directory;
    const wchar_t* log_directory;
    FrameLabD3D12InitReportFn report; /* Optional, called synchronously during create. */
    void* report_user;
    /* Appended after v0.4.0. Callers built against the shorter layout are accepted, and a zero
     * extent selects `extent`. Depth and motion vectors use this extent, which must not exceed
     * `extent`; color and the generated image use `extent`. With D3D12 SR, pass the SR render
     * extent and the SR output as color. */
    FrameLabExtent render_extent;
} FrameLabD3D12FgConfig;

typedef struct FrameLabD3D12FgImage {
    uint64_t resource_id;
    ID3D12Resource* resource;
    uint32_t state; /* Producer-tracked D3D12_RESOURCE_STATES. */
} FrameLabD3D12FgImage;

typedef struct FrameLabD3D12FgBindings {
    uint32_t struct_size, abi_version;
    uint64_t command_stream_id;
    ID3D12GraphicsCommandList* commands;
    FrameLabD3D12FgImage color, depth, motion_vectors, generated;
} FrameLabD3D12FgBindings;

/* Joins or initializes the process NGX D3D12 session shared with D3D12 SR handles, then queries
 * native FG capability. Joining needs the same device and identical project, engine and directory
 * arguments; otherwise FRAMELAB_BUSY. The last FG or SR handle shuts NGX down. The host provides
 * live COM interfaces and an increasing completion timeline signaled by this direct queue. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_create(
    const FrameLabD3D12FgConfig* config, FrameLabD3D12Fg** out_fg);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_destroy(FrameLabD3D12Fg* fg);

/* Record NGX feature creation into an open direct list on the configured device.
 * Submit, acknowledge, and retire it before the first frame. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_record_initialize(
    FrameLabD3D12Fg* fg, ID3D12GraphicsCommandList* commands);
/* Validate metadata and native objects before attempting to record NGX Evaluate.
 * Resource or metadata rejection leaves the host command list usable. After
 * FRAMELAB_NATIVE_FAILURE from recording, discard the list and cancel the packet. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_record(
    FrameLabD3D12Fg* fg, const FrameLabFgFrame* frame, const FrameLabD3D12FgBindings* bindings);
/* Acknowledge only after executing the recorded list and queuing Signal(value). */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_mark_submitted(
    FrameLabD3D12Fg* fg, uint64_t value);
/* Cancel only after ensuring the recorded work cannot be submitted. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_cancel(FrameLabD3D12Fg* fg);
/* Check the actual completion fence. FRAMELAB_NOT_READY keeps all references. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_retire(FrameLabD3D12Fg* fg);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_invalidate(FrameLabD3D12Fg* fg);
/* Raw NGX status from the most recent recorded native operation. */
FRAMELAB_API uint32_t FRAMELAB_CALL framelab_d3d12_fg_last_ngx_result(const FrameLabD3D12Fg* fg);

#ifdef __cplusplus
}
#endif
#endif
