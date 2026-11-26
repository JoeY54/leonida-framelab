/* SPDX-License-Identifier: MIT */
#ifndef FRAMELAB_D3D12_SR_H
#define FRAMELAB_D3D12_SR_H

#include <framelab/framelab.h>
#include <d3d12.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameLabD3D12Sr FrameLabD3D12Sr;
/* Called synchronously from the calling thread. It must not unwind or re-enter FrameLab. */
typedef void (FRAMELAB_CALL *FrameLabD3D12SrReportFn)(void* user, const char* operation, uint32_t ngx_result);

/* Shared with <framelab/vulkan_sr.h>. */
#ifndef FRAMELAB_SR_MODES_DEFINED
#define FRAMELAB_SR_MODES_DEFINED
enum { FRAMELAB_SR_PERFORMANCE = 1, FRAMELAB_SR_BALANCED = 2, FRAMELAB_SR_QUALITY = 3 };
#endif

/* FrameLabD3D12SrOptions.flags */
enum {
    FRAMELAB_D3D12_SR_HDR = 1,            /* Linear HDR color. Requires RGBA16F color and output. */
    FRAMELAB_D3D12_SR_DEPTH_INVERTED = 2, /* Near depth 1, far depth 0. Frames set FRAMELAB_CAMERA_DEPTH_INVERTED. */
    FRAMELAB_D3D12_SR_AUTO_EXPOSURE = 4   /* The runtime computes exposure. */
};

typedef struct FrameLabD3D12SrConfig {
    uint32_t struct_size, abi_version;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;           /* Direct queue that executes every recorded SR list. */
    ID3D12Fence* completion_fence;       /* Signaled by the host on that queue after each SR submission. */
    const char* project_id;
    const char* engine_version;
    const wchar_t* ngx_runtime_directory;
    const wchar_t* log_directory;
    FrameLabD3D12SrReportFn report;      /* Optional. */
    void* report_user;
} FrameLabD3D12SrConfig;

typedef struct FrameLabD3D12SrOptions {
    uint32_t struct_size, abi_version;
    FrameLabExtent output_extent;
    uint32_t mode, flags;
} FrameLabD3D12SrOptions;

/* The caller sets struct_size and abi_version. FrameLab fills the extents. */
typedef struct FrameLabD3D12SrSettings {
    uint32_t struct_size, abi_version;
    FrameLabExtent optimal_render_extent;            /* Used to create the native feature. */
    FrameLabExtent min_render_extent, max_render_extent; /* Inclusive per-frame range. */
} FrameLabD3D12SrSettings;

typedef struct FrameLabD3D12SrImage {
    uint64_t resource_id;
    ID3D12Resource* resource;
    uint32_t state;              /* Producer-tracked D3D12_RESOURCE_STATES when the list executes. */
    const FrameLabLease* lease;  /* Owns the allocation until its final release callback. */
} FrameLabD3D12SrImage;

typedef struct FrameLabD3D12SrFrame {
    uint32_t struct_size, abi_version;
    uint64_t command_stream_id;
    ID3D12GraphicsCommandList* commands;
    uint64_t frame_index;
    FrameLabExtent render_extent;    /* Active input rectangle at the top-left of color, depth and motion. */
    uint32_t reset_history;          /* FRAMELAB_RESET_* bits. */
    float pre_exposure, exposure_scale; /* 0 selects 1. */
    FrameLabD3D12SrImage color, depth, motion_vectors, output;
    FrameLabCamera camera;
} FrameLabD3D12SrFrame;

/* Joins or initializes the process NGX D3D12 session and queries native DLSS SR support.
 * No GPU work is recorded. The host keeps the device, queue and fence alive. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_create(
    const FrameLabD3D12SrConfig* config, FrameLabD3D12Sr** out_sr);
/* Requires an idle handle. Releases the native feature and this handle's session reference. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_destroy(FrameLabD3D12Sr* sr);

/* Reports the runtime's render extents for an output size and mode without recording work. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_query_settings(
    FrameLabD3D12Sr* sr, const FrameLabD3D12SrOptions* options, FrameLabD3D12SrSettings* out_settings);
/* Idle only. Releases the previous feature and records creation of a new one into an open
 * direct list. Submit, acknowledge and retire it before recording frames. History restarts. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_record_configure(
    FrameLabD3D12Sr* sr, ID3D12GraphicsCommandList* commands,
    const FrameLabD3D12SrOptions* options, FrameLabD3D12SrSettings* out_settings);
/* Validates every descriptor and native object before recording NGX Evaluate. A rejection
 * leaves the host list untouched. After FRAMELAB_NATIVE_FAILURE or FRAMELAB_OUT_OF_MEMORY from
 * a record call, discard the list and cancel. NGX does not preserve list state. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_record(
    FrameLabD3D12Sr* sr, const FrameLabD3D12SrFrame* frame);
/* Acknowledge only after executing the recorded list and queuing Signal(value) on the queue. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_mark_submitted(
    FrameLabD3D12Sr* sr, uint64_t value);
/* Cancel only after ensuring the recorded work can never execute. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_cancel(FrameLabD3D12Sr* sr);
/* Reads the actual completion fence. FRAMELAB_NOT_READY keeps every reference. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_retire(FrameLabD3D12Sr* sr);
/* Idle only. The next evaluation resets temporal history. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_invalidate(FrameLabD3D12Sr* sr);
/* Raw NGX status from the most recent native operation on this handle. */
FRAMELAB_API uint32_t FRAMELAB_CALL framelab_d3d12_sr_last_ngx_result(const FrameLabD3D12Sr* sr);

#ifdef __cplusplus
}
#endif
#endif
