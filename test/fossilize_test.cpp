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

#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "fossilize_external_replayer.hpp"
#include <string.h>
#include <memory>
#include <vector>
#include <string>
#include "layer/utils.hpp"
#include "fossilize_errors.hpp"

using namespace Fossilize;

template <typename T>
static inline T fake_handle(uint64_t value)
{
	static_assert(sizeof(T) == sizeof(uint64_t), "Handle size is not 64-bit.");
	// reinterpret_cast does not work reliably on MSVC 2013 for Vulkan objects.
	return (T)value;
}

struct ReplayInterface : StateCreatorInterface
{
	StateRecorder recorder;
	Hash feature_hash = 0;

	ReplayInterface()
	{
	}

	void set_application_info(Hash hash, const VkApplicationInfo *info, const VkPhysicalDeviceFeatures2 *features) override
	{
		feature_hash = hash;

		if (info)
			if (!recorder.record_application_info(*info))
				abort();
		if (features)
			if (!recorder.record_physical_device_features(*features))
				abort();
	}

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_sampler(*create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*sampler = fake_handle<VkSampler>(hash);
		return recorder.record_sampler(*sampler, *create_info);
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_descriptor_set_layout(recorder, *create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		return recorder.record_descriptor_set_layout(*layout, *create_info);
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_pipeline_layout(recorder, *create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*layout = fake_handle<VkPipelineLayout>(hash);
		return recorder.record_pipeline_layout(*layout, *create_info);
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_shader_module(*create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*module = fake_handle<VkShaderModule>(hash);
		return recorder.record_shader_module(*module, *create_info);
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_render_pass(*create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*render_pass = fake_handle<VkRenderPass>(hash);
		return recorder.record_render_pass(*render_pass, *create_info);
	}

	bool enqueue_create_render_pass2(Hash hash, const VkRenderPassCreateInfo2 *create_info, VkRenderPass *render_pass) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_render_pass2(*create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*render_pass = fake_handle<VkRenderPass>(hash);
		return recorder.record_render_pass2(*render_pass, *create_info);
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_compute_pipeline(recorder, *create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*pipeline = fake_handle<VkPipeline>(hash);
		return recorder.record_compute_pipeline(*pipeline, *create_info, nullptr, 0);
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, *create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*pipeline = fake_handle<VkPipeline>(hash);
		return recorder.record_graphics_pipeline(*pipeline, *create_info, nullptr, 0);
	}
};

static void record_samplers(StateRecorder &recorder)
{
	VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	sampler.unnormalizedCoordinates = VK_TRUE;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
	sampler.anisotropyEnable = VK_FALSE;
	sampler.maxAnisotropy = 30.0f;
	sampler.compareOp = VK_COMPARE_OP_EQUAL;
	sampler.compareEnable = VK_TRUE;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler.mipLodBias = 90.0f;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minLod = 10.0f;
	sampler.maxLod = 20.0f;
	if (!recorder.record_sampler(fake_handle<VkSampler>(100), sampler))
		abort();
	sampler.minLod = 11.0f;
	if (!recorder.record_sampler(fake_handle<VkSampler>(101), sampler))
		abort();

	// Intentionally trip an error.
	VkSamplerYcbcrConversionCreateInfo ycbcr = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
	VkSamplerYcbcrConversionCreateInfo reduction = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT};
	sampler.pNext = &ycbcr;
	ycbcr.pNext = &reduction;
	bool ret = recorder.record_sampler(fake_handle<VkSampler>(102), sampler);
	if (ret)
	{
		// Should not reach here.
		exit(1);
	}
	else
	{
		LOGE("=== Tripped intentional error for testing ===\n");
	}
}

static void record_set_layouts(StateRecorder &recorder)
{
	VkDescriptorSetLayoutBinding bindings[3] = {};
	const VkSampler immutable_samplers[] = {
		fake_handle<VkSampler>(101),
		fake_handle<VkSampler>(100),
	};

	VkMutableDescriptorTypeListVALVE mutable_lists[3] = {};

	static const VkDescriptorType mutable_lists0[] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
	static const VkDescriptorType mutable_lists1[] = { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER };

	VkMutableDescriptorTypeCreateInfoVALVE mutable_info = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_VALVE };
	mutable_info.mutableDescriptorTypeListCount = 3;
	mutable_info.pMutableDescriptorTypeLists = mutable_lists;
	mutable_lists[0].descriptorTypeCount = sizeof(mutable_lists0) / sizeof(*mutable_lists0);
	mutable_lists[0].pDescriptorTypes = mutable_lists0;
	mutable_lists[1].descriptorTypeCount = sizeof(mutable_lists1) / sizeof(*mutable_lists1);
	mutable_lists[1].pDescriptorTypes = mutable_lists1;

	VkDescriptorSetLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layout.bindingCount = 3;
	layout.pBindings = bindings;
	bindings[0].binding = 8;
	bindings[0].descriptorCount = 2;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[0].pImmutableSamplers = immutable_samplers;

	bindings[1].binding = 9;
	bindings[1].descriptorCount = 5;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	bindings[2].binding = 2;
	bindings[2].descriptorCount = 3;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkDescriptorSetLayoutBindingFlagsCreateInfoEXT flags = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT };
	static const VkDescriptorBindingFlagsEXT binding_flags[] = {
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT_EXT,
		VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT_EXT,
		VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT,
	};
	flags.pBindingFlags = binding_flags;
	flags.bindingCount = 3;
	layout.pNext = &flags;

	flags.pNext = &mutable_info;

	if (!recorder.record_descriptor_set_layout(fake_handle<VkDescriptorSetLayout>(1000), layout))
		abort();

	layout.bindingCount = 2;
	layout.pBindings = bindings + 1;
	flags.bindingCount = 0;
	if (!recorder.record_descriptor_set_layout(fake_handle<VkDescriptorSetLayout>(1001), layout))
		abort();
}

static void record_pipeline_layouts(StateRecorder &recorder)
{
	const VkDescriptorSetLayout set_layouts0[2] = {
		fake_handle<VkDescriptorSetLayout>(1000),
		fake_handle<VkDescriptorSetLayout>(1001),
	};
	const VkDescriptorSetLayout set_layouts1[2] = {
		fake_handle<VkDescriptorSetLayout>(1001),
		fake_handle<VkDescriptorSetLayout>(1000),
	};

	VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout.pSetLayouts = set_layouts0;
	layout.setLayoutCount = 2;

	static const VkPushConstantRange ranges[2] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, 16 },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, 16, 32 },
	};
	layout.pushConstantRangeCount = 2;
	layout.pPushConstantRanges = ranges;
	if (!recorder.record_pipeline_layout(fake_handle<VkPipelineLayout>(10000), layout))
		abort();

	VkPipelineLayoutCreateInfo layout2 = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	if (!recorder.record_pipeline_layout(fake_handle<VkPipelineLayout>(10001), layout2))
		abort();

	VkPipelineLayoutCreateInfo layout3 = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout3.setLayoutCount = 2;
	layout3.pSetLayouts = set_layouts1;
	if (!recorder.record_pipeline_layout(fake_handle<VkPipelineLayout>(10002), layout3))
		abort();
}

