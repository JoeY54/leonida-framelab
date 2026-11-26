// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <framelab/d3d12_fg.h>

#include "frame_bindings.hpp"
#include "ngx_session.hpp"
#include "../../core/fg_stream_internal.hpp"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>

#include <cstring>
#include <memory>
#include <new>

using Microsoft::WRL::ComPtr;
namespace fi = frame_inputs;
namespace d3d = framelab::d3d12;

namespace {
enum class Work { none, initialize, evaluate };
enum class Phase { idle, recorded, record_failed, submitted };

NVSDK_NGX_DLSSG_Opt_Eval_Params optionsFor(const fi::FrameInputs& frame)
{
    const auto& c = frame.camera;
    NVSDK_NGX_DLSSG_Opt_Eval_Params options{};
    options.multiFrameCount = options.multiFrameIndex = 1;
    options.cameraNear = c.nearPlane;
    options.cameraFar = c.farPlane;
    options.cameraFOV = c.fovRadians;
    options.cameraAspectRatio = c.aspect;
    std::memcpy(options.cameraPos, c.position.data(), sizeof options.cameraPos);
    std::memcpy(options.cameraUp, c.up.data(), sizeof options.cameraUp);
    std::memcpy(options.cameraRight, c.right.data(), sizeof options.cameraRight);
    std::memcpy(options.cameraFwd, c.forward.data(), sizeof options.cameraFwd);
    std::memcpy(options.mvecScale, c.motionVectorScale.data(), sizeof options.mvecScale);
    std::memcpy(options.cameraViewToClip, c.viewToClip.data(), sizeof options.cameraViewToClip);
    std::memcpy(options.clipToCameraView, c.clipToView.data(), sizeof options.clipToCameraView);
    std::memcpy(options.clipToLensClip, c.clipToLens.data(), sizeof options.clipToLensClip);
    std::memcpy(options.clipToPrevClip, c.clipToPrevious.data(), sizeof options.clipToPrevClip);
    std::memcpy(options.prevClipToClip, c.previousToClip.data(), sizeof options.prevClipToClip);
    options.reset = frame.resetHistory != fi::Reset::none;
    options.depthInverted = c.depthInverted;
    options.cameraMotionIncluded = c.cameraMotionIncluded;
    options.orthoProjection = c.orthographic;
    options.motionVectorsInvalidValue = 1e30f;
    options.backbufferSubrectSize = {frame.color.extent.width, frame.color.extent.height};
    options.depthSubrectSize = {frame.depth.extent.width, frame.depth.extent.height};
    options.mvecsSubrectSize = {frame.motionVectors.extent.width, frame.motionVectors.extent.height};
    options.outputInterpSubrectSize = {frame.outputExtent.width, frame.outputExtent.height};
    return options;
}

d3d::ImageBinding borrow(const FrameLabD3D12FgImage& binding, const fi::Image& image)
{
    return {binding.resource_id, {image.allocationLease, binding.resource},
            static_cast<D3D12_RESOURCE_STATES>(binding.state)};
}
} // namespace

struct FrameLabD3D12Fg {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12GraphicsCommandList> commands;
    std::array<ComPtr<ID3D12Resource>, 4> resources;
    fi::Id deviceId{}, queueId{};
    FrameLabExtent extent{};
    uint64_t submittedValue{};
    FrameLabFgStream* stream{};
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    NVSDK_NGX_Result lastResult{};
    Work work = Work::none;
    Phase phase = Phase::idle;
    bool initialized = false;
    bool ready = false;
    bool lost = false;

    ~FrameLabD3D12Fg()
    {
        if (feature) NVSDK_NGX_D3D12_ReleaseFeature(feature);
        if (parameters) NVSDK_NGX_D3D12_DestroyParameters(parameters);
        if (initialized) NVSDK_NGX_D3D12_Shutdown1(device.Get());
        framelab_fg_stream_destroy(stream);
        framelab::d3d12::ngx::releaseExclusive();
    }

    bool directList(ID3D12GraphicsCommandList* list) const
    {
        return list && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && d3d::onDevice(*list, *device.Get());
    }

