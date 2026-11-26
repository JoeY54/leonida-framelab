# Standalone FrameLab D3D12 SR consumer

Build this directory as an external project against an installed SDK with the `D3D12SR` component. It uses only the installed FrameLab headers, the Windows SDK and the FrameLab import library. It does not include or link NVIDIA headers or libraries.

- `header_test.c` compiles the public header as C and checks the null-handle and descriptor-version contract without a GPU.
- `main.cpp` uploads a known synthetic scene: a moving blue rectangle on a two-tone checkerboard, with matching depth and motion vectors. It runs native DLSS SR and reads back every output.

See the [D3D12 SR guide](../../docs/sdk/D3D12_SR.md) for installation and the API contract.

```powershell
cmake -S samples/d3d12_sr -B build-sr-client -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_PREFIX_PATH=D:/FrameLab'
cmake --build build-sr-client --config Release
ctest --test-dir build-sr-client -C Release --output-on-failure
$exe = (Resolve-Path .\build-sr-client\Release\d3d12_sr.exe).Path
.\tests\run_d3d12_sr_checks.ps1 -Executable $exe -RuntimeDirectory 'D:\runtime\ngx' -LogRoot .\framelab-logs
```

A single mode can be launched directly:

```powershell
& $exe <NGX runtime directory> <output directory> [--validation | --gpu-validation | --timing <frames> | --device-removal]
```

| Mode | What it checks |
| --- | --- |
| default | 1280×720 Quality, resize to 960×540 Balanced, resize to 1280×720 Performance, then a fresh NGX session in the same process. Covers readback, camera cut and reset, missing-input rejection with ordinary fallback and recovery, dynamic resolution where the runtime reports a range, cancellation of unsubmitted work followed by reconfiguration, fence-gated retention and clean destruction. |
| `--validation` | Same sequence with the D3D12 debug layer. Errors and warnings are printed and fail the run. |
| `--gpu-validation` | Same sequence with GPU-based validation as well. |
| `--timing <frames>` | 1280×720 Quality with fixed inputs, no debug layer and no readback. GPU timestamps around each evaluation, CPU frame intervals and local VRAM usage and budget. |
| `--device-removal` | Removes the device with `ID3D12Device5::RemoveDevice` while a gated SR frame is in flight. Checks lease release, rejection after loss, destruction and recovery on a new device. |

## Output checks

Each readback is 1280×720 or 960×540 `RGBA8_UNORM`. The output is filled with a magenta sentinel before each evaluation. A frame passes when:

- no sentinel pixel remains, which rules out an unwritten or clear-only output;
- the count of blue object pixels is within 40% of the analytic area;
- the object's horizontal centroid is within 1% of the output width of its position for that frame. The object moves 2% of the width per frame, so a stale output fails;
- at least 85% of pixels match the current scene's checkerboard palette;
- after the camera cut, at most 2% of pixels still match the previous palette.

Every frame prints its extents, fence value, NGX result, the measurements above and an FNV-1a hash. The last 1280×720 Performance frame before the retention check is saved as `sr-1280x720-rgba8.rgba`. The runner records its SHA-256. Each mode writes a JSON summary to its output directory. The runner combines them in `d3d12-sr-results.json`.

The sample does not present to a display and does not add sub-pixel jitter. The fallback copies the render-resolution raster image into the top-left of the output without scaling. This shows that the host can keep rendering when SR rejects a frame. Thresholds are tuned for this synthetic scene and say nothing about image quality.
