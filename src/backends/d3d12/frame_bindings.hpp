// SPDX-License-Identifier: MIT
#pragma once

#include "../../../fg/FrameInputs.hpp"

#include <d3d12.h>
#include <wrl/client.h>

namespace framelab::d3d12 {
namespace fi = frame_inputs;

struct ImageBinding {
    fi::Id id;
    std::shared_ptr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state; // Supplied from the producer's authoritative state tracker.
};

struct RecordContext {
    fi::Id deviceId, queueId, commandStreamId;
    ID3D12Device& device;
    ID3D12CommandQueue& queue;
    ID3D12GraphicsCommandList& commands;
};

inline bool onDevice(ID3D12DeviceChild& object, ID3D12Device& device)
{
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    Microsoft::WRL::ComPtr<IUnknown> actual, expected;
    return SUCCEEDED(object.GetDevice(IID_PPV_ARGS(&owner))) &&
        SUCCEEDED(owner.As(&actual)) &&
        SUCCEEDED(device.QueryInterface(IID_PPV_ARGS(&expected))) && actual.Get() == expected.Get();
}

inline bool texture(const ImageBinding& binding, ID3D12Device& device, DXGI_FORMAT format,
                    fi::Extent extent)
{
    if (!binding.resource || !onDevice(*binding.resource, device)) return false;
    const auto desc = binding.resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == extent.width &&
        desc.Height == extent.height && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && desc.Format == format;
}

// The native profile: output-size RGBA8 color (for example a D3D12 SR output), depth and
// unjittered motion at a render extent no larger than the output, and a zero-jitter camera.
// Core validation already ties color to the output extent and depth/motion to the render extent.
// This check does not perform a resource barrier or a GPU wait.
inline fi::Result check(const fi::FrameInputs& frame, const RecordContext& context,
                       const ImageBinding& color, const ImageBinding& depth,
                       const ImageBinding& motion, const ImageBinding& output)
{
    if (const auto result = fi::validate(frame); !result) return result;
    if (frame.device != context.deviceId || frame.sync.queue != context.queueId ||
        frame.sync.commandStream != context.commandStreamId ||
        frame.sync.completion != fi::Completion::same_command_stream)
        return {fi::Error::sync, "D3D12 profile needs the producer's direct command stream"};
    if (!onDevice(context.queue, context.device) || !onDevice(context.commands, context.device) ||
        context.queue.GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        context.commands.GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return {fi::Error::binding, "D3D12 command list or queue does not match the device"};
    if (frame.renderExtent.width > frame.outputExtent.width ||
        frame.renderExtent.height > frame.outputExtent.height || frame.hudless || frame.ui ||
        frame.color.format != fi::Format::rgba8_unorm || frame.depth.format != fi::Format::r32_float ||
        frame.motionVectors.format != fi::Format::rg16_float ||
        frame.camera.projectionSpace != fi::ClipSpace::y_up_zero_to_one ||
        frame.camera.reprojectionSpace != fi::ClipSpace::y_up_zero_to_one ||
        frame.camera.jitterPixels != std::array<float, 2>{})
        return {fi::Error::unsupported, "unsupported D3D12 resource or camera profile"};

    const fi::Image* inputs[]{&frame.color, &frame.depth, &frame.motionVectors};
    const ImageBinding* bindings[]{&color, &depth, &motion};
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
    for (unsigned i = 0; i < 3; ++i) {
        const auto& input = *inputs[i];
        const auto& binding = *bindings[i];
        if (input.id != binding.id || input.ownerQueue != context.queueId ||
            input.allocationLease.owner_before(binding.resource) ||
            binding.resource.owner_before(input.allocationLease) ||
            !texture(binding, context.device, formats[i], input.extent))
            return {fi::Error::binding, "D3D12 resource identity, owner, lease or description mismatch"};
        if (input.state != fi::State::shader_read || binding.state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            return {fi::Error::state, "D3D12 input must be in non-pixel shader read state"};
        for (unsigned earlier = 0; earlier < i; ++earlier)
            if (bindings[earlier]->resource.get() == binding.resource.get())
                return {fi::Error::binding, "D3D12 input allocations alias"};
        if (binding.resource.get() == output.resource.get())
            return {fi::Error::binding, "D3D12 output aliases an input"};
    }
    if (!texture(output, context.device, DXGI_FORMAT_R8G8B8A8_UNORM, frame.outputExtent) ||
        output.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS ||
        !(output.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return {fi::Error::binding, "D3D12 output needs a matching UAV resource"};
    return {};
}
} // namespace framelab::d3d12
