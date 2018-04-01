# Vulkan-Pipeline-Cache

Vulkan Pipeline Cache is a simple library for serializing various persistent Vulkan objects which typically end up
in hashmaps.

**NOTE: The repo is under construction, do not use this yet.**

- VkSampler (immutable samplers in set layouts)
- VkDescriptorSetLayout
- VkPipelineLayout
- VkRenderPass
- VkShaderModule
- VkPipeline (compute/graphics)

The goal for this project is to cover some main use cases:

- For internal engine use. Extend the notion of VkPipelineCache to also include these persistent objects,
so they can be automatically created in load time rather than manually declaring everything up front.
- Ideally, this serialized cache would be shipped, and applications can assume all persistent objects are already created.
- Easy way of sending shader compilation repros to conformance. Capture internally or via a Vulkan layer and send it off.
Normally, this is very difficult with normal debuggers because they generally rely on capturing frames or similar,
which doesn't work if compilation segfaults the driver. Shader compilation in Vulkan requires a lot of state,
which requires sending more complete repro applications.
- Some convenience tools to modify/disassemble/spirv-opt parts of the cache.
- Create a Vulkan layer which can capture this cache for repro purposes.
A paranoid mode would serialize the cache before every pipeline creation, to deal with crashing drivers.

## Serialization format

Simple JSON format which represents the various `Vk*CreateInfo` structures.
`pNext` is currently not supported.
When referring to other VK handle types like `pImmutableSamplers` in `VkDescriptorSetLayout`, or `VkRenderPass` in `VkPipeline`,
a 1-indexed format is used. 0 represents `VK_NULL_HANDLE` and 1+, represents an array index into the respective array (off-by-one).
Data blobs (specialization constant data, SPIR-V) are encoded in base64, but I'll likely need something smarter to deal with large applications which have half a trillion SPIR-V files.

## Vulkan layer capture

TBD. Will try Vulkan Layer Framework for this.

## Submit shader failure repro cases

TBD

## Replay a pipeline

TBD

## Engine integration

TBD
