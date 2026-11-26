// SPDX-License-Identifier: MIT
#pragma once
// D3D12 Super Resolution rules without Windows or NGX headers. The native adapter gathers
// resource facts, owns references and calls NGX. Portable tests exercise these decisions.
#include <framelab/framelab.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace framelab::d3d12::sr {

// Numeric Windows SDK, NGX and public-header values. The native adapter static_asserts each one.
namespace dxgi {
constexpr uint32_t rgba16_float = 10, rg32_float = 16, rgba8_unorm = 28, rg16_float = 34,
    d32_float = 40, r32_float = 41, d24_unorm_s8_uint = 45;
}
namespace state {
constexpr uint32_t unordered_access = 0x8, non_pixel_shader_resource = 0x40;
}
namespace resource_flag {
constexpr uint32_t allow_unordered_access = 0x4, deny_shader_resource = 0x8;
}
namespace mode {
constexpr uint32_t performance = 1, balanced = 2, quality = 3;
}
namespace option {
constexpr uint32_t hdr = 1, depth_inverted = 2, auto_exposure = 4, known = hdr | depth_inverted | auto_exposure;
}
namespace ngx {
constexpr uint32_t fail = 0xBAD00000u;
constexpr uint32_t feature_not_supported = fail | 1, out_of_date = fail | 12, out_of_gpu_memory = fail | 13,
    denied = fail | 17, not_implemented = fail | 18;
constexpr int flag_hdr = 1 << 0, flag_mv_low_res = 1 << 1, flag_mv_jittered = 1 << 2,
    flag_depth_inverted = 1 << 3, flag_auto_exposure = 1 << 6;
constexpr bool failed(uint32_t result) { return (result & 0xFFF00000u) == fail; }
}
constexpr uint32_t texture2d = 3;
constexpr uint32_t minimum_output = 32; // DLSS programming guide, supported resolutions.
constexpr uint32_t known_reset = FRAMELAB_RESET_INITIAL | FRAMELAB_RESET_RESIZE | FRAMELAB_RESET_CAMERA_CUT |
    FRAMELAB_RESET_REACTIVATED | FRAMELAB_RESET_DISCONTINUITY;
constexpr uint32_t known_camera = FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_DEPTH_INVERTED |
    FRAMELAB_CAMERA_MOTION_INCLUDED | FRAMELAB_CAMERA_ORTHOGRAPHIC;

struct Options {
    FrameLabExtent output{};
    uint32_t mode = 0, flags = 0;
};

struct Range {
    FrameLabExtent optimal{}, minimum{}, maximum{};
};

struct Configuration {
    Options options;
    Range range;
};

// Facts read from a native resource. `identity` is compared, never dereferenced.
struct Image {
    uint64_t id = 0;
    const void* identity = nullptr;
    bool lease = false, sameDevice = false;
    uint32_t dimension = 0, format = 0, flags = 0, state = 0;
    uint64_t width = 0;
    uint32_t height = 0, arraySize = 0, mips = 0, samples = 0;
};

struct Frame {
    uint64_t commandStream = 0, frameIndex = 0;
    bool commands = false, directCommands = false, commandsSameDevice = false;
    FrameLabExtent render{};
    uint32_t reset = 0;
    float preExposure = 0, exposureScale = 0;
    Image color, depth, motion, output;
    FrameLabCamera camera{};
};

enum class Role { color, depth, motion, output };
enum class Work { none, configure, evaluate };
enum class Phase { idle, recorded, record_failed, submitted };

inline bool within(FrameLabExtent value, FrameLabExtent low, FrameLabExtent high)
{
    return value.width >= low.width && value.width <= high.width &&
        value.height >= low.height && value.height <= high.height;
}

inline FrameLabStatus checkOptions(const Options& o)
{
    if (o.flags & ~option::known) return FRAMELAB_INVALID_ARGUMENT;
    if (o.output.width < minimum_output || o.output.height < minimum_output) return FRAMELAB_INVALID_DIMENSIONS;
    if (o.mode < mode::performance || o.mode > mode::quality) return FRAMELAB_UNSUPPORTED;
    return FRAMELAB_OK;
}

