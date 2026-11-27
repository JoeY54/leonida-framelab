// SPDX-License-Identifier: MIT
// External consumer of the installed D3D12SR and D3D12FG components (FL-16). Uses only public FrameLab
// and Windows SDK headers. One process runs one mode:
//   native  ordinary rendering at the output size, no FrameLab handle
//   sr      DLSS SR only
//   fg      FG only, at the output size, created with the v1 (pre-render_extent) config layout
//   sr-fg   DLSS SR output chained into FG in one command list, on one shared NGX session
// Every SDK output is read back and checked against the known synthetic scene.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <framelab/d3d12_fg.h>
#include <framelab/d3d12_sr.h>

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
constexpr uint32_t sentinel = 0xffff00ffu;     // magenta, never produced by the scene
constexpr uint32_t objectColor = 0xfff08020u;  // R=0x20 G=0x80 B=0xF0
constexpr float objectWidth = 0.125f, objectTop = 1.0f / 3, objectHeight = 1.0f / 3, speed = 0.02f;
constexpr char projectId[] = "0c5b7e2a-9d41-4f6e-8a37-5e2c1b9d4f60";
constexpr char engineVersion[] = "framelab-d3d12-sr-fg-0.4";
constexpr uint64_t queueId = 1, deviceId = 1;

void check(HRESULT result, const char* step)
{
    if (SUCCEEDED(result)) return;
    std::fprintf(stderr, "%s: HRESULT 0x%08lx\n", step, static_cast<unsigned long>(result));
    throw std::runtime_error(step);
}

void sdk(FrameLabStatus result, const char* step)
{
    if (result == FRAMELAB_OK) return;
    std::fprintf(stderr, "%s: %s\n", step, framelab_status_string(result));
    throw std::runtime_error(step);
}

void expect(bool condition, const char* detail)
{
    if (!condition) throw std::runtime_error(detail);
}

unsigned contractChecks = 0;
void expectStatus(FrameLabStatus actual, FrameLabStatus expected, const char* name)
{
    if (actual != expected) {
        std::fprintf(stderr, "contract %s: expected %s, got %s\n", name, framelab_status_string(expected),
                     framelab_status_string(actual));
        throw std::runtime_error(name);
    }
    ++contractChecks;
    std::printf("ABI_CHECK %s=%s\n", name, framelab_status_string(actual));
}

// Session accounting from the create-time reports of both components.
unsigned ngxInits = 0, ngxJoins = 0;
void FRAMELAB_CALL report(void* user, const char* operation, uint32_t result)
{
    const bool init = std::strcmp(operation, "NGX_D3D12_Init") == 0;
    const bool join = std::strcmp(operation, "NGX_D3D12_Session_Join") == 0;
    ngxInits += init;
    ngxJoins += join;
    if (init || join || std::strstr(operation, "Available"))
        std::printf("SDK_NATIVE component=%s operation=%s value=%#x\n", static_cast<const char*>(user), operation,
                    result);
}

uint16_t half(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const int32_t exponent = int32_t((bits >> 23) & 0xff) - 127 + 15;
    const uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) return uint16_t(sign);
    if (exponent >= 31) return uint16_t(sign | 0x7c00u);
    uint32_t result = sign | (uint32_t(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u) ++result;
    return uint16_t(result);
}

uint32_t floatBits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

uint64_t fnv1a(const std::vector<uint32_t>& data)
{
    uint64_t hash = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    for (size_t i = 0; i < data.size() * sizeof(uint32_t); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

constexpr uint32_t palette[2]{0xff303030u, 0xff181818u};

struct Scene {
    float left = 0.1f; // normalized object left edge; wraps before leaving the image
    void advance()
    {
        left += speed;
        if (left + objectWidth > 0.9f) left = 0.1f;
    }
    bool wrapped() const { return left == 0.1f; } // exact: set by advance() on wrap and at start
};

struct Inputs {
    std::vector<uint32_t> color, depth, motion;
};

// Samples the analytic scene at pixel centres of an image of the given extent.
void synthesize(const Scene& s, FrameLabExtent e, Inputs& in)
{
    const size_t count = size_t(e.width) * e.height;
    in.color.resize(count);
    in.depth.resize(count);
    in.motion.resize(count);
    const uint32_t objectMotion = half(-speed); // previous minus current, normalized UV, unjittered
    for (uint32_t y = 0; y < e.height; ++y) {
        for (uint32_t x = 0; x < e.width; ++x) {
            const size_t p = size_t(y) * e.width + x;
            const float u = (x + 0.5f) / e.width, v = (y + 0.5f) / e.height;
            const bool object = u >= s.left && u < s.left + objectWidth && v >= objectTop && v < objectTop + objectHeight;
            const bool odd = ((int(u * 40) + int(v * 22.5f)) & 1) != 0;
            in.color[p] = object ? objectColor : palette[odd];
            in.depth[p] = floatBits(object ? 0.3f : 0.9f);
            in.motion[p] = object ? objectMotion : 0;
        }
    }
}

bool isObject(uint32_t pixel)
{
    const int r = pixel & 0xff, g = (pixel >> 8) & 0xff, b = (pixel >> 16) & 0xff;
    return b >= 0xc0 && r <= 0x60 && g >= 0x50 && g <= 0xb0;
}

bool inPalette(uint32_t pixel)
{
    constexpr int tolerance = 24;
    for (int shift = 0; shift < 24; shift += 8) {
        const int value = (pixel >> shift) & 0xff;
        const int a = (palette[0] >> shift) & 0xff, b = (palette[1] >> shift) & 0xff;
        if (value < std::min(a, b) - tolerance || value > std::max(a, b) + tolerance) return false;
    }
    return true;
}

struct Verdict {
    bool ok = false;
    size_t sentinelPixels = 0, objectPixels = 0, palettePixels = 0;
    double centroid = -1, expected = 0;
    uint64_t hash = 0;
};

// Detects a clear-only, stale or wrong output from the known scene at `left`.
Verdict verify(const std::vector<uint32_t>& image, FrameLabExtent e, double left)
{
    Verdict v;
    double sumX = 0;
    for (uint32_t y = 0; y < e.height; ++y)
        for (uint32_t x = 0; x < e.width; ++x) {
            const uint32_t pixel = image[size_t(y) * e.width + x];
            v.sentinelPixels += pixel == sentinel;
            if (isObject(pixel)) {
                ++v.objectPixels;
                sumX += x + 0.5;
            }
            v.palettePixels += inPalette(pixel);
        }
    const double total = double(e.width) * e.height;
    const double area = objectWidth * objectHeight * total;
    v.expected = (left + objectWidth / 2) * e.width;
    v.centroid = v.objectPixels ? sumX / v.objectPixels : -1;
    v.hash = fnv1a(image);
    v.ok = v.sentinelPixels == 0 && v.objectPixels > 0.6 * area && v.objectPixels < 1.4 * area &&
        std::fabs(v.centroid - v.expected) <= 0.01 * e.width && v.palettePixels >= 0.85 * total;
    return v;
}

struct Texture {
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    FrameLabExtent extent{};
};

struct Validation {
    ComPtr<ID3D12InfoQueue> queue;
    unsigned errors = 0, warnings = 0;

    void drain()
    {
        if (!queue) return;
        const auto count = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(queue->GetMessage(i, nullptr, &size)) || !size) continue;
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(queue->GetMessage(i, message, &size))) continue;
            const bool error = message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                message->Severity == D3D12_MESSAGE_SEVERITY_ERROR;
            const bool warning = message->Severity == D3D12_MESSAGE_SEVERITY_WARNING;
            errors += error;
            warnings += warning;
            if (error || warning)
                std::printf("D3D12_DEBUG severity=%d id=%d %s\n", int(message->Severity), int(message->ID),
                            message->pDescription);
        }
        queue->ClearStoredMessages();
    }
};

