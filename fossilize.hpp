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
#include <memory>
#include <vector>

#if defined(_MSC_VER) && (_MSC_VER <= 1800)
#define FOSSILIZE_NOEXCEPT
#else
#define FOSSILIZE_NOEXCEPT noexcept
#endif

namespace Fossilize
{
class DatabaseInterface;

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

private:
	struct Block
	{
		Block(size_t size);
		size_t offset = 0;
		std::vector<uint8_t> blob;
	};
	std::vector<Block> blocks;

	void add_block(size_t minimum_size);
};

class StateCreatorInterface
{
public:
	virtual ~StateCreatorInterface() = default;
	virtual bool set_num_samplers(unsigned /*count*/) { return true; }
	virtual bool set_num_descriptor_set_layouts(unsigned /*count*/) { return true; }
	virtual bool set_num_pipeline_layouts(unsigned /*count*/) { return true; }
	virtual bool set_num_shader_modules(unsigned /*count*/) { return true; }
	virtual bool set_num_render_passes(unsigned /*count*/) { return true; }
	virtual bool set_num_compute_pipelines(unsigned /*count*/) { return true; }
	virtual bool set_num_graphics_pipelines(unsigned /*count*/) { return true; }

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
	virtual void wait_enqueue() {}
};

class StateReplayer
{
public:
	StateReplayer();
	~StateReplayer();
	void parse(StateCreatorInterface &iface, DatabaseInterface *database, const void *buffer, size_t size);
	ScratchAllocator &get_allocator();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

class StateRecorder
{
public:
	StateRecorder();
	~StateRecorder();
	ScratchAllocator &get_allocator();

	// These methods should only be called at the very beginning of the application lifetime.
	// It will affect the hash of all create info structures.
	void record_application_info(const VkApplicationInfo &info);
	void record_physical_device_features(const VkPhysicalDeviceFeatures2 &device_features);
	void record_physical_device_features(const VkPhysicalDeviceFeatures &device_features);

	// TODO: create_device which can capture which features/exts are used to create the device.
	// This can be relevant when using more exotic features.

	void record_descriptor_set_layout(VkDescriptorSetLayout set_layout, const VkDescriptorSetLayoutCreateInfo &layout_info);
	void record_pipeline_layout(VkPipelineLayout pipeline_layout, const VkPipelineLayoutCreateInfo &layout_info);
	void record_shader_module(VkShaderModule module, const VkShaderModuleCreateInfo &create_info);
	void record_graphics_pipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo &create_info);
	void record_compute_pipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo &create_info);
	void record_render_pass(VkRenderPass render_pass, const VkRenderPassCreateInfo &create_info);
	void record_sampler(VkSampler sampler, const VkSamplerCreateInfo &create_info);

	Hash get_hash_for_descriptor_set_layout(VkDescriptorSetLayout layout) const;
	Hash get_hash_for_pipeline_layout(VkPipelineLayout layout) const;
	Hash get_hash_for_shader_module(VkShaderModule module) const;
	Hash get_hash_for_graphics_pipeline_handle(VkPipeline pipeline) const;
	Hash get_hash_for_compute_pipeline_handle(VkPipeline pipeline) const;
	Hash get_hash_for_render_pass(VkRenderPass render_pass) const;
	Hash get_hash_for_sampler(VkSampler sampler) const;

	void base_hash(Hasher &hasher) const;

	std::vector<uint8_t> serialize_graphics_pipeline(Hash hash) const;
	std::vector<uint8_t> serialize_compute_pipeline(Hash hash) const;
	std::vector<uint8_t> serialize_shader_module(Hash hash) const;
	std::vector<uint8_t> serialize() const;

	void init(DatabaseInterface *iface);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

namespace Hashing
{
Hash compute_hash_descriptor_set_layout(const StateRecorder &recorder, const VkDescriptorSetLayoutCreateInfo &layout);
Hash compute_hash_pipeline_layout(const StateRecorder &recorder, const VkPipelineLayoutCreateInfo &layout);
Hash compute_hash_shader_module(const StateRecorder &recorder, const VkShaderModuleCreateInfo &create_info);
Hash compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info);
Hash compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info);
Hash compute_hash_render_pass(const StateRecorder &recorder, const VkRenderPassCreateInfo &create_info);
Hash compute_hash_sampler(const StateRecorder &recorder, const VkSamplerCreateInfo &create_info);

Hash compute_hash_application_info(const VkApplicationInfo &info);
Hash compute_hash_physical_device_features(const VkPhysicalDeviceFeatures2 &pdf);
}

}
