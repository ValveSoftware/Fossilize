# Fossilize

[![Build Status](https://travis-ci.org/Themaister/Fossilize.svg?branch=master)](https://travis-ci.org/Themaister/Fossilize)
[![Build Status](https://ci.appveyor.com/api/projects/status/github/Themaister/Fossilize?svg=true&branch=master)](https://ci.appveyor.com/project/Themaister/fossilize)

**NOTE: The repo is under construction, do not use this yet.**

Fossilize is a simple library for serializing various persistent Vulkan objects which typically end up
in hashmaps. CreateInfo structs for these Vulkan objects can be recorded and replayed.

- VkSampler (immutable samplers in set layouts)
- VkDescriptorSetLayout
- VkPipelineLayout
- VkRenderPass
- VkShaderModule
- VkPipeline (compute/graphics)

The goal for this project is to cover some main use cases:

### High priority

- For internal engine use. Extend the notion of VkPipelineCache to also include these persistent objects,
so they can be automatically created in load time rather than manually declaring everything up front.
Ideally, this serialized cache could be shipped, and applications can assume all persistent objects are already created.
- Create a Vulkan layer which can capture this cache for repro purposes.
A paranoid mode would serialize the cache before every pipeline creation, to deal with crashing drivers.
- Easy way of sending shader compilation repros to conformance. Capture internally or via a Vulkan layer and send it off.
Normally, this is very difficult with normal debuggers because they generally rely on capturing frames or similar,
which doesn't work if compilation segfaults the driver. Shader compilation in Vulkan requires a lot of state,
which requires sending more complete repro applications.

### Mid priority

- Some convenience tools to modify/disassemble/spirv-opt parts of the cache.

### Low priority

- Serialize state in application once, replay on N devices to build up VkPipelineCache objects without having to run application.
- Benchmark a pipeline offline by feeding it fake input.

## Build

### Supported compilers

- GCC 4.8+
- Clang
- MSVC 2013/2015/2017+

If rapidjson is not already bundled in your project, you need to check out the submodule.

```
git submodule update --init
```

otherwise, you can set `FOSSILIZE_RAPIDJSON_INCLUDE_PATH` if building this library as part of your project.
It is also possible to use `FOSSILIZE_VULKAN_INCLUDE_PATH` to override Vulkan header include paths.

Standalone build:
```
mkdir build
cd build
cmake ..
cmake --build .
```

Link as part of other project:
```
add_subdirectory(fossilize)
target_link_library(your-target fossilize)
```

For Android, you can use the `android_build.sh` script. It builds the layer for armeabi-v7a and arm64-v8a.
See the script for more details.

## Serialization format

Overall, a binary format which combines JSON with varint-encoded SPIR-V (light compression).
- Magic "FOSSILIZE0000001" (16 bytes ASCII)
- Size of entire binary (64-bit LE)
- JSON magic "JSON    " (8 bytes ASCII)
- JSON size (64-bit LE)
- JSON data (JSON size bytes)
- SPIR-V magic "SPIR-V  " (8 bytes ASCII)
- SPIR-V size (64-bit LE)
- Varint-encoded SPIR-V words (SPIR-V size bytes)

64-bit little-endian values are not necessarily aligned to 8 bytes.

The JSON is a simple format which represents the various `Vk*CreateInfo` structures.
`pNext` is currently not supported.
When referring to other VK handle types like `pImmutableSamplers` in `VkDescriptorSetLayout`, or `VkRenderPass` in `VkPipeline`,
a 1-indexed format is used. 0 represents `VK_NULL_HANDLE` and 1+, represents an array index into the respective array (off-by-one).
Data blobs (specialization constant data, SPIR-V) are encoded in base64, but I'll likely need something smarter to deal with large applications which have half a trillion SPIR-V files.
When recording or replaying, a mapping from and to real Vk object handles must be provided by the application so the offset-based indexing scheme can be resolved to real handles.

`VkShaderModuleCreateInfo` refers to an encoded buffer in the SPIR-V block by codeBinaryOffset and codeBinarySize.

The varint encoding scheme encodes every 32-bit SPIR-V word by encoding 7 bits at a time starting with the LSBs,
the MSB bit in an encoded byte is set if another byte needs to be read (7 bit) for the same SPIR-V word.
Each SPIR-V word takes from 1 to 5 bytes with this scheme.

## Sample API usage

### Recording state
```
// Note that fossilize.hpp will include Vulkan headers, so make sure you include vulkan.h before
// this one if you care about which Vulkan headers you use.
#include "fossilize.hpp"

void create_state()
{
    try
    {
        Fossilize::StateRecorder recorder;
        // TODO here: Add way to capture which extensions/physical device features were used to deal with exotic things
        // which require extensions when making repro cases.

        VkDescriptorSetLayoutCreateInfo info = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };

        // Fill in stuff.

        Fossilize::Hash hash = Fossilize::Hashing::compute_hash_descriptor_set_layout(recorder, info);
        // Or use your own hash. This 64-bit hash will be provided back to application when replaying the state.
        // The builtin hashing functions are mostly useful for a Vulkan capturing layer, testing or similar.
        unsigned set_index = recorder.register_descriptor_set_layout(hash, info);

        vkCreateDescriptorSetLayout(..., &layout);

        // Register the true handle of the set layout so that the recorder can map
        // layout to an internal index when using register_pipeline_layout for example.
        // Setting handles are only required when other Vk*CreateInfo calls refer to any other Vulkan handles.
        recorder.set_descriptor_set_layout_handle(set_index, layout);

        // Do the same for render passes, pipelines, shader modules, samplers (if using immutable samplers) as necessary.

        std::vector<uint8_t> serialized = recorder.serialize();
        save_to_disk(serialized);
    }
    catch (const std::exception &e)
    {
        // Can throw exception on API misuse.
    }
}
```

### Replaying state
```
// Note that fossilize.hpp will include Vulkan headers, so make sure you include vulkan.h before
// this one if you care about which Vulkan headers you use.
#include "fossilize.hpp"

struct Device : Fossilize::StateCreatorInterface
{
    // See header for other functions.
    bool set_num_descriptor_set_layouts(unsigned count) override
    {
        // Know a-head of time how many set layouts there will be, useful for multi-threading.
        return true;
    }

    bool enqueue_create_descriptor_set_layout(Hash hash, unsigned index,
                                              const VkDescriptorSetLayoutCreateInfo *create_info,
                                              VkDescriptorSetLayout *layout) override
    {
        // Can queue this up for threaded creation (useful for pipelines).
        // create_info persists as long as Fossilize::Replayer exists.
        VkDescriptorSetLayout set_layout = populate_internal_hash_map(hash, create_info);

        // Let the replayer know how to fill in VkDescriptorSetLayout in upcoming pipeline creation calls.
        // Can use dummy values here if we don't care about using the create_info structs verbatim.
        *layout = set_layout;
        return true;
    }

    void wait_enqueue() override
    {
        // If using multi-threaded creation, join all queued tasks here.
    }
};

void replay_state(Device &device)
{
    try
    {
        Fossilize::Replayer replayer;
        replayer.parse(device, serialized_state, serialized_state_size);
        // Now internal hashmaps are warmed up, and all pipelines have been created.
    }
    catch (const std::exception &e)
    {
        // Can throw exception on API misuse.
    }
}
```

## Vulkan layer capture

Fossilize can also capture Vulkan application through the layer mechanism.
The layer name is `VK_LAYER_fossilize`.

To build, enable `FOSSILIZE_VULKAN_LAYER` CMake option. This is enabled by default.
The layer and JSON is placed in `layer/` in the build folder.

### Linux/Windows

By default the layer will serialize to `fossilize.json` in the working directory on `vkDestroyDevice`.
However, due to the nature of some drivers, there might be crashes in-between. For this, there are two other modes.

#### `export FOSSILIZE_PARANOID_MODE=1`

Before every call to `vkCreateComputePipelines` and `vkCreateGraphicsPipelines`, data is serialized to disk.
This can be quite slow for application with lots of pipelines, so only use it if the method below doesn't work ...

#### `export FOSSILIZE_DUMP_SIGSEGV=1`

This only works on Linux. A SIGSEGV handler is registered, and the state is serialized to disk in the segfault handler.
This is a bit sketchy, but should work well if drivers are crashing on pipeline creation (or just crashing in general).

#### `export FOSSILIZE_DUMP_PATH=/my/custom/path`

Custom file path for dumping state.

### Android

Options can be set through `setprop`.

#### `setprop debug.fossilize.dump_path /custom/path`

#### `setprop debug.fossilize.paranoid_mode 1`

#### `setprop debug.fossilize.dump_sigsegv 1`

## Submit shader failure repro cases

TBD

## External modules

- [volk](https://github.com/zeux/volk)
- [rapidjson](https://github.com/miloyip/rapidjson)
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools)
- [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)
