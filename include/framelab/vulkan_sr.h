/* SPDX-License-Identifier: MIT */
#ifndef FRAMELAB_VULKAN_SR_H
#define FRAMELAB_VULKAN_SR_H

#include <framelab/framelab.h>
#include <stddef.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameLabVulkanSr FrameLabVulkanSr;
typedef void (FRAMELAB_CALL *FrameLabVulkanSrReportFn)(void*, const char*, int32_t);

/* Shared with <framelab/d3d12_sr.h>. */
#ifndef FRAMELAB_SR_MODES_DEFINED
#define FRAMELAB_SR_MODES_DEFINED
enum { FRAMELAB_SR_PERFORMANCE = 1, FRAMELAB_SR_BALANCED = 2, FRAMELAB_SR_QUALITY = 3 };
#endif

typedef struct FrameLabVulkanSrConfig {
    uint32_t struct_size, abi_version;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family, viewport_id;
    const wchar_t* initialized_interposer_path;
    const FrameLabLease* device_and_runtime_lease;
    FrameLabVulkanSrReportFn report;
    void* report_user;
} FrameLabVulkanSrConfig;

typedef struct FrameLabVulkanSrOptions {
    uint32_t struct_size, abi_version;
    FrameLabExtent output_extent;
    uint32_t mode, hdr;
} FrameLabVulkanSrOptions;

typedef struct FrameLabVulkanSrImage {
    VkImage image;
    VkImageView view;
    FrameLabExtent extent;
    VkFormat format;
    VkImageLayout layout;
    VkImageUsageFlags usage;
    VkImageAspectFlags view_aspects;
    uint32_t queue_family;
    const FrameLabLease* lease;
} FrameLabVulkanSrImage;

typedef struct FrameLabVulkanSrFrame {
    uint32_t struct_size, abi_version;
    VkCommandBuffer commands;
    uint32_t frame_index, reset_history;
    FrameLabVulkanSrImage color, depth, motion_vectors, output;
    FrameLabCamera camera;
} FrameLabVulkanSrFrame;

/* The host initializes Streamline, registers the device and reserves the viewport first. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_create(
    const FrameLabVulkanSrConfig* config, FrameLabVulkanSr** out_sr);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_destroy(FrameLabVulkanSr* sr);
/* Returns the runtime's optimal input extent. Reconfiguration resets history. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_configure(
    FrameLabVulkanSr* sr, const FrameLabVulkanSrOptions* options, FrameLabExtent* out_render_extent);
/* The host establishes the declared layouts. Producers and SR use the same command buffer. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_record(
    FrameLabVulkanSr* sr, const FrameLabVulkanSrFrame* frame);
/* Signal this borrowed timeline from the submit on the configured queue. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_completion(
    const FrameLabVulkanSr* sr, VkSemaphore* out_timeline);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_mark_submitted(
    FrameLabVulkanSr* sr, uint64_t completion_value);
/* Drop the recording before cancel. Native failure can leave partial commands. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_cancel(FrameLabVulkanSr* sr);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_retire(FrameLabVulkanSr* sr);
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_invalidate(FrameLabVulkanSr* sr);
/* After device loss, the host stops submissions, drains the device and discards host recordings.
 * Release retained packets without reusing the device or its Streamline viewport. Destroy
 * this handle, shut down the old runtime/device and create a fresh backend to resume SR. */
FRAMELAB_API FrameLabStatus FRAMELAB_CALL framelab_vulkan_sr_release_lost_device(FrameLabVulkanSr* sr);

#ifdef __cplusplus
}
#endif
#endif
