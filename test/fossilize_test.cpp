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
#include <chrono>
#include <thread>
#include <future>
#include <algorithm>
#include "layer/utils.hpp"
#include "fossilize_errors.hpp"
#include "path.hpp"

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
		feature_hash = Hashing::compute_combined_application_feature_hash(Hashing::compute_application_feature_hash(info, features));
		if (feature_hash != hash)
			abort();

		if (info)
			if (!recorder.record_application_info(*info))
				abort();
		if (features)
			if (!recorder.record_physical_device_features(features))
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

	bool enqueue_create_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info,
	                                        VkPipeline *pipeline) override
	{
		Hash recorded_hash;
		if (!Hashing::compute_hash_raytracing_pipeline(recorder, *create_info, &recorded_hash))
			return false;
		if (recorded_hash != hash)
			return false;

		*pipeline = fake_handle<VkPipeline>(hash);
		return recorder.record_raytracing_pipeline(*pipeline, *create_info, nullptr, 0);
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
	VkSamplerYcbcrConversionInfo ycbcr = { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO };
	sampler.pNext = &ycbcr;
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

	VkSamplerYcbcrConversionCreateInfo ycbcr_info = { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO };
	ycbcr_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	ycbcr_info.forceExplicitReconstruction = 10;
	ycbcr_info.chromaFilter = VK_FILTER_LINEAR;
	ycbcr_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	ycbcr_info.components.g = VK_COMPONENT_SWIZZLE_G;
	ycbcr_info.components.b = VK_COMPONENT_SWIZZLE_B;
	ycbcr_info.components.a = VK_COMPONENT_SWIZZLE_A;
	ycbcr_info.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
	ycbcr_info.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
	ycbcr_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
	ycbcr_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020;
	if (!recorder.record_ycbcr_conversion(fake_handle<VkSamplerYcbcrConversion>(10), ycbcr_info))
		abort();

	ycbcr.conversion = fake_handle<VkSamplerYcbcrConversion>(10);
	if (!recorder.record_sampler(fake_handle<VkSampler>(102), sampler))
		abort();

	VkSamplerCustomBorderColorCreateInfoEXT custom_border_color =
			{ VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT };
	custom_border_color.customBorderColor.uint32[0] = 0;
	custom_border_color.customBorderColor.uint32[1] = 0;
	custom_border_color.customBorderColor.uint32[2] = 0;
	custom_border_color.customBorderColor.uint32[3] = 0;
	custom_border_color.format = VK_FORMAT_R8G8B8A8_UNORM;
	sampler.pNext = &custom_border_color;

	if (!recorder.record_sampler(fake_handle<VkSampler>(103), sampler))
		abort();

	VkSamplerReductionModeCreateInfo reduction_mode =
			{ VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO };
	reduction_mode.reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN;
	sampler.pNext = &reduction_mode;

	if (!recorder.record_sampler(fake_handle<VkSampler>(104), sampler))
		abort();

	VkSamplerBorderColorComponentMappingCreateInfoEXT border_color_mapping =
			{ VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT };
	sampler.pNext = &border_color_mapping;
	border_color_mapping.components.r = VK_COMPONENT_SWIZZLE_A;
	border_color_mapping.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	border_color_mapping.components.b = VK_COMPONENT_SWIZZLE_R;
	border_color_mapping.components.a = VK_COMPONENT_SWIZZLE_B;
	border_color_mapping.srgb = VK_TRUE;
	if (!recorder.record_sampler(fake_handle<VkSampler>(105), sampler))
		abort();
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

	VkMultisampledRenderToSingleSampledInfoEXT ms2ss =
			{ VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT };
	ms2ss.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
	ms2ss.multisampledRenderToSingleSampledEnable = VK_TRUE;

	if (!recorder.record_render_pass2(fake_handle<VkRenderPass>(40000), pass))
		abort();

	subpasses[0].pNext = &ms2ss;
	if (!recorder.record_render_pass2(fake_handle<VkRenderPass>(40002), pass))
		abort();

	VkMemoryBarrier2KHR memory_barrier2 =
			{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR };
	memory_barrier2.pNext = nullptr;
	memory_barrier2.srcStageMask = 10ull << 32;
	memory_barrier2.srcAccessMask = 34;
	memory_barrier2.dstStageMask = 199ull << 32;
	memory_barrier2.dstAccessMask = 49;
	deps[0].pNext = &memory_barrier2;

	if (!recorder.record_render_pass2(fake_handle<VkRenderPass>(40001), pass))
		abort();

	VkRenderPassCreationControlEXT creation_control =
			{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT };
	creation_control.disallowMerging = VK_TRUE;
	pass.pNext = &creation_control;
	if (!recorder.record_render_pass2(fake_handle<VkRenderPass>(40002), pass))
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
	pass.subpassCount = 2;
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

	VkRenderPassInputAttachmentAspectCreateInfo input_att_aspect = { VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO };
	VkInputAttachmentAspectReference aspects[1] = { };
	aspects[0].subpass = 0;
	aspects[0].inputAttachmentIndex = 0;
	aspects[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	input_att_aspect.aspectReferenceCount = 1;
	input_att_aspect.pAspectReferences = aspects;
	blank_multiview.pNext = &input_att_aspect;

	if (!recorder.record_render_pass(fake_handle<VkRenderPass>(30002), pass))
		abort();

	VkRenderPassFragmentDensityMapCreateInfoEXT density_map =
			{ VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT };
	density_map.fragmentDensityMapAttachment.attachment = 5;
	density_map.fragmentDensityMapAttachment.layout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
	pass.pNext = &density_map;
	if (!recorder.record_render_pass(fake_handle<VkRenderPass>(30003), pass))
		abort();

	VkSampleLocationsInfoEXT loc_info =
			{ VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT };
	loc_info.sampleLocationGridSize.width = 9;
	loc_info.sampleLocationGridSize.height = 14;
	loc_info.sampleLocationsPerPixel = VK_SAMPLE_COUNT_2_BIT;
	loc_info.sampleLocationsCount = 3;
	VkSampleLocationEXT sample_locs[3];
	loc_info.pSampleLocations = sample_locs;
	sample_locs[0].x = 0.2f;
	sample_locs[0].y = 0.3f;
	sample_locs[1].x = 0.4f;
	sample_locs[1].y = 0.2f;
	sample_locs[2].x = 0.8f;
	sample_locs[2].y = 0.1f;
	pass.pNext = &loc_info;
	if (!recorder.record_render_pass(fake_handle<VkRenderPass>(30004), pass))
		abort();
}

static void record_pipelines_pnext_shader(StateRecorder &recorder)
{
	VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	static const uint32_t data[] = { 1, 2, 3 };
	module.codeSize = sizeof(data);
	module.pCode = data;

	VkShaderModuleValidationCacheCreateInfoEXT validate_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT };
	module.pNext = &validate_info;

	VkComputePipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkGraphicsPipelineCreateInfo gpipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipe.stage.module = VK_NULL_HANDLE;
	pipe.stage.pName = "foobar";
	pipe.stage.pNext = &module;
	gpipe.stageCount = 1;
	gpipe.pStages = &pipe.stage;

	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1987), pipe, nullptr, 0))
		abort();
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1988), gpipe, nullptr, 0))
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

	VkPipelineCreationFeedbackCreateInfoEXT feedback = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT };

	Hash hash[2];
	if (!Hashing::compute_hash_compute_pipeline(recorder, pipe, &hash[0]))
		abort();
	pipe.pNext = &feedback;
	if (!Hashing::compute_hash_compute_pipeline(recorder, pipe, &hash[1]))
		abort();

	if (hash[0] != hash[1])
		abort();

	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(80002), pipe, nullptr, 0))
		abort();
}

template <typename T>
static void set_invalid_pointer(T *&ptr)
{
	ptr = reinterpret_cast<T *>(uintptr_t(64));
}

