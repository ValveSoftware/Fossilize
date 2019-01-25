/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "vulkan.h"
#include <stdint.h>
#include <stddef.h>
#include <stdexcept>

#if defined(_MSC_VER) && (_MSC_VER <= 1800)
#define FOSSILIZE_NOEXCEPT
#else
#define FOSSILIZE_NOEXCEPT noexcept
#endif

namespace Fossilize
{
enum
{
	FOSSILIZE_FORMAT_VERSION = 4
};

class DatabaseInterface;

// TODO: Do we want to get rid of this?
class Exception : public std::exception
{
public:
	Exception(const char *what)
		: msg(what)
	{
	}

	const char *what() const FOSSILIZE_NOEXCEPT override
	{
		return msg;
	}

private:
	const char *msg;
};

using Hash = uint64_t;

class Hasher;

class ScratchAllocator
{
public:
	ScratchAllocator();
	~ScratchAllocator();

	// alignof(T) doesn't work on MSVC 2013.
	template <typename T>
	T *allocate()
	{
		return static_cast<T *>(allocate_raw(sizeof(T), 16));
	}

	template <typename T>
	T *allocate_cleared()
	{
		return static_cast<T *>(allocate_raw_cleared(sizeof(T), 16));
	}

	template <typename T>
	T *allocate_n(size_t count)
	{
		if (count == 0)
			return nullptr;
		return static_cast<T *>(allocate_raw(sizeof(T) * count, 16));
	}

	template <typename T>
	T *allocate_n_cleared(size_t count)
	{
		if (count == 0)
			return nullptr;
		return static_cast<T *>(allocate_raw_cleared(sizeof(T) * count, 16));
	}

	void *allocate_raw(size_t size, size_t alignment);
	void *allocate_raw_cleared(size_t size, size_t alignment);

	void reset();

	// Disable copies (and moves).
	ScratchAllocator(const ScratchAllocator &) = delete;
	void operator=(const ScratchAllocator &) = delete;

private:
	struct Impl;
	Impl *impl;
};

class StateCreatorInterface
{
public:
	virtual ~StateCreatorInterface() = default;

	// All future calls to enqueue_create_* were created using this application info.
	// app can be nullptr, in which case no pApplicationInfo was used (allowed in Vulkan 1.0).
	// The pointer provided in app is persistent as long as StateReplayer lives.
	// A physical device features 2 structure is also passed in, as it could affect compilation.
	// For now, only robustBufferAccess is used. physical_device_features can also be nullptr, in
	// which case the relevant feature robustBufferAccess is assumed to be turned off.
	virtual void set_application_info(const VkApplicationInfo * /*app*/, const VkPhysicalDeviceFeatures2 * /*physical_device_features*/) {}

	virtual bool enqueue_create_sampler(Hash index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) = 0;
	virtual bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) = 0;
	virtual bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) = 0;
	virtual bool enqueue_create_shader_module(Hash index, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) = 0;
	virtual bool enqueue_create_render_pass(Hash index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) = 0;
	virtual bool enqueue_create_compute_pipeline(Hash index, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) = 0;
	virtual bool enqueue_create_graphics_pipeline(Hash index, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) = 0;

	// Hard dependency, replayer must sync all its workers. This is only called for derived pipelines,
	// which need to have their parent be compiled before we can create the derived one.
	virtual void sync_threads() {}

	// Wait for all shader modules to be ready.
	virtual void sync_shader_modules() {}

	// Notifies the replayer that we are done replaying a type.
	// Replay can ignore this if it deals with synchronization between replayed types.
	virtual void notify_replayed_resources_for_type() {}
};

class StateReplayer
{
public:
	StateReplayer();
	~StateReplayer();
	void parse(StateCreatorInterface &iface, DatabaseInterface *database, const void *buffer, size_t size);
	ScratchAllocator &get_allocator();

	// Disable copies (and moves).
	StateReplayer(const StateReplayer &) = delete;
	void operator=(const StateReplayer &) = delete;

private:
	struct Impl;
	Impl *impl;
};

struct StateRecorderApplicationFeatureHash
{
	Hash application_info_hash = 0;
	Hash physical_device_features_hash = 0;
};

class StateRecorder
{
public:
	StateRecorder();
	~StateRecorder();
	ScratchAllocator &get_allocator();

	// Call before init_recording_thread.
	void set_database_enable_compression(bool enable);
	void set_database_enable_checksum(bool enable);

