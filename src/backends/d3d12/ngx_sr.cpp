// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <framelab/d3d12_sr.h>

#include "frame_bindings.hpp"
#include "ngx_session.hpp"
#include "sr_contract.hpp"
#include "../../core/fg_stream_internal.hpp"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <array>
#include <memory>
#include <new>

using Microsoft::WRL::ComPtr;
namespace d3d = framelab::d3d12;
namespace sr = framelab::d3d12::sr;
namespace session = framelab::d3d12::ngx;

static_assert(sr::dxgi::rgba16_float == DXGI_FORMAT_R16G16B16A16_FLOAT && sr::dxgi::rg32_float == DXGI_FORMAT_R32G32_FLOAT &&
              sr::dxgi::rgba8_unorm == DXGI_FORMAT_R8G8B8A8_UNORM && sr::dxgi::rg16_float == DXGI_FORMAT_R16G16_FLOAT &&
              sr::dxgi::d32_float == DXGI_FORMAT_D32_FLOAT && sr::dxgi::r32_float == DXGI_FORMAT_R32_FLOAT &&
              sr::dxgi::d24_unorm_s8_uint == DXGI_FORMAT_D24_UNORM_S8_UINT);
static_assert(sr::state::unordered_access == D3D12_RESOURCE_STATE_UNORDERED_ACCESS &&
              sr::state::non_pixel_shader_resource == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
static_assert(sr::resource_flag::allow_unordered_access == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS &&
              sr::resource_flag::deny_shader_resource == D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE &&
              sr::texture2d == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
static_assert(sr::mode::performance == FRAMELAB_SR_PERFORMANCE && sr::mode::balanced == FRAMELAB_SR_BALANCED &&
              sr::mode::quality == FRAMELAB_SR_QUALITY);
static_assert(sr::option::hdr == FRAMELAB_D3D12_SR_HDR && sr::option::depth_inverted == FRAMELAB_D3D12_SR_DEPTH_INVERTED &&
              sr::option::auto_exposure == FRAMELAB_D3D12_SR_AUTO_EXPOSURE);
static_assert(sr::ngx::fail == uint32_t(NVSDK_NGX_Result_Fail) &&
              sr::ngx::feature_not_supported == uint32_t(NVSDK_NGX_Result_FAIL_FeatureNotSupported) &&
              sr::ngx::out_of_date == uint32_t(NVSDK_NGX_Result_FAIL_OutOfDate) &&
              sr::ngx::out_of_gpu_memory == uint32_t(NVSDK_NGX_Result_FAIL_OutOfGPUMemory) &&
              sr::ngx::denied == uint32_t(NVSDK_NGX_Result_FAIL_Denied) &&
              sr::ngx::not_implemented == uint32_t(NVSDK_NGX_Result_FAIL_NotImplemented));
static_assert(sr::ngx::flag_hdr == NVSDK_NGX_DLSS_Feature_Flags_IsHDR &&
              sr::ngx::flag_mv_low_res == NVSDK_NGX_DLSS_Feature_Flags_MVLowRes &&
              sr::ngx::flag_mv_jittered == NVSDK_NGX_DLSS_Feature_Flags_MVJittered &&
              sr::ngx::flag_depth_inverted == NVSDK_NGX_DLSS_Feature_Flags_DepthInverted &&
              sr::ngx::flag_auto_exposure == NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);

namespace {
template<class T> bool abi(const T& data)
{
    return data.struct_size >= sizeof(T) && data.abi_version == FRAMELAB_ABI_VERSION;
}

NVSDK_NGX_PerfQuality_Value quality(uint32_t mode)
{
    switch (mode) {
    case FRAMELAB_SR_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case FRAMELAB_SR_BALANCED: return NVSDK_NGX_PerfQuality_Value_Balanced;
    default: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

sr::Image imageFacts(const FrameLabD3D12SrImage& image, ID3D12Device& device)
{
    sr::Image result;
    result.id = image.resource_id;
    result.identity = image.resource;
    result.lease = image.lease != nullptr;
    result.state = image.state;
    if (!image.resource) return result;
    result.sameDevice = d3d::onDevice(*image.resource, device);
    const auto desc = image.resource->GetDesc();
    result.dimension = static_cast<uint32_t>(desc.Dimension);
    result.format = static_cast<uint32_t>(desc.Format);
    result.flags = static_cast<uint32_t>(desc.Flags);
    result.width = desc.Width;
    result.height = desc.Height;
    result.arraySize = desc.DepthOrArraySize;
    result.mips = desc.MipLevels;
    result.samples = desc.SampleDesc.Count;
    return result;
}

void fill(FrameLabD3D12SrSettings& out, const sr::Range& range)
{
    out.optimal_render_extent = range.optimal;
    out.min_render_extent = range.minimum;
    out.max_render_extent = range.maximum;
}

void clear(FrameLabD3D12SrSettings& out)
{
    fill(out, {});
}

FrameLabStatus recordingFailure(NVSDK_NGX_Result result)
{
    const auto status = sr::statusForNgx(static_cast<uint32_t>(result), false);
    if (status == FRAMELAB_OUT_OF_MEMORY) return status;
    return FRAMELAB_NATIVE_FAILURE;
}
} // namespace

struct FrameLabD3D12Sr {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    session::SessionRef ngx;
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    sr::Lifecycle lifecycle;
    ComPtr<ID3D12GraphicsCommandList> commands;
    std::array<ComPtr<ID3D12Resource>, 4> resources;
    std::array<std::shared_ptr<const void>, 4> leases;
    uint32_t lastResult{};
    FrameLabD3D12SrReportFn report{};
    void* user{};

    explicit FrameLabD3D12Sr(uint64_t completed) : lifecycle(completed) {}

    ~FrameLabD3D12Sr()
    {
        // Feature and parameter maps go before the session reference that may run Shutdown1.
        releaseFeature();
        if (parameters) NVSDK_NGX_D3D12_DestroyParameters(parameters);
        ngx.reset();
    }

    void note(const char* operation, uint32_t result)
    {
        lastResult = result;
        if (report) report(user, operation, result);
    }

    void releaseFeature()
    {
        if (!feature) return;
        NVSDK_NGX_D3D12_ReleaseFeature(feature);
        feature = nullptr;
    }

    void dropPending()
    {
        commands.Reset();
        for (auto& resource : resources) resource.Reset();
        for (auto& lease : leases) lease.reset();
    }

    // Checked only while idle, before new native recording.
    bool removed()
    {
        if (SUCCEEDED(device->GetDeviceRemovedReason())) return false;
        lifecycle.markLost();
        return true;
    }

    FrameLabStatus listCheck(ID3D12GraphicsCommandList& list) const
    {
        if (!d3d::onDevice(list, *device.Get())) return FRAMELAB_INVALID_IDENTITY;
        return list.GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT ? FRAMELAB_OK : FRAMELAB_INVALID_SYNC;
    }

    FrameLabStatus query(const sr::Options& options, sr::Range& range)
    {
        unsigned optimalWidth = 0, optimalHeight = 0, maxWidth = 0, maxHeight = 0, minWidth = 0, minHeight = 0;
        float sharpness = 0;
        const auto result = NGX_DLSS_GET_OPTIMAL_SETTINGS(parameters, options.output.width, options.output.height,
            quality(options.mode), &optimalWidth, &optimalHeight, &maxWidth, &maxHeight, &minWidth, &minHeight,
            &sharpness);
        note("NGX_DLSS_GET_OPTIMAL_SETTINGS", static_cast<uint32_t>(result));
        if (NVSDK_NGX_FAILED(result)) return sr::statusForNgx(static_cast<uint32_t>(result), true);
        range = {{optimalWidth, optimalHeight}, {minWidth, minHeight}, {maxWidth, maxHeight}};
        return sr::checkRange(range, options.output);
    }
};

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_create(const FrameLabD3D12SrConfig* config, FrameLabD3D12Sr** out)
{
    if (!out) return FRAMELAB_INVALID_ARGUMENT;
    *out = nullptr;
    if (!config) return FRAMELAB_INVALID_ARGUMENT;
    if (!abi(*config)) return FRAMELAB_ABI_MISMATCH;
    const auto& c = *config;
    if (!c.device || !c.queue || !c.completion_fence || !c.project_id || !*c.project_id || !c.engine_version ||
        !c.ngx_runtime_directory || !*c.ngx_runtime_directory || !c.log_directory || !*c.log_directory)
        return FRAMELAB_INVALID_ARGUMENT;
    if (!d3d::onDevice(*c.queue, *c.device) || !d3d::onDevice(*c.completion_fence, *c.device))
        return FRAMELAB_INVALID_IDENTITY;
    if (c.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return FRAMELAB_INVALID_SYNC;
    if (FAILED(c.device->GetDeviceRemovedReason())) return FRAMELAB_DEVICE_LOST;
    const auto completed = c.completion_fence->GetCompletedValue();
    if (completed == UINT64_MAX) return FRAMELAB_DEVICE_LOST;

    std::unique_ptr<FrameLabD3D12Sr> handle(new (std::nothrow) FrameLabD3D12Sr(completed));
    if (!handle) return FRAMELAB_OUT_OF_MEMORY;
    auto& s = *handle;
    s.device = c.device;
    s.queue = c.queue;
    s.fence = c.completion_fence;
    s.report = c.report;
    s.user = c.report_user;

    uint32_t init = 0;
    bool initialized = false;
    const auto joined = session::acquire(
        {c.device, c.project_id, c.engine_version, c.ngx_runtime_directory, c.log_directory}, s.ngx, init, initialized);
    if (initialized) s.note("NGX_D3D12_Init", init);
    else if (joined == FRAMELAB_OK) s.note("NGX_D3D12_Session_Join", init);
    if (joined != FRAMELAB_OK) return joined;

    // A capability map is required for the optimal-settings callback. It is reused for create and evaluate.
    const auto caps = NVSDK_NGX_D3D12_GetCapabilityParameters(&s.parameters);
    s.note("NGX_D3D12_GetCapabilityParameters", static_cast<uint32_t>(caps));
    if (NVSDK_NGX_FAILED(caps)) return sr::statusForNgx(static_cast<uint32_t>(caps), true);
    if (!s.parameters) return FRAMELAB_NATIVE_FAILURE;
    int available = 0;
    const auto queried = s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    s.note("SuperSampling.Available", static_cast<uint32_t>(queried));
    if (NVSDK_NGX_FAILED(queried) || available != 1) {
        // Values, not NGX status codes, for these three diagnostic reports.
        int needsDriver = 0, major = 0, minor = 0, initResult = 0;
        if (NVSDK_NGX_SUCCEED(s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult)))
            s.note("SuperSampling.FeatureInitResult", static_cast<uint32_t>(initResult));
        if (NVSDK_NGX_SUCCEED(s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver)))
            s.note("SuperSampling.NeedsUpdatedDriver", static_cast<uint32_t>(needsDriver));
        if (NVSDK_NGX_SUCCEED(s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major)) &&
            NVSDK_NGX_SUCCEED(s.parameters->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor))) {
            s.note("SuperSampling.MinDriverVersionMajor", static_cast<uint32_t>(major));
            s.note("SuperSampling.MinDriverVersionMinor", static_cast<uint32_t>(minor));
        }
        s.lastResult = static_cast<uint32_t>(queried);
        return FRAMELAB_UNSUPPORTED;
    }
    *out = handle.release();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_destroy(FrameLabD3D12Sr* sr)
{
    if (sr && sr->lifecycle.phase() != sr::Phase::idle) return FRAMELAB_BUSY;
    delete sr;
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_query_settings(
    FrameLabD3D12Sr* sr, const FrameLabD3D12SrOptions* options, FrameLabD3D12SrSettings* out)
{
    if (!sr || !options || !out) return FRAMELAB_INVALID_ARGUMENT;
    if (!abi(*options) || !abi(*out)) return FRAMELAB_ABI_MISMATCH;
    clear(*out);
    if (sr->lifecycle.lost()) return FRAMELAB_DEVICE_LOST;
    const sr::Options o{options->output_extent, options->mode, options->flags};
    if (const auto status = sr::checkOptions(o); status != FRAMELAB_OK) return status;
    sr::Range range;
    const auto status = sr->query(o, range);
    if (status == FRAMELAB_OK) fill(*out, range);
    return status;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_record_configure(FrameLabD3D12Sr* sr,
    ID3D12GraphicsCommandList* commands, const FrameLabD3D12SrOptions* options, FrameLabD3D12SrSettings* out)
{
    if (!sr || !commands || !options || !out) return FRAMELAB_INVALID_ARGUMENT;
    if (!abi(*options) || !abi(*out)) return FRAMELAB_ABI_MISMATCH;
    clear(*out);
    if (const auto status = sr->lifecycle.beginConfigure(); status != FRAMELAB_OK) return status;
    if (sr->removed()) return FRAMELAB_DEVICE_LOST;
    if (const auto status = sr->listCheck(*commands); status != FRAMELAB_OK) return status;
    sr::Configuration config{{options->output_extent, options->mode, options->flags}, {}};
    if (const auto status = sr::checkOptions(config.options); status != FRAMELAB_OK) return status;
    if (const auto status = sr->query(config.options, config.range); status != FRAMELAB_OK) return status;
    fill(*out, config.range);

    // Idle means the previous feature's last evaluation has retired on the GPU.
    sr->releaseFeature();
    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = config.range.optimal.width;
    create.Feature.InHeight = config.range.optimal.height;
    create.Feature.InTargetWidth = config.options.output.width;
    create.Feature.InTargetHeight = config.options.output.height;
    create.Feature.InPerfQualityValue = quality(config.options.mode);
    create.InFeatureCreateFlags = sr::featureFlags(config.options.flags);
    create.InEnableOutputSubrects = false;
    // Release the feature's memory on reconfiguration instead of caching it.
    sr->parameters->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    sr->commands = commands;
    const auto result = NGX_D3D12_CREATE_DLSS_EXT(commands, 1, 1, &sr->feature, sr->parameters, &create);
    sr->note("NGX_D3D12_CREATE_DLSS_EXT", static_cast<uint32_t>(result));
    const bool recorded = NVSDK_NGX_SUCCEED(result) && sr->feature;
    sr->lifecycle.configureRecorded(recorded, config);
    if (recorded) return FRAMELAB_OK;
    return recordingFailure(result);
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_record(FrameLabD3D12Sr* sr, const FrameLabD3D12SrFrame* f)
{
    if (!sr || !f) return FRAMELAB_INVALID_ARGUMENT;
    if (!abi(*f)) return FRAMELAB_ABI_MISMATCH;
    if (const auto status = sr->lifecycle.beginRecord(); status != FRAMELAB_OK) return status;
    if (sr->removed()) return FRAMELAB_DEVICE_LOST;

    auto& device = *sr->device.Get();
    sr::Frame facts;
    facts.commandStream = f->command_stream_id;
    facts.frameIndex = f->frame_index;
    facts.commands = f->commands != nullptr;
    if (f->commands) {
        facts.commandsSameDevice = d3d::onDevice(*f->commands, device);
        facts.directCommands = f->commands->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
    facts.render = f->render_extent;
    facts.reset = f->reset_history;
    facts.preExposure = f->pre_exposure;
    facts.exposureScale = f->exposure_scale;
    facts.color = imageFacts(f->color, device);
    facts.depth = imageFacts(f->depth, device);
    facts.motion = imageFacts(f->motion_vectors, device);
    facts.output = imageFacts(f->output, device);
    facts.camera = f->camera;
    const auto& lifecycle = sr->lifecycle;
    if (const auto status = sr::checkFrame(lifecycle.configuration(), facts, lifecycle.hasFrame(), lifecycle.lastFrame());
        status != FRAMELAB_OK)
        return status;

    const bool reset = lifecycle.resetFor(f->frame_index, f->reset_history);
    const auto scale = sr::motionScale(f->camera, f->render_extent);
    NVSDK_NGX_D3D12_DLSS_Eval_Params args{};
    args.Feature.pInColor = f->color.resource;
    args.Feature.pInOutput = f->output.resource;
    args.pInDepth = f->depth.resource;
    args.pInMotionVectors = f->motion_vectors.resource;
    args.InJitterOffsetX = f->camera.jitter_pixels[0];
    args.InJitterOffsetY = f->camera.jitter_pixels[1];
    args.InRenderSubrectDimensions = {f->render_extent.width, f->render_extent.height};
    args.InReset = reset ? 1 : 0;
    args.InMVScaleX = scale[0];
    args.InMVScaleY = scale[1];
    args.InPreExposure = f->pre_exposure;
    args.InExposureScale = f->exposure_scale;

    // Pin before recording: a failed recording can still reference these objects.
    const FrameLabD3D12SrImage* images[]{&f->color, &f->depth, &f->motion_vectors, &f->output};
    sr->commands = f->commands;
    for (unsigned i = 0; i < 4; ++i) {
        sr->resources[i] = images[i]->resource;
        sr->leases[i] = framelab::detail::retainLease(images[i]->lease);
    }
    const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(f->commands, sr->feature, sr->parameters, &args);
    const bool recorded = NVSDK_NGX_SUCCEED(result);
    sr->lastResult = static_cast<uint32_t>(result);
    if (!recorded) sr->note("NGX_D3D12_EVALUATE_DLSS_EXT", sr->lastResult);
    sr->lifecycle.evaluateRecorded(recorded, f->frame_index);
    if (recorded) return FRAMELAB_OK;
    return recordingFailure(result);
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_mark_submitted(FrameLabD3D12Sr* sr, uint64_t value)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    return sr->lifecycle.markSubmitted(value);
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_cancel(FrameLabD3D12Sr* sr)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    sr::Work cancelled{};
    if (const auto status = sr->lifecycle.cancel(cancelled); status != FRAMELAB_OK) return status;
    // The discarded list never executes and the previous evaluation already retired, so the feature
    // is not in flight. NGX tracked the discarded recording's internal resource states, so the
    // feature cannot be reused. The host configures a fresh one.
    sr->releaseFeature();
    sr->dropPending();
    return FRAMELAB_OK;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_retire(FrameLabD3D12Sr* sr)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    if (sr->lifecycle.phase() != sr::Phase::submitted) return FRAMELAB_INVALID_PHASE;
    const auto completed = sr->fence->GetCompletedValue();
    const bool removed = completed == UINT64_MAX && FAILED(sr->device->GetDeviceRemovedReason());
    sr::Work released{};
    const auto status = sr->lifecycle.retire(completed, removed, released);
    if (released != sr::Work::none) sr->dropPending();
    return status;
}

FrameLabStatus FRAMELAB_CALL framelab_d3d12_sr_invalidate(FrameLabD3D12Sr* sr)
{
    if (!sr) return FRAMELAB_INVALID_ARGUMENT;
    return sr->lifecycle.invalidate();
}

uint32_t FRAMELAB_CALL framelab_d3d12_sr_last_ngx_result(const FrameLabD3D12Sr* sr)
{
    return sr ? sr->lastResult : 0;
}