static void record_shader_modules(StateRecorder &recorder)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	static const uint32_t code[] = { 0xdeadbeef, 0xcafebabe };
	info.pCode = code;
	info.codeSize = sizeof(code);

	if (!recorder.record_shader_module(fake_handle<VkShaderModule>(5000), info))
		abort();

	static const uint32_t code2[] = { 0xabba1337, 0xbabba100, 0xdeadbeef, 0xcafebabe };
	info.pCode = code2;
	info.codeSize = sizeof(code2);
	if (!recorder.record_shader_module(fake_handle<VkShaderModule>(5001), info))
		abort();
}

static void record_render_passes2(StateRecorder &recorder)
{
	VkRenderPassCreateInfo2 pass = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 };

	VkSubpassDependency2 deps[2] = {};
	VkSubpassDescription2 subpasses[2] = {};
	VkAttachmentDescription2 att[2] = {};

	const VkAttachmentReference2 attachment_ref_shading_rate = {
		VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr,
		4, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR, 0
	};

	VkAttachmentDescriptionStencilLayoutKHR attachment_desc_stencil_layout =
			{ VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT };
	attachment_desc_stencil_layout.stencilInitialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment_desc_stencil_layout.stencilFinalLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReferenceStencilLayout attachment_ref_stencil_layout =
			{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT };
	attachment_ref_stencil_layout.stencilLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;

	const VkAttachmentReference2 ds_resolve_ref = {
		VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, &attachment_ref_stencil_layout,
		3, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0,
	};

	VkSubpassDescriptionDepthStencilResolve ds_resolve =
			{ VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE };
	ds_resolve.depthResolveMode = VK_RESOLVE_MODE_MAX_BIT;
	ds_resolve.stencilResolveMode = VK_RESOLVE_MODE_MIN_BIT;
	ds_resolve.pDepthStencilResolveAttachment = &ds_resolve_ref;

	VkFragmentShadingRateAttachmentInfoKHR shading_rate_info =
			{ VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR };
	shading_rate_info.pFragmentShadingRateAttachment = &attachment_ref_shading_rate;
	shading_rate_info.shadingRateAttachmentTexelSize.width = 8;
	shading_rate_info.shadingRateAttachmentTexelSize.height = 16;
	ds_resolve.pNext = &shading_rate_info;

	static const uint32_t correlated_view_masks[] = { 1, 4, 2 };
	pass.correlatedViewMaskCount = 3;
	pass.pCorrelatedViewMasks = correlated_view_masks;

	pass.flags = 10;
	pass.attachmentCount = 2;
	pass.pAttachments = att;
	pass.dependencyCount = 2;
	pass.pDependencies = deps;
	pass.subpassCount = 2;
	pass.pSubpasses = subpasses;

	deps[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
	deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	deps[0].dstAccessMask = 49;
	deps[0].srcAccessMask = 34;
	deps[0].dstStageMask = 199;
	deps[0].srcStageMask = 10;
	deps[0].srcSubpass = 9;
	deps[0].dstSubpass = 19;
	deps[0].viewOffset = -4;
	deps[1].dependencyFlags = 19;
	deps[1].dstAccessMask = 490;
	deps[1].srcAccessMask = 340;
	deps[1].dstStageMask = 1990;
	deps[1].srcStageMask = 100;
	deps[1].srcSubpass = 90;
	deps[1].dstSubpass = 190;
	deps[1].viewOffset = 6;

	att[0].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
	att[0].flags = 40;
	att[0].format = VK_FORMAT_R16G16_SFLOAT;
	att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].samples = VK_SAMPLE_COUNT_16_BIT;

	att[1] = att[0];
	att[1].format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	att[1].pNext = &attachment_desc_stencil_layout;

	static const uint32_t preserves[4] = { 9, 4, 2, 3 };
	static const VkAttachmentReference2 inputs[2] = {
		{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 5 },
		{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 9, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 6 }
	};
	static const VkAttachmentReference2 colors[2] = {
		{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 8, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 10 },
		{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 13 }
	};
	static const VkAttachmentReference2 resolves[2] = {
		{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 20 },
		{ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 30 }
	};
	static const VkAttachmentReference2 ds = {
		VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, &attachment_ref_stencil_layout, 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 40
	};
	subpasses[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
	subpasses[0].preserveAttachmentCount = 4;
	subpasses[0].pPreserveAttachments = preserves;
	subpasses[0].inputAttachmentCount = 2;
	subpasses[0].pInputAttachments = inputs;
	subpasses[0].colorAttachmentCount = 2;
	subpasses[0].pColorAttachments = colors;
	subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
	subpasses[0].pDepthStencilAttachment = &ds;
	subpasses[0].pResolveAttachments = resolves;
	subpasses[0].viewMask = 0xf;

	subpasses[1].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
	subpasses[1].inputAttachmentCount = 1;
	subpasses[1].pInputAttachments = inputs;
	subpasses[1].colorAttachmentCount = 2;
	subpasses[1].pColorAttachments = colors;
	subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpasses[1].viewMask = 0x7;
	subpasses[1].pNext = &ds_resolve;

	if (!recorder.record_render_pass2(fake_handle<VkRenderPass>(40000), pass))
		abort();
}

