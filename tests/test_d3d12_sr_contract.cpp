// SPDX-License-Identifier: MIT
// Portable checks of the D3D12 SR decision layer. No Windows, D3D12 or NGX headers.
#include "backends/d3d12/sr_contract.hpp"

#include <cstdio>
#include <limits>

using namespace framelab::d3d12::sr;

#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #x); return 1; } } while (false)

namespace {
int token[8];

Image image(Role role, uint64_t id, FrameLabExtent extent, int slot)
{
    Image i;
    i.id = id;
    i.identity = &token[slot];
    i.lease = i.sameDevice = true;
    i.dimension = texture2d;
    i.width = extent.width;
    i.height = extent.height;
    i.arraySize = i.mips = i.samples = 1;
    switch (role) {
    case Role::color: i.format = dxgi::rgba8_unorm; break;
    case Role::depth: i.format = dxgi::r32_float; break;
    case Role::motion: i.format = dxgi::rg16_float; break;
    case Role::output: i.format = dxgi::rgba8_unorm; break;
    }
    if (role == Role::output) {
        i.flags = resource_flag::allow_unordered_access;
        i.state = state::unordered_access;
    } else {
        i.state = state::non_pixel_shader_resource;
    }
    return i;
}

Configuration configuration()
{
    Configuration c;
    c.options = {{1280, 720}, mode::quality, 0};
    c.range = {{853, 480}, {640, 360}, {1280, 720}};
    return c;
}

Frame frame(uint64_t index)
{
    Frame f;
    f.commandStream = 1;
    f.frameIndex = index;
    f.commands = f.directCommands = f.commandsSameDevice = true;
    f.render = {853, 480};
    f.color = image(Role::color, 11, f.render, 0);
    f.depth = image(Role::depth, 12, f.render, 1);
    f.motion = image(Role::motion, 13, f.render, 2);
    f.output = image(Role::output, 14, {1280, 720}, 3);
    f.camera.flags = FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_MOTION_INCLUDED;
    f.camera.motion_convention = FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
    f.camera.motion_vector_scale[0] = f.camera.motion_vector_scale[1] = 1;
    return f;
}
} // namespace

