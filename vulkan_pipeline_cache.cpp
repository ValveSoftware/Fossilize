#include "vulkan_pipeline_cache.hpp"
#include <stdexcept>
#include <algorithm>

using namespace std;

namespace VPC
{
namespace Hashing
{
Hash compute_hash_descriptor_set_layout(const StateRecorder &recorder, const VkDescriptorSetLayoutCreateInfo &layout)
{
	Hasher h;

	h.u32(layout.bindingCount);
	h.u32(layout.flags);
	for (uint32_t i = 0; i < layout.bindingCount; i++)
	{
		auto &binding = layout.pBindings[i];
		h.u32(binding.binding);
		h.u32(binding.descriptorCount);
		h.u32(binding.descriptorType);
		h.u32(binding.stageFlags);

		if (binding.pImmutableSamplers &&
			(binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
		    binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
		{
			for (uint32_t j = 0; j < binding.descriptorCount; j++)
				h.u64(recorder.get_hash_for_sampler(binding.pImmutableSamplers[j]));
		}
	}

	return h.get();
}

Hash compute_hash_pipeline_layout(const StateRecorder &recorder, const VkPipelineLayoutCreateInfo &layout)
{
	Hasher h;

	return h.get();
}

Hash compute_hash_shader_module(const StateRecorder &recorder, const VkShaderModuleCreateInfo &create_info)
{
	Hasher h;

	return h.get();
}

Hash compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info)
{
	Hasher h;

	return h.get();
}

Hash compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info)
{
	Hasher h;

	return h.get();
}

Hash compute_hash_render_pass(const StateRecorder &recorder, const VkRenderPassCreateInfo &create_info)
{
	Hasher h;

	return h.get();
}
}

template <typename T>
T *StateRecorder::copy(const T *src, size_t count)
{
	auto *new_data = allocator.allocate_n<T>(count);
	if (new_data)
		std::copy(src, src + count, new_data);
	return new_data;
}

ScratchAllocator::Block::Block(size_t size)
{
	blob.reset(new uint8_t[size]);
	this->size = size;
}

void ScratchAllocator::add_block(size_t minimum_size)
{
	if (minimum_size < 64 * 1024)
		minimum_size = 64 * 1024;
	blocks.emplace_back(minimum_size);
}

void *ScratchAllocator::allocate_raw(size_t size, size_t alignment)
{
	if (blocks.empty())
		add_block(size + alignment);

	auto &block = blocks.back();
	if (!block.blob)
		return nullptr;

	size_t offset = (block.offset + alignment - 1) & ~alignment;
	size_t required_size = offset + size;
	if (required_size <= size)
	{
		void *ret = block.blob.get() + offset;
		block.offset = required_size;
		return ret;
	}

	add_block(size + alignment);
	return allocate_raw(size, alignment);
}

void StateRecorder::set_compute_pipeline_handle(unsigned index, VkPipeline pipeline)
{
	compute_pipeline_to_index[pipeline] = index;
}

void StateRecorder::set_descriptor_set_layout_handle(unsigned index, VkDescriptorSetLayout layout)
{
	descriptor_set_layout_to_index[layout] = index;
}

void StateRecorder::set_graphics_pipeline_handle(unsigned index, VkPipeline pipeline)
{
	graphics_pipeline_to_index[pipeline] = index;
}

void StateRecorder::set_pipeline_layout_handle(unsigned index, VkPipelineLayout layout)
{
	pipeline_layout_to_index[layout] = index;
}

void StateRecorder::set_render_pass_handle(unsigned index, VkRenderPass render_pass)
{
	render_pass_to_index[render_pass] = index;
}

void StateRecorder::set_shader_module_handle(unsigned index, VkShaderModule module)
{
	shader_module_to_index[module] = index;
}

void StateRecorder::set_sampler_handle(unsigned index, VkSampler sampler)
{
	sampler_to_index[sampler] = index;
}

unsigned StateRecorder::register_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo &layout_info)
{
	auto index = unsigned(descriptor_sets.size());
	descriptor_sets.push_back({ hash, copy_descriptor_set_layout(layout_info) });
	return index;
}

unsigned StateRecorder::register_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo &layout_info)
{
	auto index = unsigned(pipeline_layouts.size());
	pipeline_layouts.push_back({ hash, copy_pipeline_layout(layout_info) });
	return index;
}

unsigned StateRecorder::register_sampler(Hash hash, const VkSamplerCreateInfo &create_info)
{
	auto index = unsigned(samplers.size());
	samplers.push_back({ hash, copy_sampler(create_info) });
	return index;
}

unsigned StateRecorder::register_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo &create_info)
{
	auto index = unsigned(graphics_pipelines.size());
	graphics_pipelines.push_back({ hash, copy_graphics_pipeline(create_info) });
	return index;
}

unsigned StateRecorder::register_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo &create_info)
{
	auto index = unsigned(compute_pipelines.size());
	compute_pipelines.push_back({ hash, copy_compute_pipeline(create_info) });
	return index;
}

unsigned StateRecorder::register_render_pass(Hash hash, const VkRenderPassCreateInfo &create_info)
{
	auto index = unsigned(render_passes.size());
	render_passes.push_back({ hash, copy_render_pass(create_info) });
	return index;
}

unsigned StateRecorder::register_shader_module(Hash hash, const VkShaderModuleCreateInfo &create_info)
{
	auto index = unsigned(shader_modules.size());
	shader_modules.push_back({ hash, copy_shader_module(create_info) });
	return index;
}

Hash StateRecorder::get_hash_for_compute_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = compute_pipeline_to_index.find(pipeline);
	if (itr == end(compute_pipeline_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return compute_pipelines[itr->second].hash;
}

Hash StateRecorder::get_hash_for_graphics_pipeline_handle(VkPipeline pipeline) const
{
	auto itr = graphics_pipeline_to_index.find(pipeline);
	if (itr == end(graphics_pipeline_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return graphics_pipelines[itr->second].hash;
}

Hash StateRecorder::get_hash_for_sampler(VkSampler sampler) const
{
	auto itr = sampler_to_index.find(sampler);
	if (itr == end(sampler_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return samplers[itr->second].hash;
}

Hash StateRecorder::get_hash_for_shader_module(VkShaderModule module) const
{
	auto itr = shader_module_to_index.find(module);
	if (itr == end(shader_module_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return shader_modules[itr->second].hash;
}

Hash StateRecorder::get_hash_for_pipeline_layout(VkPipelineLayout layout) const
{
	auto itr = pipeline_layout_to_index.find(layout);
	if (itr == end(pipeline_layout_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return pipeline_layouts[itr->second].hash;
}

Hash StateRecorder::get_hash_for_descriptor_set_layout(VkDescriptorSetLayout layout) const
{
	auto itr = descriptor_set_layout_to_index.find(layout);
	if (itr == end(descriptor_set_layout_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return descriptor_sets[itr->second].hash;
}

Hash StateRecorder::get_hash_for_render_pass(VkRenderPass render_pass) const
{
	auto itr = render_pass_to_index.find(render_pass);
	if (itr == end(render_pass_to_index))
		throw runtime_error("Handle is not registered.");
	else
		return render_passes[itr->second].hash;
}

}
