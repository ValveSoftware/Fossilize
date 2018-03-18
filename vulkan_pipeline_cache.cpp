#include "vulkan_pipeline_cache.hpp"
#include <stdexcept>
#include <algorithm>
#include <string.h>

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

	h.u32(layout.setLayoutCount);
	for (uint32_t i = 0; i < layout.setLayoutCount; i++)
	{
		if (layout.pSetLayouts[i])
			h.u64(recorder.get_hash_for_descriptor_set_layout(layout.pSetLayouts[i]));
		else
			h.u32(0);
	}

	h.u32(layout.pushConstantRangeCount);
	for (uint32_t i = 0; i < layout.pushConstantRangeCount; i++)
	{
		auto &push = layout.pPushConstantRanges[i];
		h.u32(push.stageFlags);
		h.u32(push.size);
		h.u32(push.offset);
	}

	h.u32(layout.flags);

	return h.get();
}

Hash compute_hash_shader_module(const StateRecorder &, const VkShaderModuleCreateInfo &create_info)
{
	Hasher h;
	h.data(create_info.pCode, create_info.codeSize);
	h.u32(create_info.flags);
	return h.get();
}

static void hash_specialization_info(Hasher &h, const VkSpecializationInfo &spec)
{
	h.data(static_cast<const uint8_t *>(spec.pData), spec.dataSize);
	h.u32(spec.dataSize);
	h.u32(spec.mapEntryCount);
	for (uint32_t i = 0; i < spec.mapEntryCount; i++)
	{
		h.u32(spec.pMapEntries[i].offset);
		h.u32(spec.pMapEntries[i].size);
		h.u32(spec.pMapEntries[i].constantID);
	}
}

