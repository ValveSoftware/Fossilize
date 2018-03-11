#pragma once
#include "vulkan/vulkan.h"
#include <stdint.h>
#include <vector>
#include <memory>
#include <unordered_map>

namespace VPC
{
using Hash = uint64_t;

class Hasher
{
public:
	explicit Hasher(Hash h)
		: h(h)
	{
	}

	Hasher() = default;

	template <typename T>
	inline void data(const T *data, size_t size)
	{
		size /= sizeof(*data);
		for (size_t i = 0; i < size; i++)
			h = (h * 0x100000001b3ull) ^ data[i];
	}

	inline void u32(uint32_t value)
	{
		h = (h * 0x100000001b3ull) ^ value;
	}

	inline void s32(int32_t value)
	{
		u32(uint32_t(value));
	}

	inline void f32(float value)
	{
		union
		{
			float f32;
			uint32_t u32;
		} u;
		u.f32 = value;
		u32(u.u32);
	}

	inline void u64(uint64_t value)
	{
		u32(value & 0xffffffffu);
		u32(value >> 32);
	}

	template <typename T>
	inline void pointer(T *ptr)
	{
		u64(reinterpret_cast<uintptr_t>(ptr));
	}

	inline void string(const char *str)
	{
		char c;
		u32(0xff);
		while ((c = *str++) != '\0')
			u32(uint8_t(c));
	}

	inline void string(const std::string &str)
	{
		u32(0xff);
		for (auto &c : str)
			u32(uint8_t(c));
	}

	inline Hash get() const
	{
		return h;
	}

private:
	Hash h = 0xcbf29ce484222325ull;
};

class ScratchAllocator
{
public:
	template <typename T>
	T *allocate()
	{
		return static_cast<T *>(allocate_raw(sizeof(T), alignof(T)));
	}

	template <typename T>
	T *allocate_n(size_t count)
	{
		return static_cast<T *>(allocate_raw(sizeof(T) * count, alignof(T)));
	}

	void *allocate_raw(size_t size, size_t alignment);

private:
	struct Block
	{
		Block(size_t size);
		size_t offset = 0;
		size_t size = 0;
		std::unique_ptr<uint8_t []> blob;
	};
	std::vector<Block> blocks;

	void add_block(size_t minimum_size);
};

class StateRecorder
{
public:
	bool create_device(const VkPhysicalDeviceProperties &physical_device, const VkDeviceCreateInfo &create_info);

	unsigned register_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo &layout_info);
	unsigned register_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo &layout_info);
	unsigned register_shader_module(Hash hash, const VkShaderModuleCreateInfo &create_info);
	unsigned register_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo &create_info);
	unsigned register_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo &create_info);
	unsigned register_render_pass(Hash hash, const VkRenderPassCreateInfo &create_info);
	unsigned register_sampler(Hash hash, const VkSamplerCreateInfo &create_info);

	void set_descriptor_set_layout_handle(unsigned index, VkDescriptorSetLayout layout);
	void set_pipeline_layout_handle(unsigned index, VkPipelineLayout layout);
	void set_shader_module_handle(unsigned index, VkShaderModule module);
	void set_graphics_pipeline_handle(unsigned index, VkPipeline pipeline);
	void set_compute_pipeline_handle(unsigned index, VkPipeline pipeline);
	void set_render_pass_handle(unsigned index, VkRenderPass render_pass);
	void set_sampler_handle(unsigned index, VkSampler sampler);

	Hash get_hash_for_descriptor_set_layout(VkDescriptorSetLayout layout) const;
	Hash get_hash_for_pipeline_layout(VkPipelineLayout layout) const;
	Hash get_hash_for_shader_module(VkShaderModule module) const;
	Hash get_hash_for_graphics_pipeline_handle(VkPipeline pipeline) const;
	Hash get_hash_for_compute_pipeline_handle(VkPipeline pipeline) const;
	Hash get_hash_for_render_pass(VkRenderPass render_pass) const;
	Hash get_hash_for_sampler(VkSampler sampler) const;

	std::string serialize() const;

private:
	ScratchAllocator allocator;

	template <typename T>
	struct HashedInfo
	{
		Hash hash;
		T info;
	};

	std::vector<HashedInfo<VkDescriptorSetLayoutCreateInfo>> descriptor_sets;
	std::vector<HashedInfo<VkPipelineLayoutCreateInfo>> pipeline_layouts;
	std::vector<HashedInfo<VkShaderModuleCreateInfo>> shader_modules;
	std::vector<HashedInfo<VkGraphicsPipelineCreateInfo>> graphics_pipelines;
	std::vector<HashedInfo<VkComputePipelineCreateInfo>> compute_pipelines;
	std::vector<HashedInfo<VkRenderPassCreateInfo>> render_passes;
	std::vector<HashedInfo<VkSamplerCreateInfo>> samplers;

	std::unordered_map<VkDescriptorSetLayout, unsigned> descriptor_set_layout_to_index;
	std::unordered_map<VkPipelineLayout, unsigned> pipeline_layout_to_index;
	std::unordered_map<VkShaderModule, unsigned> shader_module_to_index;
	std::unordered_map<VkPipeline, unsigned> graphics_pipeline_to_index;
	std::unordered_map<VkPipeline, unsigned> compute_pipeline_to_index;
	std::unordered_map<VkRenderPass, unsigned> render_pass_to_index;
	std::unordered_map<VkSampler, unsigned> sampler_to_index;

	VkDescriptorSetLayoutCreateInfo copy_descriptor_set_layout(const VkDescriptorSetLayoutCreateInfo &create_info);
	VkPipelineLayoutCreateInfo copy_pipeline_layout(const VkPipelineLayoutCreateInfo &create_info);
	VkShaderModuleCreateInfo copy_shader_module(const VkShaderModuleCreateInfo &create_info);
	VkGraphicsPipelineCreateInfo copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo &create_info);
	VkComputePipelineCreateInfo copy_compute_pipeline(const VkComputePipelineCreateInfo &create_info);
	VkSamplerCreateInfo copy_sampler(const VkSamplerCreateInfo &create_info);
	VkRenderPassCreateInfo copy_render_pass(const VkRenderPassCreateInfo &create_info);

	template <typename T>
	T *copy(const T *src, size_t count);
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
}

}
