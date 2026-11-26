// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ngx_session.hpp"

#include <nvsdk_ngx.h>
#include <wrl/client.h>

#include <mutex>
#include <new>
#include <string>
#include <type_traits>

using Microsoft::WRL::ComPtr;

namespace framelab::d3d12::ngx {
namespace {
struct Registry {
    std::mutex mutex;
    bool exclusive = false;
    unsigned references = 0;
    ComPtr<ID3D12Device> device;
    ComPtr<IUnknown> identity;
    std::string project, engine;
    std::wstring runtime, logs;

    void clear()
    {
        device.Reset();
        identity.Reset();
        project.clear();
        engine.clear();
        runtime.clear();
        logs.clear();
    }
};

// Intentionally never destroyed: a leaked handle must not run Shutdown1 during static teardown.
// Static storage removes the heap allocation from registry initialization, including the
// noexcept exclusive-claim path. The empty Registry must be constructible without throwing.
Registry& registry() noexcept
{
    static_assert(std::is_nothrow_default_constructible_v<Registry>);
    alignas(Registry) static unsigned char storage[sizeof(Registry)];
    static Registry* const instance = ::new (static_cast<void*>(storage)) Registry;
    return *instance;
}

FrameLabStatus statusForInit(NVSDK_NGX_Result result)
{
    switch (result) {
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
    case NVSDK_NGX_Result_FAIL_OutOfDate:
    case NVSDK_NGX_Result_FAIL_PlatformError:
        return FRAMELAB_UNSUPPORTED;
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
        return FRAMELAB_OUT_OF_MEMORY;
    default:
        return FRAMELAB_NATIVE_FAILURE;
    }
}
} // namespace

SessionRef& SessionRef::operator=(SessionRef&& other) noexcept
{
    if (this != &other) {
        reset();
        held_ = other.held_;
        other.held_ = false;
    }
    return *this;
}

void SessionRef::reset() noexcept
{
    if (!held_) return;
    held_ = false;
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    if (--r.references) return;
    // Every owner released its features before dropping the last reference.
    NVSDK_NGX_D3D12_Shutdown1(r.device.Get());
    r.clear();
}

FrameLabStatus acquire(const InitDesc& d, SessionRef& out, uint32_t& ngxResult, bool& initialized)
{
    out.reset();
    ngxResult = 0;
    initialized = false;
    ComPtr<IUnknown> identity;
    if (!d.device || FAILED(d.device->QueryInterface(IID_PPV_ARGS(&identity))))
        return FRAMELAB_INVALID_ARGUMENT;
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    if (r.exclusive) return FRAMELAB_BUSY;
    if (r.references) {
        if (identity.Get() != r.identity.Get() || r.project != d.projectId || r.engine != d.engineVersion ||
            r.runtime != d.runtimeDirectory || r.logs != d.logDirectory)
            return FRAMELAB_BUSY;
        ++r.references;
        out.held_ = true;
        ngxResult = NVSDK_NGX_Result_Success;
        return FRAMELAB_OK;
    }
    try {
        r.project = d.projectId;
        r.engine = d.engineVersion;
        r.runtime = d.runtimeDirectory;
        r.logs = d.logDirectory;
    } catch (const std::bad_alloc&) {
        r.clear();
        return FRAMELAB_OUT_OF_MEMORY;
    }
    const wchar_t* paths[] = {d.runtimeDirectory};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID(d.projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        d.engineVersion, d.logDirectory, d.device, &info, NVSDK_NGX_Version_API);
    ngxResult = static_cast<uint32_t>(result);
    initialized = true;
    if (NVSDK_NGX_FAILED(result)) {
        r.clear();
        return statusForInit(result);
    }
    r.device = d.device;
    r.identity = identity;
    r.references = 1;
    out.held_ = true;
    return FRAMELAB_OK;
}

bool claimExclusive() noexcept
{
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    if (r.exclusive || r.references) return false;
    r.exclusive = true;
    return true;
}

void releaseExclusive() noexcept
{
    auto& r = registry();
    const std::lock_guard lock(r.mutex);
    r.exclusive = false;
}

} // namespace framelab::d3d12::ngx
