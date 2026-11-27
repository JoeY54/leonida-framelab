# D3D12 Frame Generation in FrameLab

The optional D3D12FG component exposes Direct NGX FG through a Windows x64 C ABI. An external project can build against the public Windows SDK header and the installed FrameLab package. The native DLSS SDK is needed when building FrameLab. At runtime, the host supplies the separately acquired NGX runtime and all GPU resources.

## Build and link

```powershell
cmake -S . -B build-sdk -G 'Visual Studio 17 2022' -A x64 `
  -DBUILD_SHARED_LIBS=ON -DFRAMELAB_BUILD_D3D12_FG=ON `
  '-DDLSS_NGX_SDK_ROOT=D:/SDKs/DLSS'
cmake --build build-sdk --config Release
cmake --install build-sdk --config Release --prefix D:/FrameLab

cmake -S samples/d3d12_fg -B build-client -G 'Visual Studio 17 2022' -A x64 `
  '-DCMAKE_PREFIX_PATH=D:/FrameLab'
cmake --build build-client --config Release
ctest --test-dir build-client -C Release --output-on-failure
```

The client uses `find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED COMPONENTS D3D12FG)` and links `FrameLab::FrameLab`. Include `<framelab/d3d12_fg.h>`. The default portable static/shared packages expose the [core ABI](README.md). The GPU component requires the opt-in shared Windows package.

## Record and retire

1. Fill `FrameLabD3D12FgConfig` including its size and ABI version. Pass the real device, direct command queue and its completion fence. Use an application-owned project ID and directories. The optional initialization callback reports unmodified NGX result codes synchronously. It must not unwind or re-enter FrameLab. FrameLab checks actual NGX availability before it permits feature creation.
2. Record initialization into an open direct list. Execute it on the configured queue, enqueue a fence signal on that queue, and pass the value to `framelab_d3d12_fg_mark_submitted`. Retire after FrameLab observes completion. Reserve `UINT64_MAX` for device removal. Keep fence values monotonically increasing across the entire host queue timeline.
3. For each frame, pass `FrameLabFgFrame` and matching `FrameLabD3D12FgBindings`. The three input descriptors carry real owners and source-frame metadata. The producer tracks and establishes the native states. FrameLab checks COM device identity, formats, extents, resource IDs, command stream, tracked states and input ownership before calling Evaluate. The caller supplies a distinct output texture.
4. Execute and signal the queue, then acknowledge the value. The backend keeps native resource interfaces, the command list and frame leases until the real fence completes. `FRAMELAB_NOT_READY` and `FRAMELAB_BUSY` preserve ownership. The host keeps the allocator, uploads and presentation resources alive as long as its work uses them. FrameLab does not submit commands, signal completion or present.
5. After a metadata or state rejection, the host can continue ordinary rendering. If NGX recording returns `FRAMELAB_NATIVE_FAILURE`, discard the command list and cancel the packet. Only cancel after making sure that work cannot be submitted later. Invalidate history on dropped or discontinuous frames and set the reset flag on recovery. Destroy the idle backend before reclaiming its device.

The tested profile uses output-size RGBA8 color, R32_FLOAT depth and RG16_FLOAT motion in non-pixel shader read state, plus a distinct RGBA8 UAV output. Depth and motion use the config's `render_extent`, which must not exceed `extent`; a zero `render_extent`, or a config built against the shorter v1 layout, selects `extent`. Motion vectors must be unjittered and the camera jitter zero. It uses one direct command stream and one outstanding packet. Resizing requires completing outstanding work and creating a new backend at the new size.

FG shares the process-wide, reference-counted NGX D3D12 session with [D3D12SR](D3D12_SR.md#ngx-session-and-d3d12-fg). The first FG or SR handle initializes NGX; later handles on the same device with identical project ID, engine version and directories join it, and other configurations get `FRAMELAB_BUSY`. The last handle runs Shutdown1. Serialize calls on all FrameLab D3D12 handles and do not manage the same NGX D3D12 session separately while any of them exists. See [SR to FG chaining](D3D12_SR.md#sr-to-fg-chaining) for using SR output as FG color.

## DXGI presentation

FrameLab returns the interpolated image in the caller's output texture. It does not create a swapchain, select a backbuffer, schedule frames or call Present. The [external sample](../../samples/d3d12_fg) shows one application-owned arrangement:

- **Architecture.** The application presents Direct NGX output through its own flip-model swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, two buffers). This is different from Streamline DLSS-G, whose proxy swapchain inserts and schedules generated frames itself inside Present ([Streamline DLSS-G guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)). Do not combine the two for the same swapchain, or the same interval is generated twice. FrameLab's D3D12 FG path is Direct NGX only.
- **Order.** Source frame N is rendered and evaluated against N-1. The application presents the generated N-1/N image, then real frame N. Each Present copies the chosen texture into the current backbuffer on the same direct queue, transitions it to `PRESENT`, and releases the backbuffer reference before Present and before any `ResizeBuffers`.
- **Pacing and latency.** The generated image becomes available only after frame N has rendered, so interpolation adds about half a real-frame period of latency to every real frame. The sample holds real frame N after the generated Present for as long as the generated image's predecessor was shown, splitting each real-frame period evenly (`--no-pacing` disables this). With sync interval 1, Present additionally waits for the vertical blank. With sync interval 0 the sample uses `DXGI_PRESENT_ALLOW_TEARING` only when `DXGI_FEATURE_PRESENT_ALLOW_TEARING` is supported ([DXGI Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)). The sample's per-Present GPU waits and readback keep it far from production pacing; its intervals are CPU-side submission evidence.
- **UI.** Interpolate the scene without UI and compose the UI onto both the generated and the real image before Present. The synthetic scene has no UI plane.
- **Lifetime.** The host keeps the allocator, upload and readback buffers, output texture and backbuffer alive until the fence of the work that used them completes. Before resizing, the host drains the queue (including the last Present), destroys the idle FrameLab backend, resizes the swapchain and creates a new backend at the new extent with a history reset. Drain the queue again before releasing the swapchain at shutdown: on the RTX 5090 with the D3D12 debug layer, releasing it with the final Present still queued terminated the process (exit code 2173) after all checks had passed.
- **Fallback.** If backend creation or native initialization fails, or a frame is rejected before or during recording, the host discards the list, cancels a natively failed packet, re-uploads the real frame and presents it. The next accepted frame carries a history reset. After `framelab_d3d12_fg_invalidate`, a frame without a reset flag is rejected with `FRAMELAB_RESET_REQUIRED`.

## Hardware evidence

The [v0.4.0 regression comparison](RELEASE_NOTES.md#rtx-2050-regression-comparison) repeated the installed 720p FG readback and fence checks on the RTX 2050. Both revisions matched the historical readback hash and retired all three final leases. The separate ordinary-fallback verification passed with D3D12 debug-layer and GPU-based validation. The native FG readback run did not use those validation modes.

The [external sample](../../samples/d3d12_fg) was copied outside the SDK source tree and built against the installed package. The public header smoke test compiled and ran as C. The C++ executable used the public ABI for native NGX dispatch without NVIDIA headers or link inputs.

On the reference RTX 2050 Laptop (Windows 11 build 26200, driver 617.14, external SM86 0.4.2 / bundled NGX FG 310.9.16), six submissions finished and produced five verified interpolations. The test rejected invalid native state and stale depth before recording. It gated the final submission, dropped the caller's leases and resource interfaces, and verified that early retirement and destruction failed. All three final lease callbacks ran after the actual fence completed.

The last output matched the prior synthetic [D3D12 reference](../../experiments/d3d12/FRAME_INPUTS.md) byte for byte. SHA-256 `3251fb1b43c329882cfa8236c2bb2828882fd608a19372feff7423bcb2b79128`. The first attempt failed at NGX initialization with other logging sinks disabled and no callback. The successful repeat kept the normal NGX logging configuration.

This run covered image readback and fence lifetime. It skipped DXGI Present, D3D12 debug-layer validation, device-loss injection, cross-queue operation, stock-runtime compatibility and independent DLSS replication.