static void record_graphics_pipelines_robustness(StateRecorder &recorder)
{
	// Check that state is correctly ignored depending on other state.
	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.layout = fake_handle<VkPipelineLayout>(10002);
	pipe.subpass = 0;
	pipe.stageCount = 2;

	VkPipelineRenderingCreateInfoKHR rendering_create_info = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
	const VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

	{
		VkRenderPassCreateInfo rp_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		VkSubpassDescription subpasses[32] = {};
		// Exercise the fallback path for meta storage.
		rp_info.subpassCount = 32;
		rp_info.pSubpasses = subpasses;

		VkAttachmentReference blank = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };
		VkAttachmentReference att0 = { 0, VK_IMAGE_LAYOUT_GENERAL };
		for (uint32_t i = 0; i < 32; i++)
		{
			subpasses[i].colorAttachmentCount = i & 1 ? 0 : 1;
			subpasses[i].pDepthStencilAttachment = i & 2 ? &blank : &att0;
			subpasses[i].pColorAttachments = &att0;
		}

		if (!recorder.record_render_pass(fake_handle<VkRenderPass>(90), rp_info))
			abort();
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].pName = "vert";
	stages[0].module = fake_handle<VkShaderModule>(5000);
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].pName = "frag";
	stages[1].module = fake_handle<VkShaderModule>(5001);
	pipe.pStages = stages;

	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineVertexInputStateCreateInfo vi;
	VkPipelineMultisampleStateCreateInfo ms;
	VkPipelineDynamicStateCreateInfo dyn;
	VkPipelineViewportStateCreateInfo vp;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineTessellationStateCreateInfo tess;
	VkPipelineDepthStencilStateCreateInfo ds;
	VkPipelineRasterizationStateCreateInfo rs;
	VkPipelineInputAssemblyStateCreateInfo ia;
	VkPipelineColorWriteCreateInfoEXT color_write;
	VkPipelineRasterizationLineStateCreateInfoEXT line;
	VkPipelineTessellationDomainOriginStateCreateInfo domain_info;
	VkPipelineColorBlendAdvancedStateCreateInfoEXT color_blend_advanced;
	VkPipelineRasterizationStateStreamCreateInfoEXT stream_info;
	VkPipelineRasterizationConservativeStateCreateInfoEXT conservative_info;
	VkPipelineRasterizationDepthClipStateCreateInfoEXT depth_clip;
	VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex;
	VkPipelineViewportDepthClipControlCreateInfoEXT negative_one_to_one;
	VkPipelineSampleLocationsStateCreateInfoEXT sample_locations;
	VkGraphicsPipelineLibraryCreateInfoEXT gpl;
	VkViewport viewports[2] = {};
	VkRect2D scissors[2] = {};
	VkBool32 color_write_enable;

	uint64_t counter = 1000000;
	uint32_t sample_mask;
	Hash hash[5];

	const auto reset_state = [&]() {
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		pipe.pViewportState = nullptr;
		pipe.pMultisampleState = nullptr;
		pipe.pColorBlendState = nullptr;
		pipe.pDepthStencilState = nullptr;
		pipe.pInputAssemblyState = nullptr;
		pipe.pVertexInputState = nullptr;
		pipe.pTessellationState = nullptr;
		pipe.flags = 0;
		pipe.subpass = 0;
		pipe.pNext = nullptr;
		vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		tess = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
		ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		color_write = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT };
		line = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT };
		domain_info = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO };
		color_blend_advanced = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT };
		stream_info = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT };
		conservative_info = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT };
		depth_clip = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT };
		provoking_vertex = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT };
		negative_one_to_one = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
		sample_locations = { VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT };
		sample_locations.sampleLocationsInfo = { VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT };
		gpl = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT };
		pipe.renderPass = fake_handle<VkRenderPass>(90);
		color_write_enable = VK_FALSE;
		color_write.pColorWriteEnables = &color_write_enable;
		color_write.attachmentCount = 1;
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		sample_mask = 0xaaaaaaaa;
		ms.pSampleMask = &sample_mask;
	};

	const auto reset_state_set_all = [&]() {
		reset_state();
		pipe.pVertexInputState = &vi;
		pipe.pViewportState = &vp;
		pipe.pMultisampleState = &ms;
		pipe.pDepthStencilState = &ds;
		pipe.pColorBlendState = &blend;
		pipe.pTessellationState = &tess;
		pipe.pInputAssemblyState = &ia;
		vp.pViewports = viewports;
		vp.pScissors = scissors;
		vp.viewportCount = 1;
		vp.scissorCount = 1;
	};

	const auto hash_and_record = [&](unsigned index) {
		if (!Hashing::compute_hash_graphics_pipeline(recorder, pipe, &hash[index]))
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(counter), pipe, nullptr, 0))
			abort();
		counter++;
	};

	// First, verify that rasterization-disable ignores relevant state and yields same hash.
	{
		reset_state();
		rs.rasterizerDiscardEnable = VK_TRUE;
		pipe.pRasterizationState = &rs;
		hash_and_record(0);
		set_invalid_pointer(pipe.pViewportState);
		set_invalid_pointer(pipe.pColorBlendState);
		set_invalid_pointer(pipe.pDepthStencilState);
		set_invalid_pointer(pipe.pMultisampleState);
		hash_and_record(1);

		if (hash[0] != hash[1])
			abort();
	}

	// Without the discard, verify the parameters are not ignored and yield different hashes.
	{
		reset_state();
		rs.rasterizerDiscardEnable = VK_FALSE;

		hash_and_record(0);
		pipe.pViewportState = &vp;
		hash_and_record(1);
		pipe.pMultisampleState = &ms;
		hash_and_record(2);

		if (hash[0] == hash[1] || hash[1] == hash[2])
			abort();
	}

	// Another way to test discard enable. If rasterizer discard is dynamic, ignore rasterizerDiscardEnable.
	{
		reset_state();
		rs.rasterizerDiscardEnable = VK_TRUE;
		const VkDynamicState state = VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT;
		dyn.pDynamicStates = &state;
		dyn.dynamicStateCount = 1;
		pipe.pDynamicState = &dyn;

		hash_and_record(0);
		pipe.pViewportState = &vp;
		hash_and_record(1);
		pipe.pMultisampleState = &ms;
		hash_and_record(2);

		if (hash[0] == hash[1] || hash[1] == hash[2])
			abort();
	}

	// Without a shader stage doing tessellation, verify the parameters are ignored.
	{
		reset_state();
		hash_and_record(0);
		set_invalid_pointer(pipe.pTessellationState);
		hash_and_record(1);
		if (hash[0] != hash[1])
			abort();
	}

	// If we have a shader doing tessellation, verify the tess parameter is used.
	{
		reset_state();
		stages[0].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		stages[1].stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
		hash_and_record(0);
		pipe.pTessellationState = &tess;
		hash_and_record(1);
		if (hash[0] == hash[1])
			abort();
	}

	// If we have a mesh shader stage, verify vertex input and input state is ignored.
	{
		reset_state();
		stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
		stages[1].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
		hash_and_record(0);
		set_invalid_pointer(pipe.pVertexInputState);
		set_invalid_pointer(pipe.pInputAssemblyState);
		hash_and_record(1);
		if (hash[0] != hash[1])
			abort();
	}

	// If we have VERTEX_INPUT_EXT dynamic state, verify that pVertexInputState is ignored.
	{
		reset_state();
		const VkDynamicState state = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT;
		dyn.pDynamicStates = &state;
		dyn.dynamicStateCount = 1;
		pipe.pDynamicState = &dyn;

		hash_and_record(0);
		set_invalid_pointer(pipe.pVertexInputState);
		hash_and_record(1);
		if (hash[0] != hash[1])
			abort();
	}

	// If we set VERTEX_INPUT_DYNAMIC_STRIDE state, verify that stride is ignored.
	{
		reset_state();
		const VkDynamicState state = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT;
		dyn.pDynamicStates = &state;
		dyn.dynamicStateCount = 1;
		pipe.pDynamicState = &dyn;

		hash_and_record(0);
		pipe.pVertexInputState = &vi;

		VkVertexInputBindingDescription binding = {};
		vi.vertexBindingDescriptionCount = 1;
		vi.pVertexBindingDescriptions = &binding;
		hash_and_record(1);
		binding.stride = 4;
		hash_and_record(2);
		if (hash[0] == hash[1])
			abort();
		if (hash[1] != hash[2])
			abort();
	}

	// If color and depth exists in subpass, verify that pColor and pDepth both contribute.
	{
		reset_state();
		hash_and_record(0);
		pipe.pColorBlendState = &blend;
		hash_and_record(1);
		pipe.pDepthStencilState = &ds;
		hash_and_record(2);
		if (hash[0] == hash[1] || hash[1] == hash[2])
			abort();

		// Test same thing, but with dynamic rendering.
		reset_state();
		pipe.renderPass = VK_NULL_HANDLE;
		pipe.pNext = &rendering_create_info;
		rendering_create_info = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		rendering_create_info.colorAttachmentCount = 1;
		rendering_create_info.pColorAttachmentFormats = &color_format;
		rendering_create_info.stencilAttachmentFormat = depth_format;
		rendering_create_info.viewMask = 3;
		hash_and_record(0);
		pipe.pColorBlendState = &blend;
		hash_and_record(1);
		pipe.pDepthStencilState = &ds;
		hash_and_record(2);
		if (hash[0] == hash[1] || hash[1] == hash[2])
			abort();
	}

	// If only depth exists in subpass, verify that pDepth contributes and pColor is ignored.
	{
		reset_state();
		pipe.subpass = 9;
		hash_and_record(0);
		pipe.pColorBlendState = &blend; // TODO: Cannot handle invalid pointer here yet in copy_pipeline.
		hash_and_record(1);
		pipe.pDepthStencilState = &ds;
		hash_and_record(2);
		if (hash[0] != hash[1] || hash[1] == hash[2])
			abort();

		// Test same thing, but with dynamic rendering
		reset_state();
		pipe.renderPass = VK_NULL_HANDLE;
		pipe.pNext = &rendering_create_info;
		rendering_create_info = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		rendering_create_info.depthAttachmentFormat = depth_format;
		rendering_create_info.viewMask = 3;
		hash_and_record(0);
		pipe.pColorBlendState = &blend;
		hash_and_record(1);
		pipe.pDepthStencilState = &ds;
		hash_and_record(2);
		if (hash[0] != hash[1] || hash[1] == hash[2])
			abort();
	}

	// If only color exists in subpass, verify that pColor contributes and pDepth is ignored.
	{
		reset_state();
		pipe.subpass = 18;
		hash_and_record(0);
		pipe.pColorBlendState = &blend;
		hash_and_record(1);
		pipe.pDepthStencilState = &ds; // TODO: Cannot handle invalid pointer here yet in copy_pipeline.
		hash_and_record(2);
		if (hash[0] == hash[1] || hash[1] != hash[2])
			abort();

		// Test same thing, but with dynamic rendering ...
		reset_state();
		pipe.renderPass = VK_NULL_HANDLE;
		pipe.pNext = &rendering_create_info;
		rendering_create_info = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		rendering_create_info.colorAttachmentCount = 1;
		rendering_create_info.pColorAttachmentFormats = &color_format;
		rendering_create_info.viewMask = 3;
		hash_and_record(0);
		pipe.pColorBlendState = &blend;
		hash_and_record(1);
		pipe.pDepthStencilState = &ds;
		hash_and_record(2);
		if (hash[0] == hash[1] || hash[1] != hash[2])
			abort();
	}

	// If neither exists, verify that neither contribute.
	{
		reset_state();
		pipe.subpass = 31;
		hash_and_record(0);
		// TODO: Cannot handle invalid pointer here yet in copy_pipeline.
		pipe.pColorBlendState = &blend;
		pipe.pDepthStencilState = &ds;
		hash_and_record(1);
		if (hash[0] != hash[1])
			abort();

		// Test same thing, but with dynamic rendering ...
		reset_state();
		pipe.renderPass = VK_NULL_HANDLE;
		pipe.pNext = &rendering_create_info;
		rendering_create_info = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		rendering_create_info.viewMask = 3;
		hash_and_record(0);
		pipe.pColorBlendState = &blend;
		hash_and_record(1);
		pipe.pDepthStencilState = &ds;
		hash_and_record(2);
		if (hash[0] != hash[1] || hash[1] != hash[2])
			abort();

		// With neither dynamic rendering, nor render pass, ignore as well.
		reset_state();
		pipe.renderPass = VK_NULL_HANDLE;
		hash_and_record(0);
		pipe.pColorBlendState = &blend;
		hash_and_record(1);
		pipe.pDepthStencilState = &ds;
		hash_and_record(2);
		if (hash[0] != hash[1] || hash[1] != hash[2])
			abort();
	}

	// If only vertex input stage is, verify that formats and such do not contribute.
	{
		reset_state();
		pipe.renderPass = VK_NULL_HANDLE;
		pipe.pNext = &rendering_create_info;
		rendering_create_info = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		rendering_create_info.pNext = &gpl;
		gpl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
		pipe.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
		hash_and_record(0);
		rendering_create_info.viewMask = 3;
		hash_and_record(1);
		rendering_create_info.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
		rendering_create_info.colorAttachmentCount = 10;
		hash_and_record(2);
		if (hash[0] != hash[1] || hash[1] != hash[2])
			abort();

		// Now view mask is significant, but not formats.
		gpl.flags |= VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
		             VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
		rendering_create_info.viewMask = 0;
		rendering_create_info.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
		rendering_create_info.colorAttachmentCount = 0;
		hash_and_record(0);
		rendering_create_info.viewMask = 3;
		hash_and_record(1);
		rendering_create_info.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
		rendering_create_info.colorAttachmentCount = 10;
		// Test that we ignore NULL pColorAttachmentFormats here.
		hash_and_record(2);
		if (hash[0] == hash[1] || hash[1] != hash[2])
			abort();

		// Now everything is significant.
		gpl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
		rendering_create_info.viewMask = 0;
		rendering_create_info.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
		rendering_create_info.colorAttachmentCount = 0;
		hash_and_record(0);
		rendering_create_info.viewMask = 3;
		hash_and_record(1);
		rendering_create_info.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
		rendering_create_info.colorAttachmentCount = 1;
		rendering_create_info.pColorAttachmentFormats = &rendering_create_info.depthAttachmentFormat;
		hash_and_record(2);
		if (hash[0] == hash[1] || hash[1] == hash[2])
			abort();
	}

	// If dynamic color write is used, verify that pColorWriteEnable is ignored.
	{
		reset_state();
		const VkDynamicState state = VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT;
		dyn.pDynamicStates = &state;
		dyn.dynamicStateCount = 1;
		pipe.pDynamicState = &dyn;
		pipe.pColorBlendState = &blend;
		blend.pNext = &color_write;

		hash_and_record(0);
		set_invalid_pointer(color_write.pColorWriteEnables);
		hash_and_record(1);
		if (hash[0] != hash[1])
			abort();
	}

	// If multiple EDS3 states are used together, verify that pAttachments in blend state is ignored.
	{
		reset_state();
		const VkDynamicState states[] = { VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
		                                  VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
		                                  VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT };
		dyn.pDynamicStates = states;
		dyn.dynamicStateCount = 3;
		pipe.pDynamicState = &dyn;
		pipe.pColorBlendState = &blend;
		blend.attachmentCount = 4;

		blend.pAttachments = nullptr;
		hash_and_record(0);
		set_invalid_pointer(blend.pAttachments);
		hash_and_record(1);
		if (hash[0] != hash[1])
			abort();
	}

	struct HashInvarianceTest
	{
		VkDynamicState state;
		uint32_t *state_word_u32;
		float *state_word_f32;
		std::function<void ()> init_state_static;
		std::function<void ()> init_state_dynamic;
		bool invariant_in_static_case;
		bool variant_in_dynamic_case;
	};
	std::vector<HashInvarianceTest> hash_invariance_tests;

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_VIEWPORT, nullptr, &viewports[0].x });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_SCISSOR, &scissors[0].extent.width, nullptr });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT, &vp.viewportCount, &viewports[0].x });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT, &vp.scissorCount, nullptr });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LINE_WIDTH, nullptr, &rs.lineWidth });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BIAS, nullptr, &rs.depthBiasConstantFactor, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BIAS, nullptr, &rs.depthBiasConstantFactor,
	                                  [&]() { rs.depthBiasEnable = VK_TRUE; }});

	const auto set_blend_factor_invariant = [&]() {
		blend.attachmentCount = 1;
		blend.pAttachments = &blend_attachment;
		blend_attachment = {};
		blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
	};
	const auto set_blend_factor = [&]() {
		set_blend_factor_invariant();
		blend_attachment.blendEnable = VK_TRUE;
	};
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_BLEND_CONSTANTS, nullptr, &blend.blendConstants[0],
									  set_blend_factor_invariant, set_blend_factor_invariant, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_BLEND_CONSTANTS, nullptr, &blend.blendConstants[0],
									  set_blend_factor, set_blend_factor });

	const auto set_depth_bounds_test = [&]() { ds.depthBoundsTestEnable = VK_TRUE; };
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BOUNDS, nullptr, &ds.minDepthBounds, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BOUNDS, nullptr, &ds.minDepthBounds, set_depth_bounds_test, set_depth_bounds_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BOUNDS, nullptr, &ds.maxDepthBounds, set_depth_bounds_test, set_depth_bounds_test });

	const auto set_stencil_test = [&]() { ds.stencilTestEnable = VK_TRUE; };
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, &ds.front.compareMask, nullptr, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, &ds.back.compareMask, nullptr, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, &ds.front.compareMask, nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, &ds.back.compareMask, nullptr, set_stencil_test, set_stencil_test });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, &ds.front.writeMask, nullptr, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, &ds.back.writeMask, nullptr, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, &ds.front.writeMask, nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, &ds.back.writeMask, nullptr, set_stencil_test, set_stencil_test });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_REFERENCE, &ds.front.reference, nullptr, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_REFERENCE, &ds.back.reference, nullptr, {}, {}, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_REFERENCE, &ds.front.reference, nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_REFERENCE, &ds.back.reference, nullptr, set_stencil_test, set_stencil_test });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_OP_EXT, reinterpret_cast<uint32_t *>(&ds.front.failOp), nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_OP_EXT, reinterpret_cast<uint32_t *>(&ds.front.passOp), nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_OP_EXT, reinterpret_cast<uint32_t *>(&ds.front.depthFailOp), nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_OP_EXT, reinterpret_cast<uint32_t *>(&ds.back.failOp), nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_OP_EXT, reinterpret_cast<uint32_t *>(&ds.back.passOp), nullptr, set_stencil_test, set_stencil_test });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_OP_EXT, reinterpret_cast<uint32_t *>(&ds.back.depthFailOp), nullptr, set_stencil_test, set_stencil_test });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT, &ds.stencilTestEnable, nullptr });
	// Verify that using dynamic stencil test without dynamic stencil ops makes it variant.
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT, reinterpret_cast<uint32_t *>(&ds.front.passOp), nullptr,
	                                  set_stencil_test, {}, false, true });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_CULL_MODE_EXT, &rs.cullMode, nullptr });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_FRONT_FACE_EXT, reinterpret_cast<uint32_t *>(&rs.frontFace), nullptr });
	// Primitive topology still needs to consider the class, so make the hash variant even with dynamic state set.
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT, reinterpret_cast<uint32_t *>(&ia.topology), nullptr, {}, {}, false, true });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT, &ds.depthTestEnable, nullptr });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, &ds.depthWriteEnable, nullptr });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT, reinterpret_cast<uint32_t *>(&ds.depthCompareOp), nullptr });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT, &ds.depthBoundsTestEnable, nullptr });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT, &rs.depthBiasEnable, nullptr });
	// Verify that we pick up bias constants even if depthBiasEnable isn't set and depth bias itself isn't dynamic.
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT, nullptr, &rs.depthBiasConstantFactor,
	                                  [&]() { rs.depthBiasEnable = VK_TRUE; },
	                                  {}, false, true });

	const auto set_tess_shaders = [&]() {
		stages[0].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		stages[1].stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	};
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT, &tess.patchControlPoints, nullptr,
									  {}, {}, true, false });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT, &tess.patchControlPoints, nullptr,
	                                  set_tess_shaders, set_tess_shaders });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT, &rs.rasterizerDiscardEnable });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT, &ia.primitiveRestartEnable });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LOGIC_OP_EXT, reinterpret_cast<uint32_t *>(&blend.logicOp), nullptr,
	                                  {}, {}, true });

	// Verify that we pick up pColorWriteEnable if present.
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT, &color_write_enable, nullptr,
	                                  [&]() { blend.pNext = &color_write; },
	                                  [&]() { blend.pNext = &color_write; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LINE_STIPPLE_EXT, &line.lineStippleFactor, nullptr,
	                                  [&]() { rs.pNext = &line; line.stippledLineEnable = VK_TRUE; },
	                                  [&]() { rs.pNext = &line; line.stippledLineEnable = VK_TRUE; }});
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LINE_STIPPLE_EXT, &line.lineStippleFactor, nullptr,
	                                  [&]() { rs.pNext = &line; },
	                                  [&]() { rs.pNext = &line; }, true, false});
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT, &line.lineStippleFactor, nullptr,
	                                  [&]() { rs.pNext = &line; },
	                                  [&]() { rs.pNext = &line; }, true, true});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT,
	                                  reinterpret_cast<uint32_t *>(&line.lineRasterizationMode), nullptr,
	                                  [&]() { rs.pNext = &line; },
	                                  [&]() { rs.pNext = &line; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_TESSELLATION_DOMAIN_ORIGIN_EXT,
	                                  reinterpret_cast<uint32_t *>(&domain_info.domainOrigin), nullptr,
	                                  [&]() { set_tess_shaders(); tess.pNext = &domain_info; },
	                                  [&]() { set_tess_shaders(); tess.pNext = &domain_info; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT,
	                                  &rs.depthClampEnable, nullptr,
	                                  {}, {}});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
	                                  reinterpret_cast<uint32_t *>(&rs.polygonMode), nullptr,
	                                  {}, {}});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT,
	                                  &ms.alphaToCoverageEnable, nullptr,
	                                  {}, {}});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT,
	                                  &ms.alphaToOneEnable, nullptr,
	                                  {}, {}});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT,
	                                  &blend.logicOpEnable, nullptr,
	                                  {}, {}});
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT,
	                                  reinterpret_cast<uint32_t *>(&blend.logicOp), nullptr,
	                                  {}, {}, true, true});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
	                                  &blend_attachment.blendEnable, nullptr,
	                                  set_blend_factor_invariant, set_blend_factor_invariant });
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
	                                  reinterpret_cast<uint32_t *>(&blend_attachment.alphaBlendOp),
	                                  nullptr,
	                                  set_blend_factor_invariant, set_blend_factor_invariant, true, true });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
	                                  reinterpret_cast<uint32_t *>(&blend_attachment.alphaBlendOp), nullptr,
	                                  set_blend_factor,
	                                  set_blend_factor});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT,
	                                  reinterpret_cast<uint32_t *>(&blend_attachment.colorBlendOp), nullptr,
	                                  set_blend_factor,
	                                  set_blend_factor});
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_COLOR_BLEND_ADVANCED_EXT,
	                                  &color_blend_advanced.dstPremultiplied, nullptr,
	                                  [&]() { set_blend_factor(); blend.pNext = &color_blend_advanced; },
	                                  [&]() { set_blend_factor(); blend.pNext = &color_blend_advanced; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
	                                  &blend_attachment.colorWriteMask, nullptr,
	                                  set_blend_factor, set_blend_factor });

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_RASTERIZATION_STREAM_EXT,
	                                  &stream_info.rasterizationStream, nullptr,
	                                  [&]() { rs.pNext = &stream_info; },
	                                  [&]() { rs.pNext = &stream_info; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT,
	                                  reinterpret_cast<uint32_t *>(&conservative_info.conservativeRasterizationMode),
	                                  nullptr,
	                                  [&]() { rs.pNext = &conservative_info; },
	                                  [&]() { rs.pNext = &conservative_info; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT,
	                                  nullptr,
	                                  &conservative_info.extraPrimitiveOverestimationSize,
	                                  [&]() { rs.pNext = &conservative_info; },
	                                  [&]() { rs.pNext = &conservative_info; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT,
	                                  &depth_clip.depthClipEnable, nullptr,
	                                  [&]() { rs.pNext = &depth_clip; },
	                                  [&]() { rs.pNext = &depth_clip; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT,
	                                  reinterpret_cast<uint32_t *>(&provoking_vertex.provokingVertexMode), nullptr,
	                                  [&]() { rs.pNext = &provoking_vertex; },
	                                  [&]() { rs.pNext = &provoking_vertex; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT,
	                                  &negative_one_to_one.negativeOneToOne, nullptr,
	                                  [&]() { vp.pNext = &negative_one_to_one; },
	                                  [&]() { vp.pNext = &negative_one_to_one; }});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT,
	                                  &sample_locations.sampleLocationsInfo.sampleLocationGridSize.width, nullptr,
	                                  [&]() { ms.pNext = &sample_locations; },
	                                  [&]() { ms.pNext = &sample_locations; }, true, false});
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT,
	                                  &sample_locations.sampleLocationsInfo.sampleLocationGridSize.width, nullptr,
	                                  [&]() { ms.pNext = &sample_locations; sample_locations.sampleLocationsEnable = VK_TRUE; },
	                                  [&]() { ms.pNext = &sample_locations; sample_locations.sampleLocationsEnable = VK_TRUE; }});
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_ENABLE_EXT,
	                                  &sample_locations.sampleLocationsInfo.sampleLocationGridSize.width, nullptr,
	                                  [&]() { ms.pNext = &sample_locations; },
	                                  [&]() { ms.pNext = &sample_locations; }, true, true});

	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT,
	                                  reinterpret_cast<uint32_t *>(&ms.rasterizationSamples), nullptr,
	                                  {}, [&]() { ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(16); }});
	hash_invariance_tests.push_back({ VK_DYNAMIC_STATE_SAMPLE_MASK_EXT,
	                                  &sample_mask, nullptr,
	                                  {}, [&]() { ms.rasterizationSamples = VK_SAMPLE_COUNT_64_BIT; set_invalid_pointer(ms.pSampleMask); }});

	for (auto &test : hash_invariance_tests)
	{
		// First, verify that changing the state without dynamic state changes the hash.
		reset_state_set_all();
		if (test.init_state_static)
			test.init_state_static();

		hash_and_record(0);
		if (test.state_word_u32)
			*test.state_word_u32 += 1;
		if (test.state_word_f32)
			*test.state_word_f32 += 1.0f;
		hash_and_record(1);

		if ((hash[0] == hash[1]) != test.invariant_in_static_case)
			abort();

		// Set the dynamic state. Changing the state value should *not* modify hash now.
		reset_state_set_all();
		if (test.init_state_dynamic)
			test.init_state_dynamic();

		dyn.pDynamicStates = &test.state;
		dyn.dynamicStateCount = 1;
		hash_and_record(0);
		if (test.state_word_u32)
			*test.state_word_u32 += 1;
		if (test.state_word_f32)
			*test.state_word_f32 += 1.0f;
		hash_and_record(1);
		if ((hash[0] != hash[1]) != test.variant_in_dynamic_case)
			abort();
	}
}

