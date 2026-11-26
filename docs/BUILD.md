# Build and reproduce

## CPU contract tests — portable default

Requires CMake 3.20+ and C++20. No game data or GPU dependencies:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Pass `-DFRAMEINPUTS_VULKAN_INCLUDE=/path/to/Vulkan/Include` to build the additional CPU-only Vulkan-binding tests. Without headers, only the neutral test target is built. The validated Windows environment ran both targets (59 checks).

## Windows GPU reference — opt-in

Use a 64-bit Windows developer environment with MSVC 2022 and CMake 3.20+. Obtain the following dependencies separately under their respective terms.

| Dependency | Tested version or layout |
| --- | --- |
| Vulkan SDK | 1.4.363.1 with `glslangValidator` and the Khronos validation layer |
| [nvpro_core](https://github.com/nvpro-samples/nvpro_core) | `bc19d6ac3ef62938d0ea0e099735878457ce1b6e`, including its submodules |
| [Streamline SDK](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.1) | 2.14.1, with `include/` and `bin/x64/` |
| [DLSS SDK](https://github.com/NVIDIA/DLSS/tree/v310.9.1) | v310.9.1, with Vulkan NGX FG headers and the Windows x64 NGX library |
| GPU runtime | A runtime that reports working support for the chosen backend. The RTX 2050 evidence uses an external SM86 0.4.2 profile with Bundled runtime 310.9.16 and an explicit cubin layer. |

From the repository root, replace these example paths with your SDK locations. Use a writable nvpro_core checkout for its own generated files.

```powershell
git clone https://github.com/nvpro-samples/nvpro_core.git D:/dependencies/nvpro_core
git -C D:/dependencies/nvpro_core checkout bc19d6ac3ef62938d0ea0e099735878457ce1b6e
git -C D:/dependencies/nvpro_core submodule update --init --recursive

$env:VULKAN_SDK = 'D:\SDKs\VulkanSDK'
cmake -S . -B build-gpu -G 'Visual Studio 17 2022' -A x64 `
  -DFRAMELAB_BUILD_SAMPLE=ON `
  '-DBASE_DIRECTORY=D:/dependencies' `
  '-DSTREAMLINE_ROOT_DIR=D:/SDKs/Streamline' `
  '-DDLSS_NGX_SDK_ROOT=D:/SDKs/DLSS'
cmake --build build-gpu --config Release --parallel
$exe = (Resolve-Path '.\build-gpu\bin\Release\vk_streamline.exe').Path
```

`BASE_DIRECTORY` is the parent of `nvpro_core/`. CMake generates FrameLab shader headers in the build tree and copies the local Streamline runtime files next to the built sample. Keep these files local.

### Configure the runtime

| Setting | Behavior |
| --- | --- |
| `FRAMELAB_NGX_RUNTIME_DIR` | Optional NGX feature search directory. Defaults to the executable's directory. |
| `FRAMELAB_LOG_DIR` | Optional output directory. Defaults to `framelab-logs` under the working directory. The sample creates `streamline/` and, in Direct NGX mode, `direct/` beneath it. |
| `FRAMELAB_SM86_PROXY` | Optional explicit path to the existing app-local SM86 library for the historical Direct NGX profile. Keep it with its own private runtime and configuration. The SR-only mode does not load this proxy. |
| `VK_ADD_LAYER_PATH` or `VK_LAYER_PATH`, plus `VK_INSTANCE_LAYERS` | Standard Vulkan loader variables for locating and enabling an external explicit layer. Point them at your own manifest and layer name if the profile needs one. Include the SDK's validation manifest directory if using `VK_LAYER_PATH`. |

The historical RTX 2050 profile uses `VK_LAYER_DLSSG_OPEN_cubin`, `DLSSG_OPEN_RETARGET_SM=86`, and the external SM86 proxy. These are explicit settings for that profile. The sample leaves device selection and third-party layer settings to the launch environment. Start a normal, non-elevated PowerShell session so the loader can honor explicit layer paths. See the [Khronos loader guide](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md#layer-discovery).

### Run the GPU checks

```powershell
& $exe --verify-direct-fg --frames 6 --validation --no-vsync
.\tests\run_gpu_checks.ps1 -Executable $exe -LogRoot .\framelab-logs
# Optional comparison with a locally retained FrameLab readback
.\tests\run_gpu_checks.ps1 -Executable $exe -BaselineReadback 'D:\evidence\generated-5.rgba16f'
```

The runner covers Immediate and FIFO presentation, both with image readback, plus rejection and recovery for an intentionally missing depth input. A different actual present mode is reported as a profile mismatch. Every run gets separate case directories and a `gpu-results.json`. Without an external baseline it still checks the actual generated output and records the image SHA-256. Byte identity is an optional comparison for a matching setup.

Use `& $exe --verify-sr --validation` to reproduce the [standalone Super Resolution check](evidence/sr-only-rtx2050.md). On main, use the component guides for [installed VulkanSR](sdk/VULKAN_SR.md), [installed D3D12FG](sdk/D3D12_FG.md) and [installed D3D12SR](sdk/D3D12_SR.md). For the separate, unmerged installed Vulkan FG, combined SR + FG or multi-queue variants, use the exact checkout and build instructions in [#20](https://github.com/5headsmartbtw/leonida-framelab/pull/20), [#23](https://github.com/5headsmartbtw/leonida-framelab/pull/23) or [#27](https://github.com/5headsmartbtw/leonida-framelab/pull/27). Extended SyncVal remains open in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24). See the [sample options](../sample/README.md) and [reference validation results](phase3/VALIDATION.en.md).

For independent replication, report the FrameLab commit, GPU model, driver, OS build, SDK/runtime and layer versions, exact commands, observed present modes, and the result or first failing step. Share sanitized text and counts in [Coordination](https://github.com/5headsmartbtw/leonida-framelab/issues/4). Device-specific results belong to their tested configuration.

## Components not built by default

`FRAMELAB_BUILD_LAYER=ON` builds only the Windows layer from source against Vulkan headers; it does not provide the external SM86 host/proxy route or an SDK runtime. Legacy probe .cmd files remain provenance/reproduction references for their original paths. No prebuilt GPU executable, SDK DLL or kernel payload is published here.