static void record_render_passes(StateRecorder &recorder)
{
	VkRenderPassCreateInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	VkSubpassDependency deps[2] = {};
	VkSubpassDescription subpasses[2] = {};
	VkAttachmentDescription att[2] = {};

	pass.flags = 8;

	deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	deps[0].dstAccessMask = 49;
	deps[0].srcAccessMask = 34;
	deps[0].dstStageMask = 199;
	deps[0].srcStageMask = 10;
	deps[0].srcSubpass = 9;
	deps[0].dstSubpass = 19;
	deps[1].dependencyFlags = 19;
	deps[1].dstAccessMask = 490;
	deps[1].srcAccessMask = 340;
	deps[1].dstStageMask = 1990;
	deps[1].srcStageMask = 100;
	deps[1].srcSubpass = 90;
	deps[1].dstSubpass = 190;

	att[0].flags = 40;
	att[0].format = VK_FORMAT_R16G16_SFLOAT;
	att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].samples = VK_SAMPLE_COUNT_16_BIT;

	static const uint32_t preserves[4] = { 9, 4, 2, 3 };
	static const VkAttachmentReference inputs[2] = { { 3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { 9, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
	static const VkAttachmentReference colors[2] = { { 8, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
	static const VkAttachmentReference resolves[2] = { { 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
	static const VkAttachmentReference ds = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	subpasses[0].preserveAttachmentCount = 4;
	subpasses[0].pPreserveAttachments = preserves;
	subpasses[0].inputAttachmentCount = 2;
	subpasses[0].pInputAttachments = inputs;
	subpasses[0].colorAttachmentCount = 2;
	subpasses[0].pColorAttachments = colors;
	subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
	subpasses[0].pDepthStencilAttachment = &ds;
	subpasses[0].pResolveAttachments = resolves;

	subpasses[1].inputAttachmentCount = 1;
	subpasses[1].pInputAttachments = inputs;
	subpasses[1].colorAttachmentCount = 2;
	subpasses[1].pColorAttachments = colors;
	subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

	pass.attachmentCount = 2;
	pass.pAttachments = att;
	pass.subpassCount = 1;
	pass.pSubpasses = subpasses;
	pass.dependencyCount = 0;
	pass.pDependencies = deps;

	VkRenderPassMultiviewCreateInfo multiview = { VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO };
	multiview.subpassCount = 3;
	static const uint32_t view_masks[3] = { 2, 4, 5 };
	multiview.pViewMasks = view_masks;
	multiview.dependencyCount = 2;
	static const int32_t view_offsets[2] = { -2, 1 };
	multiview.pViewOffsets = view_offsets;
	multiview.correlationMaskCount = 4;
	static const uint32_t correlation_masks[4] = { 1, 2, 3, 4 };
	multiview.pCorrelationMasks = correlation_masks;
	pass.pNext = &multiview;

	if (!recorder.record_render_pass(fake_handle<VkRenderPass>(30000), pass))
		abort();

	pass.dependencyCount = 0;
	VkRenderPassMultiviewCreateInfo blank_multiview = { VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO };
	pass.pNext = &blank_multiview;
	if (!recorder.record_render_pass(fake_handle<VkRenderPass>(30001), pass))
		abort();
}

static void record_compute_pipelines(StateRecorder &recorder)
{
	VkComputePipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipe.stage.module = fake_handle<VkShaderModule>(5000);
	pipe.stage.pName = "main";

	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required_size =
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT };
	required_size.requiredSubgroupSize = 64;
	pipe.stage.pNext = &required_size;

	VkSpecializationInfo spec = {};
	spec.dataSize = 16;
	static const float data[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
	spec.pData = data;
	spec.mapEntryCount = 2;
	static const VkSpecializationMapEntry entries[2] = {
		{ 0, 4, 8 },
		{ 4, 4, 16 },
	};
	spec.pMapEntries = entries;
	pipe.stage.pSpecializationInfo = &spec;
	pipe.layout = fake_handle<VkPipelineLayout>(10001);

	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(80000), pipe, nullptr, 0))
		abort();

	//pipe.basePipelineHandle = fake_handle<VkPipeline>(80000);
	pipe.basePipelineIndex = 10;
	pipe.stage.pSpecializationInfo = nullptr;
	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(80001), pipe, nullptr, 0))
		abort();
}

static void record_graphics_pipelines(StateRecorder &recorder)
{
	VkSpecializationInfo spec = {};
	spec.dataSize = 16;
	static const float data[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
	spec.pData = data;
	spec.mapEntryCount = 2;
	static const VkSpecializationMapEntry entries[2] = {
		{ 0, 4, 8 },
		{ 4, 4, 16 },
	};
	spec.pMapEntries = entries;

	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.layout = fake_handle<VkPipelineLayout>(10002);
	pipe.subpass = 1;
	pipe.renderPass = fake_handle<VkRenderPass>(30001);
	pipe.stageCount = 2;
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].pName = "vert";
	stages[0].module = fake_handle<VkShaderModule>(5000);
	stages[0].pSpecializationInfo = &spec;
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].pName = "frag";
	stages[1].module = fake_handle<VkShaderModule>(5001);
	stages[1].pSpecializationInfo = &spec;

	VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT required_size =
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT };
	required_size.requiredSubgroupSize = 16;
	stages[1].pNext = &required_size;

	pipe.pStages = stages;

	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineTessellationStateCreateInfo tess = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };

	VkPipelineVertexInputDivisorStateCreateInfoEXT divisor = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT };
	VkPipelineVertexInputDivisorStateCreateInfoEXT divisor2 = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT };
	divisor.vertexBindingDivisorCount = 2;
	divisor2.vertexBindingDivisorCount = 1;

	VkVertexInputBindingDivisorDescriptionEXT divisor_descs[2] = {};
	divisor_descs[0].binding = 0;
	divisor_descs[0].divisor = 1;
	divisor_descs[1].binding = 1;
	divisor_descs[1].divisor = 4;
	divisor.pVertexBindingDivisors = divisor_descs;
	divisor2.pVertexBindingDivisors = divisor_descs;
	vi.pNext = &divisor;
	divisor.pNext = &divisor2;

	VkPipelineColorBlendAdvancedStateCreateInfoEXT advanced = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT };
	advanced.blendOverlap = VK_BLEND_OVERLAP_CONJOINT_EXT;
	advanced.srcPremultiplied = VK_TRUE;
	advanced.dstPremultiplied = VK_TRUE;
	blend.pNext = &advanced;

	static const VkVertexInputAttributeDescription attrs[2] = {
		{ 2, 1, VK_FORMAT_R16G16_SFLOAT, 5 },
		{ 9, 1, VK_FORMAT_R8_UINT, 5 },
	};
	static const VkVertexInputBindingDescription binds[2] = {
		{ 8, 1, VK_VERTEX_INPUT_RATE_INSTANCE },
		{ 9, 6, VK_VERTEX_INPUT_RATE_VERTEX },
	};
	vi.vertexBindingDescriptionCount = 2;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexBindingDescriptions = binds;
	vi.pVertexAttributeDescriptions = attrs;

	ms.rasterizationSamples = VK_SAMPLE_COUNT_16_BIT;
	ms.sampleShadingEnable = VK_TRUE;
	ms.minSampleShading = 0.5f;
	ms.alphaToCoverageEnable = VK_TRUE;
	ms.alphaToOneEnable = VK_TRUE;
	static const uint32_t mask = 0xf;
	ms.pSampleMask = &mask;

	static const VkDynamicState dyn_states[3] = {
			VK_DYNAMIC_STATE_BLEND_CONSTANTS,
			VK_DYNAMIC_STATE_DEPTH_BIAS,
			VK_DYNAMIC_STATE_LINE_WIDTH
	};
	dyn.dynamicStateCount = 3;
	dyn.pDynamicStates = dyn_states;

	static const VkViewport vps[2] = {
		{ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f },
		{ 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f },
	};
	static const VkRect2D sci[2] = {
		{ { 3, 4 }, { 8, 9 }},
		{ { 13, 14 }, { 18, 19 }},
	};
	vp.viewportCount = 2;
	vp.scissorCount = 2;
	vp.pViewports = vps;
	vp.pScissors = sci;

	static const VkPipelineColorBlendAttachmentState blend_attachments[2] = {
			{ VK_TRUE,
					VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_OP_ADD,
					VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_OP_SUBTRACT,
					0xf },
			{ VK_TRUE,
					VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_OP_ADD,
					VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_OP_SUBTRACT,
					0x3 },
	};
	blend.logicOpEnable = VK_TRUE;
	blend.logicOp = VK_LOGIC_OP_AND_INVERTED;
	blend.blendConstants[0] = 9.0f;
	blend.blendConstants[1] = 19.0f;
	blend.blendConstants[2] = 29.0f;
	blend.blendConstants[3] = 39.0f;
	blend.attachmentCount = 2;
	blend.pAttachments = blend_attachments;

	tess.patchControlPoints = 9;
	VkPipelineTessellationDomainOriginStateCreateInfo domain =
			{ VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO };
	domain.domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT;
	tess.pNext = &domain;

	ds.front.compareOp = VK_COMPARE_OP_GREATER;
	ds.front.writeMask = 9;
	ds.front.reference = 10;
	ds.front.failOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	ds.front.depthFailOp = VK_STENCIL_OP_INVERT;
	ds.front.compareMask = 19;
	ds.front.passOp = VK_STENCIL_OP_REPLACE;
	ds.back.compareOp = VK_COMPARE_OP_LESS;
	ds.back.writeMask = 79;
	ds.back.reference = 80;
	ds.back.failOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
	ds.back.depthFailOp = VK_STENCIL_OP_ZERO;
	ds.back.compareMask = 29;
	ds.back.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	ds.stencilTestEnable = VK_TRUE;
	ds.minDepthBounds = 0.1f;
	ds.maxDepthBounds = 0.2f;
	ds.depthCompareOp = VK_COMPARE_OP_EQUAL;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthTestEnable = VK_TRUE;
	ds.depthBoundsTestEnable = VK_TRUE;

	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.polygonMode = VK_POLYGON_MODE_LINE;
	rs.depthClampEnable = VK_TRUE;
	rs.depthBiasEnable = VK_TRUE;
	rs.depthBiasSlopeFactor = 0.3f;
	rs.depthBiasConstantFactor = 0.8f;
	rs.depthBiasClamp = 0.5f;
	rs.rasterizerDiscardEnable = VK_TRUE;
	rs.lineWidth = 0.1f;
	rs.cullMode = VK_CULL_MODE_FRONT_AND_BACK;

	VkPipelineRasterizationDepthClipStateCreateInfoEXT clip_state =
			{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
	clip_state.depthClipEnable = VK_TRUE;
	rs.pNext = &clip_state;

	VkPipelineRasterizationStateStreamCreateInfoEXT stream_state =
			{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT };
	stream_state.rasterizationStream = VK_TRUE;
	clip_state.pNext = &stream_state;

	VkPipelineRasterizationConservativeStateCreateInfoEXT conservative_state =
			{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT };
	conservative_state.flags = 0;
	conservative_state.extraPrimitiveOverestimationSize = 2.5f;
	conservative_state.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
	stream_state.pNext = &conservative_state;

	VkPipelineRasterizationLineStateCreateInfoEXT line_state =
			{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT };
	line_state.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT;
	line_state.lineStippleFactor = 2;
	line_state.lineStipplePattern = 3;
	line_state.stippledLineEnable = VK_TRUE;
	conservative_state.pNext = &line_state;

	ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	ia.primitiveRestartEnable = VK_TRUE;

	pipe.pVertexInputState = &vi;
	pipe.pMultisampleState = &ms;
	pipe.pDynamicState = &dyn;
	pipe.pViewportState = &vp;
	pipe.pColorBlendState = &blend;
	pipe.pTessellationState = &tess;
	pipe.pDepthStencilState = &ds;
	pipe.pRasterizationState = &rs;
	pipe.pInputAssemblyState = &ia;

	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100000), pipe, nullptr, 0))
		abort();

	vp.viewportCount = 0;
	vp.scissorCount = 0;
	pipe.basePipelineHandle = fake_handle<VkPipeline>(100000);
	pipe.basePipelineIndex = 200;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100001), pipe, nullptr, 0))
		abort();
}

