# Fossilize

[![Build Status](https://travis-ci.org/ValveSoftware/Fossilize.svg?branch=master)](https://travis-ci.org/ValveSoftware/Fossilize)

Fossilize is a library and Vulkan layer for serializing various persistent Vulkan objects which typically end up
in hashmaps. CreateInfo structs for these Vulkan objects can be recorded and replayed.

- VkSampler (immutable samplers in set layouts)
- VkDescriptorSetLayout
- VkPipelineLayout
- VkRenderPass
- VkShaderModule
- VkPipeline (compute/graphics)

The goal for this project is to cover some main use cases:

- For internal engine use. Extend the notion of VkPipelineCache to also include these persistent objects,
so they can be automatically created in load time rather than manually declaring everything up front.
Ideally, this serialized cache could be shipped, and applications can assume all persistent objects are already created.
- Create a Vulkan layer which can capture this cache for repro purposes when errors occur before we can create a conventional capture.
- Serialize state in application once, replay on N devices to build up VkPipelineCache objects without having to run application.

## Build

### Supported compilers

- GCC 4.8+
- Clang
- MSVC 2013/2015/2017+

If rapidjson is not already bundled in your project, you need to check out the submodules.

```shell
git submodule update --init
```

otherwise, you can set `FOSSILIZE_RAPIDJSON_INCLUDE_PATH` if building this library as part of your project.
It is also possible to use `FOSSILIZE_VULKAN_INCLUDE_PATH` to override Vulkan header include paths.

Normally, the CLI tools will be built. These require SPIRV-Tools and SPIRV-Cross submodules to be initialized, however, if you're only building Fossilize as a library/layer, you can use CMake options `-DFOSSILIZE_CLI=OFF` and `-DFOSSILIZE_TESTS=OFF` to disable all those requirements for submodules (assuming you have custom include path for rapidjson).
Standalone build:
```shell
mkdir build
cd build
cmake ..
cmake --build .
```

Link as part of other project:
```cmake
add_subdirectory(fossilize EXCLUDE_FROM_ALL)
target_link_library(your-target fossilize)
```

For Android, you can use the `android_build.sh` script. It builds the layer for armeabi-v7a and arm64-v8a.
See the script for more details.

## Serialization format

Overall, a binary database format which contains deflated JSON or deflated varint-encoded SPIR-V (light compression).
The database is a bespoke format with extension ".foz".
It is designed to be robust in cases where writes to the database are
cut off abrubtly due to external instability issues,
which can happen when capturing real applications in a layer which applications might not know about.
See `fossilize_db.cpp` for details on the archive format.

The JSON is a simple format which represents the various `Vk*CreateInfo` structures.
When referring to other VK handle types like `pImmutableSamplers` in `VkDescriptorSetLayout`, or `VkRenderPass` in `VkPipeline`,
a hash is used. 0 represents `VK_NULL_HANDLE` and anything else represents a key.
Small data blobs like specialization constant data are encoded in base64.
When recording or replaying, a mapping from and to real Vk object handles must be provided by the application so the key-based indexing scheme can be resolved to real handles.

The varint encoding scheme encodes every 32-bit SPIR-V word by encoding 7 bits at a time starting with the LSBs,
the MSB bit in an encoded byte is set if another byte needs to be read (7 bit) for the same SPIR-V word.
Each SPIR-V word takes from 1 to 5 bytes with this scheme.

## Sample API usage

### Recording state
```cpp
// Note that fossilize.hpp will include Vulkan headers, so make sure you include vulkan.h before
// this one if you care about which Vulkan headers you use.
#include "fossilize.hpp"
#include "fossilize_db.hpp"

void create_state()
{
    auto db = std::unique_ptr<Fossilize::DatabaseInterface>(
            Fossilize::create_database("/tmp/test.foz", Fossilize::DatabaseMode::OverWrite));
    if (!db)
        return;

    Fossilize::StateRecorder recorder;

    // If using a database, you cannot serialize later with recorder.serialize().
    // This is optional.
    // The database method is useful if you intend to replay the archive in fossilize-replay or similar.
    // The recorder interface calls db->prepare(), so it is not necessary to do so here.
    recorder.init_recording_thread(db.get());

    // TODO here: Add way to capture which extensions/physical device features were used to deal with exotic things
    // which require extensions when making repro cases.

    VkDescriptorSetLayoutCreateInfo info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };

    // Fill in stuff.
    vkCreateDescriptorSetLayout(..., &layout);

    // Records the descriptor set layout.
    bool success = recorder.record_descriptor_set_layout(layout, info);

    // Do the same for render passes, pipelines, shader modules, samplers (if using immutable samplers) as necessary.

    // If you don't use the database recording thread, you can create a single monolithic JSON,
    // otherwise, just make sure the state recorder is destroyed before the database interface.
    uint8_t *serialized;
    size_t size;
    recorder.serialize(&serialized, &size);
    save_to_disk(serialized, size);
    recorder.free_serialized(serialized);
}
```

### Replaying state
```cpp
// Note that fossilize.hpp will include Vulkan headers, so make sure you include vulkan.h before
// this one if you care about which Vulkan headers you use.
#include "fossilize.hpp"

struct Device : Fossilize::StateCreatorInterface
{
    // See header for other functions.

    bool enqueue_create_descriptor_set_layout(Hash hash,
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

    void notify_replayed_resources_for_type() override
    {
        // If using multi-threaded creation, join all queued tasks here.
    }

    void sync_threads() override
    {
        // If using multi-threaded creation, join all queued tasks here.
    }

    void sync_threads_modules() override
    {
        // If using multi-threaded creation, join all queued tasks here if we have pending VkShaderModule creations.
    }
};

void replay_state(Device &device)
{
    Fossilize::Replayer replayer;
    bool success = replayer.parse(device, nullptr, serialized_state, serialized_state_size);
    // Now internal hashmaps are warmed up, and all pipelines have been created.

    // It is possible to keep parsing state blobs.
    // This is useful if the state blobs come from Fossilize stream archives.
}
```

## Vulkan layer capture

Fossilize can also capture Vulkan application through the layer mechanism.
The layer name is `VK_LAYER_fossilize`.

To build, enable `FOSSILIZE_VULKAN_LAYER` CMake option. This is enabled by default.
The layer and JSON is placed in `layer/` in the build folder.

### Linux/Windows

By default the layer will serialize to `fossilize.$hash.$index.foz` in the working directory on `vkDestroyDevice`.
However, due to the nature of some drivers, there might be crashes in-between. For this, there are two other modes.

#### `export FOSSILIZE=1`

Fossilize in an implicit layer with an `enable_environment` mechanism.
Set this environment variable to automatically load the Fossilize layer.

#### `export FOSSILIZE_DUMP_SIGSEGV=1`

On Linux and Android, a SIGSEGV handler is registered on instance creation,
and the offending pipeline is serialized to disk in the segfault handler.
This is very sketchy for general use since it's not guaranteed to work and it overrides any application handlers,
but it should work well if drivers are crashing on pipeline creation.
On Windows, the global SEH handler is overridden instead.

If an access violation is triggered, the serialization thread is flushed.
A message box will appear on Windows, notifying user about this,
and immediately terminates the process after.

#### `export FOSSILIZE_DUMP_SYNC=1`

Similar to use case for `FOSSILIZE_DUMP_SIGSEGV=1`, but used when `SIGSEGV` dumping does not work correctly, e.g.
when application relies on using these signal handlers internally. In this mode, all recording is done fully synchronized
before calling into drivers, which is robust, but likely very slow.

#### `export FOSSILIZE_DUMP_PATH=/my/custom/path`

Custom file path for capturing state. The actual path which is written to disk will be `$FOSSILIZE_DUMP_PATH.$hash.$index.foz`.
This is to allow multiple processes and applications to dump concurrently.

### Android

By default the layer will serialize to `/sdcard/fossilize.json` on `vkDestroyDevice`.
However, this path might not be writeable, so you will probably have to override your path to something like
`/sdcard/Android/data/<package.name>/capture.json`.
Make sure your app has external write permissions if using the default path.

Due to the nature of some drivers, there might be crashes in-between. For this, there are two other modes.
Options can be set through `setprop`.

#### Setprop options

- `setprop debug.fossilize.dump_path /custom/path`
- `setprop debug.fossilize.dump_sigsegv 1`

To force layer to be enabled outside application: `setprop debug.vulkan.layers "VK_LAYER_fossilize"`.
The layer .so needs to be part of the APK for the loader to find the layer.

Use `adb logcat -s Fossilize` to isolate log messages coming from Fossilize.
You should see something like:
```
04-18 21:49:41.692 17444 17461 I Fossilize: Overriding serialization path: "/sdcard/fossilize.json".
04-18 21:49:43.741 17444 17461 I Fossilize: Serialized to "/sdcard/fossilize.json".
```
if capturing is working correctly.

## CLI

The CLI currently has 3 tools available. These are found in `cli/` after build.

### `fossilize-replay`

This tool is for taking a capture, and replaying it on any device.
Currently, all basic PhysicalDeviceFeatures (not PhysicalDeviceFeatures2 stuff) and extensions will be enabled
to make sure a capture will validate properly. robustBufferAccess however is set to whatever the database has used.

This tool serves as the main "repro" tool as well as a pipeline driver cache warming tool.
After you have a capture, you should ideally be able to repro crashes using this tool.
To make replay faster, use `--graphics-pipeline-range [start-index] [end-index]` and `--compute-pipeline-range [start-index] [end-index]` to isolate which pipelines are actually compiled.

### `fossilize-merge-db`

This tool merges and appends multiple databases into one database.

### `fossilize-convert-db`

This tool can convert the binary Fossilize database to a human readable representation and back to a Fossilize database.
This can be used to inspect individual database entries by hand.

### `fossilize-disasm`

**NOTE: This tool hasn't been updated since the change to the new database format. It might not work as intended at the moment.**

This tool can disassemble any pipeline into something human readable. Three modes are provided:
- ASM (using SPIRV-Tools)
- Vulkan GLSL (using SPIRV-Cross)
- AMD ISA (using `VK_AMD_shader_info` if available)

TODO is disassembling more of the other state for quick introspection. Currently only SPIR-V disassembly is provided.

### `fossilize-opt`

**NOTE: This tool hasn't been updated since the change to the new database format. It might not work as intended at the moment.**

Runs spirv-opt over all shader modules in the capture and serializes out an optimized version.
Useful to sanity check that an optimized capture can compile on your driver.

### Android

Running the CLI apps on Android is also supported.
Push the binaries generated to `/data/local/tmp`, `chmod +x` them if needed, and use the binaries like regular Linux.

## Submit shader failure repro cases

TBD

## External modules

- [volk](https://github.com/zeux/volk)
- [rapidjson](https://github.com/miloyip/rapidjson)
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools)
- [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)
