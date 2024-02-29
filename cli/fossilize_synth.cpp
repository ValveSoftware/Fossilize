/* Copyright (c) 2019 Hans-Kristian Arntzen
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

#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "vulkan/vulkan.h"
#include "spirv_cross_c.h"
#include "file.hpp"
#include <algorithm>
#include <memory>
#include <string.h>

using namespace Fossilize;

static void print_help()
{
	LOGE("Usage: fossilize-synth\n"
	     "\t[--vert shader.spv]\n"
	     "\t[--task shader.spv]\n"
	     "\t[--mesh shader.spv]\n"
	     "\t[--tesc shader.spv]\n"
	     "\t[--tese shader.spv]\n"
	     "\t[--geom shader.spv]\n"
	     "\t[--frag shader.spv]\n"
	     "\t[--comp shader.spv]\n"
	     "\t[--multiview views]\n"
	     "\t[--output out.foz]\n"
	     "\t[--spec <ID> <f32/u32/i32> <value>\n"
	     "\t[--multi-spec <index> <count>\n");
}

enum ShaderStage
{
	STAGE_VERT = 0,
	STAGE_TASK,
	STAGE_MESH,
	STAGE_TESC,
	STAGE_TESE,
	STAGE_GEOM,
	STAGE_FRAG,
	STAGE_COMP,
	STAGE_COUNT
};

static const VkShaderStageFlagBits to_vk_shader_stage[STAGE_COUNT] = {
	VK_SHADER_STAGE_VERTEX_BIT,
	VK_SHADER_STAGE_TASK_BIT_EXT,
	VK_SHADER_STAGE_MESH_BIT_EXT,
	VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
	VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
	VK_SHADER_STAGE_GEOMETRY_BIT,
	VK_SHADER_STAGE_FRAGMENT_BIT,
	VK_SHADER_STAGE_COMPUTE_BIT,
};

struct SpecConstant
{
	std::vector<uint32_t> data;
	std::vector<VkSpecializationMapEntry> map_entries;

	struct
	{
		uint32_t index;
		uint32_t count;
	} iteration = {};
};

static bool load_shader_modules(const std::string *stages, std::vector<uint8_t> *modules)
{
	for (unsigned i = 0; i < STAGE_COUNT; i++)
	{
		if (stages[i].empty())
			continue;

		modules[i] = load_buffer_from_file(stages[i].c_str());
		if (modules[i].empty())
		{
			LOGE("Failed to load file: %s\n", stages[i].c_str());
			return false;
		}
	}

	return true;
}

static bool reflect_shader_modules(spvc_context ctx, const std::vector<uint8_t> *modules, spvc_compiler *compilers)
{
	for (unsigned i = 0; i < STAGE_COUNT; i++)
	{
		if (modules[i].empty())
			continue;

		auto *data = reinterpret_cast<const uint32_t *>(modules[i].data());
		auto size = modules[i].size() / sizeof(uint32_t);

		spvc_parsed_ir parsed;
		if (spvc_context_parse_spirv(ctx, data, size, &parsed) != SPVC_SUCCESS)
		{
			LOGE("Failed to parse SPIR-V.\n");
			return false;
		}

		if (spvc_context_create_compiler(ctx, SPVC_BACKEND_NONE, parsed,
		                                 SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compilers[i]) != SPVC_SUCCESS)
		{
			LOGE("Failed to create compiler.\n");
			return false;
		}
	}

	return true;
}

static bool append_descriptor(std::vector<VkDescriptorSetLayoutBinding> *bindings,
                              std::vector<VkDescriptorBindingFlags> *binding_flags,
                              uint32_t desc_set, uint32_t binding,
                              VkDescriptorType desc_type, spvc_type type)
{
	if (desc_set >= 8)
	{
		LOGE("Descriptor set %u is out of range.\n", desc_set);
		return false;
	}

	auto &binds = bindings[desc_set];

	auto itr = std::find_if(binds.begin(), binds.end(), [&](const VkDescriptorSetLayoutBinding &bind) {
		return bind.binding == binding;
	});

	unsigned num_array_dimensions = spvc_type_get_num_array_dimensions(type);
	uint32_t desc_count = 1;
	if (num_array_dimensions == 1)
	{
		if (!spvc_type_array_dimension_is_literal(type, 0))
		{
			LOGE("Array size dimensions of resources must be constant literals.\n");
			return false;
		}
		desc_count = spvc_type_get_array_dimension(type, 0);
	}

	if (itr != binds.end())
	{
		if (itr->descriptorType != desc_type)
		{
			LOGE("Overlap in descriptor type for binding (%u, %u) (was %u, now %u).\n",
			     desc_set, binding, itr->descriptorType, desc_type);
			return false;
		}

		if (desc_count == 0)
			desc_count = 1000000;

		if (itr->descriptorCount != desc_count)
		{
			LOGE("Descriptor count mismatch for (%u, %u) (was %u, now %u).\n",
			     desc_set, binding, itr->descriptorCount, desc_count);
			return false;
		}
	}
	else
	{
		VkDescriptorSetLayoutBinding bind = {};
		bind.binding = binding;
		bind.stageFlags = VK_SHADER_STAGE_ALL;
		bind.descriptorType = desc_type;
		bind.descriptorCount = desc_count != 0 ? desc_count : 1000000;
		binds.push_back(bind);
		binding_flags[desc_set].push_back(desc_count == 0 ? VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT : 0);
	}

	return true;
}

static bool append_descriptors(std::vector<VkDescriptorSetLayoutBinding> *bindings,
                               std::vector<VkDescriptorBindingFlags> *binding_flags,
                               spvc_compiler compiler, spvc_resources resources,
                               spvc_resource_type resource_type, VkDescriptorType desc_type)
{
	const spvc_reflected_resource *list;
	size_t count;

	if (spvc_resources_get_resource_list_for_type(resources, resource_type, &list, &count) != SPVC_SUCCESS)
		return false;

	for (size_t i = 0; i < count; i++)
	{
		uint32_t desc_set = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
		uint32_t binding = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
		spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].type_id);
		if (!append_descriptor(bindings, binding_flags, desc_set, binding, desc_type, type))
			return false;
	}

	return true;
}

static bool add_bindings(spvc_compiler compiler,
                         std::vector<VkDescriptorSetLayoutBinding> *bindings,
                         std::vector<VkDescriptorBindingFlags> *binding_flags,
                         uint32_t &push_constant_size)
{
	spvc_resources resources;
	if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS)
	{
		LOGE("Failed to reflect resources.\n");
		return false;
	}

	const spvc_reflected_resource *list;
	size_t count;

	if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, &list, &count) != SPVC_SUCCESS)
		return false;

	for (size_t i = 0; i < count; i++)
	{
		uint32_t desc_set = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
		uint32_t binding = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
		spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].type_id);

		VkDescriptorType desc_type;
		if (spvc_type_get_image_dimension(type) == SpvDimBuffer)
			desc_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		else
			desc_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

		if (!append_descriptor(bindings, binding_flags, desc_set, binding, desc_type, type))
			return false;
	}

	if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STORAGE_IMAGE, &list, &count) != SPVC_SUCCESS)
		return false;

	for (size_t i = 0; i < count; i++)
	{
		uint32_t desc_set = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
		uint32_t binding = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
		spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].type_id);

		VkDescriptorType desc_type;
		if (spvc_type_get_image_dimension(type) == SpvDimBuffer)
			desc_type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
		else
			desc_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

		if (!append_descriptor(bindings, binding_flags, desc_set, binding, desc_type, type))
			return false;
	}

	if (!append_descriptors(bindings, binding_flags, compiler, resources,
	                        SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS, VK_DESCRIPTOR_TYPE_SAMPLER))
		return false;
	if (!append_descriptors(bindings, binding_flags, compiler, resources,
	                        SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		return false;
	if (!append_descriptors(bindings, binding_flags, compiler, resources,
	                        SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))
		return false;
	if (!append_descriptors(bindings, binding_flags, compiler, resources,
	                        SPVC_RESOURCE_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER))
		return false;
	if (!append_descriptors(bindings, binding_flags, compiler, resources,
	                        SPVC_RESOURCE_TYPE_SUBPASS_INPUT, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT))
		return false;

	if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_PUSH_CONSTANT, &list, &count) != SPVC_SUCCESS)
		return false;

	for (size_t i = 0; i < count; i++)
	{
		size_t push_size;
		if (spvc_compiler_get_declared_struct_size(compiler, spvc_compiler_get_type_handle(compiler, list[i].base_type_id), &push_size) != SPVC_SUCCESS)
			return false;

		if (push_size > push_constant_size)
			push_constant_size = push_size;
	}

	return true;
}

static VkPipelineLayout synthesize_pipeline_layout(StateRecorder &recorder, spvc_compiler *compilers)
{
	VkDescriptorSetLayout set_layouts[8] = {};
	unsigned num_set_layouts = 0;
	VkPushConstantRange push_constant_range = {};
	push_constant_range.stageFlags = VK_SHADER_STAGE_ALL;

	std::vector<VkDescriptorSetLayoutBinding> bindings[8];
	std::vector<VkDescriptorBindingFlags> binding_flags[8];

	for (unsigned i = 0; i < STAGE_COUNT; i++)
	{
		if (!compilers[i])
			continue;
		if (!add_bindings(compilers[i], bindings, binding_flags, push_constant_range.size))
			return VK_NULL_HANDLE;
	}

	for (unsigned i = 0; i < 8; i++)
		if (!bindings[i].empty())
			num_set_layouts = i + 1;

	for (unsigned i = 0; i < num_set_layouts; i++)
	{
		VkDescriptorSetLayoutCreateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		set_info.bindingCount = bindings[i].size();
		set_info.pBindings = bindings[i].data();

		VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };

		for (auto &flag : binding_flags[i])
		{
			if (flag != 0)
			{
				flags_info.bindingCount = set_info.bindingCount;
				flags_info.pBindingFlags = binding_flags[i].data();
				set_info.pNext = &flags_info;
				break;
			}
		}

		set_layouts[i] = (VkDescriptorSetLayout)uint64_t(i + 1);
		if (!recorder.record_descriptor_set_layout(set_layouts[i], set_info))
			return VK_NULL_HANDLE;
	}

	VkPipelineLayoutCreateInfo pipeline_layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipeline_layout_info.setLayoutCount = num_set_layouts;
	pipeline_layout_info.pSetLayouts = set_layouts;
	pipeline_layout_info.pushConstantRangeCount = push_constant_range.size != 0 ? 1 : 0;
	pipeline_layout_info.pPushConstantRanges = &push_constant_range;

	if (!recorder.record_pipeline_layout((VkPipelineLayout)uint64_t(1), pipeline_layout_info))
		return VK_NULL_HANDLE;

	return (VkPipelineLayout)uint64_t(1);
}

static VkRenderPass synthesize_render_pass(StateRecorder &recorder, spvc_compiler frag,
                                           uint32_t view_count, uint8_t &active_rt_mask)
{
	if (!frag)
		return VK_NULL_HANDLE;

	spvc_resources resources;
	if (spvc_compiler_create_shader_resources(frag, &resources) != SPVC_SUCCESS)
	{
		LOGE("Failed to reflect resources.\n");
		return VK_NULL_HANDLE;
	}

	const spvc_reflected_resource *list;
	size_t count;

	VkFormat rt_formats[8] = {};
	VkFormat input_rt_formats[8] = {};
	unsigned num_rts = 0;
	unsigned num_input_rts = 0;

	if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, &list, &count) != SPVC_SUCCESS)
		return VK_NULL_HANDLE;

	for (size_t i = 0; i < count; i++)
	{
		unsigned rt_count = 1;
		spvc_type rt_type = spvc_compiler_get_type_handle(frag, list[i].type_id);

		if (spvc_type_get_num_array_dimensions(rt_type) == 1)
		{
			if (!spvc_type_array_dimension_is_literal(rt_type, 0))
				return VK_NULL_HANDLE;
			rt_count = spvc_type_get_array_dimension(rt_type, 0);
		}

		uint32_t location = spvc_compiler_get_decoration(frag, list[i].id, SpvDecorationLocation);
		if (location + rt_count >= 8)
		{
			LOGE("RT index %u (array size %u) is out of range.\n", location, rt_count);
			return VK_NULL_HANDLE;
		}

		VkFormat base_format = VK_FORMAT_UNDEFINED;
		switch (spvc_type_get_basetype(rt_type))
		{
		case SPVC_BASETYPE_FP16:
		case SPVC_BASETYPE_FP32:
			base_format = VK_FORMAT_R8G8B8A8_UNORM;
			break;

		case SPVC_BASETYPE_INT16:
		case SPVC_BASETYPE_INT32:
			base_format = VK_FORMAT_R8G8B8A8_SINT;
			break;

		case SPVC_BASETYPE_UINT16:
		case SPVC_BASETYPE_UINT32:
			base_format = VK_FORMAT_R8G8B8A8_UINT;
			break;

		default:
			break;
		}

		for (unsigned j = 0; j < rt_count; j++)
		{
			rt_formats[location + j] = base_format;
			active_rt_mask |= 1u << (location + j);
		}
	}

	if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SUBPASS_INPUT, &list, &count) != SPVC_SUCCESS)
		return VK_NULL_HANDLE;

	for (size_t i = 0; i < count; i++)
	{
		unsigned rt_count = 1;
		spvc_type rt_type = spvc_compiler_get_type_handle(frag, list[i].type_id);

		if (spvc_type_get_num_array_dimensions(rt_type) == 1)
		{
			if (!spvc_type_array_dimension_is_literal(rt_type, 0))
				return VK_NULL_HANDLE;
			rt_count = spvc_type_get_array_dimension(rt_type, 0);
		}

		uint32_t location = spvc_compiler_get_decoration(frag, list[i].id, SpvDecorationInputAttachmentIndex);
		if (location + rt_count >= 8)
		{
			LOGE("Input attachment index %u (array size %u) is out of range.\n", location, rt_count);
			return VK_NULL_HANDLE;
		}

		VkFormat base_format = VK_FORMAT_UNDEFINED;
		switch (spvc_type_get_basetype(rt_type))
		{
		case SPVC_BASETYPE_FP16:
		case SPVC_BASETYPE_FP32:
			base_format = VK_FORMAT_R8G8B8A8_UNORM;
			break;

		case SPVC_BASETYPE_INT16:
		case SPVC_BASETYPE_INT32:
			base_format = VK_FORMAT_R8G8B8A8_SINT;
			break;

		case SPVC_BASETYPE_UINT16:
		case SPVC_BASETYPE_UINT32:
			base_format = VK_FORMAT_R8G8B8A8_UINT;
			break;

		default:
			break;
		}

		for (unsigned j = 0; j < rt_count; j++)
			input_rt_formats[location + j] = base_format;
	}

	for (unsigned i = 0; i < 8; i++)
		if (rt_formats[i] != VK_FORMAT_UNDEFINED)
			num_rts = i + 1;

	for (unsigned i = 0; i < 8; i++)
		if (input_rt_formats[i] != VK_FORMAT_UNDEFINED)
			num_input_rts = i + 1;

	if (num_rts + num_input_rts > 8)
	{
		LOGE("Number of total attachments exceeds 8.\n");
		return VK_NULL_HANDLE;
	}

	unsigned output_location_to_attachment[8] = {};
	unsigned input_location_to_attachment[8] = {};

	VkRenderPassCreateInfo2 info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 };
	VkAttachmentDescription2 attachments[8] = {};

	for (unsigned i = 0; i < num_rts; i++)
	{
		if (rt_formats[i] != VK_FORMAT_UNDEFINED)
		{
			auto &att = attachments[info.attachmentCount];
			att.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			att.format = rt_formats[i];
			att.samples = VK_SAMPLE_COUNT_1_BIT;
			att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			output_location_to_attachment[i] = info.attachmentCount;
			info.attachmentCount++;
		}
	}

	for (unsigned i = 0; i < num_input_rts; i++)
	{
		if (input_rt_formats[i] != VK_FORMAT_UNDEFINED)
		{
			auto &att = attachments[info.attachmentCount];
			att.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			att.format = input_rt_formats[i];
			att.samples = VK_SAMPLE_COUNT_1_BIT;
			att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			input_location_to_attachment[i] = info.attachmentCount;
			info.attachmentCount++;
		}
	}

	VkSubpassDescription2 subpass = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 };
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	// view_count of 0 translates to default 0 mask.
	subpass.viewMask = (1u << view_count) - 1u;

	VkAttachmentReference2 references[8] = {};
	VkAttachmentReference2 input_references[8] = {};

	for (unsigned i = 0; i < num_rts; i++)
	{
		references[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		if (rt_formats[i] != VK_FORMAT_UNDEFINED)
		{
			references[i].attachment = output_location_to_attachment[i];
			references[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
		else
		{
			references[i].attachment = VK_ATTACHMENT_UNUSED;
			references[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	for (unsigned i = 0; i < num_input_rts; i++)
	{
		input_references[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		if (input_rt_formats[i] != VK_FORMAT_UNDEFINED)
		{
			input_references[i].attachment = input_location_to_attachment[i];
			input_references[i].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}
		else
		{
			input_references[i].attachment = VK_ATTACHMENT_UNUSED;
			input_references[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;
		}
	}

	subpass.colorAttachmentCount = num_rts;
	subpass.pColorAttachments = references;
	subpass.inputAttachmentCount = num_input_rts;
	subpass.pInputAttachments = input_references;

	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.pAttachments = attachments;

	if (!recorder.record_render_pass2((VkRenderPass)uint64_t(1), info))
		return VK_NULL_HANDLE;

	return (VkRenderPass)uint64_t(1);
}

static VkPipeline synthesize_compute_pipeline(StateRecorder &recorder,
                                              const std::vector<uint8_t> *modules, VkPipelineLayout layout,
                                              SpecConstant &specs)
{
	VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	module_info.codeSize = modules[STAGE_COMP].size();
	module_info.pCode = reinterpret_cast<const uint32_t *>(modules[STAGE_COMP].data());
	if (!recorder.record_shader_module((VkShaderModule)uint64_t(1), module_info))
		return VK_NULL_HANDLE;

	VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkSpecializationInfo spec_info = {};

	spec_info.dataSize = specs.data.size() * sizeof(uint32_t);
	spec_info.pData = specs.data.data();
	spec_info.mapEntryCount = specs.map_entries.size();
	spec_info.pMapEntries = specs.map_entries.data();

	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = (VkShaderModule)uint64_t(1);
	info.stage.pName = "main";
	if (spec_info.dataSize != 0)
		info.stage.pSpecializationInfo = &spec_info;
	info.layout = layout;

	if (!recorder.record_compute_pipeline((VkPipeline) uint64_t(1), info, nullptr, 0))
		return VK_NULL_HANDLE;

	if (specs.iteration.count && specs.iteration.index < specs.data.size())
	{
		for (uint32_t i = 0; i < specs.iteration.count; i++)
		{
			specs.data[specs.iteration.index] = i;
			if (!recorder.record_compute_pipeline((VkPipeline) uint64_t(2 + i), info, nullptr, 0))
				return VK_NULL_HANDLE;
		}
	}

	return (VkPipeline)uint64_t(1);
}

static bool append_attributes(spvc_compiler vert,
                              std::vector<VkVertexInputAttributeDescription> &attributes,
                              uint32_t &stride)
{
	if (!vert)
		return false;

	spvc_resources resources;
	if (spvc_compiler_create_shader_resources(vert, &resources) != SPVC_SUCCESS)
		return false;

	const spvc_reflected_resource *list;
	size_t size;
	if (spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &size) != SPVC_SUCCESS)
		return false;

	for (size_t i = 0; i < size; i++)
	{
		uint32_t location = spvc_compiler_get_decoration(vert, list[i].id, SpvDecorationLocation);
		spvc_type type = spvc_compiler_get_type_handle(vert, list[i].type_id);
		uint32_t col = spvc_type_get_columns(type);

		VkFormat fmt = VK_FORMAT_UNDEFINED;
		uint32_t fmt_size = 0;

		switch (spvc_type_get_basetype(type))
		{
		case SPVC_BASETYPE_FP16:
		case SPVC_BASETYPE_FP32:
			fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
			fmt_size = 16;
			break;

		case SPVC_BASETYPE_INT16:
		case SPVC_BASETYPE_INT32:
			fmt = VK_FORMAT_R32G32B32A32_SINT;
			fmt_size = 16;
			break;

		case SPVC_BASETYPE_UINT16:
		case SPVC_BASETYPE_UINT32:
			fmt = VK_FORMAT_R32G32B32A32_UINT;
			fmt_size = 16;
			break;

		default:
			LOGE("Unrecognized attribute basetype.\n");
			return false;
		}

		for (uint32_t c = 0; c < col; c++)
		{
			attributes.push_back({ location + c, 0, fmt, stride });
			stride += fmt_size;
		}
	}
	return true;
}

static VkPipeline synthesize_graphics_pipeline(StateRecorder &recorder,
                                               const std::vector<uint8_t> *modules,
                                               spvc_compiler *compilers,
                                               VkPipelineLayout layout,
                                               VkRenderPass render_pass,
                                               uint8_t active_rt_mask,
                                               SpecConstant &specs)
{
	std::vector<VkPipelineShaderStageCreateInfo> stages;
	stages.reserve(STAGE_COUNT - 1);

	VkSpecializationInfo spec_info = {};

	spec_info.dataSize = specs.data.size() * sizeof(uint32_t);
	spec_info.pData = specs.data.data();
	spec_info.mapEntryCount = specs.map_entries.size();
	spec_info.pMapEntries = specs.map_entries.data();

	for (unsigned i = 0; i <= STAGE_FRAG; i++)
	{
		if (!modules[i].empty())
		{
			VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			module_info.codeSize = modules[i].size();
			module_info.pCode = reinterpret_cast<const uint32_t *>(modules[i].data());
			if (!recorder.record_shader_module((VkShaderModule)uint64_t(1 + i), module_info))
				return VK_NULL_HANDLE;

			VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			stage.module = (VkShaderModule)uint64_t(1 + i);
			stage.pName = "main";
			stage.stage = to_vk_shader_stage[i];
			if (spec_info.dataSize != 0)
				stage.pSpecializationInfo = &spec_info;
			stages.push_back(stage);
		}
	}

	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	const VkRect2D sci = {{0, 0}, {1024, 1024}};
	const VkViewport viewport = {0, 0, 1024, 1024, 0, 1};
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	{
		vp.scissorCount = 1;
		vp.viewportCount = 1;
		vp.pScissors = &sci;
		vp.pViewports = &viewport;
		info.pViewportState = &vp;
	}

	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	const VkSampleMask sample_mask = 0xffffffffu;
	{
		ms.pSampleMask = &sample_mask;
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		info.pMultisampleState = &ms;
	}

	VkPipelineTessellationStateCreateInfo tess = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
	{
		tess.patchControlPoints = 1;
		info.pTessellationState = &tess;
	}

	VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	{
		info.pDynamicState = &dyn;
	}

	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState attachments[8] = {};
	{
		for (unsigned i = 0; i < 8; i++)
		{
			if (active_rt_mask & (1u << i))
			{
				blend.attachmentCount = i + 1;
				attachments[i].colorWriteMask = 0xf;
			}
		}
		blend.pAttachments = attachments;
		info.pColorBlendState = &blend;
	}

	std::vector<VkVertexInputAttributeDescription> attributes;
	VkVertexInputBindingDescription vertex_binding = {};

	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	if (compilers[STAGE_VERT])
	{
		if (!append_attributes(compilers[STAGE_VERT], attributes, vertex_binding.stride))
			return VK_NULL_HANDLE;

		vi.vertexBindingDescriptionCount = 1;
		vi.pVertexBindingDescriptions = &vertex_binding;
		vi.vertexAttributeDescriptionCount = uint32_t(attributes.size());
		vi.pVertexAttributeDescriptions = attributes.data();
		info.pVertexInputState = &vi;
	}

	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	if (compilers[STAGE_VERT])
	{
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		info.pInputAssemblyState = &ia;
	}

	VkPipelineRasterizationStateCreateInfo ras = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	{
		ras.cullMode = VK_CULL_MODE_NONE;
		ras.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		ras.polygonMode = VK_POLYGON_MODE_FILL;
		info.pRasterizationState = &ras;
	}

	info.stageCount = uint32_t(stages.size());
	info.pStages = stages.data();
	info.layout = layout;
	info.renderPass = render_pass;

	if (!recorder.record_graphics_pipeline((VkPipeline)uint64_t(1), info, nullptr, 0))
		return VK_NULL_HANDLE;

	if (specs.iteration.count && specs.iteration.index < specs.data.size())
	{
		for (uint32_t i = 0; i < specs.iteration.count; i++)
		{
			specs.data[specs.iteration.index] = i;
			if (!recorder.record_graphics_pipeline((VkPipeline) uint64_t(2 + i), info, nullptr, 0))
				return VK_NULL_HANDLE;
		}
	}

	return (VkPipeline)uint64_t(1);
}

int main(int argc, char *argv[])
{
	CLICallbacks cbs;
	std::string spv_paths[STAGE_COUNT];
	std::string output_path;
	SpecConstant spec_constants;
	uint32_t view_count = 0;

	cbs.add("--vert", [&](CLIParser &parser) { spv_paths[STAGE_VERT] = parser.next_string(); });
	cbs.add("--tesc", [&](CLIParser &parser) { spv_paths[STAGE_TESC] = parser.next_string(); });
	cbs.add("--tese", [&](CLIParser &parser) { spv_paths[STAGE_TESE] = parser.next_string(); });
	cbs.add("--geom", [&](CLIParser &parser) { spv_paths[STAGE_GEOM] = parser.next_string(); });
	cbs.add("--frag", [&](CLIParser &parser) { spv_paths[STAGE_FRAG] = parser.next_string(); });
	cbs.add("--comp", [&](CLIParser &parser) { spv_paths[STAGE_COMP] = parser.next_string(); });
	cbs.add("--task", [&](CLIParser &parser) { spv_paths[STAGE_TASK] = parser.next_string(); });
	cbs.add("--mesh", [&](CLIParser &parser) { spv_paths[STAGE_MESH] = parser.next_string(); });
	cbs.add("--output", [&](CLIParser &parser) { output_path = parser.next_string(); });
	cbs.add("--help", [&](CLIParser &parser) { parser.end(); });
	cbs.add("--spec", [&](CLIParser &parser) {
		VkSpecializationMapEntry map_entry = {};
		map_entry.size = sizeof(uint32_t);
		map_entry.offset = spec_constants.data.size() * sizeof(uint32_t);
		map_entry.constantID = parser.next_uint();
		spec_constants.map_entries.push_back(map_entry);

		uint32_t raw_data;

		const char *type = parser.next_string();
		if (strcmp(type, "f32") == 0)
		{
			auto f = float(parser.next_double());
			memcpy(&raw_data, &f, sizeof(uint32_t));
		}
		else if (strcmp(type, "u32") == 0)
			raw_data = parser.next_uint();
		else if (strcmp(type, "i32") == 0)
			raw_data = parser.next_sint();
		else
		{
			LOGE("Invalid spec constant type.\n");
			print_help();
			exit(EXIT_FAILURE);
		}

		spec_constants.data.push_back(raw_data);
	});
	cbs.add("--multi-spec", [&](CLIParser &parser) {
		uint32_t index = parser.next_uint();
		uint32_t count = parser.next_uint();
		spec_constants.iteration = { index, count };
	});
	cbs.add("--multiview", [&](CLIParser &parser) {
		view_count = parser.next_uint();
	});

	CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
	{
		print_help();
		return EXIT_FAILURE;
	}
	else if (parser.is_ended_state())
	{
		print_help();
		return EXIT_SUCCESS;
	}

	if (output_path.empty())
	{
		LOGE("Need to provide an output path.\n");
		print_help();
		return EXIT_FAILURE;
	}

	auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(output_path.c_str(), DatabaseMode::OverWrite));
	if (!db->prepare())
	{
		LOGE("Failed to prepare output archive.\n");
		return EXIT_FAILURE;
	}

	StateRecorder recorder;

	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.pApplicationName = "fossilize-synth";
	app_info.apiVersion = VK_API_VERSION_1_3;

	if (!recorder.record_application_info(app_info))
		return EXIT_FAILURE;

	// Make sure we record mesh shader features so replayer can pick it up if applicable.
	VkPhysicalDeviceFeatures2 features2 =
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features =
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
	mesh_features.taskShader = VK_TRUE;
	mesh_features.meshShader = VK_TRUE;
	features2.pNext = &mesh_features;
	if (!recorder.record_physical_device_features(&features2))
		return EXIT_FAILURE;

	recorder.init_recording_thread(db.get());

	spvc_context context;
	if (spvc_context_create(&context) != SPVC_SUCCESS)
		return EXIT_FAILURE;

	struct ContextDeleter { void operator()(spvc_context ctx) { spvc_context_destroy(ctx); }};
	std::unique_ptr<spvc_context_s, ContextDeleter> holder(context);

	std::vector<uint8_t> modules[STAGE_COUNT];
	spvc_compiler compilers[STAGE_COUNT] = {};
	if (!load_shader_modules(spv_paths, modules))
		return EXIT_FAILURE;
	if (!reflect_shader_modules(context, modules, compilers))
		return EXIT_FAILURE;

	VkPipelineLayout layout = synthesize_pipeline_layout(recorder, compilers);
	if (layout == VK_NULL_HANDLE)
		return EXIT_FAILURE;

	VkRenderPass render_pass = VK_NULL_HANDLE;
	uint8_t active_rt_mask = 0;
	if (compilers[STAGE_FRAG])
	{
		render_pass = synthesize_render_pass(recorder, compilers[STAGE_FRAG], view_count, active_rt_mask);
		if (render_pass == VK_NULL_HANDLE)
			return EXIT_FAILURE;
	}

	VkPipeline pipeline;
	if (compilers[STAGE_COMP])
		pipeline = synthesize_compute_pipeline(recorder, modules, layout, spec_constants);
	else
		pipeline = synthesize_graphics_pipeline(recorder, modules, compilers, layout, render_pass, active_rt_mask, spec_constants);

	if (pipeline == VK_NULL_HANDLE)
		return EXIT_FAILURE;

	recorder.tear_down_recording_thread();
	LOGI("Successfully synthesized a FOZ archive to %s.\n", output_path.c_str());
}