static bool test_database()
{
	remove(".__test_tmp.foz");
	remove(".__test_tmp_copy.foz");

	// Try clean write.
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_tmp.foz", DatabaseMode::OverWrite));
		if (!db->prepare())
			return false;

		static const uint8_t entry1[] = { 1, 2, 3 };

		if (!db->write_entry(RESOURCE_SAMPLER, 1, entry1, sizeof(entry1),
				PAYLOAD_WRITE_COMPRESS_BIT | PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT))
			return false;

		static const uint8_t entry2[] = { 10, 20, 30, 40, 50 };
		if (!db->write_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, 2, entry2, sizeof(entry2),
				PAYLOAD_WRITE_COMPRESS_BIT | PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT))
			return false;
	}

	// Try appending now.
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_tmp.foz", DatabaseMode::Append));
		if (!db->prepare())
			return false;

		// Check that has_entry behaves.
		if (!db->has_entry(RESOURCE_SAMPLER, 1))
			return false;
		if (!db->has_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, 2))
			return false;
		if (db->has_entry(RESOURCE_SHADER_MODULE, 3))
			return false;

		static const uint8_t entry3[] = { 1, 2, 3, 1, 2, 3 };
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 3, entry3, sizeof(entry3), PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT))
			return false;
	}

	// Try to copy over raw blobs to a new archive.
	{
		auto db_target = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_tmp_copy.foz", DatabaseMode::OverWrite));
		auto db_source = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_tmp.foz", DatabaseMode::ReadOnly));

		if (!db_target->prepare())
			return false;
		if (!db_source->prepare())
			return false;

		for (unsigned i = 0; i < RESOURCE_COUNT; i++)
		{
			auto tag = static_cast<ResourceTag>(i);

			size_t hash_count = 0;
			if (!db_source->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
				return false;
			std::vector<Hash> hashes(hash_count);
			if (!db_source->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
				return false;

			for (auto &hash : hashes)
			{
				size_t blob_size = 0;
				if (!db_source->read_entry(tag, hash, &blob_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
					return false;
				std::vector<uint8_t> blob(blob_size);
				if (!db_source->read_entry(tag, hash, &blob_size, blob.data(), PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
					return false;
				if (!db_target->write_entry(tag, hash, blob.data(), blob.size(), PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT))
					return false;
			}
		}
	}

	// Try playback multiple times.
	for (unsigned iter = 0; iter < 2; iter++)
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_tmp_copy.foz", DatabaseMode::ReadOnly));
		if (!db->prepare())
			return false;

		const auto compare = [](const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) -> bool {
			if (a.size() != b.size())
				return false;
			return memcmp(a.data(), b.data(), a.size()) == 0;
		};

		if (!db->has_entry(RESOURCE_SAMPLER, 1))
			return false;
		if (!db->has_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, 2))
			return false;
		if (!db->has_entry(RESOURCE_SHADER_MODULE, 3))
			return false;
		if (db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 3))
			return false;

		size_t blob_size;
		std::vector<uint8_t> blob;

		if (!db->read_entry(RESOURCE_SAMPLER, 1, &blob_size, nullptr, 0))
			return false;
		blob.resize(blob_size);
		if (!db->read_entry(RESOURCE_SAMPLER, 1, &blob_size, blob.data(), 0))
			return false;
		if (!compare(blob, { 1, 2, 3 }))
			return false;

		if (!db->read_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, 2, &blob_size, nullptr, 0))
			return false;
		blob.resize(blob_size);
		if (!db->read_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, 2, &blob_size, blob.data(), 0))
			return false;
		if (!compare(blob, { 10, 20, 30, 40, 50 }))
			return false;

		if (!db->read_entry(RESOURCE_SHADER_MODULE, 3, &blob_size, nullptr, 0))
			return false;
		blob.resize(blob_size);
		if (!db->read_entry(RESOURCE_SHADER_MODULE, 3, &blob_size, blob.data(), 0))
			return false;
		if (!compare(blob, { 1, 2, 3, 1, 2, 3 }))
			return false;
	}

	return true;
}

