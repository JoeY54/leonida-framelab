# D3D12 SR + FG consumer (FL-16)

An external consumer of the installed `D3D12SR` and `D3D12FG` components. It uses only the public FrameLab and Windows SDK headers. Each process runs one mode and checks every SDK output by reading it back:

| Mode | What runs | Checked |
| --- | --- | --- |
| `native` | Ordinary rendering at the output size. No FrameLab handle. | Scene readback |
| `sr` | DLSS SR from the optimal render extent to the output | SR output: fully written, object position and palette |
| `fg` | FG on output-size inputs. The handle is created with the shorter v1 config layout (no `render_extent`). | Generated image: fully written, differs from both real frames, object between its two real positions |
| `sr-fg` | SR output used as FG color in the same command list. Depth and motion stay at the render extent. One NGX session for both. | Both of the above, plus lifetime and session checks below |

The `sr-fg` mode also checks:

- **Session.** The handle created first initializes NGX, and the second joins (`--order sr-first`, the default, or `--order fg-first`). FG with different session arguments gets `FRAMELAB_BUSY`. The handle created first is destroyed first, and the other one then evaluates two more verified frames on the still-live session. After both are gone, a new FG handle initializes NGX again.
- **Lifetime.** On the last chained frame the queue is held behind a gate, and the host drops every lease and resource. Both handles return `FRAMELAB_NOT_READY` from retire and `FRAMELAB_BUSY` from destroy. After the gate opens, SR's retirement releases only the render-size color lease. The SR output, depth and motion leases are released only when FG retires, after its final read.
- **Contract.** A render extent larger than the output, a config shorter than v1, a frame claiming the output size as render extent, and the SR output still bound in UAV state are each rejected before recording.
- **Fallback.** From six frames on, SR rejects one frame in the middle (missing depth). The host renders that frame without SR and FG, invalidates FG history, and both handles reset on the next frame.

```powershell
cmake -S samples/d3d12_sr_fg -B build-sr-fg -G "Visual Studio 18 2026" -A x64 -DCMAKE_PREFIX_PATH=<FrameLab install>
cmake --build build-sr-fg --config Release
build-sr-fg\Release\d3d12_sr_fg.exe <NGX runtime directory> <output directory> --mode sr-fg [--order fg-first]
    [--quality performance|balanced|quality] [--output 2560x1440] [--frames 120] [--debug]
```

The NGX runtime directory must contain `nvngx_dlss.dll` and `nvngx_dlssg.dll`. The install must have been built with both `FRAMELAB_BUILD_D3D12_SR` and `FRAMELAB_BUILD_D3D12_FG`. [`tests/run_d3d12_sr_fg_checks.ps1`](../../tests/run_d3d12_sr_fg_checks.ps1) runs every mode, both orders, the debug layer, the 1080p/1440p/4K × quality matrix and the staged frame counts.

This client reads results back and does not present. DXGI presentation of FG output is covered by [samples/d3d12_fg](../d3d12_fg). The synthetic scene has zero jitter, and FG is given unjittered motion vectors and a zero-jitter camera.