Hash compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info)
{
	Hasher h;

	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		h.u64(recorder.get_hash_for_graphics_pipeline_handle(create_info.basePipelineHandle));
		h.s32(create_info.basePipelineIndex);
	}

	h.u64(recorder.get_hash_for_pipeline_layout(create_info.layout));
	h.u64(recorder.get_hash_for_render_pass(create_info.renderPass));
	h.u32(create_info.subpass);
	h.u32(create_info.stageCount);

	bool dynamic_stencil_compare = false;
	bool dynamic_stencil_reference = false;
	bool dynamic_stencil_write_mask = false;
	bool dynamic_depth_bounds = false;
	bool dynamic_depth_bias = false;
	bool dynamic_line_width = false;
	bool dynamic_blend_constants = false;
	bool dynamic_scissor = false;
	bool dynamic_viewport = false;
	if (create_info.pDynamicState)
	{
		auto &state = *create_info.pDynamicState;
		h.u32(state.dynamicStateCount);
		h.u32(state.flags);
		for (uint32_t i = 0; i < state.dynamicStateCount; i++)
		{
			h.u32(state.pDynamicStates[i]);
			switch (state.pDynamicStates[i])
			{
			case VK_DYNAMIC_STATE_DEPTH_BIAS:
				dynamic_depth_bias = true;
				break;
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
				dynamic_depth_bounds = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
				dynamic_stencil_write_mask = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
				dynamic_stencil_reference = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
				dynamic_stencil_compare = true;
				break;
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
				dynamic_blend_constants = true;
				break;
			case VK_DYNAMIC_STATE_SCISSOR:
				dynamic_scissor = true;
				break;
			case VK_DYNAMIC_STATE_VIEWPORT:
				dynamic_viewport = true;
				break;
			case VK_DYNAMIC_STATE_LINE_WIDTH:
				dynamic_line_width = true;
				break;
			default:
				break;
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pDepthStencilState)
	{
		auto &ds = *create_info.pDepthStencilState;
		h.u32(ds.flags);
		h.u32(ds.depthBoundsTestEnable);
		h.u32(ds.depthCompareOp);
		h.u32(ds.depthTestEnable);
		h.u32(ds.depthWriteEnable);
		h.u32(ds.front.compareOp);
		h.u32(ds.front.depthFailOp);
		h.u32(ds.front.failOp);
		h.u32(ds.front.passOp);
		h.u32(ds.back.compareOp);
		h.u32(ds.back.depthFailOp);
		h.u32(ds.back.failOp);
		h.u32(ds.back.passOp);
		h.u32(ds.stencilTestEnable);

		if (!dynamic_depth_bounds && ds.depthBoundsTestEnable)
		{
			h.f32(ds.minDepthBounds);
			h.f32(ds.maxDepthBounds);
		}

		if (ds.stencilTestEnable)
		{
			if (!dynamic_stencil_compare)
			{
				h.u32(ds.front.compareMask);
				h.u32(ds.back.compareMask);
			}

			if (!dynamic_stencil_reference)
			{
				h.u32(ds.front.reference);
				h.u32(ds.back.reference);
			}

			if (!dynamic_stencil_write_mask)
			{
				h.u32(ds.front.writeMask);
				h.u32(ds.back.writeMask);
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pInputAssemblyState)
	{
		auto &ia = *create_info.pInputAssemblyState;
		h.u32(ia.flags);
		h.u32(ia.primitiveRestartEnable);
		h.u32(ia.topology);
	}
	else
		h.u32(0);

	if (create_info.pRasterizationState)
	{
		auto &rs = *create_info.pRasterizationState;
		h.u32(rs.flags);
		h.u32(rs.cullMode);
		h.u32(rs.depthClampEnable);
		h.u32(rs.frontFace);
		h.u32(rs.rasterizerDiscardEnable);
		h.u32(rs.polygonMode);
		h.u32(rs.depthBiasEnable);

		if (rs.depthBiasEnable && !dynamic_depth_bias)
		{
			h.f32(rs.depthBiasClamp);
			h.f32(rs.depthBiasSlopeFactor);
			h.f32(rs.depthBiasConstantFactor);
		}

		if (!dynamic_line_width)
			h.f32(rs.lineWidth);
	}
	else
		h.u32(0);

	if (create_info.pMultisampleState)
	{
		auto &ms = *create_info.pMultisampleState;
		h.u32(ms.flags);
		h.u32(ms.alphaToCoverageEnable);
		h.u32(ms.alphaToOneEnable);
		h.f32(ms.minSampleShading);
		h.u32(ms.rasterizationSamples);
		h.u32(ms.sampleShadingEnable);
		if (ms.pSampleMask)
		{
			uint32_t elems = (ms.rasterizationSamples + 31) / 32;
			for (uint32_t i = 0; i < elems; i++)
				h.u32(ms.pSampleMask[i]);
		}
		else
			h.u32(0);
	}

	if (create_info.pViewportState)
	{
		auto &vp = *create_info.pViewportState;
		h.u32(vp.flags);
		h.u32(vp.scissorCount);
		h.u32(vp.viewportCount);
		if (!dynamic_scissor)
		{
			for (uint32_t i = 0; i < vp.scissorCount; i++)
			{
				h.s32(vp.pScissors[i].offset.x);
				h.s32(vp.pScissors[i].offset.y);
				h.u32(vp.pScissors[i].extent.width);
				h.u32(vp.pScissors[i].extent.height);
			}
		}

		if (!dynamic_viewport)
		{
			for (uint32_t i = 0; i < vp.viewportCount; i++)
			{
				h.f32(vp.pViewports[i].x);
				h.f32(vp.pViewports[i].y);
				h.f32(vp.pViewports[i].width);
				h.f32(vp.pViewports[i].height);
				h.f32(vp.pViewports[i].minDepth);
				h.f32(vp.pViewports[i].maxDepth);
			}
		}
	}
	else
		h.u32(0);

	if (create_info.pVertexInputState)
	{
		auto &vi = *create_info.pVertexInputState;
		h.u32(vi.flags);
		h.u32(vi.vertexAttributeDescriptionCount);
		h.u32(vi.vertexBindingDescriptionCount);

		for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; i++)
		{
			h.u32(vi.pVertexAttributeDescriptions[i].offset);
			h.u32(vi.pVertexAttributeDescriptions[i].binding);
			h.u32(vi.pVertexAttributeDescriptions[i].format);
			h.u32(vi.pVertexAttributeDescriptions[i].location);
		}

		for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++)
		{
			h.u32(vi.pVertexBindingDescriptions[i].binding);
			h.u32(vi.pVertexBindingDescriptions[i].inputRate);
			h.u32(vi.pVertexBindingDescriptions[i].stride);
		}
	}
	else
		h.u32(0);

	if (create_info.pColorBlendState)
	{
		auto &b = *create_info.pColorBlendState;
		h.u32(b.flags);
		h.u32(b.attachmentCount);
		h.u32(b.logicOpEnable);
		h.u32(b.logicOp);

		bool need_blend_constants = false;

		for (uint32_t i = 0; i < b.attachmentCount; i++)
		{
			h.u32(b.pAttachments[i].blendEnable);
			if (b.pAttachments[i].blendEnable)
			{
				h.u32(b.pAttachments[i].colorWriteMask);
				h.u32(b.pAttachments[i].alphaBlendOp);
				h.u32(b.pAttachments[i].colorBlendOp);
				h.u32(b.pAttachments[i].dstAlphaBlendFactor);
				h.u32(b.pAttachments[i].srcAlphaBlendFactor);
				h.u32(b.pAttachments[i].dstColorBlendFactor);
				h.u32(b.pAttachments[i].srcColorBlendFactor);

				if (b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR)
				{
					need_blend_constants = true;
				}
			}
			else
				h.u32(0);
		}

		if (need_blend_constants && !dynamic_blend_constants)
			for (auto &blend_const : b.blendConstants)
				h.f32(blend_const);
	}
	else
		h.u32(0);

	if (create_info.pTessellationState)
	{
		auto &tess = *create_info.pTessellationState;
		h.u32(tess.flags);
		h.u32(tess.patchControlPoints);
	}
	else
		h.u32(0);

	for (uint32_t i = 0; i < create_info.stageCount; i++)
	{
		auto &stage = create_info.pStages[i];
		h.u32(stage.flags);
		h.string(stage.pName);
		h.u32(stage.stage);
		h.u64(recorder.get_hash_for_shader_module(stage.module));
		if (stage.pSpecializationInfo)
		{
			hash_specialization_info(h, *stage.pSpecializationInfo);

		}
		else
			h.u32(0);
	}

	return h.get();
}

Hash compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info)
{
	Hasher h;

	h.u64(recorder.get_hash_for_pipeline_layout(create_info.layout));
	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		h.u64(recorder.get_hash_for_compute_pipeline_handle(create_info.basePipelineHandle));
		h.s32(create_info.basePipelineIndex);
	}
	else
		h.u32(0);

	h.u64(recorder.get_hash_for_shader_module(create_info.stage.module));
	h.string(create_info.stage.pName);
	h.u32(create_info.stage.flags);
	h.u32(create_info.stage.stage);

	if (create_info.stage.pSpecializationInfo)
		hash_specialization_info(h, *create_info.stage.pSpecializationInfo);
	else
		h.u32(0);

	return h.get();
}