	// These methods should only be called at the very beginning of the application lifetime.
	// It will affect the hash of all create info structures.
	// These are never recorded in a thread, so it's safe to query the application/feature hash right after calling these methods.
	void record_application_info(const VkApplicationInfo &info);

	// TODO: create_device which can capture which features/exts are used to create the device.
	// This can be relevant when using more exotic features.
	void record_physical_device_features(const VkPhysicalDeviceFeatures2 &device_features);
	void record_physical_device_features(const VkPhysicalDeviceFeatures &device_features);

	const StateRecorderApplicationFeatureHash &get_application_feature_hash() const;

	void record_descriptor_set_layout(VkDescriptorSetLayout set_layout, const VkDescriptorSetLayoutCreateInfo &layout_info);
	void record_pipeline_layout(VkPipelineLayout pipeline_layout, const VkPipelineLayoutCreateInfo &layout_info);
	void record_shader_module(VkShaderModule module, const VkShaderModuleCreateInfo &create_info);
	void record_graphics_pipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo &create_info);
	void record_compute_pipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo &create_info);
	void record_render_pass(VkRenderPass render_pass, const VkRenderPassCreateInfo &create_info);
	void record_sampler(VkSampler sampler, const VkSamplerCreateInfo &create_info);

	// Used by hashing functions in Hashing namespace. Should be considered an implementation detail.
	Hash get_hash_for_descriptor_set_layout(VkDescriptorSetLayout layout) const;
	Hash get_hash_for_pipeline_layout(VkPipelineLayout layout) const;
	Hash get_hash_for_shader_module(VkShaderModule module) const;
	Hash get_hash_for_graphics_pipeline_handle(VkPipeline pipeline) const;
	Hash get_hash_for_compute_pipeline_handle(VkPipeline pipeline) const;
	Hash get_hash_for_render_pass(VkRenderPass render_pass) const;
	Hash get_hash_for_sampler(VkSampler sampler) const;

	// If database is non-null, serialize cannot not be called later, as the implementation will not retain
	// memory for the create info structs, but rather rely on the database interface to make objects persist.
	// The database interface will be fed with all information on the fly.
	//
	// This call can be omitted, in which case all recording calls will be called inline instead.
	// This is mostly useful for testing purposes.
	//
	// NOTE: If iface is non-null,
	// prepare() will be called asynchronously on the DatabaseInterface to hide the cost of up-front file I/O.
	// Do not attempt to call prepare() on the calling thread!
	void init_recording_thread(DatabaseInterface *iface);

	// Serializes and allocates data for it. This can only be used if a database interface was not used.
	// The result is a monolithic JSON document which contains all recorded state.
	// Free with free_serialized() to make sure alloc/frees happens in same module.
	bool serialize(uint8_t **serialized, size_t *serialized_size);
	static void free_serialized(uint8_t *serialized);

	// Disable copies (and moves).
	StateRecorder(const StateRecorder &) = delete;
	void operator=(const StateRecorder &) = delete;

private:
	struct Impl;
	Impl *impl;
};

namespace Hashing
{
// Computes a base hash which can be used to compute some other hashes without having to create a full StateRecorder.
// application_info and/or physical_device_features can be nullptr.
StateRecorderApplicationFeatureHash compute_application_feature_hash(const VkApplicationInfo *application_info,
                                                                     const VkPhysicalDeviceFeatures2 *physical_device_features);

// Shader modules, samplers and render passes are standalone modules, so they can be hashed in isolation.
Hash compute_hash_shader_module(const StateRecorderApplicationFeatureHash &base_hash, const VkShaderModuleCreateInfo &create_info);
Hash compute_hash_sampler(const StateRecorderApplicationFeatureHash &base_hash, const VkSamplerCreateInfo &create_info);
Hash compute_hash_render_pass(const StateRecorderApplicationFeatureHash &base_hash, const VkRenderPassCreateInfo &create_info);

// If you are recording in threaded mode, these must only be called from the recording thread,
// as the dependent objects might not have been recorded yet, and thus the mapping of dependent objects is unknown.
// If you need to use these for whatever reason, you cannot use StateRecorder in the threaded mode.
Hash compute_hash_descriptor_set_layout(const StateRecorder &recorder, const VkDescriptorSetLayoutCreateInfo &layout);
Hash compute_hash_pipeline_layout(const StateRecorder &recorder, const VkPipelineLayoutCreateInfo &layout);
Hash compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info);
Hash compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info);
}

}