// A runtime without dynamic resolution reports minimum = maximum = optimal. An unsupported mode
// reports a zero optimal extent.
inline FrameLabStatus checkRange(const Range& r, FrameLabExtent output)
{
    const auto ordered = [](uint32_t low, uint32_t optimal, uint32_t high, uint32_t out) {
        return low && low <= optimal && optimal <= high && high <= out;
    };
    return ordered(r.minimum.width, r.optimal.width, r.maximum.width, output.width) &&
        ordered(r.minimum.height, r.optimal.height, r.maximum.height, output.height) ?
        FRAMELAB_OK : FRAMELAB_UNSUPPORTED;
}

inline int featureFlags(uint32_t flags)
{
    // Render-resolution motion vectors without jitter. Display-resolution or jittered motion is not exposed.
    int result = ngx::flag_mv_low_res;
    if (flags & option::hdr) result |= ngx::flag_hdr;
    if (flags & option::depth_inverted) result |= ngx::flag_depth_inverted;
    if (flags & option::auto_exposure) result |= ngx::flag_auto_exposure;
    return result;
}

// FrameLab motion is previous-minus-current UV after `motion_vector_scale`. NGX expects render pixels.
inline std::array<float, 2> motionScale(const FrameLabCamera& camera, FrameLabExtent render)
{
    return {camera.motion_vector_scale[0] * float(render.width),
            camera.motion_vector_scale[1] * float(render.height)};
}

inline FrameLabStatus statusForNgx(uint32_t result, bool capability)
{
    if (!ngx::failed(result)) return FRAMELAB_OK;
    if (result == ngx::out_of_gpu_memory) return FRAMELAB_OUT_OF_MEMORY;
    if (capability && (result == ngx::feature_not_supported || result == ngx::out_of_date ||
                       result == ngx::denied || result == ngx::not_implemented))
        return FRAMELAB_UNSUPPORTED;
    return FRAMELAB_NATIVE_FAILURE;
}

template<std::size_t N> bool finite(const float (&values)[N])
{
    for (const float value : values)
        if (!std::isfinite(value)) return false;
    return true;
}

inline FrameLabStatus checkCamera(const FrameLabCamera& c, bool depthInverted)
{
    if (!finite(c.view_to_clip) || !finite(c.clip_to_view) || !finite(c.clip_to_lens) ||
        !finite(c.clip_to_previous) || !finite(c.previous_to_clip) || !finite(c.position) ||
        !finite(c.up) || !finite(c.right) || !finite(c.forward) || !finite(c.jitter_pixels) ||
        !finite(c.motion_vector_scale) || !std::isfinite(c.near_plane) || !std::isfinite(c.far_plane) ||
        !std::isfinite(c.fov_radians) || !std::isfinite(c.aspect))
        return FRAMELAB_INVALID_CAMERA;
    if (!(c.flags & FRAMELAB_CAMERA_VERIFIED) || (c.flags & ~known_camera) ||
        c.motion_convention != FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV)
        return FRAMELAB_INVALID_CAMERA;
    // Direct NGX SR consumes full motion. Object-only motion would need a camera-motion pass.
    if (!(c.flags & FRAMELAB_CAMERA_MOTION_INCLUDED)) return FRAMELAB_UNSUPPORTED;
    if (bool(c.flags & FRAMELAB_CAMERA_DEPTH_INVERTED) != depthInverted) return FRAMELAB_INVALID_CAMERA;
    if (std::fabs(c.jitter_pixels[0]) > 0.5f || std::fabs(c.jitter_pixels[1]) > 0.5f ||
        c.motion_vector_scale[0] == 0 || c.motion_vector_scale[1] == 0)
        return FRAMELAB_INVALID_CAMERA;
    return FRAMELAB_OK;
}

inline bool allowedFormat(Role role, uint32_t format, bool hdr)
{
    switch (role) {
    case Role::color:
    case Role::output: return format == dxgi::rgba16_float || (!hdr && format == dxgi::rgba8_unorm);
    case Role::depth: return format == dxgi::r32_float || format == dxgi::d32_float || format == dxgi::d24_unorm_s8_uint;
    case Role::motion: return format == dxgi::rg16_float || format == dxgi::rg32_float;
    }
    return false;
}

