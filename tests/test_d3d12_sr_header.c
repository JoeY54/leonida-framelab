/* SPDX-License-Identifier: MIT */
/* Compiles the public D3D12 SR header as C next to D3D12 FG. Links no FrameLab symbols. */
#include <framelab/d3d12_sr.h>
#include <framelab/d3d12_fg.h>

#include <stddef.h>

#define LAYOUT(type, member, offset) _Static_assert(offsetof(type, member) == (offset), #type "." #member)

_Static_assert(FRAMELAB_SR_PERFORMANCE == 1 && FRAMELAB_SR_BALANCED == 2 && FRAMELAB_SR_QUALITY == 3, "modes");
_Static_assert(FRAMELAB_D3D12_SR_HDR == 1 && FRAMELAB_D3D12_SR_DEPTH_INVERTED == 2 &&
               FRAMELAB_D3D12_SR_AUTO_EXPOSURE == 4, "flags");
#if defined(_WIN64)
LAYOUT(FrameLabD3D12SrConfig, device, 8);
LAYOUT(FrameLabD3D12SrConfig, completion_fence, 24);
LAYOUT(FrameLabD3D12SrConfig, report_user, 72);
_Static_assert(sizeof(FrameLabD3D12SrConfig) == 80, "config size");
_Static_assert(sizeof(FrameLabD3D12SrOptions) == 24, "options size");
_Static_assert(sizeof(FrameLabD3D12SrSettings) == 32, "settings size");
LAYOUT(FrameLabD3D12SrImage, state, 16);
LAYOUT(FrameLabD3D12SrImage, lease, 24);
_Static_assert(sizeof(FrameLabD3D12SrImage) == 32, "image size");
LAYOUT(FrameLabD3D12SrFrame, commands, 16);
LAYOUT(FrameLabD3D12SrFrame, frame_index, 24);
LAYOUT(FrameLabD3D12SrFrame, render_extent, 32);
LAYOUT(FrameLabD3D12SrFrame, reset_history, 40);
LAYOUT(FrameLabD3D12SrFrame, exposure_scale, 48);
LAYOUT(FrameLabD3D12SrFrame, color, 56);
LAYOUT(FrameLabD3D12SrFrame, output, 152);
LAYOUT(FrameLabD3D12SrFrame, camera, 184);
_Static_assert(sizeof(FrameLabD3D12SrFrame) == 600, "frame size");
#endif

int main(void)
{
    FrameLabD3D12SrFrame frame = {0};
    frame.struct_size = sizeof frame;
    frame.abi_version = FRAMELAB_ABI_VERSION;
    return frame.reset_history == 0 && frame.color.resource == NULL ? 0 : 1;
}