static void record_pipeline_flags2(StateRecorder &recorder)
{
	VkGraphicsPipelineCreateInfo graphics_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	VkComputePipelineCreateInfo compute_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkRayTracingPipelineCreateInfoKHR rt_info = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
	VkPipelineCreateFlags2CreateInfoKHR flags2_info = { VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR };

	// To avoid compute hashing failing.
	VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	compute_info.stage.pNext = &module_info;
	compute_info.stage.pName = "foo";

	graphics_info.pNext = &flags2_info;
	compute_info.pNext = &flags2_info;
	rt_info.pNext = &flags2_info;

	Hash gfx_hash[2], compute_hash[2], rt_hash[2];

	// First, ensure that base flags is ignored when there is a pNext.
	if (!Hashing::compute_hash_graphics_pipeline(recorder, graphics_info, &gfx_hash[0]))
		abort();
	graphics_info.flags = 1;
	if (!Hashing::compute_hash_graphics_pipeline(recorder, graphics_info, &gfx_hash[1]))
		abort();
	if (gfx_hash[0] != gfx_hash[1])
		abort();
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1), graphics_info, nullptr, 0))
		abort();

	if (!Hashing::compute_hash_compute_pipeline(recorder, compute_info, &compute_hash[0]))
		abort();
	compute_info.flags = 1;
	if (!Hashing::compute_hash_compute_pipeline(recorder, compute_info, &compute_hash[1]))
		abort();
	if (compute_hash[0] != compute_hash[1])
		abort();
	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1), compute_info, nullptr, 0))
		abort();

	if (!Hashing::compute_hash_raytracing_pipeline(recorder, rt_info, &rt_hash[0]))
		abort();
	rt_info.flags = 1;
	if (!Hashing::compute_hash_raytracing_pipeline(recorder, rt_info, &rt_hash[1]))
		abort();
	if (rt_hash[0] != rt_hash[1])
		abort();
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(1), rt_info, nullptr, 0))
		abort();

	// Ignored flags.
	flags2_info.flags =
			VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT |
			VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT |
			VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR;
	if (!Hashing::compute_hash_graphics_pipeline(recorder, graphics_info, &gfx_hash[1]))
		abort();
	if (!Hashing::compute_hash_compute_pipeline(recorder, compute_info, &compute_hash[1]))
		abort();
	if (!Hashing::compute_hash_raytracing_pipeline(recorder, rt_info, &rt_hash[1]))
		abort();
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1), graphics_info, nullptr, 0))
		abort();
	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1), compute_info, nullptr, 0))
		abort();
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(1), rt_info, nullptr, 0))
		abort();

	if (gfx_hash[0] != gfx_hash[1])
		abort();
	if (compute_hash[0] != compute_hash[1])
		abort();
	if (rt_hash[0] != rt_hash[1])
		abort();

	flags2_info.flags |= 1ull << 63;

	if (!Hashing::compute_hash_graphics_pipeline(recorder, graphics_info, &gfx_hash[1]))
		abort();
	if (!Hashing::compute_hash_compute_pipeline(recorder, compute_info, &compute_hash[1]))
		abort();
	if (!Hashing::compute_hash_raytracing_pipeline(recorder, rt_info, &rt_hash[1]))
		abort();
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1), graphics_info, nullptr, 0))
		abort();
	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1), compute_info, nullptr, 0))
		abort();
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(1), rt_info, nullptr, 0))
		abort();

	if (gfx_hash[0] == gfx_hash[1])
		abort();
	if (compute_hash[0] == compute_hash[1])
		abort();
	if (rt_hash[0] == rt_hash[1])
		abort();

	flags2_info.flags = 1ull << 63;
	if (!Hashing::compute_hash_graphics_pipeline(recorder, graphics_info, &gfx_hash[0]))
		abort();
	if (!Hashing::compute_hash_compute_pipeline(recorder, compute_info, &compute_hash[0]))
		abort();
	if (!Hashing::compute_hash_raytracing_pipeline(recorder, rt_info, &rt_hash[0]))
		abort();
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1), graphics_info, nullptr, 0))
		abort();
	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1), compute_info, nullptr, 0))
		abort();
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(1), rt_info, nullptr, 0))
		abort();

	if (gfx_hash[0] != gfx_hash[1])
		abort();
	if (compute_hash[0] != compute_hash[1])
		abort();
	if (rt_hash[0] != rt_hash[1])
		abort();
}