    void clearPending()
    {
        for (auto& resource : resources) resource.Reset();
        commands.Reset();
        work = Work::none;
        phase = Phase::idle;
    }
};

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_create(const FrameLabD3D12FgConfig* config,
                                                       FrameLabD3D12Fg** out)
{
    if (!out) return FRAMELAB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!config) return FRAMELAB_INVALID_ARGUMENT;
    if (config->struct_size < sizeof *config || config->abi_version != FRAMELAB_ABI_VERSION)
        return FRAMELAB_ABI_MISMATCH;
    const auto& c = *config;
    if (!c.device || !c.queue || !c.completion_fence || !c.device_id || !c.queue_id ||
        !c.extent.width || !c.extent.height || !c.project_id || !*c.project_id ||
        !c.engine_version || !c.ngx_runtime_directory || !*c.ngx_runtime_directory ||
        !c.log_directory || !*c.log_directory)
        return FRAMELAB_INVALID_ARGUMENT;
    if (c.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !d3d::onDevice(*c.queue, *c.device) || !d3d::onDevice(*c.completion_fence, *c.device))
        return FRAMELAB_INVALID_RESOURCE_STATE;
    const auto completed = c.completion_fence->GetCompletedValue();
    if (completed == UINT64_MAX) return FRAMELAB_DEVICE_LOST;
    // FG initializes and shuts down NGX itself, so it cannot share a live D3D12 SR session.
    if (!framelab::d3d12::ngx::claimExclusive()) return FRAMELAB_BUSY;
    std::unique_ptr<FrameLabD3D12Fg> fg(new (std::nothrow) FrameLabD3D12Fg);
    if (!fg) {
        framelab::d3d12::ngx::releaseExclusive();
        return FRAMELAB_OUT_OF_MEMORY;
    }
    fg->device = c.device;
    fg->queue = c.queue;
    fg->fence = c.completion_fence;
    fg->deviceId = c.device_id;
    fg->queueId = c.queue_id;
    fg->extent = c.extent;
    fg->submittedValue = completed;
    const auto stream = framelab_fg_stream_create(FRAMELAB_ABI_VERSION, &fg->stream);
    if (stream != FRAMELAB_OK) return stream;
    const auto report = [&](const char* operation) {
        if (c.report) c.report(c.report_user, operation, static_cast<uint32_t>(fg->lastResult));
    };
    const wchar_t* paths[] = {c.ngx_runtime_directory};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    fg->lastResult = NVSDK_NGX_D3D12_Init_with_ProjectID(
        c.project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, c.engine_version, c.log_directory,
        c.device, &info, NVSDK_NGX_Version_API);
    report("NGX_D3D12_Init");
    if (NVSDK_NGX_FAILED(fg->lastResult)) return FRAMELAB_NATIVE_FAILURE;
    fg->initialized = true;
    NVSDK_NGX_Parameter* caps{};
    fg->lastResult = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    report("NGX_D3D12_GetCapabilityParameters");
    if (NVSDK_NGX_FAILED(fg->lastResult) || !caps) {
        if (caps) NVSDK_NGX_D3D12_DestroyParameters(caps);
        return FRAMELAB_NATIVE_FAILURE;
    }
    int available = 0;
    fg->lastResult = caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
    report("FrameGeneration.Available");
    NVSDK_NGX_D3D12_DestroyParameters(caps);
    if (NVSDK_NGX_FAILED(fg->lastResult) || available != 1) return FRAMELAB_UNSUPPORTED;
    fg->lastResult = NVSDK_NGX_D3D12_AllocateParameters(&fg->parameters);
    report("NGX_D3D12_AllocateParameters");
    if (NVSDK_NGX_FAILED(fg->lastResult) || !fg->parameters) return FRAMELAB_NATIVE_FAILURE;
    *out = fg.release();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_destroy(FrameLabD3D12Fg* fg)
{
    if (fg && fg->phase != Phase::idle) return FRAMELAB_BUSY;
    delete fg;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_record_initialize(
    FrameLabD3D12Fg* fg, ID3D12GraphicsCommandList* commands)
{
    if (!fg) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->lost) return FRAMELAB_DEVICE_LOST;
    if (fg->phase != Phase::idle) return FRAMELAB_BUSY;
    if (fg->ready) return FRAMELAB_INVALID_PHASE;
    if (!fg->directList(commands)) return FRAMELAB_INVALID_RESOURCE_STATE;
    NVSDK_NGX_DLSSG_Create_Params info{};
    info.Width = info.RenderWidth = fg->extent.width;
    info.Height = info.RenderHeight = fg->extent.height;
    info.NativeBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    fg->parameters->Set("DLSSG.SynchronousInit", 1u);
    fg->parameters->Set("DLSSG.EnableInterp", 1u);
    fg->commands = commands;
    fg->work = Work::initialize;
    fg->lastResult = NGX_D3D12_CREATE_DLSSG(commands, 1, 1, &fg->feature, fg->parameters, &info);
    const bool success = NVSDK_NGX_SUCCEED(fg->lastResult) && fg->feature;
    fg->phase = success ? Phase::recorded : Phase::record_failed;
    return success ? FRAMELAB_OK : FRAMELAB_NATIVE_FAILURE;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_record(
    FrameLabD3D12Fg* fg, const FrameLabFgFrame* frame, const FrameLabD3D12FgBindings* bindings)
{
    if (!fg || !frame || !bindings) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->lost) return FRAMELAB_DEVICE_LOST;
    if (fg->phase != Phase::idle) return FRAMELAB_BUSY;
    if (!fg->ready) return FRAMELAB_INVALID_PHASE;
    if (bindings->struct_size < sizeof *bindings || bindings->abi_version != FRAMELAB_ABI_VERSION)
        return FRAMELAB_ABI_MISMATCH;
    if (!fg->directList(bindings->commands) || !bindings->command_stream_id)
        return FRAMELAB_INVALID_RESOURCE_STATE;
    const auto prepared = framelab_fg_prepare(fg->stream, frame);
    if (prepared != FRAMELAB_OK) return prepared;
    const auto& input = *framelab::detail::preparedInputs(fg->stream);
    auto reject = [&](FrameLabStatus status) {
        framelab_fg_cancel(fg->stream);
        return status;
    };
    if (input.outputExtent != fi::Extent{fg->extent.width, fg->extent.height})
        return reject(FRAMELAB_INVALID_DIMENSIONS);
    if (input.frameIndex == UINT64_MAX) return reject(FRAMELAB_INVALID_SEQUENCE);
    const auto color = borrow(bindings->color, input.color);
    const auto depth = borrow(bindings->depth, input.depth);
    const auto motion = borrow(bindings->motion_vectors, input.motionVectors);
    const auto& generated = bindings->generated;
    const d3d::ImageBinding output{generated.resource_id, {color.resource, generated.resource},
                                  static_cast<D3D12_RESOURCE_STATES>(generated.state)};
    const d3d::RecordContext context{fg->deviceId, fg->queueId, bindings->command_stream_id,
                                    *fg->device.Get(), *fg->queue.Get(), *bindings->commands};
    if (const auto native = d3d::check(input, context, color, depth, motion, output); !native) {
        if (native.error == fi::Error::unsupported) return reject(FRAMELAB_UNSUPPORTED);
        if (native.error == fi::Error::sync) return reject(FRAMELAB_INVALID_SYNC);
        return reject(FRAMELAB_INVALID_RESOURCE_STATE);
    }
    fg->commands = bindings->commands;
    fg->resources[0] = bindings->color.resource;
    fg->resources[1] = bindings->depth.resource;
    fg->resources[2] = bindings->motion_vectors.resource;
    fg->resources[3] = bindings->generated.resource;
    fg->work = Work::evaluate;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params args{};
    args.pBackbuffer = color.resource.get();
    args.pDepth = depth.resource.get();
    args.pMVecs = motion.resource.get();
    args.pOutputInterpFrame = output.resource.get();
    auto options = optionsFor(input);
    fg->parameters->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(input.frameIndex + 1));
    fg->parameters->Set("DLSSG.EnableInterp", 1u);
    fg->parameters->Set("DLSSG.IsRecording", 0u);
    fg->parameters->Set("DLSSG.FlushRequired", 0u);
    fg->lastResult = NGX_D3D12_EVALUATE_DLSSG(bindings->commands, fg->feature, fg->parameters, &args, &options);
    const bool success = NVSDK_NGX_SUCCEED(fg->lastResult);
    fg->phase = success ? Phase::recorded : Phase::record_failed;
    return success ? FRAMELAB_OK : FRAMELAB_NATIVE_FAILURE;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_mark_submitted(FrameLabD3D12Fg* fg, uint64_t value)
{
    if (!fg || value == UINT64_MAX) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::recorded) return FRAMELAB_INVALID_PHASE;
    if (value <= fg->submittedValue) return FRAMELAB_INVALID_SYNC;
    if (fg->work == Work::evaluate) framelab_fg_mark_submitted(fg->stream);
    fg->submittedValue = value;
    fg->phase = Phase::submitted;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_cancel(FrameLabD3D12Fg* fg)
{
    if (!fg) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::recorded && fg->phase != Phase::record_failed) return FRAMELAB_INVALID_PHASE;
    if (fg->work == Work::evaluate) framelab_fg_cancel(fg->stream);
    else if (fg->feature) {
        NVSDK_NGX_D3D12_ReleaseFeature(fg->feature);
        fg->feature = nullptr;
    }
    fg->clearPending();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_retire(FrameLabD3D12Fg* fg)
{
    if (!fg) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::submitted) return FRAMELAB_INVALID_PHASE;
    const auto completed = fg->fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        // Reserve this value for device removal. Never treat it as a successful frame.
        if (SUCCEEDED(fg->device->GetDeviceRemovedReason())) return FRAMELAB_INVALID_SYNC;
        fg->lost = true;
    } else if (completed < fg->submittedValue) {
        return FRAMELAB_NOT_READY;
    }
    if (fg->work == Work::evaluate) framelab_fg_retire(fg->stream);
    else fg->ready = !fg->lost;
    fg->clearPending();
    return fg->lost ? FRAMELAB_DEVICE_LOST : FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_fg_invalidate(FrameLabD3D12Fg* fg)
{
    if (!fg) return FRAMELAB_INVALID_ARGUMENT;
    if (fg->phase != Phase::idle) return FRAMELAB_BUSY;
    return framelab_fg_invalidate(fg->stream);
}

uint32_t FRAMELAB_CALL framelab_d3d12_fg_last_ngx_result(const FrameLabD3D12Fg* fg)
{
    return fg ? static_cast<uint32_t>(fg->lastResult) : 0;
}
