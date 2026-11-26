// SPDX-License-Identifier: MIT
// External consumer of the installed D3D12SR component. Uses only public FrameLab and Windows SDK
// headers. Uploads a known synthetic scene, runs native DLSS SR and verifies the readback.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <framelab/d3d12_sr.h>
#ifdef FRAMELAB_SAMPLE_D3D12_FG
#include <framelab/d3d12_fg.h>
#endif

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
constexpr char projectId[] = "6f1f8c5e-2b7a-4d39-9c61-3a0d2e8b5f14";

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
        std::fprintf(stderr, "ABI_CHECK %s expected=%s actual=%s\n", name, framelab_status_string(expected),
                     framelab_status_string(actual));
        throw std::runtime_error(name);
    }
    ++contractChecks;
    std::printf("ABI_CHECK %s=%s\n", name, framelab_status_string(actual));
}

void FRAMELAB_CALL report(void*, const char* operation, uint32_t result)
{
    std::printf("SDK_SR_NATIVE operation=%s value=%#x\n", operation, result);
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

// Two-tone checkerboard palette per scene. Scene 1 follows the camera cut.
struct Palette { uint32_t a, b; };
constexpr Palette palettes[2]{{0xff303030u, 0xff181818u}, {0xff20a010u, 0xff107008u}};

struct Scene {
    int palette = 0;
    float left = 0.1f; // normalized object left edge
};

struct Inputs {
    std::vector<uint32_t> color, depth, motion;
};

// Samples the analytic scene at pixel centres of the active rectangle. Texels outside it carry
// garbage so that a consumer reading beyond the active rectangle is visible in the output.
void synthesize(const Scene& s, FrameLabExtent active, FrameLabExtent texture, Inputs& in)
{
    const size_t count = size_t(texture.width) * texture.height;
    in.color.assign(count, sentinel);
    in.depth.assign(count, floatBits(0.5f));
    in.motion.assign(count, uint32_t(half(9.0f)) | (uint32_t(half(9.0f)) << 16));
    const uint32_t objectMotion = half(-speed); // previous minus current, normalized UV
    for (uint32_t y = 0; y < active.height; ++y) {
        for (uint32_t x = 0; x < active.width; ++x) {
            const size_t p = size_t(y) * texture.width + x;
            const float u = (x + 0.5f) / active.width, v = (y + 0.5f) / active.height;
            const bool object = u >= s.left && u < s.left + objectWidth && v >= objectTop && v < objectTop + objectHeight;
            const bool odd = ((int(u * 40) + int(v * 22.5f)) & 1) != 0;
            in.color[p] = object ? objectColor : (odd ? palettes[s.palette].a : palettes[s.palette].b);
            in.depth[p] = floatBits(object ? 0.3f : 0.9f);
            in.motion[p] = object ? objectMotion : 0;
        }
    }
}

struct Verdict {
    bool ok = false;
    size_t sentinelPixels = 0, objectPixels = 0, palettePixels = 0, previousPalettePixels = 0;
    double centroid = 0, expected = 0;
    uint64_t hash = 0;
};

bool inPalette(uint32_t pixel, const Palette& palette)
{
    constexpr int tolerance = 24;
    for (int shift = 0; shift < 24; shift += 8) {
        const int value = (pixel >> shift) & 0xff;
        const int a = (palette.a >> shift) & 0xff, b = (palette.b >> shift) & 0xff;
        if (value < std::min(a, b) - tolerance || value > std::max(a, b) + tolerance) return false;
    }
    return true;
}

// Detects a clear-only, stale or wrong-scene output from the known synthetic input.
Verdict verify(const std::vector<uint32_t>& image, FrameLabExtent e, const Scene& s, int previousPalette)
{
    Verdict v;
    double sumX = 0;
    for (uint32_t y = 0; y < e.height; ++y) {
        for (uint32_t x = 0; x < e.width; ++x) {
            const uint32_t pixel = image[size_t(y) * e.width + x];
            const int r = pixel & 0xff, g = (pixel >> 8) & 0xff, b = (pixel >> 16) & 0xff;
            if (pixel == sentinel) ++v.sentinelPixels;
            if (b >= 0xc0 && r <= 0x60 && g >= 0x50 && g <= 0xb0) {
                ++v.objectPixels;
                sumX += x + 0.5;
            }
            if (inPalette(pixel, palettes[s.palette])) ++v.palettePixels;
            if (previousPalette >= 0 && inPalette(pixel, palettes[previousPalette])) ++v.previousPalettePixels;
        }
    }
    const double total = double(e.width) * e.height;
    const double area = objectWidth * objectHeight * total;
    v.expected = (s.left + objectWidth / 2) * e.width;
    v.centroid = v.objectPixels ? sumX / v.objectPixels : -1;
    v.hash = fnv1a(image);
    v.ok = v.sentinelPixels == 0 && v.objectPixels > 0.6 * area && v.objectPixels < 1.4 * area &&
        std::fabs(v.centroid - v.expected) <= 0.01 * e.width && v.palettePixels >= 0.85 * total &&
        v.previousPalettePixels <= 0.02 * total;
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

    void initialize(bool debugLayer, bool gpuValidation)
    {
        expect(event != nullptr, "create fence event");
        if (debugLayer) {
            ComPtr<ID3D12Debug> debug;
            check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "enable D3D12 debug layer");
            debug->EnableDebugLayer();
            if (gpuValidation) {
                ComPtr<ID3D12Debug1> debug1;
                check(debug.As(&debug1), "query GPU-based validation interface");
                debug1->SetEnableGPUBasedValidation(TRUE);
            }
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
                std::printf("GPU name=\"%ls\" device_id=%#x dedicated_mb=%llu\n", desc.Description, desc.DeviceId,
                            static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20));
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
        sampleMemory();
    }

    void sampleMemory()
    {
        ComPtr<IDXGIAdapter3> adapter3;
        if (!adapter || FAILED(adapter.As(&adapter3))) return;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return;
        peakLocal = std::max<uint64_t>(peakLocal, info.CurrentUsage);
        budget = info.Budget;
    }

    uint64_t currentLocal()
    {
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!adapter || FAILED(adapter.As(&adapter3)) ||
            FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return 0;
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
        sampleMemory();
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

    void transition(Texture& texture, D3D12_RESOURCE_STATES state)
    {
        if (texture.state == state) return;
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

    ComPtr<ID3D12Resource> readback(Texture& texture, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout)
    {
        uint64_t size{};
        layout = footprint(texture, size);
        auto result = buffer(size, D3D12_HEAP_TYPE_READBACK);
        if (texture.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            D3D12_RESOURCE_BARRIER uav{};
            uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uav.UAV.pResource = texture.resource.Get();
            list->ResourceBarrier(1, &uav);
        }
        transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{result.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = layout;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        return result;
    }

    std::vector<uint32_t> read(ID3D12Resource& buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout,
                               FrameLabExtent extent)
    {
        std::vector<uint32_t> image(size_t(extent.width) * extent.height);
        void* mapped{};
        check(buffer.Map(0, nullptr, &mapped), "map readback");
        for (uint32_t y = 0; y < extent.height; ++y)
            std::memcpy(image.data() + size_t(y) * extent.width,
                        static_cast<const char*>(mapped) + layout.Offset + size_t(y) * layout.Footprint.RowPitch,
                        size_t(extent.width) * sizeof(uint32_t));
        buffer.Unmap(0, nullptr);
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
using Handle = std::unique_ptr<FrameLabD3D12Sr, decltype(&framelab_d3d12_sr_destroy)>;

struct Gate {
    ComPtr<ID3D12Fence> fence;
    ~Gate() { if (fence) fence->Signal(1); }
};

void writeFile(const fs::path& path, const void* data, size_t bytes)
{
    FILE* file = _wfopen(path.c_str(), L"wb");
    expect(file != nullptr, "open output file");
    const auto written = std::fwrite(data, 1, bytes, file);
    const auto closed = std::fclose(file);
    expect(written == bytes && closed == 0, "write output file");
}

FrameLabCamera camera()
{
    FrameLabCamera c{};
    c.flags = FRAMELAB_CAMERA_VERIFIED | FRAMELAB_CAMERA_MOTION_INCLUDED;
    c.projection_space = c.reprojection_space = FRAMELAB_CLIP_Y_UP_ZERO_TO_ONE;
    c.motion_convention = FRAMELAB_MOTION_PREVIOUS_MINUS_CURRENT_UV;
    c.motion_vector_scale[0] = c.motion_vector_scale[1] = 1; // texels already hold normalized UV motion
    c.near_plane = 0.1f;
    c.far_plane = 1000;
    c.fov_radians = 1;
    c.aspect = 16.0f / 9;
    c.up[1] = c.right[0] = c.forward[2] = 1;
    for (unsigned i = 0; i < 16; i += 5)
        c.view_to_clip[i] = c.clip_to_view[i] = c.clip_to_lens[i] = c.clip_to_previous[i] = c.previous_to_clip[i] = 1;
    return c; // zero jitter: the CPU scene is not jittered
}

struct Args {
    fs::path runtime, output;
    bool validation = false, gpuValidation = false, removal = false;
    unsigned timing = 0;
};

class Run {
public:
    explicit Run(const Args& args) : args_(args) {}

    int correctness();
    int timing();
    int removal();

private:
    struct Stage {
        FrameLabExtent output{}, render{}, texture{};
        uint32_t mode = 0;
    };

    const Args& args_;
    Gpu gpu_;
    unsigned created_ = 0, releases_ = 0, evaluations_ = 0, verified_ = 0;
    uint64_t acked_ = 0; // last value acknowledged to the SR handle
    uint64_t index_ = 0;
    Stage stage_;
    Texture color_, depth_, motion_, output_;
    Lease colorLease_{nullptr, &framelab_lease_destroy}, depthLease_{nullptr, &framelab_lease_destroy},
        motionLease_{nullptr, &framelab_lease_destroy}, outputLease_{nullptr, &framelab_lease_destroy};
    Scene scene_;
    std::vector<uint32_t> last_;
    bool cutVerified_ = false, fallbackVerified_ = false, recoveryVerified_ = false, cancelVerified_ = false,
        retentionVerified_ = false, sharedVerified_ = false, reinitVerified_ = false;
    int dynamicResolution_ = -1; // -1 skipped, 1 verified
    unsigned resizes_ = 0;

    FrameLabD3D12SrConfig config(const fs::path& logs) const
    {
        static std::wstring runtime, directory;
        runtime = args_.runtime.wstring();
        directory = logs.wstring();
        return {sizeof(FrameLabD3D12SrConfig), FRAMELAB_ABI_VERSION, gpu_.device.Get(), gpu_.queue.Get(),
                gpu_.fence.Get(), projectId, "framelab-d3d12-sr-0.4", runtime.c_str(), directory.c_str(),
                report, nullptr};
    }

    Handle create(const fs::path& logs)
    {
        const auto c = config(logs);
        FrameLabD3D12Sr* raw{};
        sdk(framelab_d3d12_sr_create(&c, &raw), "initialize NGX and query native DLSS SR support");
        return {raw, &framelab_d3d12_sr_destroy};
    }

    Lease pin(const Texture& texture)
    {
        FrameLabLease* handle{};
        auto payload = std::make_unique<LeasePayload>(LeasePayload{texture.resource, &releases_});
        sdk(framelab_lease_create(payload.get(), release, &handle), "create allocation lease");
        payload.release();
        ++created_;
        return {handle, &framelab_lease_destroy};
    }

    void acknowledge(FrameLabD3D12Sr* sr, uint64_t value, const char* step)
    {
        sdk(framelab_d3d12_sr_mark_submitted(sr, value), step);
        acked_ = value;
    }

    void recorded(FrameLabD3D12Sr* sr, FrameLabStatus result, const char* step)
    {
        if (result == FRAMELAB_OK) return;
        if (result == FRAMELAB_NATIVE_FAILURE || result == FRAMELAB_OUT_OF_MEMORY) {
            // A failed native recording can leave partial commands: never execute this list.
            gpu_.list->Close();
            framelab_d3d12_sr_cancel(sr);
        }
        sdk(result, step);
    }

    void configure(FrameLabD3D12Sr* sr, FrameLabExtent output, uint32_t mode, bool contract, bool reallocate = true)
    {
        const FrameLabD3D12SrOptions options{sizeof options, FRAMELAB_ABI_VERSION, output, mode, 0};
        FrameLabD3D12SrSettings settings{};
        settings.struct_size = sizeof settings;
        settings.abi_version = FRAMELAB_ABI_VERSION;
        sdk(framelab_d3d12_sr_query_settings(sr, &options, &settings), "query DLSS SR render extents");
        gpu_.begin();
        recorded(sr, framelab_d3d12_sr_record_configure(sr, gpu_.list.Get(), &options, &settings),
                 "record DLSS SR feature creation");
        if (contract) {
            FrameLabD3D12SrFrame frame{};
            frame.struct_size = sizeof frame;
            frame.abi_version = FRAMELAB_ABI_VERSION;
            expectStatus(framelab_d3d12_sr_record(sr, &frame), FRAMELAB_BUSY, "record_during_configure");
            expectStatus(framelab_d3d12_sr_destroy(sr), FRAMELAB_BUSY, "destroy_unsubmitted_configure");
            expectStatus(framelab_d3d12_sr_retire(sr), FRAMELAB_INVALID_PHASE, "retire_unsubmitted_configure");
        }
        const auto value = gpu_.execute();
        acknowledge(sr, value, "acknowledge feature creation");
        gpu_.wait();
        sdk(framelab_d3d12_sr_retire(sr), "retire feature creation");
        std::printf("SDK_SR_CONFIGURE output=%ux%u mode=%u optimal=%ux%u min=%ux%u max=%ux%u fence=%llu\n",
                    output.width, output.height, mode, settings.optimal_render_extent.width,
                    settings.optimal_render_extent.height, settings.min_render_extent.width,
                    settings.min_render_extent.height, settings.max_render_extent.width,
                    settings.max_render_extent.height, static_cast<unsigned long long>(value));
        const bool resize = stage_.output.width && (stage_.output.width != output.width || stage_.output.height != output.height);
        resizes_ += resize;
        stage_ = {output, settings.optimal_render_extent, settings.optimal_render_extent, mode};
        minRender_ = settings.min_render_extent;
        if (reallocate) allocate();
    }

    FrameLabExtent minRender_{};

    // Drops the previous textures (their leases release after the last retirement) and pins new ones.
    void allocate()
    {
        colorLease_.reset();
        depthLease_.reset();
        motionLease_.reset();
        outputLease_.reset();
        constexpr auto none = D3D12_RESOURCE_FLAG_NONE;
        color_ = gpu_.texture(DXGI_FORMAT_R8G8B8A8_UNORM, stage_.texture, none);
        depth_ = gpu_.texture(DXGI_FORMAT_R32_FLOAT, stage_.texture, none);
        motion_ = gpu_.texture(DXGI_FORMAT_R16G16_FLOAT, stage_.texture, none);
        output_ = gpu_.texture(DXGI_FORMAT_R8G8B8A8_UNORM, stage_.output, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        colorLease_ = pin(color_);
        depthLease_ = pin(depth_);
        motionLease_ = pin(motion_);
        outputLease_ = pin(output_);
        last_.clear();
    }

    FrameLabD3D12SrFrame describe(uint32_t reset) const
    {
        FrameLabD3D12SrFrame f{};
        f.struct_size = sizeof f;
        f.abi_version = FRAMELAB_ABI_VERSION;
        f.command_stream_id = index_ + 1;
        f.commands = gpu_.list.Get();
        f.frame_index = index_;
        f.render_extent = stage_.render;
        f.reset_history = reset;
        f.color = {11, color_.resource.Get(), uint32_t(color_.state), colorLease_.get()};
        f.depth = {12, depth_.resource.Get(), uint32_t(depth_.state), depthLease_.get()};
        f.motion_vectors = {13, motion_.resource.Get(), uint32_t(motion_.state), motionLease_.get()};
        f.output = {14, output_.resource.Get(), uint32_t(output_.state), outputLease_.get()};
        f.camera = camera();
        return f;
    }

    void uploadFrame()
    {
        Inputs inputs;
        synthesize(scene_, stage_.render, stage_.texture, inputs);
        constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        gpu_.upload(color_, inputs.color, read);
        gpu_.upload(depth_, inputs.depth, read);
        gpu_.upload(motion_, inputs.motion, read);
        const std::vector<uint32_t> clear(size_t(stage_.output.width) * stage_.output.height, sentinel);
        gpu_.upload(output_, clear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void rejections(FrameLabD3D12Sr* sr, const FrameLabD3D12SrFrame& good, Texture& foreign, FrameLabLease* foreignLease)
    {
        auto f = good;
        f.color.state = D3D12_RESOURCE_STATE_COPY_DEST;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_RESOURCE_STATE, "wrong_input_state");
        f = good;
        f.output.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_RESOURCE_STATE, "wrong_output_state");
        f = good;
        f.motion_vectors.lease = nullptr;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_MISSING_LEASE, "missing_motion_lease");
        f = good;
        f.depth.resource = nullptr;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_IMAGE_SEMANTICS, "missing_depth");
        f = good;
        f.output = f.color;
        f.output.resource_id = 14;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_IDENTITY, "aliased_output");
        f = good;
        f.render_extent.width = 100000;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_DIMENSIONS, "render_out_of_range");
        f = good;
        f.camera.flags |= FRAMELAB_CAMERA_DEPTH_INVERTED;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_CAMERA, "depth_convention_mismatch");
        f = good;
        f.camera.flags &= ~FRAMELAB_CAMERA_MOTION_INCLUDED;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_UNSUPPORTED, "object_only_motion");
        f = good;
        f.commands = nullptr;
        expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_ARGUMENT, "missing_command_list");
        if (foreign.resource) {
            f = good;
            f.color = {11, foreign.resource.Get(), uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), foreignLease};
            expectStatus(framelab_d3d12_sr_record(sr, &f), FRAMELAB_INVALID_IDENTITY, "wrong_device_resource");
        } else {
            std::puts("ABI_CHECK wrong_device_resource=SKIPPED reason=no_warp_device");
        }
        expectStatus(framelab_d3d12_sr_cancel(sr), FRAMELAB_INVALID_PHASE, "nothing_recorded_after_rejections");
    }

    // Records, submits and verifies one SR frame. Returns the verdict.
    Verdict evaluate(FrameLabD3D12Sr* sr, uint32_t reset, int previousPalette, bool contract = false)
    {
        gpu_.begin();
        uploadFrame();
        const auto frame = describe(reset);
        recorded(sr, framelab_d3d12_sr_record(sr, &frame), "record DLSS SR evaluate");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        auto readback = gpu_.readback(output_, layout);
        const auto value = gpu_.execute();
        if (contract) {
            expectStatus(framelab_d3d12_sr_mark_submitted(sr, acked_), FRAMELAB_INVALID_SYNC, "non_increasing_fence");
            expectStatus(framelab_d3d12_sr_mark_submitted(sr, UINT64_MAX), FRAMELAB_INVALID_ARGUMENT, "reserved_fence_value");
        }
        acknowledge(sr, value, "acknowledge SR submission");
        gpu_.wait();
        sdk(framelab_d3d12_sr_retire(sr), "retire SR frame");
        ++evaluations_;
        last_ = gpu_.read(*readback.Get(), layout, stage_.output);
        auto verdict = verify(last_, stage_.output, scene_, previousPalette);
        verified_ += verdict.ok;
        std::printf("SDK_SR_FRAME frame=%llu output=%ux%u render=%ux%u format=RGBA8_UNORM reset_bits=%u fence=%llu "
                    "ngx=%#x sentinel=%zu object_px=%zu centroid=%.1f expected=%.1f palette_px=%zu "
                    "previous_palette_px=%zu fnv1a=%016llx verified=%d\n",
                    static_cast<unsigned long long>(index_), stage_.output.width, stage_.output.height,
                    stage_.render.width, stage_.render.height, reset, static_cast<unsigned long long>(value),
                    framelab_d3d12_sr_last_ngx_result(sr), verdict.sentinelPixels, verdict.objectPixels,
                    verdict.centroid, verdict.expected, verdict.palettePixels, verdict.previousPalettePixels,
                    static_cast<unsigned long long>(verdict.hash), int(verdict.ok));
        ++index_;
        scene_.left += speed;
        return verdict;
    }

    void stage(FrameLabD3D12Sr* sr, unsigned frames)
    {
        for (unsigned i = 0; i < frames; ++i) evaluate(sr, i ? 0 : FRAMELAB_RESET_RESIZE, -1);
    }

    // Missing depth: SR rejects before recording, the host keeps the same list and renders normally.
    void fallback(FrameLabD3D12Sr* sr)
    {
        gpu_.begin();
        Inputs inputs;
        synthesize(scene_, stage_.render, stage_.texture, inputs);
        gpu_.upload(color_, inputs.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const std::vector<uint32_t> clear(size_t(stage_.output.width) * stage_.output.height, sentinel);
        gpu_.upload(output_, clear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto frame = describe(0);
        frame.depth.resource = nullptr;
        expectStatus(framelab_d3d12_sr_record(sr, &frame), FRAMELAB_INVALID_IMAGE_SEMANTICS, "fallback_missing_depth");
        // Ordinary rendering: the unscaled raster image at the top-left of the output.
        gpu_.transition(color_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu_.transition(output_, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{output_.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{color_.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_BOX box{0, 0, 0, stage_.render.width, stage_.render.height, 1};
        gpu_.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        gpu_.transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        auto readback = gpu_.readback(output_, layout);
        gpu_.execute();
        gpu_.wait();
        const auto image = gpu_.read(*readback.Get(), layout, stage_.output);
        bool exact = true;
        for (uint32_t y = 0; y < stage_.output.height && exact; ++y)
            for (uint32_t x = 0; x < stage_.output.width && exact; ++x) {
                const auto expected = x < stage_.render.width && y < stage_.render.height ?
                    inputs.color[size_t(y) * stage_.texture.width + x] : sentinel;
                exact = image[size_t(y) * stage_.output.width + x] == expected;
            }
        fallbackVerified_ = exact;
        std::printf("SDK_SR_FALLBACK frame=%llu ordinary_copy_exact=%d\n", static_cast<unsigned long long>(index_), int(exact));
        expect(exact, "ordinary rendering fallback output mismatch");
        ++index_; // the fallback consumed a source frame, so the next SR frame resets automatically
        scene_.left += speed;
    }

    // Recorded but never executed work is cancelled. FrameLab releases its references at once and
    // drops the NGX feature, whose internal state tracking assumed the discarded list would run.
    void cancelUnsubmitted(FrameLabD3D12Sr* sr)
    {
        // The discarded list never executes, so its recorded transitions never happen.
        const D3D12_RESOURCE_STATES saved[]{color_.state, depth_.state, motion_.state, output_.state};
        gpu_.begin();
        uploadFrame();
        const auto frame = describe(0);
        recorded(sr, framelab_d3d12_sr_record(sr, &frame), "record SR frame to cancel");
        check(gpu_.list->Close(), "close discarded list");
        expectStatus(framelab_d3d12_sr_mark_submitted(sr, acked_), FRAMELAB_INVALID_SYNC, "stale_fence_before_cancel");
        const unsigned before = releases_;
        expectStatus(framelab_d3d12_sr_cancel(sr), FRAMELAB_OK, "cancel_unsubmitted");
        expectStatus(framelab_d3d12_sr_cancel(sr), FRAMELAB_INVALID_PHASE, "cancel_twice");
        expectStatus(framelab_d3d12_sr_retire(sr), FRAMELAB_INVALID_PHASE, "retire_after_cancel");
        color_.state = saved[0];
        depth_.state = saved[1];
        motion_.state = saved[2];
        output_.state = saved[3];
        cancelVerified_ = releases_ == before; // the host still holds its leases
        auto next = describe(0);
        expectStatus(framelab_d3d12_sr_record(sr, &next), FRAMELAB_INVALID_PHASE, "record_after_cancel_needs_configure");
        configure(sr, stage_.output, stage_.mode, false, false);
        // The discarded upload work never ran, so the next frame re-records the same source frame.
    }

    void dynamicResolution(FrameLabD3D12Sr* sr)
    {
        FrameLabExtent dynamic{std::max(minRender_.width, uint32_t(std::lround(stage_.render.width * 0.8))),
                               std::max(minRender_.height, uint32_t(std::lround(stage_.render.height * 0.8)))};
        if (dynamic.width >= stage_.render.width && dynamic.height >= stage_.render.height) {
            std::puts("SDK_SR_DYNAMIC skipped=1 reason=runtime_reports_fixed_render_extent");
            return;
        }
        const auto optimal = stage_.render;
        stage_.render = dynamic; // same textures, smaller active rectangle
        bool ok = true;
        for (unsigned i = 0; i < 2; ++i) ok &= evaluate(sr, 0, -1).ok;
        stage_.render = optimal;
        ok &= evaluate(sr, 0, -1).ok;
        dynamicResolution_ = ok ? 1 : 0;
        std::printf("SDK_SR_DYNAMIC active=%ux%u texture=%ux%u verified=%d\n", dynamic.width, dynamic.height,
                    stage_.texture.width, stage_.texture.height, int(ok));
    }

    // The queue waits on a host gate. FrameLab must keep every reference until the real fence.
    void gatedRetention(FrameLabD3D12Sr* sr)
    {
        gpu_.begin();
        uploadFrame();
        const auto frame = describe(0);
        recorded(sr, framelab_d3d12_sr_record(sr, &frame), "record gated SR frame");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        auto readback = gpu_.readback(output_, layout);
        Gate gate;
        check(gpu_.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "create execution gate");
        check(gpu_.queue->Wait(gate.fence.Get(), 1), "block queue on the gate");
        acknowledge(sr, gpu_.execute(), "acknowledge gated submission");
        const unsigned before = releases_;
        colorLease_.reset();
        depthLease_.reset();
        motionLease_.reset();
        outputLease_.reset();
        color_.resource.Reset();
        depth_.resource.Reset();
        motion_.resource.Reset();
        output_.resource.Reset();
        expectStatus(framelab_d3d12_sr_retire(sr), FRAMELAB_NOT_READY, "early_retire");
        expect(releases_ == before, "early retirement released pending leases");
        expectStatus(framelab_d3d12_sr_destroy(sr), FRAMELAB_BUSY, "destroy_in_flight");
        expectStatus(framelab_d3d12_sr_invalidate(sr), FRAMELAB_BUSY, "invalidate_in_flight");
        auto next = frame;
        ++next.frame_index;
        expectStatus(framelab_d3d12_sr_record(sr, &next), FRAMELAB_BUSY, "second_packet_in_flight");
        check(gate.fence->Signal(1), "release execution gate");
        gpu_.wait();
        expect(releases_ == before, "leases released before retirement");
        sdk(framelab_d3d12_sr_retire(sr), "retire gated SR frame");
        ++evaluations_;
        last_ = gpu_.read(*readback.Get(), layout, stage_.output);
        const auto verdict = verify(last_, stage_.output, scene_, -1);
        verified_ += verdict.ok;
        retentionVerified_ = releases_ == before + 4 && verdict.ok;
        std::printf("SDK_SR_RETENTION frame=%llu releases_after_retire=%u verified=%d\n",
                    static_cast<unsigned long long>(index_), releases_ - before, int(verdict.ok));
        expect(retentionVerified_, "gated retention or output check failed");
        ++index_;
    }

    void writeSummary(const char* mode, bool success, double gpuP50 = 0, double gpuP95 = 0, double cpuP50 = 0)
    {
        const auto path = args_.output / (std::string("d3d12-sr-") + mode + ".json");
        char text[2048];
        const int n = std::snprintf(text, sizeof text,
            "{\n  \"mode\": \"%s\",\n  \"result\": \"%s\",\n  \"evaluations\": %u,\n  \"verified\": %u,\n"
            "  \"contract_checks\": %u,\n  \"resizes\": %u,\n  \"camera_cut_verified\": %s,\n"
            "  \"fallback_verified\": %s,\n  \"recovery_verified\": %s,\n  \"cancel_verified\": %s,\n"
            "  \"retention_verified\": %s,\n  \"shared_session_verified\": %s,\n  \"reinit_verified\": %s,\n"
            "  \"dynamic_resolution\": \"%s\",\n  \"leases_created\": %u,\n  \"lease_releases\": %u,\n"
            "  \"validation\": {\"debug_layer\": %s, \"gpu_based\": %s, \"errors\": %u, \"warnings\": %u},\n"
            "  \"memory_bytes\": {\"peak_local\": %llu, \"budget\": %llu},\n"
            "  \"timing_ms\": {\"gpu_p50\": %.4f, \"gpu_p95\": %.4f, \"cpu_interval_p50\": %.4f}\n}\n",
            mode, success ? "pass" : "fail", evaluations_, verified_, contractChecks, resizes_,
            cutVerified_ ? "true" : "false", fallbackVerified_ ? "true" : "false", recoveryVerified_ ? "true" : "false",
            cancelVerified_ ? "true" : "false", retentionVerified_ ? "true" : "false",
            sharedVerified_ ? "true" : "false", reinitVerified_ ? "true" : "false",
            dynamicResolution_ < 0 ? "skipped" : dynamicResolution_ ? "verified" : "failed", created_, releases_,
            args_.validation ? "true" : "false", args_.gpuValidation ? "true" : "false", gpu_.validation.errors,
            gpu_.validation.warnings, static_cast<unsigned long long>(gpu_.peakLocal),
            static_cast<unsigned long long>(gpu_.budget), gpuP50, gpuP95, cpuP50);
        if (n > 0) writeFile(path, text, size_t(std::min<int>(n, sizeof text - 1)));
    }

    void sessionChecks(FrameLabD3D12Sr* sr)
    {
        (void)sr;
        // A second handle joins the live NGX session. Destroying it must not shut NGX down.
        auto second = create(args_.output / L"ngx");
        expectStatus(framelab_d3d12_sr_destroy(second.release()), FRAMELAB_OK, "destroy_joined_handle");
        auto different = config(args_.output / L"ngx-other");
        FrameLabD3D12Sr* raw{};
        expectStatus(framelab_d3d12_sr_create(&different, &raw), FRAMELAB_BUSY, "different_session_arguments");
#ifdef FRAMELAB_SAMPLE_D3D12_FG
        static const std::wstring logs = (args_.output / L"ngx").wstring();
        static const std::wstring runtime = args_.runtime.wstring();
        const FrameLabD3D12FgConfig fg{sizeof fg, FRAMELAB_ABI_VERSION, 1, 1, gpu_.device.Get(), gpu_.queue.Get(),
            gpu_.fence.Get(), {1280, 720}, projectId, "framelab-d3d12-sr-0.4", runtime.c_str(), logs.c_str(), nullptr, nullptr};
        FrameLabD3D12Fg* fgHandle{};
        expectStatus(framelab_d3d12_fg_create(&fg, &fgHandle), FRAMELAB_BUSY, "d3d12_fg_while_sr_session_live");
#else
        std::puts("ABI_CHECK d3d12_fg_while_sr_session_live=SKIPPED reason=D3D12FG_not_installed");
#endif
        sharedVerified_ = true;
    }
};

int Run::correctness()
{
    gpu_.initialize(args_.validation, args_.gpuValidation);
    const auto logs = args_.output / L"ngx";
    fs::create_directories(logs);
    auto sr = create(logs);
    auto* raw = sr.get();
    sessionChecks(raw);

    // Option validation before any GPU work.
    FrameLabD3D12SrSettings settings{};
    settings.struct_size = sizeof settings;
    settings.abi_version = FRAMELAB_ABI_VERSION;
    FrameLabD3D12SrOptions bad{sizeof bad, FRAMELAB_ABI_VERSION, {1280, 720}, 0, 0};
    expectStatus(framelab_d3d12_sr_query_settings(raw, &bad, &settings), FRAMELAB_UNSUPPORTED, "unknown_mode");
    bad = {sizeof bad, FRAMELAB_ABI_VERSION, {16, 16}, FRAMELAB_SR_QUALITY, 0};
    expectStatus(framelab_d3d12_sr_query_settings(raw, &bad, &settings), FRAMELAB_INVALID_DIMENSIONS, "tiny_output");
    bad = {sizeof bad - 1, FRAMELAB_ABI_VERSION, {1280, 720}, FRAMELAB_SR_QUALITY, 0};
    expectStatus(framelab_d3d12_sr_query_settings(raw, &bad, &settings), FRAMELAB_ABI_MISMATCH, "short_options");
    FrameLabD3D12SrFrame empty{};
    empty.struct_size = sizeof empty;
    empty.abi_version = FRAMELAB_ABI_VERSION;
    expectStatus(framelab_d3d12_sr_record(raw, &empty), FRAMELAB_INVALID_PHASE, "record_before_configure");

    // Optional foreign device for the wrong-device rejection.
    Texture foreign;
    Lease foreignLease{nullptr, &framelab_lease_destroy};
    ComPtr<ID3D12Device> warp;
    {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&warp)))) {
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = desc.Height = 64;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            check(warp->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&foreign.resource)), "create foreign texture");
            foreignLease = pin(foreign);
        }
    }

    // Stage 1: 1280x720 Quality.
    configure(raw, {1280, 720}, FRAMELAB_SR_QUALITY, true);
    gpu_.begin();
    uploadFrame();
    rejections(raw, describe(FRAMELAB_RESET_INITIAL), foreign, foreignLease.get());
    check(gpu_.list->Close(), "close rejection list");
    // The rejected list is untouched. Execute its uploads to keep the host state tracker truthful.
    {
        ID3D12CommandList* lists[]{gpu_.list.Get()};
        gpu_.queue->ExecuteCommandLists(1, lists);
        check(gpu_.queue->Signal(gpu_.fence.Get(), ++gpu_.signal), "signal rejection list");
        gpu_.wait();
    }
    foreign.resource.Reset();
    foreignLease.reset();
    warp.Reset();
    evaluate(raw, FRAMELAB_RESET_INITIAL, -1);
    {
        // A replayed frame index is rejected before recording.
        gpu_.begin();
        auto stale = describe(0);
        stale.frame_index = index_ - 1;
        expectStatus(framelab_d3d12_sr_record(raw, &stale), FRAMELAB_INVALID_SEQUENCE, "replayed_frame_index");
        check(gpu_.list->Close(), "close sequence list");
    }
    evaluate(raw, 0, -1, true);
    evaluate(raw, 0, -1);
    cancelUnsubmitted(raw);
    evaluate(raw, 0, -1);
    // Camera cut: new palette and object position, explicit reset.
    scene_ = {1, 0.55f};
    cutVerified_ = evaluate(raw, FRAMELAB_RESET_CAMERA_CUT, 0).ok;
    evaluate(raw, 0, 0);
    fallback(raw);
    recoveryVerified_ = evaluate(raw, 0, -1).ok;
    expectStatus(framelab_d3d12_sr_invalidate(raw), FRAMELAB_OK, "invalidate_idle");
    evaluate(raw, 0, -1);
    dynamicResolution(raw);

    // Stage 2: resize to 960x540 Balanced. Stage 3: back to 1280x720 Performance.
    scene_ = {0, 0.1f};
    configure(raw, {960, 540}, FRAMELAB_SR_BALANCED, false);
    stage(raw, 4);
    scene_ = {0, 0.1f};
    configure(raw, {1280, 720}, FRAMELAB_SR_PERFORMANCE, false);
    stage(raw, 3);
    writeFile(args_.output / L"sr-1280x720-rgba8.rgba", last_.data(), last_.size() * sizeof(uint32_t));
    gatedRetention(raw);
    expectStatus(framelab_d3d12_sr_destroy(sr.release()), FRAMELAB_OK, "destroy_idle");

    // The last handle shut NGX down. A new handle initializes it again in the same process.
    {
        auto again = create(logs);
        scene_ = {0, 0.1f};
        stage_ = {};
        configure(again.get(), {1280, 720}, FRAMELAB_SR_QUALITY, false);
        reinitVerified_ = evaluate(again.get(), FRAMELAB_RESET_INITIAL, -1).ok;
        colorLease_.reset();
        depthLease_.reset();
        motionLease_.reset();
        outputLease_.reset();
        expectStatus(framelab_d3d12_sr_destroy(again.release()), FRAMELAB_OK, "destroy_reinitialized");
    }
    color_ = depth_ = motion_ = output_ = {};
    gpu_.validation.drain();
    const bool leasesBalanced = releases_ == created_;
    const bool success = verified_ == evaluations_ && cutVerified_ && fallbackVerified_ && recoveryVerified_ &&
        cancelVerified_ && retentionVerified_ && sharedVerified_ && reinitVerified_ && dynamicResolution_ != 0 &&
        resizes_ == 2 && leasesBalanced && gpu_.validation.errors == 0 && gpu_.validation.warnings == 0;
    std::printf("SDK_SR_MEMORY peak_local_mb=%.1f budget_mb=%.1f\n", gpu_.peakLocal / 1048576.0, gpu_.budget / 1048576.0);
    std::printf("SDK_SR_VALIDATION debug_layer=%d gpu_based=%d errors=%u warnings=%u\n", int(args_.validation),
                int(args_.gpuValidation), gpu_.validation.errors, gpu_.validation.warnings);
    std::printf("SDK_SR_SUMMARY evaluations=%u verified=%u contract_checks=%u resizes=%u leases=%u/%u result=%s\n",
                evaluations_, verified_, contractChecks, resizes_, releases_, created_, success ? "PASS" : "FAIL");
    writeSummary(args_.validation ? (args_.gpuValidation ? "gpu-validation" : "validation") : "correctness", success);
    return success ? 0 : 1;
}