static bool file_exists(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (file)
	{
		fclose(file);
		return true;
	}
	else
		return false;
}

static bool test_concurrent_database_extra_paths()
{
	// Test a normal flow. First time we don't have the read-only database.
	// Next time we have one.
	remove(".__test_concurrent.foz");
	remove(".__test_concurrent.1.foz");
	remove(".__test_concurrent.2.foz");
	remove(".__test_concurrent.3.foz");
	remove(".__test_concurrent.4.foz");

	static const char *extra_paths = ".__test_concurrent.1.foz;.__test_concurrent.2.foz;.__test_concurrent.3.foz";
	static const uint8_t blob[] = {1, 2, 3};

	{
		auto db0 = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                         DatabaseMode::Append, nullptr, 0));
		if (!db0->prepare())
			return false;

		if (!db0->write_entry(RESOURCE_SAMPLER, 2, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db0->write_entry(RESOURCE_SAMPLER, 3, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;

		auto db1 = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                         DatabaseMode::Append, nullptr, 0));
		if (!db1->prepare())
			return false;

		if (!db1->write_entry(RESOURCE_SAMPLER, 3, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db1->write_entry(RESOURCE_SAMPLER, 4, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;

		auto db2 = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                         DatabaseMode::Append, nullptr, 0));
		if (!db2->prepare())
			return false;

		if (!db2->write_entry(RESOURCE_SAMPLER, 1, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db2->write_entry(RESOURCE_SAMPLER, 1, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
	}

	if (!file_exists(".__test_concurrent.1.foz"))
		return false;
	if (!file_exists(".__test_concurrent.2.foz"))
		return false;
	if (!file_exists(".__test_concurrent.3.foz"))
		return false;

	auto db = std::unique_ptr<DatabaseInterface>(
			create_concurrent_database_with_encoded_extra_paths(nullptr, DatabaseMode::ReadOnly, extra_paths));

	auto append_db = std::unique_ptr<DatabaseInterface>(
			create_concurrent_database_with_encoded_extra_paths(".__test_concurrent",
			                                                    DatabaseMode::Append, extra_paths));
	if (!db->prepare())
		return false;
	if (!append_db->prepare())
		return false;

	size_t num_samplers;
	if (!db->get_hash_list_for_resource_tag(RESOURCE_SAMPLER, &num_samplers, nullptr))
		return false;
	if (num_samplers != 4)
		return false;

	for (Hash i = 1; i <= 4; i++)
	{
		size_t blob_size;
		if (!db->read_entry(RESOURCE_SAMPLER, i, &blob_size, nullptr, 0))
			return false;
		if (blob_size != sizeof(blob))
			return false;
	}

	if (!append_db->write_entry(RESOURCE_SAMPLER, 4, blob, sizeof(blob), 0))
		return false;

	// This should not be written.
	if (file_exists(".__test_concurrent.4.foz"))
		return false;

	if (!append_db->write_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, 4, blob, sizeof(blob), 0))
		return false;

	// .. but now it should exist.
	if (!file_exists(".__test_concurrent.4.foz"))
		return false;

	return true;
}

static bool test_concurrent_database()
{
	// Test a normal flow. First time we don't have the read-only database.
	// Next time we have one.
	for (unsigned iter = 0; iter < 2; iter++)
	{
		if (iter == 0)
			remove(".__test_concurrent.foz");
		remove(".__test_concurrent.1.foz");
		remove(".__test_concurrent.2.foz");
		remove(".__test_concurrent.3.foz");

		static const uint8_t blob[] = {1, 2, 3};

		{
			{
				auto db0 = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
				                                                                         DatabaseMode::Append, nullptr, 0));
				if (!db0->prepare())
					return false;

				if (!db0->write_entry(RESOURCE_SAMPLER, 2, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
					return false;
				if (!db0->write_entry(RESOURCE_SAMPLER, 3, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
					return false;
			}

			{
				auto db1 = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
				                                                                         DatabaseMode::Append, nullptr, 0));
				if (!db1->prepare())
					return false;

				if (!db1->write_entry(RESOURCE_SAMPLER, 3, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
					return false;
				if (!db1->write_entry(RESOURCE_SAMPLER, 4, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
					return false;
			}

			{
				auto db2 = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
				                                                                         DatabaseMode::Append, nullptr, 0));
				if (!db2->prepare())
					return false;

				if (!db2->write_entry(RESOURCE_SAMPLER, 1, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
					return false;
				if (!db2->write_entry(RESOURCE_SAMPLER, 1, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
					return false;
			}
		}

		bool expected_exist = iter == 0;

		if (expected_exist != file_exists(".__test_concurrent.1.foz"))
			return false;
		if (expected_exist != file_exists(".__test_concurrent.2.foz"))
			return false;
		if (expected_exist != file_exists(".__test_concurrent.3.foz"))
			return false;

		static const char *append_paths[] = {
			".__test_concurrent.1.foz",
			".__test_concurrent.2.foz",
			".__test_concurrent.3.foz",
		};

		if (iter == 0)
		{
			if (!merge_concurrent_databases(".__test_concurrent.foz", append_paths, 3))
				return false;
		}
	}

	return true;
}

static bool test_implicit_whitelist()
{
	remove(".__test_concurrent.foz");
	remove(".__test_concurrent.1.foz");
	remove(".__test_concurrent.2.foz");
	remove(".__test_concurrent.3.foz");
	static const uint8_t blob[] = {1, 2, 3};

	{
		auto whitelist_db = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_concurrent_whitelist.foz", DatabaseMode::OverWrite));
		if (!whitelist_db || !whitelist_db->prepare())
			return false;

		if (!whitelist_db->write_entry(RESOURCE_SHADER_MODULE, 1, nullptr, 0, PAYLOAD_WRITE_NO_FLAGS))
			return false;
	}

	{
		auto db0 = std::unique_ptr<DatabaseInterface>(
				create_concurrent_database(".__test_concurrent",
				                           DatabaseMode::Append, nullptr, 0));
		auto db1 = std::unique_ptr<DatabaseInterface>(
				create_concurrent_database(".__test_concurrent",
				                           DatabaseMode::Append, nullptr, 0));
		auto db2 = std::unique_ptr<DatabaseInterface>(
				create_concurrent_database(".__test_concurrent",
				                           DatabaseMode::Append, nullptr, 0));

		if (!db0 || !db0->prepare())
			return false;
		if (!db1 || !db1->prepare())
			return false;
		if (!db2 || !db2->prepare())
			return false;

		if (!db0->write_entry(RESOURCE_SHADER_MODULE, 1, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db1->write_entry(RESOURCE_GRAPHICS_PIPELINE, 2, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db2->write_entry(RESOURCE_COMPUTE_PIPELINE, 3, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;

		if (!db0->write_entry(RESOURCE_SHADER_MODULE, 2, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db1->write_entry(RESOURCE_GRAPHICS_PIPELINE, 3, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db2->write_entry(RESOURCE_COMPUTE_PIPELINE, 4, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;

		// Should not return a database for Append mode.
		if (db0->get_sub_database(0))
			return false;
		if (db1->get_sub_database(0))
			return false;
		if (db2->get_sub_database(0))
			return false;
	}

	static const char *extra_paths[] = {
		".__test_concurrent.1.foz",
		".__test_concurrent.2.foz",
		".__test_concurrent.3.foz",
	};
	auto replay_db = std::unique_ptr<DatabaseInterface>(
			create_concurrent_database(nullptr, DatabaseMode::ReadOnly, extra_paths, 3));

	if (!replay_db->load_whitelist_database(".__test_concurrent_whitelist.foz"))
		return false;
	replay_db->promote_sub_database_to_whitelist(3);
	if (!replay_db || !replay_db->prepare())
		return false;

	// This should be whitelisted by primary whitelist.
	if (!replay_db->has_entry(RESOURCE_SHADER_MODULE, 1))
		return false;
	// Should not exist.
	if (replay_db->has_entry(RESOURCE_SHADER_MODULE, 2))
		return false;
	if (replay_db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 2))
		return false;
	if (replay_db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 3))
		return false;
	// Should be whitelisted by implicit whitelist.
	if (!replay_db->has_entry(RESOURCE_COMPUTE_PIPELINE, 3))
		return false;
	if (!replay_db->has_entry(RESOURCE_COMPUTE_PIPELINE, 4))
		return false;

	size_t size = 0;
	Hash hashes[3];

	if (!replay_db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &size, nullptr) || size != 1)
		return false;
	if (!replay_db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &size, hashes) || size != 1)
		return false;
	if (hashes[0] != 1)
		return false;

	if (!replay_db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &size, nullptr) || size != 0)
		return false;

	if (!replay_db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &size, nullptr) || size != 2)
		return false;
	if (!replay_db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &size, hashes) || size != 2)
		return false;
	if (hashes[0] != 3)
		return false;
	if (hashes[1] != 4)
		return false;

	// We have no primary database.
	if (replay_db->get_sub_database(0))
		return false;
	if (!replay_db->get_sub_database(1))
		return false;
	if (!replay_db->get_sub_database(2))
		return false;
	if (!replay_db->get_sub_database(3))
		return false;
	if (replay_db->get_sub_database(4))
		return false;

	replay_db.reset();
	remove(".__test_concurrent.1.foz");
	remove(".__test_concurrent.2.foz");
	remove(".__test_concurrent.3.foz");
	remove(".__test_concurrent_whitelist.foz");
	return true;
}

static bool test_filter()
{
	static const uint8_t blob[4] = { 1, 2, 3, 4 };

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_filter.foz",
		                                                                             DatabaseMode::OverWrite));
		if (!db->prepare())
			return false;

		if (!db->write_entry(RESOURCE_SHADER_MODULE, 10, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 11, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 12, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;

		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 10, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 11, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 12, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;

		if (!db->write_entry(RESOURCE_COMPUTE_PIPELINE, 10, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db->write_entry(RESOURCE_COMPUTE_PIPELINE, 11, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
		if (!db->write_entry(RESOURCE_COMPUTE_PIPELINE, 12, blob, sizeof(blob), PAYLOAD_WRITE_NO_FLAGS))
			return false;
	}

	{
		auto whitelist = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_whitelist.foz",
		                                                                                   DatabaseMode::OverWrite));
		auto blacklist = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_blacklist.foz",
		                                                                                   DatabaseMode::OverWrite));

		if (!whitelist->prepare())
			return false;
		if (!blacklist->prepare())
			return false;

		if (!whitelist->write_entry(RESOURCE_SHADER_MODULE, 10, nullptr, 0, 0))
			return false;
		if (!whitelist->write_entry(RESOURCE_SHADER_MODULE, 11, nullptr, 0, 0))
			return false;
		if (!whitelist->write_entry(RESOURCE_SHADER_MODULE, 12, nullptr, 0, 0))
			return false;

		if (!whitelist->write_entry(RESOURCE_GRAPHICS_PIPELINE, 11, nullptr, 0, 0))
			return false;
		if (!whitelist->write_entry(RESOURCE_GRAPHICS_PIPELINE, 12, nullptr, 0, 0))
			return false;
		if (!blacklist->write_entry(RESOURCE_GRAPHICS_PIPELINE, 10, nullptr, 0, 0))
			return false;

		if (!whitelist->write_entry(RESOURCE_COMPUTE_PIPELINE, 10, nullptr, 0, 0))
			return false;
		if (!whitelist->write_entry(RESOURCE_COMPUTE_PIPELINE, 12, nullptr, 0, 0))
			return false;
		if (!blacklist->write_entry(RESOURCE_COMPUTE_PIPELINE, 11, nullptr, 0, 0))
			return false;
	}

	for (unsigned i = 0; i < 3; i++)
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_filter.foz",
		                                                                            DatabaseMode::ReadOnly));
		switch (i)
		{
		case 0:
			if (!db->load_whitelist_database(i ? ".__test_blacklist.foz" : ".__test_whitelist.foz"))
				return false;
			break;

		case 1:
			if (!db->load_blacklist_database(".__test_blacklist.foz"))
				return false;
			break;

		case 2:
			if (!db->load_whitelist_database(".__test_whitelist.foz"))
				return false;
			if (!db->load_blacklist_database(".__test_blacklist.foz"))
				return false;
			break;

		default:
			return false;
		}

		if (!db->prepare())
			return false;

		size_t count;
		Hash hashes[3];

		// Test whitelist filtering of different types.
		if (!db->has_entry(RESOURCE_SHADER_MODULE, 10))
			return false;
		if (!db->has_entry(RESOURCE_SHADER_MODULE, 11))
			return false;
		if (!db->has_entry(RESOURCE_SHADER_MODULE, 12))
			return false;
		if (db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 10))
			return false;
		if (db->has_entry(RESOURCE_COMPUTE_PIPELINE, 11))
			return false;

		// Test get_hash_list.
		if (!db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &count, nullptr))
			return false;
		if (count != 3)
			return false;
		if (!db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &count, hashes))
			return false;

		static const Hash expected_0[3] = { 10, 11, 12 };
		if (memcmp(expected_0, hashes, sizeof(expected_0)) != 0)
			return false;

		if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &count, nullptr))
			return false;
		if (count != 2)
			return false;
		if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &count, hashes))
			return false;

		static const Hash expected_1[2] = { 11, 12 };
		if (memcmp(expected_1, hashes, sizeof(expected_1)) != 0)
			return false;

		if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &count, nullptr))
			return false;
		if (count != 2)
			return false;
		if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &count, hashes))
			return false;

		static const Hash expected_2[2] = { 10, 12 };
		if (memcmp(expected_2, hashes, sizeof(expected_2)) != 0)
			return false;
	}

	remove(".__test_filter.foz");
	remove(".__test_whitelist.foz");
	remove(".__test_blacklist.foz");
	return true;
}

