// SPDX-License-Identifier: MIT
// Compiles the public D3D12 SR header as C++. Links no FrameLab symbols.
#include <framelab/d3d12_sr.h>
#include <framelab/d3d12_fg.h>

#include <type_traits>

template<class T> constexpr bool plain = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;
static_assert(plain<FrameLabD3D12SrConfig> && plain<FrameLabD3D12SrOptions> && plain<FrameLabD3D12SrSettings> &&
              plain<FrameLabD3D12SrImage> && plain<FrameLabD3D12SrFrame>);
static_assert(std::is_same_v<decltype(&framelab_d3d12_sr_record),
              FrameLabStatus (FRAMELAB_CALL*)(FrameLabD3D12Sr*, const FrameLabD3D12SrFrame*)>);

int main()
{
    FrameLabD3D12SrSettings settings{};
    settings.struct_size = sizeof settings;
    settings.abi_version = FRAMELAB_ABI_VERSION;
    return settings.optimal_render_extent.width == 0 ? 0 : 1;
}