// Separate from validation and readback: fixed inputs, GPU timestamps around each evaluation.
int Run::timing()
{
    gpu_.initialize(false, false);
    const auto logs = args_.output / L"ngx";
    fs::create_directories(logs);
    auto sr = create(logs);
    const uint64_t before = gpu_.currentLocal();
    configure(sr.get(), {1280, 720}, FRAMELAB_SR_QUALITY, false);
    gpu_.begin();
    uploadFrame();
    gpu_.execute();
    gpu_.wait();
    ComPtr<ID3D12QueryHeap> heap;
    D3D12_QUERY_HEAP_DESC heapDesc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    check(gpu_.device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap)), "create timestamp heap");
    auto stamps = gpu_.buffer(2 * sizeof(uint64_t), D3D12_HEAP_TYPE_READBACK);
    UINT64 frequency = 0;
    check(gpu_.queue->GetTimestampFrequency(&frequency), "read timestamp frequency");
    std::vector<double> gpuMs, cpuMs;
    auto previous = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < args_.timing; ++i) {
        gpu_.begin();
        const auto frame = describe(i ? 0 : FRAMELAB_RESET_INITIAL);
        gpu_.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        recorded(sr.get(), framelab_d3d12_sr_record(sr.get(), &frame), "record timed SR evaluate");
        gpu_.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        gpu_.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(), 0);
        acknowledge(sr.get(), gpu_.execute(), "acknowledge timed submission");
        gpu_.wait();
        sdk(framelab_d3d12_sr_retire(sr.get()), "retire timed frame");
        ++evaluations_;
        ++index_;
        uint64_t* t{};
        const D3D12_RANGE range{0, 2 * sizeof(uint64_t)};
        check(stamps->Map(0, &range, reinterpret_cast<void**>(&t)), "map timestamps");
        gpuMs.push_back(double(t[1] - t[0]) * 1000.0 / double(frequency));
        stamps->Unmap(0, nullptr);
        const auto now = std::chrono::steady_clock::now();
        cpuMs.push_back(std::chrono::duration<double, std::milli>(now - previous).count());
        previous = now;
    }
    const uint64_t steady = gpu_.currentLocal();
    const auto percentile = [](std::vector<double> v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(p * (v.size() - 1) + 0.5))];
    };
    // Skip the first (reset, warm-up) sample in the CPU interval statistics.
    if (!cpuMs.empty()) cpuMs.erase(cpuMs.begin());
    const double p50 = percentile(gpuMs, 0.5), p95 = percentile(gpuMs, 0.95), cpu50 = percentile(cpuMs, 0.5);
    std::printf("SDK_SR_TIMING evaluations=%u output=1280x720 render=%ux%u gpu_ms_p50=%.4f gpu_ms_p95=%.4f "
                "gpu_ms_max=%.4f cpu_interval_ms_p50=%.4f cpu_interval_ms_p95=%.4f\n",
                evaluations_, stage_.render.width, stage_.render.height, p50, p95, percentile(gpuMs, 1.0), cpu50,
                percentile(cpuMs, 0.95));
    std::printf("SDK_SR_MEMORY before_create_mb=%.1f steady_mb=%.1f peak_local_mb=%.1f budget_mb=%.1f\n",
                before / 1048576.0, steady / 1048576.0, gpu_.peakLocal / 1048576.0, gpu_.budget / 1048576.0);
    colorLease_.reset();
    depthLease_.reset();
    motionLease_.reset();
    outputLease_.reset();
    sdk(framelab_d3d12_sr_destroy(sr.release()), "destroy timed handle");
    color_ = depth_ = motion_ = output_ = {};
    const bool success = evaluations_ == args_.timing && releases_ == created_;
    writeSummary("timing", success, p50, p95, cpu50);
    return success ? 0 : 1;
}