static void hash_attachment(Hasher &h, const VkAttachmentDescription &att)
{
	h.u32(att.flags);
	h.u32(att.initialLayout);
	h.u32(att.finalLayout);
	h.u32(att.format);
	h.u32(att.loadOp);
	h.u32(att.storeOp);
	h.u32(att.stencilLoadOp);
	h.u32(att.stencilStoreOp);
	h.u32(att.samples);
}

static void hash_dependency(Hasher &h, const VkSubpassDependency &dep)
{
	h.u32(dep.dependencyFlags);
	h.u32(dep.dstAccessMask);
	h.u32(dep.srcAccessMask);
	h.u32(dep.srcSubpass);
	h.u32(dep.dstSubpass);
	h.u32(dep.srcStageMask);
	h.u32(dep.dstStageMask);
}

static void hash_subpass(Hasher &h, const VkSubpassDescription &subpass)
{
	h.u32(subpass.flags);
	h.u32(subpass.colorAttachmentCount);
	h.u32(subpass.inputAttachmentCount);
	h.u32(subpass.preserveAttachmentCount);
	h.u32(subpass.pipelineBindPoint);

	for (uint32_t i = 0; i < subpass.preserveAttachmentCount; i++)
		h.u32(subpass.pPreserveAttachments[i]);

	for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
	{
		h.u32(subpass.pColorAttachments[i].attachment);
		h.u32(subpass.pColorAttachments[i].layout);
	}

	for (uint32_t i = 0; i < subpass.inputAttachmentCount; i++)
	{
		h.u32(subpass.pInputAttachments[i].attachment);
		h.u32(subpass.pInputAttachments[i].layout);
	}

	if (subpass.pResolveAttachments)
	{
		for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
		{
			h.u32(subpass.pResolveAttachments[i].attachment);
			h.u32(subpass.pResolveAttachments[i].layout);
		}
	}

	if (subpass.pDepthStencilAttachment)
	{
		h.u32(subpass.pDepthStencilAttachment->attachment);
		h.u32(subpass.pDepthStencilAttachment->layout);
	}
	else
		h.u32(0);
}

