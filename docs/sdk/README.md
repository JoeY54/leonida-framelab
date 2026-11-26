# FrameLab SDK core

FrameLab provides a C++20 library and a C ABI for validating temporal frame metadata and tracking resource ownership across a frame submission. This experimental package contains the FG input contract and lifecycle. The opt-in Windows x64 components on main are [D3D12FG](D3D12_FG.md) for Direct NGX FG, [D3D12SR](D3D12_SR.md) for Direct NGX DLSS SR and [VulkanSR](VULKAN_SR.md) for Streamline SR. The legacy Vulkan sample also exercises FG. Installed Vulkan FG and combined SR → FG are verified in open drafts [#20](https://github.com/5headsmartbtw/leonida-framelab/pull/20) and [#23](https://github.com/5headsmartbtw/leonida-framelab/pull/23), with extended SyncVal tracked in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24).

## Install and link

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
cmake --install build --config Release --prefix /path/to/framelab
```

Use `BUILD_SHARED_LIBS=ON` for the shared library. Configure the client with `-DCMAKE_PREFIX_PATH=/path/to/framelab`. A C client compiles as C and links with the C++ runtime used by the library. For a Windows shared build, put the installed DLL on the application's DLL search path or next to the executable.

```cmake
project(client LANGUAGES C CXX)
find_package(FrameLab 0.4.0 EXACT CONFIG REQUIRED)
add_executable(client main.c)
target_link_libraries(client PRIVATE FrameLab::FrameLab)
```

Include `<framelab/framelab.h>`. The installed header uses fixed-width fields, C linkage and opaque handles, with a documented C calling convention. The SDK core builds without GPU SDKs or a game runtime. Native NVIDIA SDKs/runtime for the [optional sample](../BUILD.md) are acquired separately.

## ABI and ownership

Compare `framelab_abi_version()` with `FRAMELAB_ABI_VERSION` before using ABI v1. Initialize frame descriptors to zero, set `struct_size` to `sizeof(FrameLabFgFrame)` and set `abi_version`. Use the enum constants for formats, completion modes and flags. Preserve header-native alignment. The header and library must use the same target ABI. Treat the package as experimental; the CMake package defaults to exact-version matching.

1. Create a stream and one host lease per actual owned allocation or synchronization primitive. The release callback takes ownership of a release obligation only after `framelab_lease_create` succeeds. Its host payload must remain valid until that callback, even if the host drops its lease handle first.
2. Prepare a frame. The SDK checks that color, depth, motion, camera, frame identity, state, dimensions, producer completion metadata and temporal continuity agree. The same lease identity must be reused for an unchanged allocation. Declared timeline values are metadata until a backend performs a real wait.
3. Record and submit GPU work using the host/backend's native API. After successful submission, call `framelab_fg_mark_submitted` to advance history. Cancel an unsubmitted packet only when its command work cannot later run. Use `framelab_fg_invalidate` on an idle stream after a continuity break.
4. Retire the frame after verifying that the actual GPU users of the packet have finished. A queued fence or a successful return from the renderer is insufficient. The host keeps any additional ownership required by presentation or temporal history. Device-loss paths must establish that the device can no longer touch the resources before retirement.
5. Destroy the idle stream and remaining lease handles. An outstanding frame makes stream destruction return `FRAMELAB_BUSY`. Each final release callback runs once, on the thread dropping the last reference.

Serialize calls on a stream externally. Release callbacks must not throw or call back into the same stream. ABI v1 supports one pending or in-flight packet per stream. An invalid descriptor does not advance history. Keep FG and SR feature policy independent so that either path can run without the other and both can fall back to ordinary presentation.

See the [installed-package consumer tests](../../tests/external-consumer/), the [FrameInputs semantics](../phase3/CONTRACT.en.md) and the [experimental release checklist](RELEASE_CHECKLIST.md).

[Rust bindings and ABI tests](../../bindings/rust/README.md) are available for the shared package.
