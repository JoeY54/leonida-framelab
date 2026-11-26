// SPDX-License-Identifier: MIT
#pragma once
// Process-wide owner of NVSDK_NGX_D3D12_Init for one device. NGX initializes once per device and
// Shutdown1 tears down every feature on it, so FrameLab components must not each own a session.

#include <framelab/framelab.h>

#include <d3d12.h>

#include <cstdint>

namespace framelab::d3d12::ngx {

struct InitDesc {
    ID3D12Device* device;
    const char* projectId;
    const char* engineVersion;
    const wchar_t* runtimeDirectory;
    const wchar_t* logDirectory;
};

// A counted reference to the shared session. The last reference calls Shutdown1 after every
// owner has released its features. Move-only; safe to destroy on any thread.
class SessionRef {
public:
    SessionRef() = default;
    SessionRef(const SessionRef&) = delete;
    SessionRef& operator=(const SessionRef&) = delete;
    SessionRef(SessionRef&& other) noexcept : held_(other.held_) { other.held_ = false; }
    SessionRef& operator=(SessionRef&& other) noexcept;
    ~SessionRef() { reset(); }

    explicit operator bool() const { return held_; }
    void reset() noexcept;

private:
    friend FrameLabStatus acquire(const InitDesc&, SessionRef&, uint32_t&, bool&);
    bool held_ = false;
};

// Initializes NGX for the device or joins the live session. Joining requires the same device and
// identical initialization arguments. `initialized` reports whether this call ran NGX Init.
// FRAMELAB_BUSY: an exclusive owner holds NGX, or the live session differs.
FrameLabStatus acquire(const InitDesc& desc, SessionRef& out, uint32_t& ngxResult, bool& initialized);

// For a component that calls NGX Init/Shutdown1 itself (the current D3D12 FG module). It fails
// while any shared session or another exclusive owner is live.
bool claimExclusive() noexcept;
void releaseExclusive() noexcept;

} // namespace framelab::d3d12::ngx