static void record_pipelines_identifier(StateRecorder &recorder)
{
	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipe.pStages = &stage;
	pipe.stageCount = 1;

	VkPipelineShaderStageModuleIdentifierCreateInfoEXT ident = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT };
	ident.identifierSize = 1;
	const uint8_t zero = 0;
	ident.pIdentifier = &zero;
	stage.pNext = &ident;

	Hash hash;

	// These should fail.
	if (Hashing::compute_hash_graphics_pipeline(recorder, pipe, &hash))
		abort();
	// These should succeed as noops.
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(40000), pipe, nullptr, 0))
		abort();

	VkComputePipelineCreateInfo cpipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	cpipe.stage = stage;
	if (Hashing::compute_hash_compute_pipeline(recorder, cpipe, &hash))
		abort();
	if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(40000), cpipe, nullptr, 0))
		abort();

	VkRayTracingPipelineCreateInfoKHR rtpipe = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
	rtpipe.pStages = &stage;
	rtpipe.stageCount = 1;
	if (Hashing::compute_hash_raytracing_pipeline(recorder, rtpipe, &hash))
		abort();
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(40000), rtpipe, nullptr, 0))
		abort();
}

static void record_graphics_pipeline_libraries(StateRecorder &recorder)
{
	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.layout = fake_handle<VkPipelineLayout>(10002);
	pipe.subpass = 1;
	pipe.renderPass = fake_handle<VkRenderPass>(30001);
	pipe.stageCount = 2;
	pipe.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
	VkPipelineShaderStageCreateInfo stages[2] = {};
	VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	static const uint32_t code[] = { 1, 2, 3 };
	module.pCode = code;
	module.codeSize = sizeof(code);
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].pName = "a";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].pName = "b";
	stages[0].pNext = &module;
	stages[1].pNext = &module;
	pipe.pStages = stages;

	// First, test hash invariance. Based on which pipeline state we're creating, we have to ignore some state.
	VkGraphicsPipelineLibraryCreateInfoEXT library_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT };
	pipe.pNext = &library_info;

	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineTessellationStateCreateInfo tes = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };

	auto gpipe = pipe;
	library_info.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
	{
		Hash baseline_hash = 0;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &baseline_hash))
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1999), gpipe, nullptr, 0))
			abort();

		// This state must be ignored.
		set_invalid_pointer(gpipe.pColorBlendState);
		set_invalid_pointer(gpipe.pMultisampleState);
		set_invalid_pointer(gpipe.pDepthStencilState);
		gpipe.subpass = 1932414;
		set_invalid_pointer(gpipe.pViewportState);
		set_invalid_pointer(gpipe.pRasterizationState);
		set_invalid_pointer(gpipe.pTessellationState);
		set_invalid_pointer(gpipe.pStages);
		gpipe.stageCount = 3243;
		gpipe.renderPass = fake_handle<VkRenderPass>(234234234);
		gpipe.layout = fake_handle<VkPipelineLayout>(234234235);

		// State we care about:
		// Vertex input state
		// Input assembly state
		// Dynamic state
		Hash hash[4];
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[0]))
			abort();
		if (hash[0] != baseline_hash)
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1999), gpipe, nullptr, 0))
			abort();
		gpipe.pVertexInputState = &vi;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[1]))
			abort();
		if (hash[1] == hash[0])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2000), gpipe, nullptr, 0))
			abort();
		gpipe.pInputAssemblyState = &ia;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[2]))
			abort();
		if (hash[2] == hash[1])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2001), gpipe, nullptr, 0))
			abort();
		gpipe.pDynamicState = &dyn;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[3]))
			abort();
		if (hash[3] == hash[2])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2002), gpipe, nullptr, 0))
			abort();
	}

	gpipe = pipe;
	library_info.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
	{
		Hash baseline_hash = 0;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &baseline_hash))
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1999), gpipe, nullptr, 0))
			abort();

		// This state must be ignored.
		set_invalid_pointer(gpipe.pDepthStencilState);
		set_invalid_pointer(gpipe.pViewportState);
		set_invalid_pointer(gpipe.pRasterizationState);
		set_invalid_pointer(gpipe.pTessellationState);
		set_invalid_pointer(gpipe.pStages);
		set_invalid_pointer(gpipe.pVertexInputState);
		set_invalid_pointer(gpipe.pInputAssemblyState);
		gpipe.stageCount = 3243;
		gpipe.layout = fake_handle<VkPipelineLayout>(234234235);

		// State we care about:
		// Multisampling
		// ColorBlend
		// Renderpass / subpass
		Hash hash[4];
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[0]))
			abort();
		if (hash[0] != baseline_hash)
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1999), gpipe, nullptr, 0))
			abort();
		gpipe.pMultisampleState = &ms;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[1]))
			abort();
		if (hash[1] == hash[0])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2000), gpipe, nullptr, 0))
			abort();
		gpipe.pColorBlendState = &cb;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[2]))
			abort();
		if (hash[2] == hash[1])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2001), gpipe, nullptr, 0))
			abort();
		gpipe.subpass = 0;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[3]))
			abort();
		if (hash[3] == hash[2])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2001), gpipe, nullptr, 0))
			abort();
	}

	gpipe = pipe;
	library_info.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
	{
		gpipe.renderPass = VK_NULL_HANDLE;

		Hash baseline_hash = 0;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &baseline_hash))
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1999), gpipe, nullptr, 0))
			abort();

		// This state must be ignored.
		set_invalid_pointer(gpipe.pColorBlendState);
		set_invalid_pointer(gpipe.pViewportState);
		set_invalid_pointer(gpipe.pRasterizationState);
		set_invalid_pointer(gpipe.pTessellationState);
		set_invalid_pointer(gpipe.pVertexInputState);
		set_invalid_pointer(gpipe.pInputAssemblyState);

		// State we care about:
		// Layout
		// Multisampling
		// Renderpass / subpass
		// Depth-stencil
		// Shaders
		Hash hash[6];
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[0]))
			abort();
		if (hash[0] != baseline_hash)
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2000), gpipe, nullptr, 0))
			abort();
		gpipe.layout = VK_NULL_HANDLE;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[1]))
			abort();
		if (hash[1] == hash[0])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2001), gpipe, nullptr, 0))
			abort();
		gpipe.pMultisampleState = &ms;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[2]))
			abort();
		if (hash[2] == hash[1])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2002), gpipe, nullptr, 0))
			abort();
		gpipe.subpass = 0;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[3]))
			abort();
		if (hash[3] == hash[2])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2003), gpipe, nullptr, 0))
			abort();
		gpipe.pDepthStencilState = &ds;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[4]))
			abort();
		if (hash[4] == hash[3])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2004), gpipe, nullptr, 0))
			abort();
		stages[0].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[5]))
			abort();
		if (hash[5] == hash[4])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2005), gpipe, nullptr, 0))
			abort();
	}

	gpipe = pipe;
	library_info.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
	{
		Hash baseline_hash = 0;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &baseline_hash))
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1999), gpipe, nullptr, 0))
			abort();

		// This state must be ignored.
		set_invalid_pointer(gpipe.pColorBlendState);
		set_invalid_pointer(gpipe.pMultisampleState);
		set_invalid_pointer(gpipe.pDepthStencilState);
		set_invalid_pointer(gpipe.pVertexInputState);
		set_invalid_pointer(gpipe.pInputAssemblyState);

		// State we care about:
		// Layout
		// Viewport
		// Renderpass / subpass
		// Rasterization
		// Tessellation
		// Shaders
		Hash hash[7];
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[0]))
			abort();
		if (hash[0] != baseline_hash)
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2000), gpipe, nullptr, 0))
			abort();
		gpipe.layout = VK_NULL_HANDLE;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[1]))
			abort();
		if (hash[1] == hash[0])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2001), gpipe, nullptr, 0))
			abort();
		gpipe.pViewportState = &vp;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[2]))
			abort();
		if (hash[2] == hash[1])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2002), gpipe, nullptr, 0))
			abort();
		gpipe.subpass = 0;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[3]))
			abort();
		if (hash[3] == hash[2])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2003), gpipe, nullptr, 0))
			abort();
		gpipe.pRasterizationState = &rs;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[4]))
			abort();
		if (hash[4] == hash[3])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2004), gpipe, nullptr, 0))
			abort();
		stages[0].stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
		gpipe.pTessellationState = &tes;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[5]))
			abort();
		if (hash[5] == hash[4])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2005), gpipe, nullptr, 0))
			abort();
		stages[0].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[6]))
			abort();
		if (hash[6] == hash[5])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(2006), gpipe, nullptr, 0))
			abort();
	}

	gpipe = pipe;
	{
		gpipe.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
		VkPipelineLibraryCreateInfoKHR libs = { VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
		VkPipeline libraries[4] = {
			fake_handle<VkPipeline>(1999),
			fake_handle<VkPipeline>(2000),
			fake_handle<VkPipeline>(2001),
			fake_handle<VkPipeline>(2002),
		};
		libs.libraryCount = 3;
		libs.pLibraries = libraries;
		gpipe.pNext = &libs;

		Hash hash[3];
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[0]))
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(3000), gpipe, nullptr, 0))
			abort();
		libs.libraryCount = 4;
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[1]))
			abort();
		if (hash[0] == hash[1])
			abort();
		libraries[3] = fake_handle<VkPipeline>(2003);
		if (!Hashing::compute_hash_graphics_pipeline(recorder, gpipe, &hash[2]))
			abort();
		if (hash[2] == hash[1])
			abort();
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(3001), gpipe, nullptr, 0))
			abort();
	}
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
	rs.rasterizerDiscardEnable = VK_FALSE;
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

	VkPipelineColorWriteCreateInfoEXT color_write =
			{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT };
	VkBool32 color_write_enables[1] = { };
	color_write_enables[0] = VK_TRUE;
	color_write.attachmentCount = 1;
	color_write.pColorWriteEnables = color_write_enables;
	advanced.pNext = &color_write;

	VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex =
			{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT };
	provoking_vertex.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
	line_state.pNext = &provoking_vertex;

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

	VkPipelineCreationFeedbackCreateInfoEXT feedback = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT };

	Hash hash[2];
	if (!Hashing::compute_hash_graphics_pipeline(recorder, pipe, &hash[0]))
		abort();
	pipe.pNext = &feedback;
	if (!Hashing::compute_hash_graphics_pipeline(recorder, pipe, &hash[1]))
		abort();

	if (hash[0] != hash[1])
		abort();

	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100002), pipe, nullptr, 0))
		abort();

	VkPipelineDiscardRectangleStateCreateInfoEXT discard_rectangle =
			{ VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT };
	VkRect2D discard_rectangles[1] = {};
	discard_rectangles[0].offset.x = 0;
	discard_rectangles[0].offset.y = 0;
	discard_rectangles[0].extent.width = 32;
	discard_rectangles[0].extent.height = 32;
	discard_rectangle.flags = 0;
	discard_rectangle.discardRectangleMode = VK_DISCARD_RECTANGLE_MODE_EXCLUSIVE_EXT;
	discard_rectangle.discardRectangleCount = 1;
	discard_rectangle.pDiscardRectangles = discard_rectangles;
	pipe.pNext = &discard_rectangle;

	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100003), pipe, nullptr, 0))
		abort();

	VkPipelineFragmentShadingRateStateCreateInfoKHR fragment_shading_rate =
			{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR };
	fragment_shading_rate.fragmentSize.width = 2;
	fragment_shading_rate.fragmentSize.height = 2;
	fragment_shading_rate.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
	fragment_shading_rate.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
	pipe.pNext = &fragment_shading_rate;

	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100004), pipe, nullptr, 0))
		abort();

	VkPipelineSampleLocationsStateCreateInfoEXT sample_location = { VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT };
	sample_location.sampleLocationsInfo.sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT;
	sample_location.sampleLocationsInfo.sampleLocationGridSize = { 2, 3 };
	sample_location.sampleLocationsInfo.sampleLocationsPerPixel = VK_SAMPLE_COUNT_2_BIT;
	sample_location.sampleLocationsInfo.sampleLocationsCount = 2;
	const VkSampleLocationEXT locations[2] = {{ 0.125f, 0.5f }, { -0.25f, 0.25f }};
	sample_location.sampleLocationsInfo.pSampleLocations = locations;
	ms.pNext = &sample_location;
	pipe.basePipelineHandle = VK_NULL_HANDLE;

	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100005), pipe, nullptr, 0))
		abort();
	sample_location.sampleLocationsEnable = VK_TRUE;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100006), pipe, nullptr, 0))
		abort();

	VkPipelineViewportDepthClipControlCreateInfoEXT clip_control = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT };
	clip_control.negativeOneToOne = VK_FALSE;
	vp.pNext = &clip_control;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100007), pipe, nullptr, 0))
		abort();
	clip_control.negativeOneToOne = VK_TRUE;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100008), pipe, nullptr, 0))
		abort();

	VkPipelineRobustnessCreateInfoEXT robustness_info = { VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT };
	robustness_info.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT;
	robustness_info.uniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT;
	robustness_info.storageBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT;
	robustness_info.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT;
	pipe.pNext = &robustness_info;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100009), pipe, nullptr, 0))
		abort();
	robustness_info.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100010), pipe, nullptr, 0))
		abort();

	VkDepthBiasRepresentationInfoEXT depth_bias_representation_info =
			{ VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT };
	depth_bias_representation_info.depthBiasRepresentation =
			VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
	depth_bias_representation_info.depthBiasExact = VK_TRUE;
	rs.pNext = &depth_bias_representation_info;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100011), pipe, nullptr, 0))
		abort();

	VkPipelineViewportDepthClampControlCreateInfoEXT clamp_control = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT };
	vp.pNext = &clamp_control;
	clamp_control.depthClampMode = VK_DEPTH_CLAMP_MODE_VIEWPORT_RANGE_EXT;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100012), pipe, nullptr, 0))
		abort();
	clamp_control.depthClampMode = VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT;
	VkDepthClampRangeEXT range = { 0.5f, 0.8f };
	clamp_control.pDepthClampRange = &range;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100013), pipe, nullptr, 0))
		abort();
	range = { 0.8f, 0.2f };
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100014), pipe, nullptr, 0))
		abort();

	VkRenderingAttachmentLocationInfoKHR attachment_location_info = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR };
	pipe.pNext = &attachment_location_info;
	attachment_location_info.colorAttachmentCount = 1;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100015), pipe, nullptr, 0))
		abort();
	attachment_location_info.colorAttachmentCount = 2;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100016), pipe, nullptr, 0))
		abort();

	const uint32_t color_locs[] = { 5, 4, UINT32_MAX };
	attachment_location_info.pColorAttachmentLocations = color_locs;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100017), pipe, nullptr, 0))
		abort();

	VkRenderingInputAttachmentIndexInfoKHR input_attachment_info = { VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR };
	input_attachment_info.colorAttachmentCount = 2;
	pipe.pNext = &input_attachment_info;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100018), pipe, nullptr, 0))
		abort();
	input_attachment_info.pColorAttachmentInputIndices = color_locs;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100019), pipe, nullptr, 0))
		abort();
	input_attachment_info.pDepthInputAttachmentIndex = color_locs + 1;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100020), pipe, nullptr, 0))
		abort();
	input_attachment_info.pStencilInputAttachmentIndex = color_locs + 2;
	if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(100021), pipe, nullptr, 0))
		abort();
}

