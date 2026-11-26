/* SPDX-License-Identifier: MIT */
/* C consumer of the installed D3D12SR header. Runs without a GPU. */
#include <framelab/d3d12_sr.h>

#include <stdio.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

int main(void)
{
    FrameLabD3D12SrConfig config = {0};
    FrameLabD3D12SrOptions options = {0};
    FrameLabD3D12SrSettings settings = {0};
    FrameLabD3D12SrFrame frame = {0};
    FrameLabD3D12Sr* sr = NULL;

    CHECK(framelab_abi_version() == FRAMELAB_ABI_VERSION);
    CHECK(framelab_d3d12_sr_create(&config, &sr) == FRAMELAB_ABI_MISMATCH && !sr);
    config.struct_size = sizeof config;
    config.abi_version = FRAMELAB_ABI_VERSION;
    CHECK(framelab_d3d12_sr_create(&config, &sr) == FRAMELAB_INVALID_ARGUMENT && !sr);
    CHECK(framelab_d3d12_sr_create(&config, NULL) == FRAMELAB_INVALID_ARGUMENT);

    options.struct_size = sizeof options;
    options.abi_version = FRAMELAB_ABI_VERSION;
    options.output_extent.width = 1280;
    options.output_extent.height = 720;
    options.mode = FRAMELAB_SR_QUALITY;
    settings.struct_size = sizeof settings;
    settings.abi_version = FRAMELAB_ABI_VERSION;
    frame.struct_size = sizeof frame;
    frame.abi_version = FRAMELAB_ABI_VERSION;
    CHECK(framelab_d3d12_sr_query_settings(sr, &options, &settings) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(framelab_d3d12_sr_record_configure(sr, NULL, &options, &settings) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(framelab_d3d12_sr_record(sr, &frame) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(framelab_d3d12_sr_mark_submitted(sr, 1) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(framelab_d3d12_sr_cancel(sr) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(framelab_d3d12_sr_retire(sr) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(framelab_d3d12_sr_invalidate(sr) == FRAMELAB_INVALID_ARGUMENT);
    CHECK(framelab_d3d12_sr_last_ngx_result(sr) == 0);
    CHECK(framelab_d3d12_sr_destroy(sr) == FRAMELAB_OK);
    printf("PASS D3D12 SR C header and null-handle contract\n");
    return 0;
}