static bool test_export_single_archive()
{
	static const uint16_t one = 1;
	static const uint32_t two = 2;
	static const uint64_t three = 3;
	char export_path[DatabaseInterface::OSHandleNameSize];
	DatabaseInterface::get_unique_os_export_name(export_path, sizeof(export_path));

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_archive.foz",
		                                                                            DatabaseMode::OverWrite));
		if (!db || !db->prepare())
			return false;

		if (!db->write_entry(RESOURCE_SHADER_MODULE, 1, &one, sizeof(one), 0))
			return false;
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 2, &two, sizeof(two), 0))
			return false;
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 3, &three, sizeof(three), 0))
			return false;

		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 300, &one, sizeof(one), 0))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 200, &two, sizeof(two), 0))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 100, &three, sizeof(three), 0))
			return false;
	}

	intptr_t handle;
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_archive.foz", DatabaseMode::ReadOnly));
		if (!db || !db->prepare())
			return false;
		handle = db->export_metadata_to_os_handle(export_path);
#ifdef __WIN32
		if (handle == 0)
			return false;
#else
		if (handle < 0)
			return false;
#endif
	}

	auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_archive.foz", DatabaseMode::ReadOnly));
	if (!db || !db->import_metadata_from_os_handle(handle) || !db->prepare())
		return false;

	size_t count;

	for (unsigned i = 0; i < RESOURCE_COUNT; i++)
	{
		if (!db->get_hash_list_for_resource_tag(ResourceTag(i), &count, nullptr))
			return false;

		switch (ResourceTag(i))
		{
		case RESOURCE_GRAPHICS_PIPELINE:
		case RESOURCE_SHADER_MODULE:
			if (count != 3)
				return false;
			break;

		default:
			if (count != 0)
				return false;
			break;
		}
	}

	Hash hashes[3];
	count = 3;
	if (!db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &count, hashes))
		return false;
	static const Hash reference_hashes_module[3] = { 1, 2, 3 };
	static const Hash reference_hashes_pipeline[3] = { 100, 200, 300 };
	if (memcmp(reference_hashes_module, hashes, sizeof(hashes)) != 0)
		return false;

	if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &count, hashes))
		return false;
	if (memcmp(reference_hashes_pipeline, hashes, sizeof(hashes)) != 0)
		return false;

	union
	{
		uint16_t u16;
		uint32_t u32;
		uint64_t u64;
	} u;

	size_t blob_size;

	blob_size = sizeof(u.u16);
	if (!db->read_entry(RESOURCE_SHADER_MODULE, 1, &blob_size, &u.u16, 0) || u.u16 != one)
		return false;

	blob_size = sizeof(u.u32);
	if (!db->read_entry(RESOURCE_SHADER_MODULE, 2, &blob_size, &u.u32, 0) || u.u32 != two)
		return false;

	blob_size = sizeof(u.u64);
	if (!db->read_entry(RESOURCE_SHADER_MODULE, 3, &blob_size, &u.u64, 0) || u.u64 != three)
		return false;

	if (db->has_entry(RESOURCE_SHADER_MODULE, 0) || db->has_entry(RESOURCE_SHADER_MODULE, 4))
		return false;

	blob_size = sizeof(u.u16);
	if (!db->read_entry(RESOURCE_GRAPHICS_PIPELINE, 300, &blob_size, &u.u16, 0) || u.u16 != one)
		return false;

	blob_size = sizeof(u.u32);
	if (!db->read_entry(RESOURCE_GRAPHICS_PIPELINE, 200, &blob_size, &u.u32, 0) || u.u32 != two)
		return false;

	blob_size = sizeof(u.u64);
	if (!db->read_entry(RESOURCE_GRAPHICS_PIPELINE, 100, &blob_size, &u.u64, 0) || u.u64 != three)
		return false;

	if (db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 150) ||
	    db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 99) ||
	    db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 400))
		return false;

	db.reset();
	remove(".__test_archive.foz");
	return true;
}

