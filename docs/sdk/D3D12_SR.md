# D3D12 Super Resolution in FrameLab

The optional `D3D12SR` component exposes native DLSS Super Resolution through a versioned C ABI on Windows x64. It calls Direct NGX from the separately acquired [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS). It is off by default. The application owns the device, queue, command lists, allocators, completion fence, resources and presentation. An external project builds against the installed FrameLab package and the public Windows SDK. It needs no NVIDIA headers or libraries.

Status: the component, the portable contract tests and the external consumer are in this repository. Native evaluation on NVIDIA hardware has not been recorded for this revision yet. See [Verification](#verification).

## Build and link

```powershell
cmake -S . -B build-sdk -G 'Visual Studio 17 2022' -A x64 `
  -DBUILD_SHARED_LIBS=ON -DFRAMELAB_BUILD_D3D12_SR=ON `
  '-DDLSS_NGX_SDK_ROOT=D:/SDKs/DLSS'
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix D:/FrameLab

cmake -S samples/d3d12_sr -B build-sr-client -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_PREFIX_PATH=D:/FrameLab'
cmake --build build-sr-client --config Release
ctest --test-dir build-sr-client -C Release --output-on-failure
```

`DLSS_NGX_SDK_ROOT` is the same DLSS SDK root that [D3D12FG](D3D12_FG.md) uses. `FRAMELAB_BUILD_D3D12_SR` and `FRAMELAB_BUILD_D3D12_FG` can be enabled together. The client uses `find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS D3D12SR)`, links `FrameLab::FrameLab` and includes `<framelab/d3d12_sr.h>`. The portable static and shared packages are unchanged and expose only the [core ABI](README.md).

## Lifecycle

1. **Create.** Fill `FrameLabD3D12SrConfig` with its size and ABI version, the real device, a direct queue, a completion fence on that device, an application project ID and the NGX runtime and log directories. `framelab_d3d12_sr_create` joins or initializes the process NGX session and reads `SuperSampling.Available` from the NGX capability parameters. An unavailable feature returns `FRAMELAB_UNSUPPORTED`. The optional callback reports unmodified NGX results. When SR is unavailable, it also reports the values of `SuperSampling.FeatureInitResult`, `SuperSampling.NeedsUpdatedDriver` and the minimum driver version. Create records no GPU work.
2. **Query.** `framelab_d3d12_sr_query_settings` calls `NGX_DLSS_GET_OPTIMAL_SETTINGS` for an output extent and mode. It returns the optimal render extent and the inclusive dynamic range. FrameLab requires `0 < min <= optimal <= max <= output` in each dimension. A zero optimal extent means the mode is unsupported.
3. **Configure.** On an idle handle, `framelab_d3d12_sr_record_configure` releases the previous feature and records `NGX_D3D12_CREATE_DLSS_EXT` into an open direct list at the optimal extent. Execute the list on the configured queue, signal the fence, acknowledge the value with `framelab_d3d12_sr_mark_submitted` and retire it. Frames are rejected with `FRAMELAB_INVALID_PHASE` until that creation retires.
4. **Record.** Fill `FrameLabD3D12SrFrame`. FrameLab validates the descriptor and every native object before it records `NGX_D3D12_EVALUATE_DLSS_EXT`. A rejection leaves the list untouched.
5. **Submit and retire.** Execute the list, signal a new fence value on the configured queue, acknowledge that value, and call `framelab_d3d12_sr_retire` until it returns `FRAMELAB_OK`. `FRAMELAB_NOT_READY` keeps every reference.
6. **Destroy.** Destroy idle handles. A recorded or submitted packet makes destroy return `FRAMELAB_BUSY`.

ABI v1 allows one recorded or in-flight packet per handle. A second record, configure or invalidate returns `FRAMELAB_BUSY` until retirement. Serialize calls on a handle. NGX is not thread-safe, so serialize calls on all FrameLab D3D12 handles in the process.

## Supported profile

| Binding | DXGI format | Extent | Declared state | Description |
| --- | --- | --- | --- | --- |
| Color | `R16G16B16A16_FLOAT`, or `R8G8B8A8_UNORM` without HDR | At least the frame's `render_extent` | `NON_PIXEL_SHADER_RESOURCE` | No `DENY_SHADER_RESOURCE` |
| Depth | `R32_FLOAT`, `D32_FLOAT` or `D24_UNORM_S8_UINT` | At least `render_extent` | `NON_PIXEL_SHADER_RESOURCE` | No `DENY_SHADER_RESOURCE` |
| Motion vectors | `R16G16_FLOAT` or `R32G32_FLOAT` | At least `render_extent` | `NON_PIXEL_SHADER_RESOURCE` | No `DENY_SHADER_RESOURCE` |
| Output | `R16G16B16A16_FLOAT`, or `R8G8B8A8_UNORM` without HDR | Exactly the configured output extent | `UNORDERED_ACCESS` | `ALLOW_UNORDERED_ACCESS` |

Every binding is a 2D texture with one mip, one array layer and one sample on the configured device. The four resources and their nonzero `resource_id` values are distinct. Each binding carries a lease. The inputs are read at the top-left `render_extent` rectangle. Modes are `FRAMELAB_SR_PERFORMANCE`, `FRAMELAB_SR_BALANCED` and `FRAMELAB_SR_QUALITY`. The output is at least 32×32. Formats outside this table return `FRAMELAB_UNSUPPORTED`, even where NVIDIA lists broader support.

- **Color range and exposure.** Color is LDR by default. `FRAMELAB_D3D12_SR_HDR` declares linear HDR color and requires RGBA16F color and output. `pre_exposure` and `exposure_scale` are passed to NGX for each frame. Zero selects 1. Negative or non-finite values are rejected. `FRAMELAB_D3D12_SR_AUTO_EXPOSURE` makes the runtime compute exposure. ABI v1 has no exposure texture.
- **Depth.** Near depth is 0 and far depth is 1. `FRAMELAB_D3D12_SR_DEPTH_INVERTED` selects near 1 and far 0. The frame's `FRAMELAB_CAMERA_DEPTH_INVERTED` flag must match the configured flag.
- **Motion vectors.** Motion is at render resolution, without jitter, and includes camera motion. Set `FRAMELAB_CAMERA_MOTION_INCLUDED`. Stored values multiplied by `camera.motion_vector_scale` are previous-minus-current positions in UV over the active rectangle, with the origin at the top-left and Y down. FrameLab passes `motion_vector_scale × render_extent` to NGX as the pixel scale. Object-only motion returns `FRAMELAB_UNSUPPORTED`.
- **Jitter.** `camera.jitter_pixels` is the sub-pixel offset applied to the projection, in render pixels, using the motion-vector axes. Each component is within [-0.5, 0.5].
- **Camera.** The camera needs `FRAMELAB_CAMERA_VERIFIED` and `FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV`, and every field must be finite. Direct NGX SR does not consume the matrices, position, basis, planes, field of view or aspect ratio.

The motion and jitter conventions follow the DLSS programming guide. The synthetic sample uses zero jitter, so the jitter sign convention has not been exercised on hardware.

## Resize, dynamic resolution and history

A change of output extent, mode or flags needs an idle handle and a new `record_configure`. FrameLab releases the old feature only when idle. Idle means that the last evaluation using it has retired. The new feature starts with empty history. Recreate the input and output resources as needed. The old leases are released after their last retirement.

The feature is created at the optimal render extent, as the DLSS guide requires. Each frame's `render_extent` may change within the inclusive range from the query without resetting history. Inputs must be at least that large. Keep the render aspect ratio equal to the output aspect ratio. A runtime without dynamic resolution reports `min = max = optimal`.

FrameLab sets the NGX reset flag for the first frame after configuration, including the reconfiguration that follows a cancellation. It also sets it for any `FRAMELAB_RESET_*` bit, for a frame index that does not follow the last acknowledged frame, and after `framelab_d3d12_sr_invalidate`. Frame indices increase strictly over the lifetime of a handle, including across reconfiguration. Use `FRAMELAB_RESET_CAMERA_CUT` for scene changes.

## Ownership, states and completion

| Object | Owner and lifetime |
| --- | --- |
| Device, direct queue, completion fence | Host. The handle holds COM references until destruction. |
| Command list | Host. FrameLab holds a reference to the list of the pending packet until retirement or cancellation. |
| Command allocator | Host. FrameLab cannot see it. Do not reset it until the packet retires, or until cancellation of a list that never executed. |
| Color, depth, motion and output resources | Host. For each packet FrameLab holds a COM reference and retains the explicit lease until retirement or cancellation. |
| NGX feature and parameter map | FrameLab. One feature per handle, released on reconfiguration and destruction while idle. |
| NGX D3D12 session | FrameLab, shared within the process. See below. |

FrameLab records no barriers. The host transitions every binding to its declared state before the list executes, and the declared state must be the actual state at that point. The DLSS guide states that the evaluation may change states internally but restores these states. The host state tracker therefore stays valid after the evaluation. NGX does not preserve D3D12 command list state. Re-bind root signatures, descriptor heaps, pipeline state and render targets after a FrameLab record call.

The host signals `completion_fence` on the configured queue after executing each SR list. Each acknowledged value must exceed every value previously acknowledged to the handle, and the value observed at create. `UINT64_MAX` is reserved for device removal. Retirement reads the fence's actual completed value. Recording a packet never waits on the GPU.

## Rejection, failure and fallback

| Status | Cause, all detected before native recording unless noted |
| --- | --- |
| `FRAMELAB_ABI_MISMATCH` | Short `struct_size` or another `abi_version` in any descriptor. |
| `FRAMELAB_INVALID_ARGUMENT` | Null handle, descriptor or command list, unknown option or reset bits, or invalid exposure values. |
| `FRAMELAB_INVALID_PHASE` | Record before a configuration has retired, or a lifecycle call in the wrong phase. |
| `FRAMELAB_BUSY` | A packet is pending, or the NGX session belongs to another configuration or to D3D12 FG. |
| `FRAMELAB_INVALID_IDENTITY` | A queue, fence, list or resource on another device, a zero or repeated resource ID, or aliased resources. |
| `FRAMELAB_INVALID_SYNC` | A non-direct queue or list, a zero command stream ID, or a non-increasing fence value. |
| `FRAMELAB_INVALID_SEQUENCE` | A frame index at or below the last acknowledged frame. |
| `FRAMELAB_INVALID_DIMENSIONS` | An output below 32×32, a render extent outside the range, inputs smaller than the render extent, or a mismatched output. |
| `FRAMELAB_INVALID_IMAGE_SEMANTICS` / `FRAMELAB_MISSING_LEASE` | A required resource or lease is missing. |
| `FRAMELAB_INVALID_IMAGE_DESCRIPTION` | Wrong dimension, mips, layers, samples or resource flags. |
| `FRAMELAB_INVALID_RESOURCE_STATE` | A declared state other than the one in the table. |
| `FRAMELAB_INVALID_CAMERA` / `FRAMELAB_UNSUPPORTED` | Camera or motion conventions, unsupported formats, modes, or runtime capability. |
| `FRAMELAB_NATIVE_FAILURE` / `FRAMELAB_OUT_OF_MEMORY` | Returned from a record call after NGX recording failed. The list may contain partial commands. |
| `FRAMELAB_DEVICE_LOST` | Device removal observed at create, record, configure or retirement. |

After a rejection, the host can record its ordinary rendering path into the same list and present that result. After `FRAMELAB_NATIVE_FAILURE` or `FRAMELAB_OUT_OF_MEMORY` from a record call, close and discard the list without executing it and call `framelab_d3d12_sr_cancel`. Then record the fallback into a fresh list. Cancel only work that can never execute. Cancellation releases the native feature, so record a new configuration before the next frame. NGX updates the feature's internal resource-state tracking while it records an evaluation. After a discarded recording, that tracking no longer matches the GPU. On the RTX 5090, an evaluation that followed a discarded recording on the same feature produced four D3D12 debug-layer errors (ID 527, barrier before-state mismatch) on NGX-internal resources. Rejections before native recording need no cancellation and keep the feature.

When the device is removed after an acknowledged submission, the fence reports `UINT64_MAX` and retirement returns `FRAMELAB_DEVICE_LOST` after releasing the packet's references. If the host sees removal before acknowledging a recorded packet, for example from a failed `Signal`, the work cannot execute and the host cancels it. FrameLab also checks for removal before each record or configure. After loss, destroy every FrameLab D3D12 handle on the device, release the host's device references and create a new device and handle. Reconfigure and send the next frame with a reset.

## NGX session and D3D12 FG

NGX is initialized once per device, and `NVSDK_NGX_D3D12_Shutdown1` releases every feature on that device. FrameLab therefore keeps one process-wide, reference-counted NGX D3D12 session. The first SR handle calls `NVSDK_NGX_D3D12_Init_with_ProjectID`. Later handles on the same device with identical project ID, engine version and directories join it. Other configurations get `FRAMELAB_BUSY`. Each handle releases its feature and parameter map before its session reference. The last reference calls `Shutdown1`, then releases the session's device reference. A new handle after that initializes NGX again.

The current [D3D12 FG module](D3D12_FG.md) calls Init and Shutdown1 itself. It now claims the same process-wide slot exclusively. `framelab_d3d12_fg_create` returns `FRAMELAB_BUSY` while any SR handle is alive, and SR create returns `FRAMELAB_BUSY` while FG is alive. FG on its own behaves as before. This prevents either module from shutting NGX down under the other. Concurrent SR and FG is the scope of [FL-16](https://github.com/5headsmartbtw/leonida-framelab/issues/31). The planned change moves FG onto the shared session with identical initialization arguments. Both features then live on one session, and Shutdown1 runs after both have released their features and parameter maps.

## Memory

FrameLab makes no GPU allocations or copies. NGX owns the feature's internal resources. FrameLab sets `FreeMemOnReleaseFeature` before creation so that reconfiguration frees the old feature's memory instead of caching it. In-flight work is bounded to one packet per handle. The sample reports local VRAM usage and budget through `IDXGIAdapter3::QueryVideoMemoryInfo`.

## Verification

Portable checks run in CI on Linux and Windows without NVIDIA downloads. `d3d12_sr_contract` exercises the decision layer in [sr_contract.hpp](../../src/backends/d3d12/sr_contract.hpp): option and range checks, every descriptor rejection, reset rules, monotonic fences, `NOT_READY` retention, cancellation, reconfiguration and simulated device removal. On Windows, `d3d12_sr_header_c` and `d3d12_sr_header_cpp` compile the public header as C and C++ with the D3D12 FG header and check the x64 layout. The native adapter `static_assert`s every numeric constant the decision layer uses against the Windows SDK, NGX and public headers. The native module needs the DLSS SDK and is not built in hosted CI.

The [external consumer](../../samples/d3d12_sr) and [`tests/run_d3d12_sr_checks.ps1`](../../tests/run_d3d12_sr_checks.ps1) run the hardware checks in separate processes. Covered are standard and D3D12 debug-layer correctness runs, GPU-based validation, timing without validation or readback, and native device removal. The checks include verified readback of a known synthetic scene that detects clear-only and stale output, three output configurations with two resizes, a camera cut, missing-input rejection with fallback and recovery, dynamic resolution where the runtime reports a range, cancellation, fence-gated retention, a joined session handle, rejection of FG while SR is alive and NGX re-initialization.

| Hardware run | Result |
| --- | --- |
| RTX 5090, official runtime | Not yet run for this revision. |
| RTX 2050 Laptop (4 GB) merge gate | Pending maintainer comparison per the [release checklist](RELEASE_CHECKLIST.md#rtx-2050-low-memory-merge-gate). |
| RTX 2080 Ti, RTX 5060 Laptop | Optional follow-up. |

Not covered by the sample: sub-pixel jitter, HDR and exposure, depth-stencil formats, presentation, allocation failure injection, concurrent SR and FG, and image quality.
