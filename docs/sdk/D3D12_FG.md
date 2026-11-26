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

The tested profile uses native-size RGBA8 color, R32_FLOAT depth and RG16_FLOAT motion in non-pixel shader read state, plus a distinct RGBA8 UAV output. It uses one direct command stream, a zero-jitter Y-up camera and one outstanding packet. Resizing requires completing outstanding work and creating a new backend at the new size. Only one FrameLab D3D12 FG backend may own the NGX D3D12 initialization in a process. While it exists, [D3D12SR](D3D12_SR.md#ngx-session-and-d3d12-fg) handles cannot be created, and FG creation returns `FRAMELAB_BUSY` while an SR handle exists. Serialize calls on it and do not manage the same NGX D3D12 session separately during its lifetime.

## Hardware evidence

The [external sample](../../samples/d3d12_fg) was copied outside the SDK source tree and built against the installed package. The public header smoke test compiled and ran as C. The C++ executable used the public ABI for native NGX dispatch without NVIDIA headers or link inputs.

On the reference RTX 2050 Laptop (Windows 11 build 26200, driver 617.14, external SM86 0.4.2 / bundled NGX FG 310.9.16), six submissions finished and produced five verified interpolations. The test rejected invalid native state and stale depth before recording. It gated the final submission, dropped the caller's leases and resource interfaces, and verified that early retirement and destruction failed. All three final lease callbacks ran after the actual fence completed.

The last output matched the prior synthetic [D3D12 reference](../../experiments/d3d12/FRAME_INPUTS.md) byte for byte. SHA-256 `3251fb1b43c329882cfa8236c2bb2828882fd608a19372feff7423bcb2b79128`. The first attempt failed at NGX initialization with other logging sinks disabled and no callback. The successful repeat kept the normal NGX logging configuration.

This run covered image readback and fence lifetime. It skipped DXGI Present, D3D12 debug-layer validation, device-loss injection, cross-queue operation, stock-runtime compatibility and independent DLSS replication.
