/* SPDX-License-Identifier: MIT */
#include <framelab/d3d12_sr.h>

int main(void)
{
    FrameLabD3D12SrConfig config = {0};
    FrameLabD3D12Sr* sr = 0;
    if (framelab_d3d12_sr_create(&config, &sr) != FRAMELAB_ABI_MISMATCH || sr)
        return 1;
    config.struct_size = sizeof config;
    config.abi_version = FRAMELAB_ABI_VERSION;
    if (framelab_d3d12_sr_create(&config, &sr) != FRAMELAB_INVALID_ARGUMENT || sr)
        return 2;
    if (framelab_d3d12_sr_record(sr, 0) != FRAMELAB_INVALID_ARGUMENT ||
        framelab_d3d12_sr_retire(sr) != FRAMELAB_INVALID_ARGUMENT ||
        framelab_d3d12_sr_last_ngx_result(sr) != 0)
        return 3;
    return framelab_d3d12_sr_destroy(sr) == FRAMELAB_OK ? 0 : 4;
}