static bool test_export_concurrent_archive(bool with_read_only)
{
	remove(".__test_archive.foz");
	static const uint16_t one = 1;
	static const uint32_t two = 2;
	static const uint64_t three = 3;
	static const uint8_t four = 4;

	char export_path[DatabaseInterface::OSHandleNameSize];
	DatabaseInterface::get_unique_os_export_name(export_path, sizeof(export_path));

	if (with_read_only)
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_archive.foz",
		                                                                            DatabaseMode::OverWrite));
		if (!db || !db->prepare())
			return false;

		if (!db->write_entry(RESOURCE_SHADER_MODULE, 1000, &four, sizeof(four), 0))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 1300, &four, sizeof(four), 0))
			return false;
	}

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_archive1.foz",
		                                                                            DatabaseMode::OverWrite));
		if (!db || !db->prepare())
			return false;

		if (!db->write_entry(RESOURCE_SHADER_MODULE, 1, &one, sizeof(one), 0))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 300, &one, sizeof(one), 0))
			return false;

		// Duplicate.
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 2, &two, sizeof(two), 0))
			return false;
	}

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_archive2.foz",
		                                                                            DatabaseMode::OverWrite));
		if (!db || !db->prepare())
			return false;

		if (!db->write_entry(RESOURCE_SHADER_MODULE, 2, &two, sizeof(two), 0))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 200, &two, sizeof(two), 0))
			return false;
	}

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_archive3.foz",
		                                                                            DatabaseMode::OverWrite));
		if (!db || !db->prepare())
			return false;

		if (!db->write_entry(RESOURCE_SHADER_MODULE, 3, &three, sizeof(three), 0))
			return false;
		if (!db->write_entry(RESOURCE_GRAPHICS_PIPELINE, 100, &three, sizeof(three), 0))
			return false;
	}

	static const char *extra_paths[] = { ".__test_archive1.foz", ".__test_archive_bogus.foz", ".__test_archive2.foz", ".__test_archive3.foz" };
	intptr_t handle;
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_archive", DatabaseMode::ReadOnly, extra_paths, 4));
		if (!db || !db->prepare())
			return false;
		handle = db->export_metadata_to_os_handle(export_path);
#ifdef __WIN32
		if (handle == 0)
			return false;
#else
		if (handle < 0)
			return false;