static void record_raytracing_pipelines(StateRecorder &recorder)
{
	VkRayTracingPipelineCreateInfoKHR pipe = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };

	pipe.maxPipelineRayRecursionDepth = 3;
	pipe.flags = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
	pipe.layout = fake_handle<VkPipelineLayout>(10002);

	VkSpecializationInfo spec = {};
	VkSpecializationMapEntry map_entry = {};
	const uint32_t spec_data = 100;
	spec.dataSize = 4;
	spec.mapEntryCount = 1;
	spec.pData = &spec_data;
	map_entry.offset = 0;
	map_entry.size = 4;
	map_entry.constantID = 5;
	spec.pMapEntries = &map_entry;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	pipe.stageCount = 2;
	pipe.pStages = stages;
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	stages[0].pName = "gen";
	stages[0].module = fake_handle<VkShaderModule>(5000);
	stages[0].pSpecializationInfo = &spec;
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
	stages[1].pName = "miss";
	stages[1].module = fake_handle<VkShaderModule>(5001);
	stages[1].pSpecializationInfo = &spec;

	VkRayTracingShaderGroupCreateInfoKHR groups[2] = {};
	pipe.groupCount = 2;
	pipe.pGroups = groups;
	groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
	groups[0].closestHitShader = 1;
	groups[0].anyHitShader = 2;
	groups[0].generalShader = 3;
	groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
	groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
	groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
	groups[1].closestHitShader = 4;
	groups[1].anyHitShader = 5;
	groups[1].generalShader = 6;
	groups[1].intersectionShader = 7;

	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(40), pipe, nullptr, 0))
		abort();

	pipe.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
	pipe.basePipelineHandle = fake_handle<VkPipeline>(40);
	pipe.basePipelineIndex = -1;
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(41), pipe, nullptr, 0))
		abort();

	VkRayTracingPipelineInterfaceCreateInfoKHR iface = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR };
	iface.maxPipelineRayHitAttributeSize = 8;
	iface.maxPipelineRayPayloadSize = 12;
	pipe.pLibraryInterface = &iface;
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(42), pipe, nullptr, 0))
		abort();

	VkPipelineLibraryCreateInfoKHR lib = { VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
	lib.libraryCount = 2;
	const VkPipeline lib_pipelines[] = { fake_handle<VkPipeline>(40), fake_handle<VkPipeline>(41) };
	lib.pLibraries = lib_pipelines;
	pipe.pLibraryInfo = &lib;
	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(43), pipe, nullptr, 0))
		abort();

	VkPipelineCreationFeedbackCreateInfoEXT feedback = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT };

	Hash hash[2];
	if (!Hashing::compute_hash_raytracing_pipeline(recorder, pipe, &hash[0]))
		abort();
	pipe.pNext = &feedback;
	if (!Hashing::compute_hash_raytracing_pipeline(recorder, pipe, &hash[1]))
		abort();

	if (hash[0] != hash[1])
		abort();

	if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(44), pipe, nullptr, 0))
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

	remove(".__test_tmp.foz");
	remove(".__test_tmp_copy.foz");

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

	remove(".__test_concurrent.foz");
	remove(".__test_concurrent.1.foz");
	remove(".__test_concurrent.2.foz");
	remove(".__test_concurrent.3.foz");
	remove(".__test_concurrent.4.foz");

	return true;
}

static bool test_concurrent_database_bucket_touch()
{
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                         DatabaseMode::Append, nullptr, 0));
		if (!db->set_bucket_path("buck", "base"))
			return false;
		if (!db->prepare())
			return false;
	}

	if (!Path::touch(".__test_concurrent.tmp"))
		return false;
	// This should fail.
	if (Path::mkdir(".__test_concurrent.tmp"))
		return false;
	remove(".__test_concurrent.tmp");

	if (!Path::is_directory(".__test_concurrent.buck"))
		return false;
	if (!Path::is_file(".__test_concurrent.buck/TOUCH"))
		return false;

	uint64_t current_mtime;
	if (!Path::get_mtime_us(".__test_concurrent.buck/TOUCH", current_mtime))
		return false;

#ifdef __APPLE__
	// Hack for now since there was no obvious way to get accurate mtime, it's only second granularity.
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
#else
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                         DatabaseMode::Append, nullptr, 0));
		if (!db->set_bucket_path("buck", "base"))
			return false;
		if (!db->prepare())
			return false;
	}

	uint64_t update_mtime;
	if (!Path::get_mtime_us(".__test_concurrent.buck/TOUCH", update_mtime))
		return false;

	if (update_mtime == current_mtime)
		return false;

	// Makes sure that we can spam TOUCH concurrently without issue.
	auto touch_lambda = []() -> bool {
		for (unsigned i = 0; i < 1024; i++)
			if (!Path::touch(".__test_concurrent.buck/TOUCH2"))
				return false;
		return true;
	};
	std::vector<std::future<bool>> futures;
	for (unsigned i = 0; i < 16; i++)
		futures.push_back(std::async(std::launch::async, touch_lambda));
	for (auto &fut : futures)
		if (!fut.get())
			return false;

	if (!Path::is_file(".__test_concurrent.buck/TOUCH2"))
		return false;

	remove(".__test_concurrent.buck/TOUCH");
	remove(".__test_concurrent.buck/TOUCH2");
	remove(".__test_concurrent.buck");
	return true;
}