// Native device removal while a gated SR frame is in flight, then same-process recovery.
int Run::removal()
{
    gpu_.initialize(false, false);
    const auto logs = args_.output / L"ngx";
    fs::create_directories(logs);
    {
        auto sr = create(logs);
        configure(sr.get(), {1280, 720}, FRAMELAB_SR_QUALITY, false);
        gpu_.begin();
        uploadFrame();
        const auto frame = describe(FRAMELAB_RESET_INITIAL);
        recorded(sr.get(), framelab_d3d12_sr_record(sr.get(), &frame), "record SR frame before removal");
        Gate gate;
        check(gpu_.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence)), "create execution gate");
        check(gpu_.queue->Wait(gate.fence.Get(), 1), "block queue on the gate");
        acknowledge(sr.get(), gpu_.execute(), "acknowledge gated submission");
        const unsigned before = releases_;
        colorLease_.reset();
        depthLease_.reset();
        motionLease_.reset();
        outputLease_.reset();
        color_ = depth_ = motion_ = output_ = {};
        ComPtr<ID3D12Device5> device5;
        check(gpu_.device.As(&device5), "query ID3D12Device5");
        device5->RemoveDevice();
        gate.fence.Reset();
        expect(gpu_.fence->GetCompletedValue() == UINT64_MAX, "removed device did not report UINT64_MAX");
        expectStatus(framelab_d3d12_sr_retire(sr.get()), FRAMELAB_DEVICE_LOST, "retire_after_removal");
        expect(releases_ == before + 4, "removal retirement did not release leases");
        auto frame2 = frame;
        ++frame2.frame_index;
        expectStatus(framelab_d3d12_sr_record(sr.get(), &frame2), FRAMELAB_DEVICE_LOST, "record_after_removal");
        expectStatus(framelab_d3d12_sr_destroy(sr.release()), FRAMELAB_OK, "destroy_after_removal");
        const auto c = config(logs);
        FrameLabD3D12Sr* rawLost{};
        expectStatus(framelab_d3d12_sr_create(&c, &rawLost), FRAMELAB_DEVICE_LOST, "create_on_removed_device");
    }
    // Recovery: release every reference to the removed device, then start over on a new one.
    gpu_.validation = {};
    gpu_.transfers.clear();
    gpu_.list.Reset();
    gpu_.allocator.Reset();
    gpu_.queue.Reset();
    gpu_.fence.Reset();
    gpu_.device.Reset();
    gpu_.adapter.Reset();
    gpu_.signal = acked_ = 0;
    gpu_.initialize(false, false);
    auto sr = create(logs);
    scene_ = {0, 0.1f};
    stage_ = {};
    configure(sr.get(), {1280, 720}, FRAMELAB_SR_QUALITY, false);
    const bool recovered = evaluate(sr.get(), FRAMELAB_RESET_INITIAL, -1).ok;
    colorLease_.reset();
    depthLease_.reset();
    motionLease_.reset();
    outputLease_.reset();
    sdk(framelab_d3d12_sr_destroy(sr.release()), "destroy recovered handle");
    color_ = depth_ = motion_ = output_ = {};
    recoveryVerified_ = recovered;
    const bool success = recovered && releases_ == created_;
    std::printf("SDK_SR_REMOVAL native_removal_released=1 same_process_recovery=%d leases=%u/%u result=%s\n",
                int(recovered), releases_, created_, success ? "PASS" : "FAIL");
    writeSummary("device-removal", success);
    return success ? 0 : 1;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args args;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--validation") args.validation = true;
        else if (arg == L"--gpu-validation") args.validation = args.gpuValidation = true;
        else if (arg == L"--device-removal") args.removal = true;
        else if (arg == L"--timing" && i + 1 < argc) args.timing = unsigned(std::wcstoul(argv[++i], nullptr, 10));
        else if (positional == 0) { args.runtime = argv[i]; ++positional; }
        else if (positional == 1) { args.output = argv[i]; ++positional; }
        else positional = 3;
    }
    if (positional != 2 || (args.timing && (args.validation || args.removal)) || (args.removal && args.validation)) {
        std::fprintf(stderr, "usage: d3d12_sr <NGX runtime directory> <output directory> "
                             "[--validation | --gpu-validation | --timing <frames> | --device-removal]\n");
        return 2;
    }
    try {
        args.runtime = fs::absolute(args.runtime);
        args.output = fs::absolute(args.output);
        fs::create_directories(args.output);
        expect(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) != 0,
               "set app-local DLL search policy");
        expect(AddDllDirectory(args.runtime.c_str()) != nullptr, "add external NGX runtime directory");
        Run run(args);
        if (args.timing) return run.timing();
        if (args.removal) return run.removal();
        return run.correctness();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "D3D12 SR public SDK test: %s\n", error.what());
        return 1;
    }
}