struct Gpu {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    std::vector<ComPtr<ID3D12Resource>> transfers;
    Validation validation;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t signal = 0;
    uint64_t peakLocal = 0, budget = 0;

    ~Gpu() { if (event) CloseHandle(event); }

    void initialize(bool debugLayer)
    {
        expect(event != nullptr, "create fence event");
        if (debugLayer) {
            ComPtr<ID3D12Debug> debug;
            check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "enable D3D12 debug layer");
            debug->EnableDebugLayer();
        }
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto enumerated = factory->EnumAdapters1(i, &candidate);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            check(enumerated, "enumerate DXGI adapter");
            DXGI_ADAPTER_DESC1 desc{};
            check(candidate->GetDesc1(&desc), "read adapter description");
            if (desc.VendorId != 0x10de) continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
                adapter = candidate;
                LARGE_INTEGER umd{};
                const bool version = SUCCEEDED(candidate->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
                std::printf("GPU name=\"%ls\" device_id=%#x dedicated_mb=%llu umd=%u.%u.%u.%u\n", desc.Description,
                            desc.DeviceId, static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20),
                            version ? unsigned(HIWORD(umd.HighPart)) : 0, version ? unsigned(LOWORD(umd.HighPart)) : 0,
                            version ? unsigned(HIWORD(umd.LowPart)) : 0, version ? unsigned(LOWORD(umd.LowPart)) : 0);
                break;
            }
        }
        expect(device.Get() != nullptr, "NVIDIA D3D12 device unavailable");
        if (debugLayer) {
            check(device.As(&validation.queue), "query D3D12 info queue");
            validation.queue->SetMessageCountLimit(16384);
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "create direct queue");
        check(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "create allocator");
        check(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "create list");
        check(list->Close(), "close initial list");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create completion fence");
        currentLocal();
    }

    uint64_t currentLocal()
    {
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!adapter || FAILED(adapter.As(&adapter3)) ||
            FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return 0;
        peakLocal = std::max<uint64_t>(peakLocal, info.CurrentUsage);
        budget = info.Budget;
        return info.CurrentUsage;
    }

    void begin()
    {
        transfers.clear();
        check(allocator->Reset(), "reset allocator");
        check(list->Reset(allocator.Get(), nullptr), "reset list");
    }

    uint64_t execute()
    {
        check(list->Close(), "close submitted list");
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++signal), "signal completion fence");
        return signal;
    }

    void wait()
    {
        check(fence->SetEventOnCompletion(signal, event), "watch fence");
        expect(WaitForSingleObject(event, 30'000) == WAIT_OBJECT_0, "GPU fence timed out");
        check(device->GetDeviceRemovedReason(), "check device after fence");
        currentLocal();
        validation.drain();
    }

    Texture texture(DXGI_FORMAT format, FrameLabExtent extent, D3D12_RESOURCE_FLAGS flags)
    {
        Texture output;
        output.extent = extent;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, output.state, nullptr,
                                               IID_PPV_ARGS(&output.resource)), "create texture");
        return output;
    }

    ComPtr<ID3D12Resource> buffer(uint64_t bytes, D3D12_HEAP_TYPE type)
    {
        ComPtr<ID3D12Resource> output;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const auto state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&output)), "create transfer buffer");
        return output;
    }

    void barrierUav(Texture& texture)
    {
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = texture.resource.Get();
        list->ResourceBarrier(1, &uav);
    }

    void transition(Texture& texture, D3D12_RESOURCE_STATES state)
    {
        if (texture.state == state) return;
        if (texture.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) barrierUav(texture);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, texture.state, state};
        list->ResourceBarrier(1, &barrier);
        texture.state = state;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint(const Texture& texture, uint64_t& size)
    {
        const auto desc = texture.resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT result{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &result, nullptr, nullptr, &size);
        return result;
    }

    // Every format in this sample has 4-byte texels.
    void upload(Texture& texture, const std::vector<uint32_t>& data, D3D12_RESOURCE_STATES finalState)
    {
        uint64_t size{};
        const auto layout = footprint(texture, size);
        auto input = buffer(size, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{};
        check(input->Map(0, nullptr, &mapped), "map upload buffer");
        const size_t rowBytes = size_t(texture.extent.width) * sizeof(uint32_t);
        for (uint32_t y = 0; y < texture.extent.height; ++y)
            std::memcpy(static_cast<char*>(mapped) + layout.Offset + size_t(y) * layout.Footprint.RowPitch,
                        data.data() + size_t(y) * texture.extent.width, rowBytes);
        input->Unmap(0, nullptr);
        transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{input.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = layout;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(texture, finalState);
        transfers.push_back(std::move(input));
    }

    struct Pending {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        FrameLabExtent extent{};
    };

    Pending readback(Texture& texture)
    {
        Pending result;
        uint64_t size{};
        result.layout = footprint(texture, size);
        result.extent = texture.extent;
        result.buffer = buffer(size, D3D12_HEAP_TYPE_READBACK);
        transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{result.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = result.layout;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        return result;
    }

    std::vector<uint32_t> read(const Pending& pending)
    {
        const auto e = pending.extent;
        std::vector<uint32_t> image(size_t(e.width) * e.height);
        void* mapped{};
        check(pending.buffer->Map(0, nullptr, &mapped), "map readback");
        for (uint32_t y = 0; y < e.height; ++y)
            std::memcpy(image.data() + size_t(y) * e.width,
                        static_cast<const char*>(mapped) + pending.layout.Offset + size_t(y) * pending.layout.Footprint.RowPitch,
                        size_t(e.width) * sizeof(uint32_t));
        pending.buffer->Unmap(0, nullptr);
        return image;
    }
};

struct LeasePayload {
    ComPtr<ID3D12Resource> resource;
    unsigned* releases;
};

void FRAMELAB_CALL release(void* user)
{
    std::unique_ptr<LeasePayload> payload(static_cast<LeasePayload*>(user));
    ++*payload->releases;
}

using Lease = std::unique_ptr<FrameLabLease, decltype(&framelab_lease_destroy)>;
using SrHandle = std::unique_ptr<FrameLabD3D12Sr, decltype(&framelab_d3d12_sr_destroy)>;
using FgHandle = std::unique_ptr<FrameLabD3D12Fg, decltype(&framelab_d3d12_fg_destroy)>;

struct Gate {
    ComPtr<ID3D12Fence> fence;
    ~Gate() { if (fence) fence->Signal(1); }
};

FrameLabCamera camera(FrameLabExtent e)
{
    FrameLabCamera c{};
    c.flags = FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_MOTION_INCLUDED;
    c.projection_space = c.reprojection_space = FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
    c.motion_convention = FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
    c.motion_vector_scale[0] = c.motion_vector_scale[1] = 1; // texels already hold normalized UV motion
    c.near_plane = 0.1f;
    c.far_plane = 1000;
    c.fov_radians = 1;
    c.aspect = float(e.width) / e.height;
    c.up[1] = c.right[0] = c.forward[2] = 1;
    for (unsigned i = 0; i < 16; i += 5)
        c.view_to_clip[i] = c.clip_to_view[i] = c.clip_to_lens[i] = c.clip_to_previous[i] = c.previous_to_clip[i] = 1;
    return c; // zero jitter: the CPU scene is not jittered
}

enum class Mode { native, sr, fg, srFg };

struct Args {
    fs::path runtime, output;
    Mode mode = Mode::srFg;
    bool debug = false, fgFirst = false;
    unsigned frames = 8, srMode = FRAMELAB_SR_QUALITY;
    FrameLabExtent extent{1920, 1080};
};

const char* modeName(Mode m)
{
    switch (m) {
    case Mode::native: return "native";
    case Mode::sr: return "sr";
    case Mode::fg: return "fg";
    default: return "sr-fg";
    }
}

struct Counters {
    unsigned frames = 0, nativeVerified = 0, srEvaluations = 0, srVerified = 0, fgEvaluations = 0, fgVerified = 0;
    unsigned fgPositioned = 0, expectedInterpolations = 0, fallbacks = 0, resets = 0, soloFrames = 0;
    bool retention = false, solo = false, fallback = false, reinit = false;
    double minGap = 1e9, maxGap = 0; // generated centroid offset from the SR midpoint, in units of motion
};

class Run {
public:
    explicit Run(const Args& args) : a_(args) {}
    int execute();

private:
    const Args& a_;
    Gpu gpu_;
    Counters n_;
    unsigned created_ = 0, releases_ = 0;
    uint64_t index_ = 0, generation_ = 0; // generation_ changes with every reallocation
    Scene scene_;
    FrameLabExtent output_{}, render_{};
    Texture color_, depth_, motion_, srOut_, generated_;
    Lease colorLease_{nullptr, &framelab_lease_destroy}, depthLease_{nullptr, &framelab_lease_destroy},
        motionLease_{nullptr, &framelab_lease_destroy}, srOutLease_{nullptr, &framelab_lease_destroy};
    std::vector<uint32_t> previous_;        // last real output image (SR output or native color)
    double previousLeft_ = 0;
    uint64_t warm_ = 0, end_ = 0;
    std::wstring runtime_, logs_, otherLogs_;

    Lease pin(const Texture& texture)
    {
        FrameLabLease* handle{};
        auto payload = std::make_unique<LeasePayload>(LeasePayload{texture.resource, &releases_});
        sdk(framelab_lease_create(payload.get(), release, &handle), "create allocation lease");
        payload.release();
        ++created_;
        return {handle, &framelab_lease_destroy};
    }

    FrameLabD3D12SrConfig srConfig() const
    {
        return {sizeof(FrameLabD3D12SrConfig), FRAMELAB_ABI_VERSION, gpu_.device.Get(), gpu_.queue.Get(),
                gpu_.fence.Get(), projectId, engineVersion, runtime_.c_str(), logs_.c_str(), report,
                const_cast<char*>("sr")};
    }

    FrameLabD3D12FgConfig fgConfig(FrameLabExtent render) const
    {
        FrameLabD3D12FgConfig c{};
        c.struct_size = sizeof c;
        c.abi_version = FRAMELAB_ABI_VERSION;
        c.device_id = deviceId;
        c.queue_id = queueId;
        c.device = gpu_.device.Get();
        c.queue = gpu_.queue.Get();
        c.completion_fence = gpu_.fence.Get();
        c.extent = output_;
        c.project_id = projectId;
        c.engine_version = engineVersion;
        c.ngx_runtime_directory = runtime_.c_str();
        c.log_directory = logs_.c_str();
        c.report = report;
        c.report_user = const_cast<char*>("fg");
        c.render_extent = render;
        return c;
    }

    SrHandle createSr()
    {
        const auto c = srConfig();
        FrameLabD3D12Sr* raw{};
        sdk(framelab_d3d12_sr_create(&c, &raw), "create D3D12 SR");
        return {raw, &framelab_d3d12_sr_destroy};
    }

    FgHandle createFg(FrameLabExtent render, bool legacyLayout = false)
    {
        auto c = fgConfig(render);
        if (legacyLayout) c.struct_size = uint32_t(offsetof(FrameLabD3D12FgConfig, render_extent));
        FrameLabD3D12Fg* raw{};
        sdk(framelab_d3d12_fg_create(&c, &raw), "create D3D12 FG");
        return {raw, &framelab_d3d12_fg_destroy};
    }

    // Records FG feature creation in its own submission and retires it.
    void initializeFg(FrameLabD3D12Fg* fg)
    {
        gpu_.begin();
        const auto status = framelab_d3d12_fg_record_initialize(fg, gpu_.list.Get());
        if (status != FRAMELAB_OK) {
            gpu_.list->Close();
            framelab_d3d12_fg_cancel(fg);
            sdk(status, "record native FG create");
        }
        sdk(framelab_d3d12_fg_mark_submitted(fg, gpu_.execute()), "acknowledge FG create");
        gpu_.wait();
        sdk(framelab_d3d12_fg_retire(fg), "retire FG create");
    }

    FrameLabExtent configureSr(FrameLabD3D12Sr* sr)
    {
        const FrameLabD3D12SrOptions options{sizeof options, FRAMELAB_ABI_VERSION, output_, a_.srMode, 0};
        FrameLabD3D12SrSettings settings{};
        settings.struct_size = sizeof settings;
        settings.abi_version = FRAMELAB_ABI_VERSION;
        sdk(framelab_d3d12_sr_query_settings(sr, &options, &settings), "query DLSS SR render extents");
        gpu_.begin();
        const auto status = framelab_d3d12_sr_record_configure(sr, gpu_.list.Get(), &options, &settings);
        if (status != FRAMELAB_OK) {
            gpu_.list->Close();
            framelab_d3d12_sr_cancel(sr);
            sdk(status, "record DLSS SR feature creation");
        }
        sdk(framelab_d3d12_sr_mark_submitted(sr, gpu_.execute()), "acknowledge SR configure");
        gpu_.wait();
        sdk(framelab_d3d12_sr_retire(sr), "retire SR configure");
        std::printf("SDK_SR_CONFIGURE output=%ux%u mode=%u render=%ux%u\n", output_.width, output_.height, a_.srMode,
                    settings.optimal_render_extent.width, settings.optimal_render_extent.height);
        return settings.optimal_render_extent;
    }

    // Drops the previous textures (their leases release after the last retirement) and pins new ones.
    void allocate(FrameLabExtent inputExtent, bool srOutput)
    {
        colorLease_.reset();
        depthLease_.reset();
        motionLease_.reset();
        srOutLease_.reset();
        ++generation_;
        constexpr auto none = D3D12_RESOURCE_FLAG_NONE;
        constexpr auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        color_ = gpu_.texture(DXGI_FORMAT_R8G8B8A8_UNORM, inputExtent, none);
        depth_ = gpu_.texture(DXGI_FORMAT_R32_FLOAT, inputExtent, none);
        motion_ = gpu_.texture(DXGI_FORMAT_R16G16_FLOAT, inputExtent, none);
        generated_ = gpu_.texture(DXGI_FORMAT_R8G8B8A8_UNORM, output_, uav);
        colorLease_ = pin(color_);
        depthLease_ = pin(depth_);
        motionLease_ = pin(motion_);
        srOut_ = {};
        if (srOutput) {
            srOut_ = gpu_.texture(DXGI_FORMAT_R8G8B8A8_UNORM, output_, uav);
            srOutLease_ = pin(srOut_);
        }
        previous_.clear();
    }

    void uploadInputs(FrameLabExtent e)
    {
        Inputs in;
        synthesize(scene_, e, in);
        constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        gpu_.upload(color_, in.color, read);
        gpu_.upload(depth_, in.depth, read);
        gpu_.upload(motion_, in.motion, read);
    }

    void clearOutput(Texture& t)
    {
        const std::vector<uint32_t> clear(size_t(t.extent.width) * t.extent.height, sentinel);
        gpu_.upload(t, clear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    FrameLabD3D12SrFrame srFrame(uint32_t reset) const
    {
        FrameLabD3D12SrFrame f{};
        f.struct_size = sizeof f;
        f.abi_version = FRAMELAB_ABI_VERSION;
        f.command_stream_id = index_ + 1;
        f.commands = gpu_.list.Get();
        f.frame_index = index_;
        f.render_extent = render_;
        f.reset_history = reset;
        f.color = {11, color_.resource.Get(), uint32_t(color_.state), colorLease_.get()};
        f.depth = {12, depth_.resource.Get(), uint32_t(depth_.state), depthLease_.get()};
        f.motion_vectors = {13, motion_.resource.Get(), uint32_t(motion_.state), motionLease_.get()};
        f.output = {14, srOut_.resource.Get(), uint32_t(srOut_.state), srOutLease_.get()};
        f.camera = camera(render_);
        return f;
    }

    // FG reads `color` at the output extent and depth/motion at `render`.
    void fgFrame(const Texture& color, uint64_t colorId, const FrameLabLease* colorLease, FrameLabExtent render,
                 uint32_t reset, FrameLabFgFrame& f, FrameLabD3D12FgBindings& b) const
    {
        f = {};
        f.struct_size = sizeof f;
        f.abi_version = FRAMELAB_ABI_VERSION;
        f.stream_id = f.device_id = 1;
        f.generation = generation_;
        f.frame_index = index_;
        f.render_extent = render;
        f.output_extent = output_;
        f.delta_seconds = 1.0 / 60;
        f.reset_history = reset;
        f.camera = camera(render);
        f.producer = {queueId, index_ + 1, 0, 0, FRAMELAB_COMPLETION_SAME_COMMAND_STREAM, nullptr};
        f.color = {colorId, index_, queueId, output_, FRAMELAB_FORMAT_RGBA8_UNORM, FRAMELAB_STATE_SHADER_READ,
                   FRAMELAB_PROVENANCE_SYNTHETIC, colorLease};
        f.depth = {12, index_, queueId, render, FRAMELAB_FORMAT_R32_FLOAT, FRAMELAB_STATE_SHADER_READ,
                   FRAMELAB_PROVENANCE_SYNTHETIC, depthLease_.get()};
        f.motion_vectors = {13, index_, queueId, render, FRAMELAB_FORMAT_RG16_FLOAT, FRAMELAB_STATE_SHADER_READ,
                            FRAMELAB_PROVENANCE_SYNTHETIC, motionLease_.get()};
        b = {sizeof b, FRAMELAB_ABI_VERSION, index_ + 1, gpu_.list.Get(),
             {colorId, color.resource.Get(), uint32_t(color.state)},
             {12, depth_.resource.Get(), uint32_t(depth_.state)},
             {13, motion_.resource.Get(), uint32_t(motion_.state)},
             {15, generated_.resource.Get(), uint32_t(generated_.state)}};
    }

    // A generated image must be fully written, differ from both real neighbours, and place the moving
    // object between its previous and current positions.
    void judgeGenerated(const std::vector<uint32_t>& image, const std::vector<uint32_t>& current, double currentLeft)
    {
        bool written = true;
        double sumX = 0;
        size_t object = 0;
        for (uint32_t y = 0; y < output_.height; ++y)
            for (uint32_t x = 0; x < output_.width; ++x) {
                const uint32_t pixel = image[size_t(y) * output_.width + x];
                written &= pixel != sentinel;
                if (isObject(pixel)) {
                    ++object;
                    sumX += x + 0.5;
                }
            }
        const bool distinct = image != current && image != previous_;
        const bool verified = written && distinct;
        n_.fgVerified += verified;
        const double before = (previousLeft_ + objectWidth / 2) * output_.width;
        const double after = (currentLeft + objectWidth / 2) * output_.width;
        const double centroid = object ? sumX / object : -1;
        const double motion = std::fabs(after - before);
        // 0 at the midpoint, 0.5 at either real position.
        const double gap = motion > 0 && object ? std::fabs(centroid - (before + after) / 2) / motion : 1;
        const bool positioned = verified && gap < 0.4;
        n_.fgPositioned += positioned;
        n_.minGap = std::min(n_.minGap, gap);
        n_.maxGap = std::max(n_.maxGap, gap);
        std::printf("SDK_FG_FRAME frame=%llu written=%d distinct=%d object_px=%zu centroid=%.1f previous=%.1f "
                    "current=%.1f midpoint_offset=%.3f fnv1a=%016llx verified=%d positioned=%d\n",
                    static_cast<unsigned long long>(index_), int(written), int(distinct), object, centroid, before, after,
                    gap, static_cast<unsigned long long>(fnv1a(image)), int(verified), int(positioned));
    }

    void sampleMemory(unsigned i)
    {
        const auto usage = gpu_.currentLocal();
        if (i == std::min(a_.frames - 1, std::max(2u, a_.frames / 4))) warm_ = usage;
        end_ = usage;
    }

    void advance()
    {
        ++index_;
        scene_.advance();
    }

    int native();
    int srOnly();
    int fgOnly();
    int srFg();
    bool chained(FrameLabD3D12Sr* sr, FrameLabD3D12Fg* fg, uint32_t reset, bool gated);
    void soloFg(FrameLabD3D12Fg* fg);
    void soloSr(FrameLabD3D12Sr* sr);
    void writeSummary(bool pass);
};

int Run::native()
{
    render_ = output_;
    allocate(output_, false);
    for (unsigned i = 0; i < a_.frames; ++i) {
        gpu_.begin();
        uploadInputs(output_);
        const auto pending = gpu_.readback(color_);
        gpu_.execute();
        gpu_.wait();
        const auto v = verify(gpu_.read(pending), output_, scene_.left);
        n_.nativeVerified += v.ok;
        ++n_.frames;
        sampleMemory(i);
        advance();
    }
    return n_.nativeVerified == n_.frames ? 0 : 1;
}

int Run::srOnly()
{
    auto sr = createSr();
    render_ = configureSr(sr.get());
    allocate(render_, true);
    for (unsigned i = 0; i < a_.frames; ++i) {
        gpu_.begin();
        uploadInputs(render_);
        clearOutput(srOut_);
        const auto frame = srFrame(i ? (scene_.wrapped() ? FRAMELAB_RESET_DISCONTINUITY : 0) : FRAMELAB_RESET_INITIAL);
        const auto status = framelab_d3d12_sr_record(sr.get(), &frame);
        if (status != FRAMELAB_OK) {
            gpu_.list->Close();
            framelab_d3d12_sr_cancel(sr.get());
            sdk(status, "record DLSS SR evaluate");
        }
        const auto pending = gpu_.readback(srOut_);
        sdk(framelab_d3d12_sr_mark_submitted(sr.get(), gpu_.execute()), "acknowledge SR frame");
        gpu_.wait();
        sdk(framelab_d3d12_sr_retire(sr.get()), "retire SR frame");
        const auto v = verify(gpu_.read(pending), output_, scene_.left);
        ++n_.srEvaluations;
        n_.srVerified += v.ok;
        ++n_.frames;
        sampleMemory(i);
        advance();
    }
    sdk(framelab_d3d12_sr_destroy(sr.release()), "destroy SR");
    return n_.srVerified == n_.srEvaluations ? 0 : 1;
}

int Run::fgOnly()
{
    render_ = output_;
    // Callers built against the v1 header pass the shorter config. FG then uses `extent` for every input.
    auto fg = createFg({}, true);
    std::puts("ABI_CHECK fg_v1_config_layout_accepted=OK");
    ++contractChecks;
    initializeFg(fg.get());
    allocate(output_, false);
    for (unsigned i = 0; i < a_.frames; ++i) {
        Inputs in;
        synthesize(scene_, output_, in);
        gpu_.begin();
        uploadInputs(output_);
        clearOutput(generated_);
        FrameLabFgFrame f;
        FrameLabD3D12FgBindings b;
        const uint32_t reset = i ? (scene_.wrapped() ? FRAMELAB_RESET_DISCONTINUITY : 0) : FRAMELAB_RESET_INITIAL;
        fgFrame(color_, 11, colorLease_.get(), output_, reset, f, b);
        const auto status = framelab_d3d12_fg_record(fg.get(), &f, &b);
        if (status != FRAMELAB_OK) {
            gpu_.list->Close();
            framelab_d3d12_fg_cancel(fg.get());
            sdk(status, "record FG evaluate");
        }
        const auto pending = gpu_.readback(generated_);
        sdk(framelab_d3d12_fg_mark_submitted(fg.get(), gpu_.execute()), "acknowledge FG frame");
        gpu_.wait();
        sdk(framelab_d3d12_fg_retire(fg.get()), "retire FG frame");
        ++n_.fgEvaluations;
        if (!reset) {
            ++n_.expectedInterpolations;
            judgeGenerated(gpu_.read(pending), in.color, scene_.left);
        } else if (i) {
            ++n_.resets;
        }
        previous_ = in.color;
        previousLeft_ = scene_.left;
        ++n_.frames;
        sampleMemory(i);
        advance();
    }
    sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy FG");
    return n_.fgVerified == n_.expectedInterpolations && n_.fgPositioned == n_.expectedInterpolations ? 0 : 1;
}

// One source frame: SR evaluates into srOut_, which FG reads as color in the same command list.
// `gated` holds the queue, drops every host reference and checks that the SR output lease stays
// alive until FG retires, after SR has already retired.
bool Run::chained(FrameLabD3D12Sr* sr, FrameLabD3D12Fg* fg, uint32_t reset, bool gated)
{
    gpu_.begin();
    uploadInputs(render_);
    clearOutput(srOut_);
    clearOutput(generated_);
    const auto sf = srFrame(reset);
    auto status = framelab_d3d12_sr_record(sr, &sf);
    if (status != FRAMELAB_OK) {
        gpu_.list->Close();
        framelab_d3d12_sr_cancel(sr);
        sdk(status, "record DLSS SR evaluate");
    }
    // SR wrote srOut_ as a UAV. Make the write visible and hand it to FG as a shader resource.
    gpu_.transition(srOut_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FrameLabFgFrame f;
    FrameLabD3D12FgBindings b;
    fgFrame(srOut_, 14, srOutLease_.get(), render_, reset, f, b);
    status = framelab_d3d12_fg_record(fg, &f, &b);
    if (status != FRAMELAB_OK) {
        gpu_.list->Close();
        framelab_d3d12_sr_cancel(sr);
        framelab_d3d12_fg_cancel(fg);
        sdk(status, "record FG evaluate on SR output");
    }
    const auto srPending = gpu_.readback(srOut_);
    const auto fgPending = gpu_.readback(generated_);
    Gate gate;
    if (gated) {
        check(gpu_.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "create execution gate");
        check(gpu_.queue->Wait(gate.fence.Get(), 1), "block queue on the gate");
    }
    const auto value = gpu_.execute();
    sdk(framelab_d3d12_sr_mark_submitted(sr, value), "acknowledge SR part");
    sdk(framelab_d3d12_fg_mark_submitted(fg, value), "acknowledge FG part");
    if (gated) {
        const unsigned before = releases_;
        colorLease_.reset();
        depthLease_.reset();
        motionLease_.reset();
        srOutLease_.reset();
        color_.resource.Reset();
        depth_.resource.Reset();
        motion_.resource.Reset();
        srOut_.resource.Reset();
        expectStatus(framelab_d3d12_sr_retire(sr), FRAMELAB_NOT_READY, "sr_early_retire");
        expectStatus(framelab_d3d12_fg_retire(fg), FRAMELAB_NOT_READY, "fg_early_retire");
        expect(releases_ == before, "early retirement released pending leases");
        expectStatus(framelab_d3d12_sr_destroy(sr), FRAMELAB_BUSY, "sr_destroy_in_flight");
        expectStatus(framelab_d3d12_fg_destroy(fg), FRAMELAB_BUSY, "fg_destroy_in_flight");
        check(gate.fence->Signal(1), "release execution gate");
        gpu_.wait();
        expect(releases_ == before, "leases released before retirement");
        sdk(framelab_d3d12_sr_retire(sr), "retire gated SR part");
        // SR held all four leases; FG still holds the SR output, depth and motion.
        const unsigned afterSr = releases_ - before;
        sdk(framelab_d3d12_fg_retire(fg), "retire gated FG part");
        const unsigned afterFg = releases_ - before;
        n_.retention = afterSr == 1 && afterFg == 4;
        std::printf("SDK_SR_FG_RETENTION releases_after_sr_retire=%u releases_after_fg_retire=%u verified=%d\n",
                    afterSr, afterFg, int(n_.retention));
        expect(n_.retention, "SR output lease was not retained through the final FG read");
    } else {
        gpu_.wait();
        sdk(framelab_d3d12_sr_retire(sr), "retire SR part");
        sdk(framelab_d3d12_fg_retire(fg), "retire FG part");
    }
    const auto srImage = gpu_.read(srPending);
    const auto v = verify(srImage, output_, scene_.left);
    ++n_.srEvaluations;
    n_.srVerified += v.ok;
    ++n_.fgEvaluations;
    std::printf("SDK_SR_FRAME frame=%llu render=%ux%u output=%ux%u reset_bits=%u fence=%llu sr_ngx=%#x fg_ngx=%#x "
                "sentinel=%zu object_px=%zu centroid=%.1f expected=%.1f fnv1a=%016llx verified=%d\n",
                static_cast<unsigned long long>(index_), render_.width, render_.height, output_.width, output_.height,
                reset, static_cast<unsigned long long>(value), framelab_d3d12_sr_last_ngx_result(sr),
                framelab_d3d12_fg_last_ngx_result(fg), v.sentinelPixels, v.objectPixels, v.centroid, v.expected,
                static_cast<unsigned long long>(v.hash), int(v.ok));
    if (!reset) {
        ++n_.expectedInterpolations;
        judgeGenerated(gpu_.read(fgPending), srImage, scene_.left);
    } else if (index_) {
        ++n_.resets;
    }
    previous_ = srImage;
    previousLeft_ = scene_.left;
    ++n_.frames;
    advance();
    return v.ok;
}

// SR was destroyed first. FG keeps working on the still-live session, now fed an output-size
// ordinary render with render-size depth and motion.
void Run::soloFg(FrameLabD3D12Fg* fg)
{
    allocate(render_, true);
    bool ok = true;
    for (unsigned i = 0; i < 2; ++i) {
        Inputs full;
        synthesize(scene_, output_, full);
        gpu_.begin();
        uploadInputs(render_);
        gpu_.upload(srOut_, full.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        clearOutput(generated_);
        FrameLabFgFrame f;
        FrameLabD3D12FgBindings b;
        // New allocations: a new generation, and the first frame declares the discontinuity.
        const uint32_t reset = i && !scene_.wrapped() ? 0 : FRAMELAB_RESET_DISCONTINUITY;
        fgFrame(srOut_, 14, srOutLease_.get(), render_, reset, f, b);
        const auto status = framelab_d3d12_fg_record(fg, &f, &b);
        if (status != FRAMELAB_OK) {
            gpu_.list->Close();
            framelab_d3d12_fg_cancel(fg);
            sdk(status, "record FG after SR destroy");
        }
        const auto pending = gpu_.readback(generated_);
        sdk(framelab_d3d12_fg_mark_submitted(fg, gpu_.execute()), "acknowledge solo FG frame");
        gpu_.wait();
        sdk(framelab_d3d12_fg_retire(fg), "retire solo FG frame");
        ++n_.fgEvaluations;
        ++n_.soloFrames;
        if (!reset) {
            const unsigned before = n_.fgPositioned;
            ++n_.expectedInterpolations;
            judgeGenerated(gpu_.read(pending), full.color, scene_.left);
            ok &= n_.fgPositioned == before + 1;
        }
        previous_ = full.color;
        previousLeft_ = scene_.left;
        ++n_.frames;
        advance();
    }
    n_.solo = ok;
    std::printf("SDK_SR_FG_SOLO remaining=fg verified=%d\n", int(ok));
}

// FG was destroyed first. SR keeps working on the still-live session.
void Run::soloSr(FrameLabD3D12Sr* sr)
{
    allocate(render_, true);
    bool ok = true;
    for (unsigned i = 0; i < 2; ++i) {
        gpu_.begin();
        uploadInputs(render_);
        clearOutput(srOut_);
        const auto frame = srFrame(i && !scene_.wrapped() ? 0 : FRAMELAB_RESET_DISCONTINUITY);
        const auto status = framelab_d3d12_sr_record(sr, &frame);
        if (status != FRAMELAB_OK) {
            gpu_.list->Close();
            framelab_d3d12_sr_cancel(sr);
            sdk(status, "record SR after FG destroy");
        }
        const auto pending = gpu_.readback(srOut_);
        sdk(framelab_d3d12_sr_mark_submitted(sr, gpu_.execute()), "acknowledge solo SR frame");
        gpu_.wait();
        sdk(framelab_d3d12_sr_retire(sr), "retire solo SR frame");
        const auto v = verify(gpu_.read(pending), output_, scene_.left);
        ++n_.srEvaluations;
        n_.srVerified += v.ok;
        ++n_.soloFrames;
        ok &= v.ok;
        ++n_.frames;
        advance();
    }
    n_.solo = ok;
    std::printf("SDK_SR_FG_SOLO remaining=sr verified=%d\n", int(ok));
}

int Run::srFg()
{
    // Creation order is selectable. The second handle joins the session the first one initialized.
    SrHandle sr{nullptr, &framelab_d3d12_sr_destroy};
    FgHandle fg{nullptr, &framelab_d3d12_fg_destroy};
    if (a_.fgFirst) {
        // FG initializes the session. Its render extent is created from the nominal DLSS scale for the
        // mode; if the runtime's optimal SR extent differs, FG is created again on the live session.
        const double scale = a_.srMode == FRAMELAB_SR_PERFORMANCE ? 0.5 : a_.srMode == FRAMELAB_SR_BALANCED ? 0.58 : 2.0 / 3;
        const FrameLabExtent nominal{uint32_t(std::lround(output_.width * scale)), uint32_t(std::lround(output_.height * scale))};
        fg = createFg(nominal);
        expect(ngxInits == 1 && ngxJoins == 0, "FG did not initialize the session");
        sr = createSr();
        render_ = configureSr(sr.get());
        expect(ngxJoins == 1, "SR did not join the FG session");
        if (render_.width != nominal.width || render_.height != nominal.height) {
            std::printf("SDK_SESSION fg_recreated nominal=%ux%u optimal=%ux%u\n", nominal.width, nominal.height,
                        render_.width, render_.height);
            sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy FG with nominal render extent");
            fg = createFg(render_);
        }
    } else {
        sr = createSr();
        render_ = configureSr(sr.get());
        fg = createFg(render_);
        expect(ngxInits == 1 && ngxJoins == 1, "FG did not join the SR session");
    }
    std::printf("SDK_SESSION order=%s inits=%u joins=%u\n", a_.fgFirst ? "fg-first" : "sr-first", ngxInits, ngxJoins);
    initializeFg(fg.get());

    {
        // Contract checks before any frame.
        FrameLabD3D12Fg* raw{};
        auto c = fgConfig({output_.width + 1, render_.height});
        expectStatus(framelab_d3d12_fg_create(&c, &raw), FRAMELAB_INVALID_DIMENSIONS, "fg_render_larger_than_output");
        c = fgConfig(render_);
        c.log_directory = otherLogs_.c_str();
        expectStatus(framelab_d3d12_fg_create(&c, &raw), FRAMELAB_BUSY, "fg_different_session_arguments");
        c = fgConfig(render_);
        c.struct_size = uint32_t(offsetof(FrameLabD3D12FgConfig, render_extent)) - 1;
        expectStatus(framelab_d3d12_fg_create(&c, &raw), FRAMELAB_ABI_MISMATCH, "fg_short_config");
    }

    allocate(render_, true);
    {
        // FG on this handle expects the SR render extent; a native-size claim is rejected before recording.
        gpu_.begin();
        uploadInputs(render_);
        gpu_.transition(srOut_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FrameLabFgFrame f;
        FrameLabD3D12FgBindings b;
        fgFrame(srOut_, 14, srOutLease_.get(), output_, FRAMELAB_RESET_INITIAL, f, b);
        expectStatus(framelab_d3d12_fg_record(fg.get(), &f, &b), FRAMELAB_INVALID_DIMENSIONS, "fg_render_extent_mismatch");
        fgFrame(srOut_, 14, srOutLease_.get(), render_, FRAMELAB_RESET_INITIAL, f, b);
        b.color.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        expectStatus(framelab_d3d12_fg_record(fg.get(), &f, &b), FRAMELAB_INVALID_RESOURCE_STATE, "fg_sr_output_still_uav");
        // Nothing was recorded by FrameLab; run the uploads to keep the host state tracker truthful.
        gpu_.execute();
        gpu_.wait();
    }

    const unsigned fallbackAt = a_.frames >= 6 ? a_.frames / 2 : UINT32_MAX;
    uint32_t reset = FRAMELAB_RESET_INITIAL;
    for (unsigned i = 0; i < a_.frames; ++i) {
        if (i == fallbackAt) {
            // SR rejects the frame before recording (missing depth). The host renders it without SR and FG,
            // drops FG history, and both handles reset on the next frame.
            gpu_.begin();
            uploadInputs(render_);
            auto bad = srFrame(0);
            bad.depth.resource = nullptr;
            expectStatus(framelab_d3d12_sr_record(sr.get(), &bad), FRAMELAB_INVALID_IMAGE_SEMANTICS, "fallback_missing_depth");
            gpu_.execute();
            gpu_.wait();
            sdk(framelab_d3d12_fg_invalidate(fg.get()), "invalidate FG after fallback");
            n_.fallback = true;
            ++n_.fallbacks;
            ++n_.frames;
            advance();
            reset = FRAMELAB_RESET_DISCONTINUITY;
            continue;
        }
        if (scene_.wrapped() && i) reset |= FRAMELAB_RESET_DISCONTINUITY; // the object wrapped
        chained(sr.get(), fg.get(), reset, i + 1 == a_.frames);
        reset = 0;
        sampleMemory(i);
    }

    // Shutdown ordering: the handle created first is destroyed first. The other keeps working.
    if (a_.fgFirst) {
        sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy FG first");
        soloSr(sr.get());
        sdk(framelab_d3d12_sr_destroy(sr.release()), "destroy SR last");
    } else {
        sdk(framelab_d3d12_sr_destroy(sr.release()), "destroy SR first");
        soloFg(fg.get());
        sdk(framelab_d3d12_fg_destroy(fg.release()), "destroy FG last");
    }
    // The last handle shut NGX down. A new FG handle initializes it again in the same process.
    {
        const unsigned inits = ngxInits;
        auto again = createFg(render_);
        initializeFg(again.get());
        sdk(framelab_d3d12_fg_destroy(again.release()), "destroy re-initialized FG");
        n_.reinit = ngxInits == inits + 1;
        std::printf("SDK_SESSION reinitialized=%d\n", int(n_.reinit));
    }
    colorLease_.reset();
    depthLease_.reset();
    motionLease_.reset();
    srOutLease_.reset();
    return n_.srVerified == n_.srEvaluations && n_.fgVerified == n_.expectedInterpolations &&
        n_.fgPositioned == n_.expectedInterpolations && n_.retention && n_.solo && n_.reinit &&
        (fallbackAt == UINT32_MAX || n_.fallback) ? 0 : 1;
}

void Run::writeSummary(bool pass)
{
    const auto path = a_.output / (std::string("d3d12-sr-fg-") + modeName(a_.mode) + (a_.fgFirst ? "-fg-first" : "") + ".json");
    FILE* file = _wfopen(path.c_str(), L"w");
    expect(file != nullptr, "open summary");
    const uint64_t growth = end_ > warm_ ? end_ - warm_ : 0;
    std::fprintf(file,
        "{\n  \"result\": \"%s\",\n  \"mode\": \"%s\",\n  \"order\": \"%s\",\n  \"debug_layer\": %s,\n"
        "  \"output\": [%u, %u],\n  \"render\": [%u, %u],\n  \"sr_mode\": %u,\n"
        "  \"counters\": {\"frames\": %u, \"native_verified\": %u, \"sr_evaluations\": %u, \"sr_verified\": %u, "
        "\"fg_evaluations\": %u, \"expected_interpolations\": %u, \"fg_verified\": %u, \"fg_positioned\": %u, "
        "\"history_resets\": %u, \"fallbacks\": %u, \"solo_frames\": %u, \"contract_checks\": %u},\n"
        "  \"fg_midpoint_offset\": {\"min\": %.4f, \"max\": %.4f},\n"
        "  \"session\": {\"inits\": %u, \"joins\": %u, \"reinitialized\": %s},\n"
        "  \"lifetime\": {\"sr_output_retained_through_fg\": %s, \"remaining_handle_after_first_destroy\": %s},\n"
        "  \"leases\": {\"created\": %u, \"released\": %u},\n"
        "  \"memory_bytes\": {\"warm\": %llu, \"end\": %llu, \"growth_after_warm\": %llu, \"peak_local\": %llu, "
        "\"budget\": %llu},\n"
        "  \"validation\": {\"errors\": %u, \"warnings\": %u}\n}\n",
        pass ? "pass" : "fail", modeName(a_.mode), a_.fgFirst ? "fg-first" : "sr-first", a_.debug ? "true" : "false",
        output_.width, output_.height, render_.width, render_.height, a_.srMode, n_.frames, n_.nativeVerified,
        n_.srEvaluations, n_.srVerified, n_.fgEvaluations, n_.expectedInterpolations, n_.fgVerified, n_.fgPositioned,
        n_.resets, n_.fallbacks, n_.soloFrames, contractChecks, n_.minGap > 1e8 ? 0.0 : n_.minGap, n_.maxGap,
        ngxInits, ngxJoins, n_.reinit ? "true" : "false", n_.retention ? "true" : "false", n_.solo ? "true" : "false",
        created_, releases_, static_cast<unsigned long long>(warm_), static_cast<unsigned long long>(end_),
        static_cast<unsigned long long>(growth), static_cast<unsigned long long>(gpu_.peakLocal),
        static_cast<unsigned long long>(gpu_.budget), gpu_.validation.errors, gpu_.validation.warnings);
    expect(std::fclose(file) == 0, "write summary");
}

int Run::execute()
{
    gpu_.initialize(a_.debug);
    output_ = a_.extent;
    runtime_ = a_.runtime.wstring();
    logs_ = (a_.output / L"ngx").wstring();
    otherLogs_ = (a_.output / L"ngx-other").wstring();
    fs::create_directories(a_.output / L"ngx");
    fs::create_directories(a_.output / L"ngx-other");
    int result = 1;
    switch (a_.mode) {
    case Mode::native: result = native(); break;
    case Mode::sr: result = srOnly(); break;
    case Mode::fg: result = fgOnly(); break;
    case Mode::srFg: result = srFg(); break;
    }
    color_ = depth_ = motion_ = srOut_ = generated_ = {};
    colorLease_.reset();
    depthLease_.reset();
    motionLease_.reset();
    srOutLease_.reset();
    gpu_.validation.drain();
    const uint64_t growth = end_ > warm_ ? end_ - warm_ : 0;
    const bool stable = a_.frames < 120 || !warm_ || growth <= (32ull << 20);
    const bool pass = !result && releases_ == created_ && stable && !gpu_.validation.errors && !gpu_.validation.warnings;
    std::printf("SDK_SR_FG_SUMMARY mode=%s order=%s output=%ux%u render=%ux%u frames=%u sr=%u/%u fg=%u/%u "
                "positioned=%u resets=%u fallbacks=%u solo=%u contract_checks=%u leases=%u/%u peak_mb=%.1f "
                "growth_mb=%.1f debug_errors=%u debug_warnings=%u\n",
                modeName(a_.mode), a_.fgFirst ? "fg-first" : "sr-first", output_.width, output_.height, render_.width,
                render_.height, n_.frames, n_.srVerified, n_.srEvaluations, n_.fgVerified, n_.expectedInterpolations,
                n_.fgPositioned, n_.resets, n_.fallbacks, n_.soloFrames, contractChecks, releases_, created_,
                gpu_.peakLocal / 1048576.0, growth / 1048576.0, gpu_.validation.errors, gpu_.validation.warnings);
    writeSummary(pass);
    std::printf("FL16_RESULT %s\n", pass ? "pass" : "fail");
    return pass ? 0 : 1;
}

bool parse(int argc, wchar_t** argv, Args& args)
{
    if (argc < 3) return false;
    args.runtime = fs::absolute(argv[1]);
    args.output = fs::absolute(argv[2]);
    for (int i = 3; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool value = i + 1 < argc;
        if (arg == L"--mode" && value) {
            const std::wstring m = argv[++i];
            if (m == L"native") args.mode = Mode::native;
            else if (m == L"sr") args.mode = Mode::sr;
            else if (m == L"fg") args.mode = Mode::fg;
            else if (m == L"sr-fg") args.mode = Mode::srFg;
            else return false;
        } else if (arg == L"--order" && value) {
            const std::wstring o = argv[++i];
            if (o != L"sr-first" && o != L"fg-first") return false;
            args.fgFirst = o == L"fg-first";
        } else if (arg == L"--quality" && value) {
            const std::wstring q = argv[++i];
            if (q == L"performance") args.srMode = FRAMELAB_SR_PERFORMANCE;
            else if (q == L"balanced") args.srMode = FRAMELAB_SR_BALANCED;
            else if (q == L"quality") args.srMode = FRAMELAB_SR_QUALITY;
            else return false;
        } else if (arg == L"--output" && value) {
            unsigned w = 0, h = 0;
            if (std::swscanf(argv[++i], L"%ux%u", &w, &h) != 2) return false;
            args.extent = {w, h};
        } else if (arg == L"--frames" && value) {
            args.frames = unsigned(std::wcstoul(argv[++i], nullptr, 10));
        } else if (arg == L"--debug") {
            args.debug = true;
        } else {
            return false;
        }
    }
    return args.frames >= 3 && args.frames <= 100000 && args.extent.width >= 64 && args.extent.height >= 64 &&
        args.extent.width <= 7680 && args.extent.height <= 4320 && (!args.fgFirst || args.mode == Mode::srFg);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    Args args;
    if (!parse(argc, argv, args)) {
        std::fprintf(stderr,
            "usage: d3d12_sr_fg <NGX runtime directory> <output directory>\n"
            "       [--mode native|sr|fg|sr-fg] [--order sr-first|fg-first] [--quality performance|balanced|quality]\n"
            "       [--output WIDTHxHEIGHT] [--frames N] [--debug]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        fs::create_directories(args.output);
        expect(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) != 0,
               "set app-local DLL search policy");
        if (args.mode != Mode::native)
            expect(AddDllDirectory(args.runtime.c_str()) != nullptr, "add external NGX runtime directory");
        Run run(args);
        return run.execute();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "D3D12 SR+FG public SDK test: %s\n", ex.what());
        std::printf("FL16_RESULT fail\n");
        return 1;
    }
}