// Inputs cover the active render rectangle at their top-left. The output matches the configured extent.
inline FrameLabStatus checkImage(Role role, const Image& i, FrameLabExtent required, bool hdr)
{
    const bool output = role == Role::output;
    if (!i.identity) return FRAMELAB_INVALID_IMAGE_SEMANTICS;
    if (!i.lease) return FRAMELAB_MISSING_LEASE;
    if (!i.id || !i.sameDevice) return FRAMELAB_INVALID_IDENTITY;
    if (i.dimension != texture2d || i.arraySize != 1 || i.mips != 1 || i.samples != 1)
        return FRAMELAB_INVALID_IMAGE_DESCRIPTION;
    if (!allowedFormat(role, i.format, hdr)) return FRAMELAB_UNSUPPORTED;
    if (output ? !(i.flags & resource_flag::allow_unordered_access) : (i.flags & resource_flag::deny_shader_resource))
        return FRAMELAB_INVALID_IMAGE_DESCRIPTION;
    if (output ? i.width != required.width || i.height != required.height :
                 i.width < required.width || i.height < required.height)
        return FRAMELAB_INVALID_DIMENSIONS;
    if (i.state != (output ? state::unordered_access : state::non_pixel_shader_resource))
        return FRAMELAB_INVALID_RESOURCE_STATE;
    return FRAMELAB_OK;
}

// Complete metadata check. The adapter records NGX work only after this returns FRAMELAB_OK.
inline FrameLabStatus checkFrame(const Configuration& config, const Frame& f, bool hasFrame, uint64_t lastFrame)
{
    if (!f.commands) return FRAMELAB_INVALID_ARGUMENT;
    if (!f.commandsSameDevice) return FRAMELAB_INVALID_IDENTITY;
    if (!f.directCommands || !f.commandStream) return FRAMELAB_INVALID_SYNC;
    if ((f.reset & ~known_reset) || !std::isfinite(f.preExposure) || f.preExposure < 0 ||
        !std::isfinite(f.exposureScale) || f.exposureScale < 0)
        return FRAMELAB_INVALID_ARGUMENT;
    if (hasFrame && f.frameIndex <= lastFrame) return FRAMELAB_INVALID_SEQUENCE;
    if (!within(f.render, config.range.minimum, config.range.maximum)) return FRAMELAB_INVALID_DIMENSIONS;
    if (const auto camera = checkCamera(f.camera, (config.options.flags & option::depth_inverted) != 0);
        camera != FRAMELAB_OK)
        return camera;
    const bool hdr = (config.options.flags & option::hdr) != 0;
    const Image* images[]{&f.color, &f.depth, &f.motion, &f.output};
    const Role roles[]{Role::color, Role::depth, Role::motion, Role::output};
    // Presence, ownership and identity of all four bindings before any description check.
    for (unsigned i = 0; i < 4; ++i) {
        const auto& image = *images[i];
        if (!image.identity) return FRAMELAB_INVALID_IMAGE_SEMANTICS;
        if (!image.lease) return FRAMELAB_MISSING_LEASE;
        if (!image.id || !image.sameDevice) return FRAMELAB_INVALID_IDENTITY;
        for (unsigned j = 0; j < i; ++j)
            if (image.id == images[j]->id || image.identity == images[j]->identity) return FRAMELAB_INVALID_IDENTITY;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const auto required = roles[i] == Role::output ? config.options.output : f.render;
        if (const auto image = checkImage(roles[i], *images[i], required, hdr); image != FRAMELAB_OK)
            return image;
    }
    return FRAMELAB_OK;
}

// One configuration or evaluation packet at a time. References are released only after the
// completion fence reaches the acknowledged value, or after cancellation of unsubmitted work.
class Lifecycle {
public:
    explicit Lifecycle(uint64_t completedAtCreate) : submitted_(completedAtCreate) {}

