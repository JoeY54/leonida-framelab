# Roadmap

Use the [coordination issue](https://github.com/5headsmartbtw/leonida-framelab/issues/4) to claim tasks and post progress. See the [contribution rules](../CONTRIBUTING.md) before submitting changes.

## Independent Vulkan handoff coverage

QA-02 reproduced all four standalone Vulkan queue-handoff cases on the Radeon 860M and the Ryzen 9 9950X3D integrated GPU. Both AMD reports verified exact 64-pixel readback, consumer fence completion and zero diagnostics with synchronization validation enabled. The AMD results extend the standalone RTX 2050 and independent RTX 4070 Ti SUPER evidence. [Device results and original reports](evidence/independent-vulkan-handoff.md#cross-vendor-validation)

## SDK track

| Workstream | Priority | Implementation boundary |
| --- | --- | --- |
| SDK-01 | P0 | The public ABI and [D3D12 FG dispatch](sdk/D3D12_FG.md) are verified. Extract public Vulkan FG and independent SR dispatch next. |
| SDK-02 | P0 | Static/shared CMake package and installed C/C++ consumers pass on Windows and Linux. |
| SR-01 | P0 | The installed [Vulkan SR component](sdk/VULKAN_SR.md) passed the 120-evaluation RTX 2050 profile, readback, resize and history reset. Independent official-runtime validation is tracked in [#24](https://github.com/5headsmartbtw/leonida-framelab/issues/24). |
| D3D-01 | P0 | The installed [public D3D12 FG component](sdk/D3D12_FG.md) passed the external consumer, real fence/lifetime checks and matched the old readback. |
| FL-14 | P0 | The opt-in [D3D12 SR component](sdk/D3D12_SR.md) and its external C/C++ consumer passed native readback, D3D12 debug-layer and GPU-based validation, timing and device removal on an RTX 5090 with the official runtime. The RTX 2050 merge gate is pending. [Issue #29](https://github.com/5headsmartbtw/leonida-framelab/issues/29) |
| VK-01 | P1 | The [standalone image handoff](../tests/vulkan-handoff/README.md) verified binary and timeline waits on distinct queues. Wire it into the FG consumer and test lifetime, cancellation and resize. [Issue #2](https://github.com/5headsmartbtw/leonida-framelab/issues/2) |
| FFI-01 | P1 | [Raw Rust FFI](../bindings/rust/README.md) passes C layout and shared-library lifecycle tests on Windows and Linux. |
| INT-01 / INT-02 | P1 | Read-only [Leonida](integrations/leonida/README.md) and [VIctory](integrations/victory/README.md) audits identify consumer seams and missing evidence. |
| COMBO-01 | P1 | Verify independent and combined SR/FG paths with generated readback. |
| QA-01 | P1 | The [standalone Vulkan handoff replicated on an RTX 4070 Ti SUPER](evidence/independent-vulkan-handoff.md). Independent DLSS replication remains open. |

Use the [experimental release checklist](sdk/RELEASE_CHECKLIST.md) when deciding whether a profile is ready to publish.

## Completed locally

- [x] Independent NVIDIA NGX FG image on RTX 2050 / D3D12.
- [x] Native Windows Vulkan FG with a source-port layer and external SM86 dependency.
- [x] Frame generation in the sample's actual animated 3D scene.
- [x] VSync On/Off, live resize and normal-Present fallback.
- [x] API-neutral FrameInputs, typed bindings, real allocation leases and continuity checks.
- [x] Read-only Leonida pipeline audit; no backend modifications or invented game inputs.
- [x] Phase 3 image comparison, metadata rejection/recovery and validation evidence.

## Next milestones

1. **Independent reproduction:** rerun the configurable sample from a clean checkout on another developer's machine. Record the GPU, driver, SDK and runtime, actual present modes and any unsupported cases. See the [GPU test procedure](BUILD.md#run-the-gpu-checks).
2. **Multi-queue completion:** connect the verified standalone timeline/binary handoff and queue-family ownership transfers to native FG. Never label a different producer submission as the same command stream.
3. **Semantic provider:** identify final-compatible color/depth/MV/camera/jitter for a real frame with evidence. Unknown bindings remain unavailable.
4. **Resource leases in Leonida:** pin actual resource/device/view owners, exact generations and completion tickets through all FG/presentation users.
5. **Shared present seam:** integrate only after previous gates, preserve tracker rollback, lock order, deferred frames and Xbox/DXGI frame-token policy.
6. **SR + FG on the synthetic scene:** use the [verified SR-only baseline](evidence/sr-only-rtx2050.md) to compare SR-only, FG-only, and combined SR → FG separately. Define the color resolution, depth and MV resolution, jitter, motion-vector scaling, and reset rules at the boundary. Require generated-frame readback, validation and ordinary Present fallback. The current direct Vulkan consumer requires equal render and output extents.
7. **Image quality and pacing:** camera cuts, occlusion, rapid motion, resize/memory pressure, scanout and latency measurement.

See the detailed [integration plan](phase3/INTEGRATION_PLAN.en.md) for acceptance conditions.

Tracking issues: [runtime paths](https://github.com/5headsmartbtw/leonida-framelab/issues/1), [multi-queue handoff](https://github.com/5headsmartbtw/leonida-framelab/issues/2), [semantic provider](https://github.com/5headsmartbtw/leonida-framelab/issues/3).