static bool test_concurrent_database_bucket_write()
{
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                        DatabaseMode::Append,
		                                                                        nullptr, 0));
		if (!db->set_bucket_path("buck", "base"))
			return false;
		if (!db->prepare())
			return false;

		const uint8_t blob[] = { 1, 2, 3 };
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 100, blob, sizeof(blob), 0))
			return false;
	}

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_concurrent.buck/base.1.foz",
		                                                                            DatabaseMode::ReadOnly));
		if (!db->prepare())
			return false;

		if (!db->has_entry(RESOURCE_SHADER_MODULE, 100))
			return false;

		size_t size;
		if (!db->read_entry(RESOURCE_SHADER_MODULE, 100, &size, nullptr, 0) || size != 3)
			return false;
	}

	remove(".__test_concurrent.buck/TOUCH");
	remove(".__test_concurrent.buck/base.1.foz");
	remove(".__test_concurrent.buck");
	return true;
}

static bool test_concurrent_database_bucket_extra_paths()
{
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(
				".__test_concurrent", DatabaseMode::Append,
				nullptr, 0));
		if (!db->set_bucket_path("buck", "base"))
			return false;
		if (!db->prepare())
			return false;

		const uint8_t blob[] = { 1, 2, 3 };
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 100, blob, sizeof(blob), 0))
			return false;
	}

	{
		const char *extra_read_only = "$bucketdir/base.1.foz";
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database_with_encoded_extra_paths(
				".__test_concurrent", DatabaseMode::Append, extra_read_only));
		if (!db->set_bucket_path("buck", "base-other"))
			return false;
		if (!db->prepare())
			return false;

		const uint8_t blob[] = { 1, 2, 3 };
		// This should be ignored since it's already in the read-only archive.
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 100, blob, sizeof(blob), 0))
			return false;
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 101, blob, sizeof(blob), 0))
			return false;
	}

	// Also check that $bucketdir is honored without buckets.
	{
		// Test that Win32 style backslash also works just in case ...
		const char *extra_read_only = "$bucketdir\\base.1.foz";
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database_with_encoded_extra_paths(
				".__test_concurrent.buck/base-other", DatabaseMode::Append, extra_read_only));
		if (!db->prepare())
			return false;

		const uint8_t blob[] = { 1, 2, 3 };
		// This should be ignored since it's already in the read-only archive.
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 100, blob, sizeof(blob), 0))
			return false;
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 102, blob, sizeof(blob), 0))
			return false;
	}

	// Sanity check
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(
				".__test_concurrent.buck/base.1.foz", DatabaseMode::ReadOnly));
		if (!db->prepare())
			return false;

		if (!db->has_entry(RESOURCE_SHADER_MODULE, 100))
			return false;
		if (db->has_entry(RESOURCE_SHADER_MODULE, 101))
			return false;
		if (db->has_entry(RESOURCE_SHADER_MODULE, 102))
			return false;
	}

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(
				".__test_concurrent.buck/base-other.1.foz", DatabaseMode::ReadOnly));
		if (!db->prepare())
			return false;

		if (db->has_entry(RESOURCE_SHADER_MODULE, 100))
			return false;
		if (!db->has_entry(RESOURCE_SHADER_MODULE, 101))
			return false;
	}

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(
				".__test_concurrent.buck/base-other.2.foz", DatabaseMode::ReadOnly));
		if (!db->prepare())
			return false;

		if (db->has_entry(RESOURCE_SHADER_MODULE, 100))
			return false;
		if (!db->has_entry(RESOURCE_SHADER_MODULE, 102))
			return false;
	}

	remove(".__test_concurrent.buck/TOUCH");
	remove(".__test_concurrent.buck/base.1.foz");
	remove(".__test_concurrent.buck/base-other.1.foz");
	remove(".__test_concurrent.buck/base-other.2.foz");
	remove(".__test_concurrent.buck");
	return true;
}

static bool test_concurrent_database_bucket_append()
{
	for (unsigned i = 0; i < 3; i++)
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                        DatabaseMode::Append, nullptr, 0));
		if (!db->set_bucket_path("buck", "base"))
			return false;
		if (!db->prepare())
			return false;

		const uint8_t blob[] = { 1, 2, 3 };
		if (!db->write_entry(RESOURCE_SHADER_MODULE, 100 + i, blob, sizeof(blob), 0))
			return false;
	}

	const char *source_paths[3] = {
		".__test_concurrent.buck/base.1.foz",
		".__test_concurrent.buck/base.2.foz",
		".__test_concurrent.buck/base.3.foz",
	};

	// Verify that extra paths do not get redirected.
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                        DatabaseMode::Append,
		                                                                        source_paths, 3));
		if (!db->set_bucket_path("buck", "base"))
			return false;
		if (!db->prepare())
			return false;

		const uint8_t blob[] = { 1, 2, 3 };
		for (unsigned i = 0; i < 4; i++)
		{
			if (i == 3)
			{
				if (Path::is_file(".__test_concurrent.buck/base.4.foz"))
					return false;
			}

			if (!db->write_entry(RESOURCE_SHADER_MODULE, 100 + i, blob, sizeof(blob), 0))
				return false;

			if (i == 3)
			{
				// It should have been written now ...
				if (!Path::is_file(".__test_concurrent.buck/base.4.foz"))
					return false;
			}
		}
	}

	if (!merge_concurrent_databases(".__test_concurrent.buck/base.foz", source_paths, 3))
		return false;

	remove(".__test_concurrent.buck/base.1.foz");
	remove(".__test_concurrent.buck/base.2.foz");
	remove(".__test_concurrent.buck/base.3.foz");
	remove(".__test_concurrent.buck/base.4.foz");

	// Verify that read-only *is* redirected.
	{
		auto db = std::unique_ptr<DatabaseInterface>(create_concurrent_database(".__test_concurrent",
		                                                                        DatabaseMode::Append, nullptr, 0));
		if (!db->set_bucket_path("buck", "base"))
			return false;
		if (!db->prepare())
			return false;

		const uint8_t blob[] = { 1, 2, 3 };
		for (unsigned i = 0; i < 4; i++)
		{
			if (i == 3)
			{
				if (Path::is_file(".__test_concurrent.buck/base.1.foz"))
					return false;
			}

			if (!db->write_entry(RESOURCE_SHADER_MODULE, 100 + i, blob, sizeof(blob), 0))
				return false;

			if (i == 3)
			{
				// It should have been written now ...
				if (!Path::is_file(".__test_concurrent.buck/base.1.foz"))
					return false;
			}
		}
	}

	remove(".__test_concurrent.buck/TOUCH");
	remove(".__test_concurrent.buck/base.1.foz");
	remove(".__test_concurrent.buck/base.foz");
	remove(".__test_concurrent.buck");
	return true;
}

static bool test_concurrent_database_bucket()
{
	if (!test_concurrent_database_bucket_touch())
		return false;
	if (!test_concurrent_database_bucket_write())
		return false;
	if (!test_concurrent_database_bucket_append())
		return false;
	if (!test_concurrent_database_bucket_extra_paths())
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
	VkSamplerYcbcrConversionInfo conv_info = { VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO };
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

		if (userdata.warn_count != 0 || userdata.err_count != 0 || userdata.info_count != 0)
			return false;

		// Should succeed, but will fail later when trying to resolve sampler.
		if (!recorder.record_descriptor_set_layout(fake_handle<VkDescriptorSetLayout>(10), set_layout, 200))
			return false;

		recorder.tear_down_recording_thread();
		LOGI("=======================\n");

		unsigned expected_warn = i < 2 ? 1 : 0;
		if (userdata.warn_count != expected_warn || userdata.err_count != 0 || userdata.info_count != 0)
			return false;
	}
	remove(".__test_archive.foz");
	set_thread_log_callback(nullptr, nullptr);
	return true;
}

static bool test_pnext_shader_module_hashing()
{
	StateRecorder recorder;

	static const uint32_t code[] = { 1, 2, 3 };
	VkShaderModuleCreateInfo module = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	module.pCode = code;
	module.codeSize = sizeof(code);

	if (!recorder.record_shader_module(fake_handle<VkShaderModule>(1), module))
		return false;

	VkComputePipelineCreateInfo pso = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	VkGraphicsPipelineCreateInfo gpso = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pso.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pso.stage.module = fake_handle<VkShaderModule>(1);
	pso.stage.pName = "main";
	gpso.stageCount = 1;
	gpso.pStages = &pso.stage;

	Hash hash_graphics0 = 0, hash_graphics1 = 0;
	Hash hash_compute0 = 0, hash_compute1 = 0;
	if (!Hashing::compute_hash_compute_pipeline(recorder, pso, &hash_compute0))
		return false;
	if (!Hashing::compute_hash_graphics_pipeline(recorder, gpso, &hash_graphics0))
		return false;

	pso.stage.pNext = &module;
	pso.stage.module = VK_NULL_HANDLE;

	if (!Hashing::compute_hash_compute_pipeline(recorder, pso, &hash_compute1))
		return false;
	if (!Hashing::compute_hash_graphics_pipeline(recorder, gpso, &hash_graphics1))
		return false;

	if (hash_compute0 != hash_compute1)
		return false;
	if (hash_graphics0 != hash_graphics1)
		return false;

	VkShaderModuleValidationCacheCreateInfoEXT validation_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT };
	module.pNext = &validation_info;
	Hash hash_compute2 = 0, hash_graphics2 = 0;
	if (!Hashing::compute_hash_compute_pipeline(recorder, pso, &hash_compute2))
		return false;
	if (!Hashing::compute_hash_graphics_pipeline(recorder, gpso, &hash_graphics2))
		return false;

	if (hash_compute1 != hash_compute2)
		return false;
	if (hash_graphics1 != hash_graphics2)
		return false;

	return true;
}

static bool test_pdf_recording(const void *device_pnext, Hash &hash)
{
	hash = 0;
	StateRecorder recorder;
	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.apiVersion = VK_API_VERSION_1_1;
	if (!recorder.record_application_info(app_info))
		return false;
	if (!recorder.record_physical_device_features(device_pnext))
		return false;

	hash = recorder.get_application_feature_hash().physical_device_features_hash;

	uint8_t *serialized;
	size_t serialized_size;
	if (!recorder.serialize(&serialized, &serialized_size))
		return false;

	StateReplayer replayer;
	ReplayInterface iface;

	std::string serialized_str(serialized, serialized + serialized_size);
	LOGI("Serialized:\n%s\n", serialized_str.c_str());

	if (!replayer.parse(iface, nullptr, serialized, serialized_size))
		return false;

	StateRecorder::free_serialized(serialized);
	return true;
}