    Phase phase() const { return phase_; }
    Work work() const { return work_; }
    bool ready() const { return ready_; }
    bool lost() const { return lost_; }
    bool hasFrame() const { return hasFrame_; }
    uint64_t lastFrame() const { return last_; }
    uint64_t submittedValue() const { return submitted_; }
    const Configuration& configuration() const { return config_; }

    FrameLabStatus beginConfigure() const
    {
        if (lost_) return FRAMELAB_DEVICE_LOST;
        return phase_ == Phase::idle ? FRAMELAB_OK : FRAMELAB_BUSY;
    }

    // Called once the previous feature has been released and native creation was attempted.
    void configureRecorded(bool recorded, const Configuration& config)
    {
        config_ = config;
        ready_ = false;
        needReset_ = true;
        work_ = Work::configure;
        phase_ = recorded ? Phase::recorded : Phase::record_failed;
    }

    FrameLabStatus beginRecord() const
    {
        if (lost_) return FRAMELAB_DEVICE_LOST;
        if (phase_ != Phase::idle) return FRAMELAB_BUSY;
        return ready_ ? FRAMELAB_OK : FRAMELAB_INVALID_PHASE;
    }

    bool resetFor(uint64_t frameIndex, uint32_t resetBits) const
    {
        return needReset_ || resetBits || !hasFrame_ || frameIndex != last_ + 1;
    }

    void evaluateRecorded(bool recorded, uint64_t frameIndex)
    {
        pending_ = frameIndex;
        needReset_ = true;
        work_ = Work::evaluate;
        phase_ = recorded ? Phase::recorded : Phase::record_failed;
    }

    FrameLabStatus markSubmitted(uint64_t value)
    {
        if (value == UINT64_MAX) return FRAMELAB_INVALID_ARGUMENT;
        if (lost_) return FRAMELAB_DEVICE_LOST;
        if (phase_ != Phase::recorded) return FRAMELAB_INVALID_PHASE;
        if (value <= submitted_) return FRAMELAB_INVALID_SYNC;
        submitted_ = value;
        phase_ = Phase::submitted;
        if (work_ == Work::evaluate) {
            last_ = pending_;
            hasFrame_ = true;
            needReset_ = false;
        }
        return FRAMELAB_OK;
    }

    FrameLabStatus cancel(Work& cancelled)
    {
        cancelled = Work::none;
        if (phase_ != Phase::recorded && phase_ != Phase::record_failed) return FRAMELAB_INVALID_PHASE;
        cancelled = work_;
        needReset_ = true;
        work_ = Work::none;
        phase_ = Phase::idle;
        return FRAMELAB_OK;
    }

    // `completed` is the fence value the device reports. UINT64_MAX is reserved for removal.
    FrameLabStatus retire(uint64_t completed, bool deviceRemoved, Work& released)
    {
        released = Work::none;
        if (phase_ != Phase::submitted) return FRAMELAB_INVALID_PHASE;
        if (completed == UINT64_MAX) {
            if (!deviceRemoved) return FRAMELAB_INVALID_SYNC;
            lost_ = true;
        } else if (completed < submitted_) {
            return FRAMELAB_NOT_READY;
        }
        released = work_;
        if (work_ == Work::configure) ready_ = !lost_;
        work_ = Work::none;
        phase_ = Phase::idle;
        return lost_ ? FRAMELAB_DEVICE_LOST : FRAMELAB_OK;
    }

    FrameLabStatus invalidate()
    {
        if (lost_) return FRAMELAB_DEVICE_LOST;
        if (phase_ != Phase::idle) return FRAMELAB_BUSY;
        needReset_ = true;
        return FRAMELAB_OK;
    }

    // Removal observed while idle, before any new native recording.
    void markLost()
    {
        lost_ = true;
        ready_ = false;
    }

private:
    Configuration config_{};
    uint64_t submitted_ = 0, last_ = 0, pending_ = 0;
    Work work_ = Work::none;
    Phase phase_ = Phase::idle;
    bool ready_ = false, lost_ = false, hasFrame_ = false, needReset_ = true;
};

} // namespace framelab::d3d12::sr