Hash compute_hash_render_pass(const StateRecorder &, const VkRenderPassCreateInfo &create_info)
{
	Hasher h;

	h.u32(create_info.attachmentCount);
	h.u32(create_info.dependencyCount);
	h.u32(create_info.subpassCount);

	for (uint32_t i = 0; i < create_info.attachmentCount; i++)
	{
		auto &att = create_info.pAttachments[i];
		hash_attachment(h, att);
	}

	for (uint32_t i = 0; i < create_info.dependencyCount; i++)
	{
		auto &dep = create_info.pDependencies[i];
		hash_dependency(h, dep);
	}

	for (uint32_t i = 0; i < create_info.subpassCount; i++)
	{
		auto &subpass = create_info.pSubpasses[i];
		hash_subpass(h, subpass);
	}

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

VkShaderModuleCreateInfo StateRecorder::copy_shader_module(const VkShaderModuleCreateInfo &create_info)
{
	auto info = create_info;
	info.pCode = copy(info.pCode, info.codeSize / sizeof(uint32_t));
	return info;
}

VkSamplerCreateInfo StateRecorder::copy_sampler(const VkSamplerCreateInfo &create_info)
{
	return create_info;
}

VkDescriptorSetLayoutCreateInfo StateRecorder::copy_descriptor_set_layout(
	const VkDescriptorSetLayoutCreateInfo &create_info)
{
	auto info = create_info;
	info.pBindings = copy(info.pBindings, info.bindingCount);

	for (uint32_t i = 0; i < info.bindingCount; i++)
	{
		auto &b = info.pBindings[i];
		if (b.pImmutableSamplers &&
		    (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		{
			const_cast<VkSampler *>(b.pImmutableSamplers)[i] =
				reinterpret_cast<VkSampler>(uint64_t(sampler_to_index[b.pImmutableSamplers[i]] + 1));
		}
	}

	return info;
}

VkPipelineLayoutCreateInfo StateRecorder::copy_pipeline_layout(const VkPipelineLayoutCreateInfo &create_info)
{
	auto info = create_info;
	info.pPushConstantRanges = copy(info.pPushConstantRanges, info.pushConstantRangeCount);
	info.pSetLayouts = copy(info.pSetLayouts, info.setLayoutCount);
	for (uint32_t i = 0; i < info.setLayoutCount; i++)
	{
		const_cast<VkDescriptorSetLayout *>(info.pSetLayouts)[i] =
			reinterpret_cast<VkDescriptorSetLayout>(uint64_t(descriptor_set_layout_to_index[info.pSetLayouts[i]] + 1));
	}
	return info;
}

VkSpecializationInfo *StateRecorder::copy_specialization_info(const VkSpecializationInfo *info)
{
	auto *ret = copy(info, 1);
	ret->pMapEntries = copy(ret->pMapEntries, ret->mapEntryCount);
	ret->pData = copy(static_cast<const uint8_t *>(ret->pData), ret->dataSize);
	return ret;
}

VkComputePipelineCreateInfo StateRecorder::copy_compute_pipeline(const VkComputePipelineCreateInfo &create_info)
{
	auto info = create_info;
	info.stage.pSpecializationInfo = copy_specialization_info(info.stage.pSpecializationInfo);
	info.stage.module = reinterpret_cast<VkShaderModule>(uint64_t(shader_module_to_index[create_info.stage.module] + 1));
	info.stage.pName = copy(info.stage.pName, strlen(info.stage.pName) + 1);
	if (info.basePipelineHandle != VK_NULL_HANDLE)
		info.basePipelineHandle = reinterpret_cast<VkPipeline>(uint64_t(compute_pipeline_to_index[info.basePipelineHandle] + 1));
	return info;
}

VkGraphicsPipelineCreateInfo StateRecorder::copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo &create_info)
{
	auto info = create_info;

	info.pStages = copy(info.pStages, info.stageCount);
	info.pTessellationState = copy(info.pTessellationState, 1);
	info.pColorBlendState = copy(info.pColorBlendState, 1);
	info.pVertexInputState = copy(info.pVertexInputState, 1);
	info.pMultisampleState = copy(info.pMultisampleState, 1);
	info.pVertexInputState = copy(info.pVertexInputState, 1);
	info.pViewportState = copy(info.pViewportState, 1);
	info.pInputAssemblyState  = copy(info.pInputAssemblyState, 1);
	info.pDepthStencilState = copy(info.pDepthStencilState, 1);
	info.pRasterizationState = copy(info.pRasterizationState, 1);
	info.pDynamicState = copy(info.pDynamicState, 1);
	info.renderPass = reinterpret_cast<VkRenderPass>(uint64_t(render_pass_to_index[info.renderPass] + 1));
	if (info.basePipelineHandle != VK_NULL_HANDLE)
		info.basePipelineHandle = reinterpret_cast<VkPipeline>(uint64_t(graphics_pipeline_to_index[info.basePipelineHandle] + 1));

	for (uint32_t i = 0; i < info.stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info.pStages[i]);
		stage.pName = copy(stage.pName, strlen(stage.pName) + 1);
		stage.pSpecializationInfo = copy_specialization_info(stage.pSpecializationInfo);
		stage.module = reinterpret_cast<VkShaderModule>(uint64_t(shader_module_to_index[stage.module] + 1));
	}

	auto &blend = const_cast<VkPipelineColorBlendStateCreateInfo &>(*info.pColorBlendState);
	blend.pAttachments = copy(blend.pAttachments, blend.attachmentCount);

	auto &vs = const_cast<VkPipelineVertexInputStateCreateInfo &>(*info.pVertexInputState);
	vs.pVertexAttributeDescriptions = copy(vs.pVertexAttributeDescriptions, vs.vertexAttributeDescriptionCount);
	vs.pVertexBindingDescriptions = copy(vs.pVertexBindingDescriptions, vs.vertexBindingDescriptionCount);

	auto &ms = const_cast<VkPipelineMultisampleStateCreateInfo &>(*info.pMultisampleState);
	if (ms.pSampleMask)
		ms.pSampleMask = copy(ms.pSampleMask, (ms.rasterizationSamples + 31) / 32);

	const_cast<VkPipelineDynamicStateCreateInfo *>(info.pDynamicState)->pDynamicStates =
		copy(info.pDynamicState->pDynamicStates, info.pDynamicState->dynamicStateCount);

	return info;
}

VkRenderPassCreateInfo StateRecorder::copy_render_pass(const VkRenderPassCreateInfo &create_info)
{
	auto info = create_info;
	info.pAttachments = copy(info.pAttachments, info.attachmentCount);
	info.pSubpasses = copy(info.pSubpasses, info.subpassCount);
	info.pDependencies = copy(info.pDependencies, info.dependencyCount);

	for (uint32_t i = 0; i < info.subpassCount; i++)
	{
		auto &sub = const_cast<VkSubpassDescription &>(info.pSubpasses[i]);
		if (sub.pDepthStencilAttachment)
			sub.pDepthStencilAttachment = copy(sub.pDepthStencilAttachment, 1);
		if (sub.pColorAttachments)
			sub.pColorAttachments = copy(sub.pColorAttachments, sub.colorAttachmentCount);
		if (sub.pResolveAttachments)
			sub.pResolveAttachments = copy(sub.pResolveAttachments, sub.colorAttachmentCount);
		if (sub.pInputAttachments)
			sub.pInputAttachments = copy(sub.pInputAttachments, sub.inputAttachmentCount);
		if (sub.pPreserveAttachments)
			sub.pPreserveAttachments = copy(sub.pPreserveAttachments, sub.preserveAttachmentCount);
	}

	return info;
}

bool StateRecorder::create_device(const VkPhysicalDeviceProperties &,
                                  const VkDeviceCreateInfo &)
{
	return true;
}

}