static bool test_pdf_recording()
{
	Hash h;
	// Expect to fail here.
	if (test_pdf_recording(nullptr, h))
		return false;

	VkPhysicalDeviceFeatures2 pdf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPhysicalDeviceDepthClipEnableFeaturesEXT dummy = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT };
	VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, nullptr, 1, 2, 3 };
	VkPhysicalDeviceImageRobustnessFeaturesEXT image_robustness = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT, nullptr, 4 };
	VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV shading_rate_enums = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV, nullptr, 5, 6, 7 };
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading_rate = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, nullptr, 8, 9, 10 };
	VkPhysicalDeviceMeshShaderFeaturesEXT mesh = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, nullptr, 10, 20, 30, 40, 50 };
	VkPhysicalDeviceMeshShaderFeaturesNV mesh_nv = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV, nullptr, 80, 90 };
	VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, nullptr, 100, 200, 300, 400 };
	VkPhysicalDeviceShaderObjectFeaturesEXT shader_object = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, nullptr, 500 };
	VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT prim_generated_query = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, nullptr, 501, 502, 503 };
	VkPhysicalDeviceImage2DViewOf3DFeaturesEXT image_2d_view_of_3d = {
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, nullptr, 504 };

	// Expect to fail here.
	if (test_pdf_recording(&dummy, h))
		return false;

	// Expect to fail here.
	if (test_pdf_recording(&robustness2, h))
		return false;

	{
		// Hash for all of these need to be invariant.
		// dummy is ignored.
		Hash h0, h1, h2;

		if (!test_pdf_recording(&pdf2, h0))
			return false;
		pdf2.pNext = &dummy;
		if (!test_pdf_recording(&pdf2, h1))
			return false;
		dummy.pNext = &pdf2;
		pdf2.pNext = nullptr;
		if (!test_pdf_recording(&pdf2, h2))
			return false;

		if (h0 != h1 || h1 != h2)
			return false;
	}

	{
		constexpr size_t hash_count = 11;
		Hash hashes[hash_count] = {};

		pdf2.pNext = nullptr;
		if (!test_pdf_recording(&pdf2, hashes[0]))
			return false;
		pdf2.pNext = &robustness2;
		if (!test_pdf_recording(&pdf2, hashes[1]))
			return false;
		robustness2.pNext = &image_robustness;
		if (!test_pdf_recording(&pdf2, hashes[2]))
			return false;
		image_robustness.pNext = &shading_rate_enums;
		if (!test_pdf_recording(&pdf2, hashes[3]))
			return false;
		shading_rate_enums.pNext = &shading_rate;
		if (!test_pdf_recording(&pdf2, hashes[4]))
			return false;
		shading_rate.pNext = &mesh;
		if (!test_pdf_recording(&pdf2, hashes[5]))
			return false;
		mesh.pNext = &mesh_nv;
		if (!test_pdf_recording(&pdf2, hashes[6]))
			return false;
		mesh_nv.pNext = &descriptor_buffer;
		if (!test_pdf_recording(&pdf2, hashes[7]))
			return false;
		descriptor_buffer.pNext = &shader_object;
		if (!test_pdf_recording(&pdf2, hashes[8]))
			return false;
		shader_object.pNext = &prim_generated_query;
		if (!test_pdf_recording(&pdf2, hashes[9]))
			return false;
		prim_generated_query.pNext = &image_2d_view_of_3d;
		if (!test_pdf_recording(&pdf2, hashes[10]))
			return false;

		// Make sure all of these are serialized.
		for (unsigned i = 1; i < hash_count; i++)
			if (hashes[i] == hashes[i - 1])
				return false;

		// If we move PDF2 last, hash should still be invariant.
		image_2d_view_of_3d.pNext = &pdf2;
		pdf2.pNext = nullptr;
		if (!test_pdf_recording(&robustness2, hashes[0]))
			return false;

		if (hashes[0] != hashes[hash_count - 1])
			return false;
	}

	return true;
}

static bool test_reused_handles()
{
	std::vector<Hash> expect_pass[RESOURCE_COUNT];
	std::vector<Hash> expect_fail[RESOURCE_COUNT];

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_tmp.foz", DatabaseMode::OverWrite));
		if (!db)
			return false;

		StateRecorder recorder;

		VkDeviceCreateInfo dummy_pnext = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

		auto fake_sampler = fake_handle<VkSampler>(1);
		auto fake_set_layout = fake_handle<VkDescriptorSetLayout>(1);
		auto fake_layout = fake_handle<VkPipelineLayout>(1);
		auto fake_sampler2 = fake_handle<VkSampler>(2);
		auto fake_set_layout2 = fake_handle<VkDescriptorSetLayout>(2);
		auto fake_layout2 = fake_handle<VkPipelineLayout>(2);
		auto fake_layout3 = fake_handle<VkPipelineLayout>(3);

		auto fake_render_pass = fake_handle<VkRenderPass>(1);
		auto fake_render_pass2 = fake_handle<VkRenderPass>(2);
		auto fake_module = fake_handle<VkShaderModule>(1);
		auto fake_pipe = fake_handle<VkPipeline>(1);
		auto fake_pipe2 = fake_handle<VkPipeline>(2);

		VkSamplerCreateInfo samp = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		VkDescriptorSetLayoutCreateInfo set_layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		VkShaderModuleCreateInfo module_info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		VkRenderPassCreateInfo pass_info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		VkRenderPassCreateInfo2 pass_info2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
		VkGraphicsPipelineCreateInfo gpipe = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		VkComputePipelineCreateInfo cpipe = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		VkDescriptorSetLayoutBinding binding = {};
		VkSubpassDescription subpass = {};
		VkSubpassDescription2 subpass2 = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 };

		binding.pImmutableSamplers = &fake_sampler;
		binding.descriptorCount = 1;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;

		set_layout_info.bindingCount = 1;
		set_layout_info.pBindings = &binding;

		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &fake_set_layout;

		pass_info.subpassCount = 1;
		pass_info.pSubpasses = &subpass;

		pass_info2.subpassCount = 1;
		pass_info2.pSubpasses = &subpass2;

		uint32_t blank = 0xf00f;
		module_info.codeSize = sizeof(blank);
		module_info.pCode = &blank;

		recorder.init_recording_thread(db.get());

		if (!recorder.record_sampler(fake_sampler, samp, 1))
			return false;
		if (!recorder.record_sampler(fake_sampler2, samp, 1))
			return false;
		expect_pass[RESOURCE_SAMPLER].push_back(1);
		if (!recorder.record_descriptor_set_layout(fake_set_layout, set_layout_info, 1))
			return false;
		if (!recorder.record_descriptor_set_layout(fake_set_layout2, set_layout_info, 1))
			return false;
		expect_pass[RESOURCE_DESCRIPTOR_SET_LAYOUT].push_back(1);
		if (!recorder.record_pipeline_layout(fake_layout, layout_info, 1))
			return false;
		if (!recorder.record_pipeline_layout(fake_layout2, layout_info, 1))
			return false;
		if (!recorder.record_pipeline_layout(fake_layout3, layout_info, 1))
			return false;
		expect_pass[RESOURCE_PIPELINE_LAYOUT].push_back(1);
		if (!recorder.record_render_pass(fake_render_pass, pass_info, 1))
			return false;
		expect_pass[RESOURCE_RENDER_PASS].push_back(1);
		if (!recorder.record_render_pass2(fake_render_pass2, pass_info2, 2))
			return false;
		expect_pass[RESOURCE_RENDER_PASS].push_back(2);
		if (!recorder.record_shader_module(fake_module, module_info, 1))
			return false;
		expect_pass[RESOURCE_SHADER_MODULE].push_back(1);

		gpipe.layout = fake_layout;
		gpipe.renderPass = fake_render_pass;
		if (!recorder.record_graphics_pipeline(fake_pipe, gpipe, nullptr, 0, 1))
			return false;
		expect_pass[RESOURCE_GRAPHICS_PIPELINE].push_back(1);
		gpipe.renderPass = fake_render_pass2;
		if (!recorder.record_graphics_pipeline(fake_pipe2, gpipe, nullptr, 0, 2))
			return false;
		expect_pass[RESOURCE_GRAPHICS_PIPELINE].push_back(2);

		cpipe.stage.module = fake_module;
		cpipe.stage.pName = "main";
		cpipe.layout = fake_layout3;
		if (!recorder.record_compute_pipeline(fake_pipe, cpipe, nullptr, 0, 1))
			return false;
		expect_pass[RESOURCE_COMPUTE_PIPELINE].push_back(1);

		auto gpipe_derived = gpipe;
		auto cpipe_derived = cpipe;
		gpipe_derived.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
		cpipe_derived.flags = VK_PIPELINE_CREATE_DERIVATIVE_BIT;
		gpipe_derived.basePipelineHandle = fake_pipe;
		cpipe_derived.basePipelineHandle = fake_pipe;
		if (!recorder.record_graphics_pipeline(fake_pipe2, gpipe_derived, nullptr, 0, 1000))
			return false;
		expect_pass[RESOURCE_GRAPHICS_PIPELINE].push_back(1000);
		if (!recorder.record_compute_pipeline(fake_pipe2, cpipe_derived, nullptr, 0, 1000))
			return false;
		expect_pass[RESOURCE_COMPUTE_PIPELINE].push_back(1000);

		// Record a bogus sampler with reused API handle.
		samp.pNext = &dummy_pnext;
		if (recorder.record_sampler(fake_sampler, samp, 2))
			return false;
		expect_fail[RESOURCE_SAMPLER].push_back(2);

		// This descriptor set layout record should fail in serialization because of invalid sampler reference.
		if (!recorder.record_descriptor_set_layout(fake_set_layout, set_layout_info, 2))
			return false;
		expect_fail[RESOURCE_DESCRIPTOR_SET_LAYOUT].push_back(2);
		set_layout_info.pNext = &dummy_pnext;
		if (recorder.record_descriptor_set_layout(fake_set_layout2, set_layout_info, 3))
			return false;
		expect_fail[RESOURCE_DESCRIPTOR_SET_LAYOUT].push_back(3);

		// This pipeline layout will fail in serialization because of invalid descriptor set reference.
		if (!recorder.record_pipeline_layout(fake_layout, layout_info, 2))
			return false;
		expect_fail[RESOURCE_PIPELINE_LAYOUT].push_back(2);

		// Should fail in serialization.
		if (!recorder.record_graphics_pipeline(fake_pipe, gpipe, nullptr, 0, 3))
			return false;
		expect_fail[RESOURCE_GRAPHICS_PIPELINE].push_back(3);

		layout_info.pSetLayouts = &fake_set_layout2;
		if (!recorder.record_pipeline_layout(fake_layout2, layout_info, 3))
			return false;
		expect_fail[RESOURCE_PIPELINE_LAYOUT].push_back(3);

		// Should also fail in serialization.
		gpipe.layout = fake_layout2;
		if (!recorder.record_graphics_pipeline(fake_pipe, gpipe, nullptr, 0, 4))
			return false;
		expect_fail[RESOURCE_GRAPHICS_PIPELINE].push_back(4);

		// Sanity check that this still works.
		gpipe.layout = fake_layout3;
		if (!recorder.record_graphics_pipeline(fake_pipe, gpipe, nullptr, 0, 5))
			return false;
		expect_pass[RESOURCE_GRAPHICS_PIPELINE].push_back(5);

		pass_info.pNext = &dummy_pnext;
		subpass2.pNext = &dummy_pnext;
		if (recorder.record_render_pass(fake_render_pass, pass_info, 3))
			return false;
		expect_fail[RESOURCE_RENDER_PASS].push_back(3);
		if (recorder.record_render_pass2(fake_render_pass2, pass_info2, 4))
			return false;
		expect_fail[RESOURCE_RENDER_PASS].push_back(4);

		// Should fail in serialization.
		gpipe.renderPass = fake_render_pass;
		if (!recorder.record_graphics_pipeline(fake_pipe, gpipe, nullptr, 0, 6))
			return false;
		expect_fail[RESOURCE_GRAPHICS_PIPELINE].push_back(6);
		gpipe.renderPass = fake_render_pass2;
		if (!recorder.record_graphics_pipeline(fake_pipe, gpipe, nullptr, 0, 7))
			return false;
		expect_fail[RESOURCE_GRAPHICS_PIPELINE].push_back(7);

		module_info.pNext = &dummy_pnext;
		if (recorder.record_shader_module(fake_module, module_info, 2))
			return false;
		expect_fail[RESOURCE_SHADER_MODULE].push_back(2);

		// Should fail in serialization.
		if (!recorder.record_compute_pipeline(fake_pipe, cpipe, nullptr, 0, 2))
			return false;
		expect_fail[RESOURCE_COMPUTE_PIPELINE].push_back(2);

		// Should fail to serialize since derived pipelines are invalid.
		if (!recorder.record_graphics_pipeline(fake_pipe2, gpipe_derived, nullptr, 0, 1001))
			return false;
		expect_fail[RESOURCE_GRAPHICS_PIPELINE].push_back(1001);
		if (!recorder.record_compute_pipeline(fake_pipe2, cpipe_derived, nullptr, 0, 1001))
			return false;
		expect_fail[RESOURCE_COMPUTE_PIPELINE].push_back(1001);

		// Same.
		gpipe_derived.basePipelineHandle = fake_pipe2;
		cpipe_derived.basePipelineHandle = fake_pipe2;
		if (!recorder.record_graphics_pipeline(fake_pipe, gpipe_derived, nullptr, 0, 1002))
			return false;
		expect_fail[RESOURCE_GRAPHICS_PIPELINE].push_back(1002);
		if (!recorder.record_compute_pipeline(fake_pipe, cpipe_derived, nullptr, 0, 1002))
			return false;
		expect_fail[RESOURCE_COMPUTE_PIPELINE].push_back(1002);
	}

	{
		auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(".__test_tmp.foz", DatabaseMode::ReadOnly));
		if (!db || !db->prepare())
			return false;

		size_t size = 0;
		for (int i = 0; i < RESOURCE_COUNT; i++)
		{
			for (auto &hash : expect_pass[i])
				if (!db->read_entry(ResourceTag(i), hash, &size, nullptr, PAYLOAD_READ_NO_FLAGS))
					return false;

			for (auto &hash : expect_fail[i])
				if (db->read_entry(ResourceTag(i), hash, &size, nullptr, PAYLOAD_READ_NO_FLAGS))
					return false;
		}
	}

	remove(".__test_tmp.foz");
	return true;
}