#endif
	}

	auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_archive", DatabaseMode::ReadOnly, extra_paths, 4));
	if (!db || !db->import_metadata_from_os_handle(handle) || !db->prepare())
		return false;

	size_t expected_count = with_read_only ? 4 : 3;
	size_t count;

	for (unsigned i = 0; i < RESOURCE_COUNT; i++)
	{
		if (!db->get_hash_list_for_resource_tag(ResourceTag(i), &count, nullptr))
			return false;

		switch (ResourceTag(i))
		{
		case RESOURCE_GRAPHICS_PIPELINE:
		case RESOURCE_SHADER_MODULE:
			if (count != expected_count)
				return false;
			break;

		default:
			if (count != 0)
				return false;
			break;
		}
	}

	Hash hashes[4];
	count = expected_count;
	if (!db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &count, hashes))
		return false;
	static const Hash reference_hashes_module[4] = { 1, 2, 3, 1000 };
	static const Hash reference_hashes_pipeline[4] = { 100, 200, 300, 1300 };
	if (memcmp(reference_hashes_module, hashes, sizeof(*hashes) * expected_count) != 0)
		return false;

	count = expected_count;
	if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &count, hashes))
		return false;
	if (memcmp(reference_hashes_pipeline, hashes, sizeof(*hashes) * expected_count) != 0)
		return false;

	union
	{
		uint8_t u8;
		uint16_t u16;
		uint32_t u32;
		uint64_t u64;
	} u;

	size_t blob_size;

	blob_size = sizeof(u.u16);
	if (!db->read_entry(RESOURCE_SHADER_MODULE, 1, &blob_size, &u.u16, 0) || u.u16 != one)
		return false;

	blob_size = sizeof(u.u32);
	if (!db->read_entry(RESOURCE_SHADER_MODULE, 2, &blob_size, &u.u32, 0) || u.u32 != two)
		return false;

	blob_size = sizeof(u.u64);
	if (!db->read_entry(RESOURCE_SHADER_MODULE, 3, &blob_size, &u.u64, 0) || u.u64 != three)
		return false;

	if (with_read_only)
	{
		blob_size = sizeof(u.u8);
		if (!db->read_entry(RESOURCE_SHADER_MODULE, 1000, &blob_size, &u.u8, 0) || u.u8 != four)
			return false;

		blob_size = sizeof(u.u8);
		if (!db->read_entry(RESOURCE_GRAPHICS_PIPELINE, 1300, &blob_size, &u.u8, 0) || u.u8 != four)
			return false;
	}

	if (db->has_entry(RESOURCE_SHADER_MODULE, 0) || db->has_entry(RESOURCE_SHADER_MODULE, 4))
		return false;

	blob_size = sizeof(u.u16);
	if (!db->read_entry(RESOURCE_GRAPHICS_PIPELINE, 300, &blob_size, &u.u16, 0) || u.u16 != one)
		return false;

	blob_size = sizeof(u.u32);
	if (!db->read_entry(RESOURCE_GRAPHICS_PIPELINE, 200, &blob_size, &u.u32, 0) || u.u32 != two)
		return false;

	blob_size = sizeof(u.u64);
	if (!db->read_entry(RESOURCE_GRAPHICS_PIPELINE, 100, &blob_size, &u.u64, 0) || u.u64 != three)
		return false;

	if (db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 150) ||
	    db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 99) ||
	    db->has_entry(RESOURCE_GRAPHICS_PIPELINE, 400))
		return false;

	db.reset();
	remove(".__test_archive.foz");
	remove(".__test_archive1.foz");
	remove(".__test_archive2.foz");
	remove(".__test_archive3.foz");
	return true;
}

static bool test_logging()
{
	VkSamplerCreateInfo create_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	VkSamplerYcbcrConversionCreateInfo conv_info = { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO };
	create_info.pNext = &conv_info;

	const auto immutable = fake_handle<VkSampler>(100);

	VkDescriptorSetLayoutCreateInfo set_layout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkDescriptorSetLayoutBinding binding = {};
	binding.pImmutableSamplers = &immutable;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	binding.stageFlags = VK_SHADER_STAGE_ALL;
	binding.descriptorCount = 1;
	set_layout.pBindings = &binding;
	set_layout.bindingCount = 1;

	struct UserData
	{
		unsigned err_count;
		unsigned warn_count;
		unsigned info_count;
	};

	UserData userdata = {};
	set_thread_log_callback([](LogLevel level, const char *message, void *u) {
		auto *user = static_cast<UserData *>(u);
		if (level == LOG_WARNING)
			user->warn_count++;
		else if (level == LOG_ERROR)
			user->err_count++;
		else if (level == LOG_INFO)
			user->info_count++;
		fprintf(stderr, "Callback: %s", message);
	}, &userdata);

	for (unsigned i = 0; i < 3; i++)
	{
		userdata = {};

		std::unique_ptr<DatabaseInterface> db(create_stream_archive_database(
				".__test_archive.foz", DatabaseMode::OverWrite));
		StateRecorder recorder;
		if (!db || !db->prepare())
			return false;

		if (i < 2)
			LOGI("Expecting log to trigger.\n");
		else
			LOGI("Expecting log to NOT trigger.\n");
		LOGI("=======================\n");

		set_thread_log_level(LogLevel(i));
		recorder.init_recording_thread(db.get());

		// Expect failure
		if (recorder.record_sampler(immutable, create_info, 100))
			return false;

		unsigned expected_warn = i < 2 ? 1 : 0;
		if (userdata.warn_count != expected_warn || userdata.err_count != 0 || userdata.info_count != 0)
			return false;

		// Should succeed, but will fail later when trying to resolve sampler.
		if (!recorder.record_descriptor_set_layout(fake_handle<VkDescriptorSetLayout>(10), set_layout, 200))
			return false;

		recorder.tear_down_recording_thread();
		LOGI("=======================\n");

		expected_warn = i < 2 ? 2 : 0;
		if (userdata.warn_count != expected_warn || userdata.err_count != 0 || userdata.info_count != 0)
			return false;
	}
	remove(".__test_archive.foz");
	set_thread_log_callback(nullptr, nullptr);
	return true;
}

int main()
{
	if (!test_concurrent_database_extra_paths())
		return EXIT_FAILURE;
	if (!test_concurrent_database())
		return EXIT_FAILURE;
	if (!test_implicit_whitelist())
		return EXIT_FAILURE;
	if (!test_database())
		return EXIT_FAILURE;
	if (!test_filter())
		return EXIT_FAILURE;
	if (!test_export_single_archive())
		return EXIT_FAILURE;
	if (!test_export_concurrent_archive(false))
		return EXIT_FAILURE;
	if (!test_export_concurrent_archive(true))
		return EXIT_FAILURE;
	if (!test_logging())
		return EXIT_FAILURE;

	std::vector<uint8_t> res;
	{
		StateRecorder recorder;

		VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app_info.pEngineName = "test";
		app_info.pApplicationName = "testy";
		app_info.engineVersion = 1234;
		app_info.applicationVersion = 123515;
		app_info.apiVersion = VK_API_VERSION_1_1;
		if (!recorder.record_application_info(app_info))
			abort();

		VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		if (!recorder.record_physical_device_features(features))
			abort();

		record_samplers(recorder);
		record_set_layouts(recorder);
		record_pipeline_layouts(recorder);
		record_shader_modules(recorder);
		record_render_passes(recorder);
		record_render_passes2(recorder);
		record_compute_pipelines(recorder);
		record_graphics_pipelines(recorder);

		uint8_t *serialized;
		size_t serialized_size;
		if (!recorder.serialize(&serialized, &serialized_size))
			return EXIT_FAILURE;
		res = std::vector<uint8_t>(serialized, serialized + serialized_size);
		StateRecorder::free_serialized(serialized);
	}

	StateReplayer replayer;
	ReplayInterface iface;

	std::string serialized(res.begin(), res.end());
	LOGI("Serialized:\n%s\n", serialized.c_str());

	if (!replayer.parse(iface, nullptr, res.data(), res.size()))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