int main()
{
    unsigned checks = 0;
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();

    // Options and runtime-reported ranges.
    Options o{{1280, 720}, mode::quality, 0};
    CHECK(checkOptions(o) == FRAMELAB_OK);
    o.flags = option::known;
    CHECK(checkOptions(o) == FRAMELAB_OK);
    o.flags = 8;
    CHECK(checkOptions(o) == FRAMELAB_INVALID_ARGUMENT);
    o = {{31, 720}, mode::quality, 0};
    CHECK(checkOptions(o) == FRAMELAB_INVALID_DIMENSIONS);
    o = {{1280, 720}, 0, 0};
    CHECK(checkOptions(o) == FRAMELAB_UNSUPPORTED);
    o.mode = 4;
    CHECK(checkOptions(o) == FRAMELAB_UNSUPPORTED);
    CHECK(checkRange({{853, 480}, {640, 360}, {1280, 720}}, {1280, 720}) == FRAMELAB_OK);
    CHECK(checkRange({{853, 480}, {853, 480}, {853, 480}}, {1280, 720}) == FRAMELAB_OK); // fixed resolution
    CHECK(checkRange({}, {1280, 720}) == FRAMELAB_UNSUPPORTED);                        // unsupported mode
    CHECK(checkRange({{853, 480}, {900, 360}, {1280, 720}}, {1280, 720}) == FRAMELAB_UNSUPPORTED);
    CHECK(checkRange({{853, 480}, {640, 360}, {1281, 720}}, {1280, 720}) == FRAMELAB_UNSUPPORTED);

    // Native flags, motion scale and NGX status mapping.
    CHECK(featureFlags(0) == ngx::flag_mv_low_res);
    CHECK(featureFlags(option::known) ==
          (ngx::flag_mv_low_res | ngx::flag_hdr | ngx::flag_depth_inverted | ngx::flag_auto_exposure));
    CHECK(!(featureFlags(option::known) & ngx::flag_mv_jittered));
    FrameLabCamera camera{};
    camera.motion_vector_scale[0] = 1;
    camera.motion_vector_scale[1] = -0.5f;
    const auto scale = motionScale(camera, {640, 360});
    CHECK(scale[0] == 640 && scale[1] == -180);
    CHECK(statusForNgx(1, false) == FRAMELAB_OK);
    CHECK(statusForNgx(ngx::out_of_gpu_memory, false) == FRAMELAB_OUT_OF_MEMORY);
    CHECK(statusForNgx(ngx::feature_not_supported, true) == FRAMELAB_UNSUPPORTED);
    CHECK(statusForNgx(ngx::feature_not_supported, false) == FRAMELAB_NATIVE_FAILURE);
    CHECK(statusForNgx(ngx::fail | 5, true) == FRAMELAB_NATIVE_FAILURE);

    // Frame metadata and resource facts. Each rejection happens before native recording.
    const auto config = configuration();
    const auto good = frame(0);
    CHECK(checkFrame(config, good, false, 0) == FRAMELAB_OK);
    auto f = good;
    f.commands = false;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_ARGUMENT);
    f = good;
    f.commandsSameDevice = false;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IDENTITY);
    f = good;
    f.directCommands = false;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_SYNC);
    f = good;
    f.commandStream = 0;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_SYNC);
    f = good;
    f.reset = 32;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_ARGUMENT);
    f.reset = known_reset;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_OK);
    f = good;
    f.preExposure = -1;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_ARGUMENT);
    f.preExposure = nan;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_ARGUMENT);
    f = good;
    f.exposureScale = std::numeric_limits<float>::infinity();
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_ARGUMENT);
    f = good;
    f.frameIndex = 4;
    CHECK(checkFrame(config, f, true, 4) == FRAMELAB_INVALID_SEQUENCE);
    CHECK(checkFrame(config, f, true, 5) == FRAMELAB_INVALID_SEQUENCE);
    CHECK(checkFrame(config, f, true, 3) == FRAMELAB_OK);

    // Dynamic resolution stays inside the runtime range. Inputs may be larger than the active rectangle.
    f = good;
    f.render = {640, 360};
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_OK);
    f.render = {639, 360};
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_DIMENSIONS);
    f = good;
    f.render = {1280, 720};
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_DIMENSIONS); // inputs are smaller than the rectangle
    f.color.width = f.depth.width = f.motion.width = 1280;
    f.color.height = f.depth.height = f.motion.height = 720;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_OK);
    f.render = {1281, 720};
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_DIMENSIONS);
    f = good;
    f.output.width = 1279;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_DIMENSIONS);

    // Camera conventions.
    f = good;
    f.camera.flags &= ~FRAMELAB_CAMERA_VERIFIED;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_CAMERA);
    f = good;
    f.camera.flags |= 16;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_CAMERA);
    f = good;
    f.camera.motion_convention = 0;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_CAMERA);
    f = good;
    f.camera.flags &= ~FRAMELAB_CAMERA_MOTION_INCLUDED;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_UNSUPPORTED);
    f = good;
    f.camera.flags |= FRAMELAB_CAMERA_DEPTH_INVERTED;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_CAMERA);
    auto inverted = config;
    inverted.options.flags = option::depth_inverted;
    CHECK(checkFrame(inverted, f, false, 0) == FRAMELAB_OK);
    CHECK(checkFrame(inverted, good, false, 0) == FRAMELAB_INVALID_CAMERA);
    f = good;
    f.camera.jitter_pixels[0] = 0.5f;
    f.camera.jitter_pixels[1] = -0.5f;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_OK);
    f.camera.jitter_pixels[1] = -0.51f;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_CAMERA);
    f = good;
    f.camera.motion_vector_scale[1] = 0;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_CAMERA);
    f = good;
    f.camera.clip_to_previous[5] = nan;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_CAMERA);

    // Missing, wrong-device, aliased or mismatched resources.
    f = good;
    f.depth.identity = nullptr;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IMAGE_SEMANTICS);
    f = good;
    f.motion.lease = false;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_MISSING_LEASE);
    f = good;
    f.color.sameDevice = false;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IDENTITY);
    f = good;
    f.output.id = 0;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IDENTITY);
    f = good;
    f.output.id = f.color.id;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IDENTITY);
    f = good;
    f.output.identity = f.color.identity;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IDENTITY);
    f = good;
    f.depth.mips = 2;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IMAGE_DESCRIPTION);
    f = good;
    f.color.samples = 4;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IMAGE_DESCRIPTION);
    f = good;
    f.motion.dimension = 2;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IMAGE_DESCRIPTION);
    f = good;
    f.depth.flags = resource_flag::deny_shader_resource;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IMAGE_DESCRIPTION);
    f = good;
    f.output.flags = 0;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_IMAGE_DESCRIPTION);
    f = good;
    f.motion.format = dxgi::rgba16_float;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_UNSUPPORTED);
    f.motion.format = dxgi::rg32_float;
    f.depth.format = dxgi::d24_unorm_s8_uint;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_OK);
    f.depth.format = dxgi::d32_float;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_OK);
    f = good;
    f.color.state = state::unordered_access;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_RESOURCE_STATE);
    f = good;
    f.depth.state = state::non_pixel_shader_resource | 0x80;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_RESOURCE_STATE);
    f = good;
    f.output.state = state::non_pixel_shader_resource;
    CHECK(checkFrame(config, f, false, 0) == FRAMELAB_INVALID_RESOURCE_STATE);

    // HDR requires floating-point color and output.
    auto hdr = config;
    hdr.options.flags = option::hdr;
    CHECK(checkFrame(hdr, good, false, 0) == FRAMELAB_UNSUPPORTED);
    f = good;
    f.color.format = f.output.format = dxgi::rgba16_float;
    CHECK(checkFrame(hdr, f, false, 0) == FRAMELAB_OK);

    // Configuration must be submitted and retired on the GPU before evaluation.
    Lifecycle life(5);
    Work work{};
    CHECK(life.beginRecord() == FRAMELAB_INVALID_PHASE);
    CHECK(life.markSubmitted(6) == FRAMELAB_INVALID_PHASE);
    CHECK(life.retire(10, false, work) == FRAMELAB_INVALID_PHASE);
    CHECK(life.cancel(work) == FRAMELAB_INVALID_PHASE);
    CHECK(life.beginConfigure() == FRAMELAB_OK);
    life.configureRecorded(true, config);
    CHECK(life.beginConfigure() == FRAMELAB_BUSY);
    CHECK(life.beginRecord() == FRAMELAB_BUSY);
    CHECK(life.invalidate() == FRAMELAB_BUSY);
    CHECK(life.markSubmitted(5) == FRAMELAB_INVALID_SYNC); // not beyond the value observed at create
    CHECK(life.markSubmitted(UINT64_MAX) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(life.markSubmitted(6) == FRAMELAB_OK);
    CHECK(life.markSubmitted(7) == FRAMELAB_INVALID_PHASE);
    CHECK(life.cancel(work) == FRAMELAB_INVALID_PHASE); // submitted work cannot be cancelled
    CHECK(life.retire(5, false, work) == FRAMELAB_NOT_READY && work == Work::none);
    CHECK(!life.ready());
    CHECK(life.retire(6, false, work) == FRAMELAB_OK && work == Work::configure);
    CHECK(life.ready() && life.phase() == Phase::idle);

    // First frame resets. A consecutive submitted frame continues history.
    CHECK(life.beginRecord() == FRAMELAB_OK);
    CHECK(life.resetFor(0, 0));
    life.evaluateRecorded(true, 0);
    CHECK(life.markSubmitted(7) == FRAMELAB_OK && life.hasFrame() && life.lastFrame() == 0);
    CHECK(life.beginRecord() == FRAMELAB_BUSY); // one packet in flight
    CHECK(life.retire(6, false, work) == FRAMELAB_NOT_READY);
    CHECK(life.retire(9, false, work) == FRAMELAB_OK && work == Work::evaluate);
    CHECK(!life.resetFor(1, 0));
    CHECK(life.resetFor(1, FRAMELAB_RESET_CAMERA_CUT));
    CHECK(life.resetFor(2, 0)); // skipped frame index
    life.evaluateRecorded(true, 1);
    CHECK(life.markSubmitted(8) == FRAMELAB_OK);
    CHECK(life.retire(8, false, work) == FRAMELAB_OK);
    CHECK(!life.resetFor(2, 0));

    // Cancelled or failed recordings, and explicit invalidation, reset the next frame.
    life.evaluateRecorded(true, 2);
    CHECK(life.cancel(work) == FRAMELAB_OK && work == Work::evaluate);
    CHECK(life.lastFrame() == 1 && life.resetFor(2, 0));
    CHECK(life.ready());
    life.evaluateRecorded(true, 2);
    CHECK(life.markSubmitted(9) == FRAMELAB_OK);
    CHECK(life.retire(9, false, work) == FRAMELAB_OK);
    CHECK(!life.resetFor(3, 0));
    life.evaluateRecorded(false, 3);
    CHECK(life.markSubmitted(10) == FRAMELAB_INVALID_PHASE);
    CHECK(life.beginRecord() == FRAMELAB_BUSY);
    CHECK(life.cancel(work) == FRAMELAB_OK && work == Work::evaluate);
    CHECK(life.resetFor(3, 0));
    life.evaluateRecorded(true, 3);
    CHECK(life.markSubmitted(10) == FRAMELAB_OK);
    CHECK(life.retire(10, false, work) == FRAMELAB_OK);
    CHECK(life.invalidate() == FRAMELAB_OK);
    CHECK(life.resetFor(4, 0));

    // Resize: reconfiguration resets history and blocks frames until its creation retires.
    auto resized = config;
    resized.options.output = {960, 540};
    CHECK(life.beginConfigure() == FRAMELAB_OK);
    life.configureRecorded(true, resized);
    CHECK(!life.ready() && life.configuration().options.output.width == 960);
    CHECK(life.markSubmitted(11) == FRAMELAB_OK);
    CHECK(life.beginRecord() == FRAMELAB_BUSY);
    CHECK(life.retire(11, false, work) == FRAMELAB_OK && work == Work::configure);
    CHECK(life.beginRecord() == FRAMELAB_OK && life.resetFor(4, 0));
    CHECK(life.lastFrame() == 3); // frame indices stay monotonic across reconfiguration

    // A cancelled configuration leaves no usable feature.
    CHECK(life.beginConfigure() == FRAMELAB_OK);
    life.configureRecorded(false, config);
    CHECK(life.cancel(work) == FRAMELAB_OK && work == Work::configure);
    CHECK(!life.ready() && life.beginRecord() == FRAMELAB_INVALID_PHASE);
    life.configureRecorded(true, config);
    CHECK(life.markSubmitted(12) == FRAMELAB_OK);
    CHECK(life.retire(12, false, work) == FRAMELAB_OK && life.ready());

    // UINT64_MAX is reserved: without removal it keeps references, with removal it releases them.
    life.evaluateRecorded(true, 4);
    CHECK(life.markSubmitted(13) == FRAMELAB_OK);
    CHECK(life.retire(UINT64_MAX, false, work) == FRAMELAB_INVALID_SYNC && work == Work::none);
    CHECK(life.phase() == Phase::submitted);
    CHECK(life.retire(UINT64_MAX, true, work) == FRAMELAB_DEVICE_LOST && work == Work::evaluate);
    CHECK(life.lost() && life.phase() == Phase::idle);
    CHECK(life.beginRecord() == FRAMELAB_DEVICE_LOST);
    CHECK(life.beginConfigure() == FRAMELAB_DEVICE_LOST);
    CHECK(life.invalidate() == FRAMELAB_DEVICE_LOST);
    CHECK(life.markSubmitted(14) == FRAMELAB_DEVICE_LOST);

    // Device loss during configuration never marks the feature ready.
    Lifecycle lost(0);
    lost.configureRecorded(true, config);
    CHECK(lost.markSubmitted(1) == FRAMELAB_OK);
    CHECK(lost.retire(UINT64_MAX, true, work) == FRAMELAB_DEVICE_LOST && work == Work::configure);
    CHECK(!lost.ready() && lost.lost());

    // Removal observed by the host while idle.
    Lifecycle idle(0);
    idle.configureRecorded(true, config);
    CHECK(idle.markSubmitted(1) == FRAMELAB_OK && idle.retire(1, false, work) == FRAMELAB_OK);
    idle.markLost();
    CHECK(!idle.ready() && idle.beginRecord() == FRAMELAB_DEVICE_LOST);

    std::printf("PASS D3D12 SR contract checks=%u\n", checks);
    return 0;
}