static VKAPI_ATTR void VKAPI_CALL
fake_gsmcii(VkDevice, const VkShaderModuleCreateInfo *info, VkShaderModuleIdentifierEXT *ident)
{
	ident->identifierSize = 4;
	ident->identifier[0] = info->pCode[0];
	ident->identifier[1] = info->pCode[0];
	ident->identifier[2] = info->pCode[0];
	ident->identifier[3] = info->pCode[0];
}

static bool test_module_identifiers()
{
	// Verify that we can serialize module identifiers for all 4 scenarios.
	{
		auto ident_db = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_tmp_ident.foz", DatabaseMode::OverWrite));
		if (!ident_db)
			return false;

		auto db = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_tmp.foz", DatabaseMode::OverWrite));
		if (!db)
			return false;

		StateRecorder recorder;
		recorder.set_module_identifier_database_interface(ident_db.get());
		recorder.init_recording_thread(db.get());

		static const uint8_t ident_standalone[] = { 0xab, 0xab, 0xab, 0xab };
		const uint32_t module_blobs[] = { 0xab, 0xac, 0xad, 0xae };
		VkShaderModuleCreateInfo module =
				{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		VkPipelineShaderStageModuleIdentifierCreateInfoEXT ident_info =
				{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT };
		ident_info.pIdentifier = ident_standalone;
		ident_info.identifierSize = sizeof(ident_standalone);
		module.codeSize = sizeof(module_blobs[0]);
		module.pCode = &module_blobs[0];
		module.pNext = &ident_info;
		if (!recorder.record_shader_module(fake_handle<VkShaderModule>(1), module, 1000))
			return false;
		module.pNext = nullptr;

		VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
		stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
		stage.pName = "main";
		module.pCode = &module_blobs[1];
		stage.pNext = &module;

		VkGraphicsPipelineCreateInfo gp_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		gp_info.stageCount = 1;
		gp_info.pStages = &stage;
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1), gp_info, nullptr, 0, 0,
		                                       reinterpret_cast<VkDevice>(1), fake_gsmcii))
			return false;

		VkComputePipelineCreateInfo comp_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		module.pCode = &module_blobs[2];
		comp_info.stage = stage;
		if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1), comp_info, nullptr, 0, 0,
		                                      reinterpret_cast<VkDevice>(1), fake_gsmcii))
			return false;

		VkRayTracingPipelineCreateInfoKHR rt_info = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
		stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
		module.pCode = &module_blobs[3];
		rt_info.stageCount = 1;
		rt_info.pStages = &stage;
		if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(1), rt_info, nullptr, 0, 0,
		                                         reinterpret_cast<VkDevice>(1), fake_gsmcii))
			return false;
	}

	// Using the identifier cache, make sure that we can create pipelines purely from identifier.
	{
		auto ident_db = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_tmp_ident.foz", DatabaseMode::ReadOnly));
		if (!ident_db)
			return false;

		// Need this just to force recording.
		auto last_use_db = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_tmp_last_use.foz", DatabaseMode::OverWrite));
		if (!last_use_db)
			return false;

		auto db = std::unique_ptr<DatabaseInterface>(
				create_stream_archive_database(".__test_tmp_from_ident.foz", DatabaseMode::OverWrite));
		if (!db)
			return false;

		StateRecorder recorder;
		recorder.set_module_identifier_database_interface(ident_db.get());
		recorder.set_on_use_database_interface(last_use_db.get());
		recorder.init_recording_synchronized(db.get());

		static const uint8_t ident_standalone[] = { 0xab, 0xab, 0xab, 0xab };
		static const uint8_t ident_graphics[] = { 0xac, 0xac, 0xac, 0xac };
		static const uint8_t ident_compute[] = { 0xad, 0xad, 0xad, 0xad };
		static const uint8_t ident_rt[] = { 0xae, 0xae, 0xae, 0xae };
		static const uint8_t ident_bogus[] = { 1, 2, 3, 4 };

		VkPipelineShaderStageModuleIdentifierCreateInfoEXT ident_info =
				{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT };
		ident_info.pIdentifier = ident_standalone;
		ident_info.identifierSize = sizeof(ident_standalone);

		VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
		stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
		stage.pName = "main";
		stage.pNext = &ident_info;

		VkGraphicsPipelineCreateInfo gp_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		gp_info.stageCount = 1;
		gp_info.pStages = &stage;
		ident_info.pIdentifier = ident_graphics;
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1), gp_info, nullptr, 0))
			return false;
		// Try recording with an identifier that does not exist, it should result in no recording.
		ident_info.pIdentifier = ident_bogus;
		if (!recorder.record_graphics_pipeline(fake_handle<VkPipeline>(1), gp_info, nullptr, 0))
			return false;

		VkComputePipelineCreateInfo comp_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		comp_info.stage = stage;
		ident_info.pIdentifier = ident_compute;
		if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1), comp_info, nullptr, 0))
			return false;
		ident_info.pIdentifier = ident_bogus;
		if (!recorder.record_compute_pipeline(fake_handle<VkPipeline>(1), comp_info, nullptr, 0))
			return false;

		VkRayTracingPipelineCreateInfoKHR rt_info = { VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
		stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
		rt_info.stageCount = 1;
		rt_info.pStages = &stage;
		ident_info.pIdentifier = ident_rt;
		if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(1), rt_info, nullptr, 0))
			return false;
		ident_info.pIdentifier = ident_bogus;
		if (!recorder.record_raytracing_pipeline(fake_handle<VkPipeline>(1), rt_info, nullptr, 0))
			return false;
	}

	// Verify that the pipelines we recorded result in same hashes.
	{
		auto reference_db = create_stream_archive_database(".__test_tmp.foz", DatabaseMode::ReadOnly);
		auto ident_only_db = create_stream_archive_database(".__test_tmp_from_ident.foz", DatabaseMode::ReadOnly);
		if (!reference_db->prepare() || !ident_only_db->prepare())
			return false;

		static const ResourceTag tags[] = {
			RESOURCE_GRAPHICS_PIPELINE,
			RESOURCE_COMPUTE_PIPELINE,
			RESOURCE_RAYTRACING_PIPELINE
		};

		for (auto tag : tags)
		{
			size_t hash_count = 1;
			Hash hash_reference = 0;
			Hash hash_ident_only = 0;

			if (!reference_db->get_hash_list_for_resource_tag(tag, &hash_count,
			                                                  &hash_reference) || hash_count != 1)
				return false;

			if (!ident_only_db->get_hash_list_for_resource_tag(tag, &hash_count,
			                                                   &hash_ident_only) || hash_count != 1)
				return false;

			if (hash_reference != hash_ident_only)
				return false;
		}
	}

	// Verify that standalone shader module recording creates identifier
	{
		auto ident_db = create_stream_archive_database(".__test_tmp_ident.foz", DatabaseMode::ReadOnly);
		if (!ident_db->prepare())
			return false;

		uint8_t data[4];
		size_t data_size = 4;
		if (!ident_db->read_entry(RESOURCE_SHADER_MODULE, 1000, &data_size, data, PAYLOAD_READ_NO_FLAGS) || data_size != 4)
			return false;

		for (auto elem : data)
			if (elem != 0xab)
				return false;
	}

	// Verify that relevant last use hashes match up with recorded hashes and that their sizes are as expected.
	{
		auto last_use_db = create_stream_archive_database(".__test_tmp_last_use.foz", DatabaseMode::ReadOnly);
		auto ident_only_db = create_stream_archive_database(".__test_tmp_from_ident.foz", DatabaseMode::ReadOnly);
		if (!last_use_db->prepare() || !ident_only_db->prepare())
			return false;

		for (int i = 0; i < RESOURCE_COUNT; i++)
		{
			auto tag = ResourceTag(i);
			size_t hash_count_last_use = 0;
			size_t hash_count_ident = 0;

			if (!last_use_db->get_hash_list_for_resource_tag(tag, &hash_count_last_use, nullptr))
				return false;
			if (!ident_only_db->get_hash_list_for_resource_tag(tag, &hash_count_ident, nullptr))
				return false;

			if (tag == RESOURCE_SHADER_MODULE)
			{
				// Special consideration. If we're recording with identifier cache,
				// we expect the shader module to not be recorded, but we can record last use of the shader module.
				if (hash_count_last_use != 3)
					return false;
				if (hash_count_ident != 0)
					return false;

				continue;
			}

			if (hash_count_last_use != hash_count_ident)
				return false;

			std::vector<Hash> hashes_last_use(hash_count_last_use);
			std::vector<Hash> hashes_ident(hash_count_ident);

			if (!last_use_db->get_hash_list_for_resource_tag(tag, &hash_count_last_use, hashes_last_use.data()))
				return false;
			if (!ident_only_db->get_hash_list_for_resource_tag(tag, &hash_count_ident, hashes_ident.data()))
				return false;

			for (auto hash : hashes_last_use)
			{
				size_t blob_size = 0;
				if (!last_use_db->read_entry(tag, hash, &blob_size, nullptr, PAYLOAD_READ_NO_FLAGS) || blob_size != 8)
					return false;
			}

			std::sort(hashes_last_use.begin(), hashes_last_use.end());
			std::sort(hashes_ident.begin(), hashes_ident.end());
			if (memcmp(hashes_last_use.data(), hashes_ident.data(), hash_count_last_use * sizeof(Hash)) != 0)
				return false;
		}
	}

	remove(".__test_tmp.foz");
	remove(".__test_tmp_ident.foz");
	remove(".__test_tmp_last_use.foz");
	remove(".__test_tmp_from_ident.foz");
	return true;
}

int main()
{
	if (!test_concurrent_database_extra_paths())
		return EXIT_FAILURE;
	if (!test_concurrent_database())
		return EXIT_FAILURE;
	if (!test_concurrent_database_bucket())
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
	if (!test_pnext_shader_module_hashing())
		return EXIT_FAILURE;

	if (!test_pdf_recording())
		return EXIT_FAILURE;

	if (!test_reused_handles())
		return EXIT_FAILURE;

	if (!test_module_identifiers())
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
		if (!recorder.record_physical_device_features(&features))
			abort();

		record_samplers(recorder);
		record_set_layouts(recorder);
		record_pipeline_layouts(recorder);
		record_shader_modules(recorder);
		record_render_passes(recorder);
		record_render_passes2(recorder);
		record_compute_pipelines(recorder);
		record_graphics_pipelines(recorder);
		record_graphics_pipeline_libraries(recorder);
		record_pipelines_pnext_shader(recorder);
		record_raytracing_pipelines(recorder);
		record_graphics_pipelines_robustness(recorder);
		record_pipelines_identifier(recorder);
		record_pipeline_flags2(recorder);

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
