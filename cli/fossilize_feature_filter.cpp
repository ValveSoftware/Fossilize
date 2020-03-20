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

#include "volk.h"
#include "fossilize_feature_filter.hpp"
#include "spirv.hpp"
#include "logging.hpp"
#include <string.h>
#include <unordered_set>
#include <string>

namespace Fossilize
{
void *build_pnext_chain(VulkanFeatures &features)
{
	features = {};
	void *pNext = nullptr;
	void **ppNext = nullptr;

#define CHAIN(struct_type, member) \
	member.sType = struct_type; \
	if (!pNext) pNext = &member; \
	if (ppNext) *ppNext = &member; \
	ppNext = &member.pNext;

#define F(struct_type, member) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES, features.member)
#define FE(struct_type, member, ext) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext, features.member)

	F(16BIT_STORAGE, storage_16bit);
	F(MULTIVIEW, multiview);
	F(VARIABLE_POINTERS, variable_pointers);
	F(SAMPLER_YCBCR_CONVERSION, ycbcr_conversion);
	F(SHADER_DRAW_PARAMETERS, draw_parameters);
	F(8BIT_STORAGE, storage_8bit);
	F(SHADER_ATOMIC_INT64, atomic_int64);
	F(SHADER_FLOAT16_INT8, float16_int8);
	F(DESCRIPTOR_INDEXING, descriptor_indexing);
	F(VULKAN_MEMORY_MODEL, memory_model);
	F(UNIFORM_BUFFER_STANDARD_LAYOUT, ubo_standard_layout);
	F(SHADER_SUBGROUP_EXTENDED_TYPES, subgroup_extended_types);
	F(SEPARATE_DEPTH_STENCIL_LAYOUTS, separate_ds_layout);
	F(BUFFER_DEVICE_ADDRESS, buffer_device_address);
	FE(TRANSFORM_FEEDBACK, transform_feedback, EXT);
	FE(DEPTH_CLIP_ENABLE, depth_clip, EXT);
	FE(INLINE_UNIFORM_BLOCK, inline_uniform_block, EXT);
	FE(BLEND_OPERATION_ADVANCED, blend_operation_advanced, EXT);
	FE(VERTEX_ATTRIBUTE_DIVISOR, attribute_divisor, EXT);
	FE(SHADER_DEMOTE_TO_HELPER_INVOCATION, demote_to_helper, EXT);
	FE(FRAGMENT_SHADER_INTERLOCK, shader_interlock, EXT);
	FE(FRAGMENT_DENSITY_MAP, fragment_density, EXT);
	FE(BUFFER_DEVICE_ADDRESS, buffer_device_address_ext, EXT);
	FE(LINE_RASTERIZATION, line_rasterization, EXT);
	FE(SUBGROUP_SIZE_CONTROL, subgroup_size_control, EXT);
	FE(COMPUTE_SHADER_DERIVATIVES, compute_shader_derivatives, NV);
	FE(FRAGMENT_SHADER_BARYCENTRIC, barycentric_nv, NV);
	FE(SHADER_IMAGE_FOOTPRINT, image_footprint_nv, NV);
	FE(SHADING_RATE_IMAGE, shading_rate_nv, NV);
	FE(COOPERATIVE_MATRIX, cooperative_matrix_nv, NV);
	FE(SHADER_SM_BUILTINS, sm_builtins_nv, NV);
	FE(SHADER_INTEGER_FUNCTIONS_2, integer_functions2_intel, INTEL);

#undef CHAIN
#undef F
#undef FE

	return pNext;
}

void *build_pnext_chain(VulkanProperties &props)
{
	props = {};
	void *pNext = nullptr;
	void **ppNext = nullptr;

#define CHAIN(struct_type, member) \
	member.sType = struct_type; \
	if (!pNext) pNext = &member; \
	if (ppNext) *ppNext = &member; \
	ppNext = &member.pNext;

#define P(struct_type, member) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES, props.member)
#define PE(struct_type, member, ext) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext, props.member)

	P(DESCRIPTOR_INDEXING, descriptor_indexing);
	P(SUBGROUP, subgroup);
	P(FLOAT_CONTROLS, float_control);
	PE(SUBGROUP_SIZE_CONTROL, subgroup_size_control, EXT);
	PE(INLINE_UNIFORM_BLOCK, inline_uniform_block, EXT);

#undef CHAIN
#undef P
#undef PE

	return pNext;
}

struct FeatureFilter::Impl
{
	bool init(uint32_t api_version, const char **device_exts, unsigned count,
	          const VkPhysicalDeviceFeatures2 *enabled_features,
	          const VkPhysicalDeviceProperties2 *properties);

	bool sampler_is_supported(const VkSamplerCreateInfo *info) const;
	bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const;
	bool pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const;
	bool shader_module_is_supported(const VkShaderModuleCreateInfo *info) const;
	bool render_pass_is_supported(const VkRenderPassCreateInfo *info) const;
	bool graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const;
	bool compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const;

	std::unordered_set<std::string> enabled_extensions;

	uint32_t api_version = 0;
	VkPhysicalDeviceProperties2 props2 = {};
	VkPhysicalDeviceFeatures2 features2 = {};
	VulkanFeatures features = {};
	VulkanProperties props = {};

	void init_features(const void *pNext);
	void init_properties(const void *pNext);
	bool pnext_chain_is_supported(const void *pNext) const;
	bool validate_module_capabilities(const uint32_t *data, size_t size) const;
	bool validate_module_capability(spv::Capability cap) const;
};

FeatureFilter::FeatureFilter()
{
	impl = new Impl;
}

FeatureFilter::~FeatureFilter()
{
	delete impl;
}

void FeatureFilter::Impl::init_features(const void *pNext)
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);

#define F(struct_type, member) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

#define FE(struct_type, member, ext) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

		switch (base->sType)
		{
		F(16BIT_STORAGE, storage_16bit);
		F(MULTIVIEW, multiview);
		F(VARIABLE_POINTERS, variable_pointers);
		F(SAMPLER_YCBCR_CONVERSION, ycbcr_conversion);
		F(SHADER_DRAW_PARAMETERS, draw_parameters);
		F(8BIT_STORAGE, storage_8bit);
		F(SHADER_ATOMIC_INT64, atomic_int64);
		F(SHADER_FLOAT16_INT8, float16_int8);
		F(DESCRIPTOR_INDEXING, descriptor_indexing);
		F(VULKAN_MEMORY_MODEL, memory_model);
		F(UNIFORM_BUFFER_STANDARD_LAYOUT, ubo_standard_layout);
		F(SHADER_SUBGROUP_EXTENDED_TYPES, subgroup_extended_types);
		F(SEPARATE_DEPTH_STENCIL_LAYOUTS, separate_ds_layout);
		F(BUFFER_DEVICE_ADDRESS, buffer_device_address);
		FE(TRANSFORM_FEEDBACK, transform_feedback, EXT);
		FE(DEPTH_CLIP_ENABLE, depth_clip, EXT);
		FE(INLINE_UNIFORM_BLOCK, inline_uniform_block, EXT);
		FE(BLEND_OPERATION_ADVANCED, blend_operation_advanced, EXT);
		FE(VERTEX_ATTRIBUTE_DIVISOR, attribute_divisor, EXT);
		FE(SHADER_DEMOTE_TO_HELPER_INVOCATION, demote_to_helper, EXT);
		FE(FRAGMENT_SHADER_INTERLOCK, shader_interlock, EXT);
		FE(FRAGMENT_DENSITY_MAP, fragment_density, EXT);
		FE(BUFFER_DEVICE_ADDRESS, buffer_device_address_ext, EXT);
		FE(LINE_RASTERIZATION, line_rasterization, EXT);
		FE(SUBGROUP_SIZE_CONTROL, subgroup_size_control, EXT);
		FE(COMPUTE_SHADER_DERIVATIVES, compute_shader_derivatives, NV);
		FE(FRAGMENT_SHADER_BARYCENTRIC, barycentric_nv, NV);
		FE(SHADER_IMAGE_FOOTPRINT, image_footprint_nv, NV);
		FE(SHADING_RATE_IMAGE, shading_rate_nv, NV);
		FE(COOPERATIVE_MATRIX, cooperative_matrix_nv, NV);
		FE(SHADER_SM_BUILTINS, sm_builtins_nv, NV);
		FE(SHADER_INTEGER_FUNCTIONS_2, integer_functions2_intel, INTEL);
		default:
			break;
		}

#undef F
#undef FE

		pNext = base->pNext;
	}
}

void FeatureFilter::Impl::init_properties(const void *pNext)
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);

#define P(struct_type, member) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES: \
		memcpy(&props.member, base, sizeof(props.member)); \
		props.member.pNext = nullptr; \
		break

#define PE(struct_type, member, ext) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext: \
		memcpy(&props.member, base, sizeof(props.member)); \
		props.member.pNext = nullptr; \
		break

		switch (base->sType)
		{
		P(DESCRIPTOR_INDEXING, descriptor_indexing);
		P(SUBGROUP, subgroup);
		P(FLOAT_CONTROLS, float_control);
		PE(SUBGROUP_SIZE_CONTROL, subgroup_size_control, EXT);
		PE(INLINE_UNIFORM_BLOCK, inline_uniform_block, EXT);
		default:
			break;
		}

#undef P
#undef PE

		pNext = base->pNext;
	}
}

bool FeatureFilter::Impl::init(uint32_t api_version_, const char **device_exts, unsigned count,
                               const VkPhysicalDeviceFeatures2 *enabled_features,
                               const VkPhysicalDeviceProperties2 *properties)
{
	for (unsigned i = 0; i < count; i++)
		enabled_extensions.insert(device_exts[i]);

	api_version = api_version_;
	props2 = *properties;
	features2 = *enabled_features;

	init_features(enabled_features->pNext);
	init_properties(properties->pNext);

	return true;
}

bool FeatureFilter::init(uint32_t api_version, const char **device_exts, unsigned count,
                         const VkPhysicalDeviceFeatures2 *enabled_features, const VkPhysicalDeviceProperties2 *props)
{
	return impl->init(api_version, device_exts, count, enabled_features, props);
}

bool FeatureFilter::Impl::pnext_chain_is_supported(const void *pNext) const
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);

		// These are the currently pNext structs which Fossilize serialize.

		switch (base->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
			return enabled_extensions.count(VK_KHR_MAINTENANCE2_EXTENSION_NAME) != 0 || api_version >= VK_API_VERSION_1_1;

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT:
			return enabled_extensions.count(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) != 0;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
			return enabled_extensions.count(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) != 0;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
			return features.transform_feedback.geometryStreams == VK_TRUE;

		case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
			return features.multiview.multiview == VK_TRUE;

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT:
		{
			auto *flags = static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo *>(pNext);
			VkDescriptorBindingFlagsEXT flag_union = 0;
			for (uint32_t i = 0; i < flags->bindingCount; i++)
				flag_union |= flags->pBindingFlags[i];

			// Should map to descriptor type here, but shouldn't cause any pipeline compilation if we do a basic sanity check.
			if ((flag_union & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0 &&
			    features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind == VK_FALSE &&
			    features.descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind == VK_FALSE &&
			    features.descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind == VK_FALSE &&
			    features.descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_FALSE &&
			    features.descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_FALSE &&
			    features.descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind == VK_FALSE)
			{
				return false;
			}

			if ((flag_union & VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) != 0 &&
			    features.descriptor_indexing.descriptorBindingUpdateUnusedWhilePending == VK_FALSE)
			{
				return false;
			}

			if ((flag_union & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) != 0 &&
			    features.descriptor_indexing.descriptorBindingPartiallyBound == VK_FALSE)
			{
				return false;
			}

			if ((flag_union & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0 &&
			    features.descriptor_indexing.descriptorBindingVariableDescriptorCount == VK_FALSE)
			{
				return false;
			}

			return true;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
			return features.blend_operation_advanced.advancedBlendCoherentOperations == VK_TRUE;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
			return enabled_extensions.count(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME) != 0;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT:
		{
			auto *line = static_cast<const VkPipelineRasterizationLineStateCreateInfoEXT *>(pNext);
			if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT &&
			    features.line_rasterization.rectangularLines == VK_FALSE)
			{
				return false;
			}

			if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT &&
			    features.line_rasterization.bresenhamLines == VK_FALSE)
			{
				return false;
			}

			if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT &&
			    features.line_rasterization.smoothLines == VK_FALSE)
			{
				return false;
			}

			if (line->stippledLineEnable == VK_TRUE)
			{
				if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT &&
				    features.line_rasterization.stippledRectangularLines == VK_FALSE)
				{
					return false;
				}

				if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_EXT &&
				    features.line_rasterization.stippledBresenhamLines == VK_FALSE)
				{
					return false;
				}

				if (line->lineRasterizationMode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT &&
				    features.line_rasterization.stippledSmoothLines == VK_FALSE)
				{
					return false;
				}
			}

			return true;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT:
		{
			// Should correlate with stage.
			return features.subgroup_size_control.subgroupSizeControl == VK_FALSE &&
			       props.subgroup_size_control.requiredSubgroupSizeStages != 0;
		}

		default:
			LOGE("Unrecognized pNext sType: %u. Treating as unsupported.\n", unsigned(base->sType));
			return false;
		}

		pNext = base->pNext;
	}
	return true;
}

// The most basic validation, can be extended as required.

bool FeatureFilter::Impl::sampler_is_supported(const VkSamplerCreateInfo *info) const
{
	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const
{
	for (unsigned i = 0; i < info->bindingCount; i++)
	{
		if (info->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT)
		{
			if (features.inline_uniform_block.inlineUniformBlock == VK_FALSE)
				return false;
			if (info->pBindings[i].descriptorCount > props.inline_uniform_block.maxInlineUniformBlockSize)
				return false;
		}
	}
	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const
{
	unsigned max_push_constant_size = 0;
	for (unsigned i = 0; i < info->pushConstantRangeCount; i++)
	{
		unsigned required_size = info->pPushConstantRanges[i].offset + info->pPushConstantRanges[i].size;
		if (required_size > max_push_constant_size)
			max_push_constant_size = required_size;
	}

	if (max_push_constant_size > props2.properties.limits.maxPushConstantsSize)
		return false;

	if (info->setLayoutCount > props2.properties.limits.maxBoundDescriptorSets)
		return false;

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::validate_module_capability(spv::Capability cap) const
{
	// From table 75 in Vulkan spec.

	switch (cap)
	{
	case spv::CapabilityMatrix:
	case spv::CapabilityShader:
	case spv::CapabilityInputAttachment:
	case spv::CapabilitySampled1D:
	case spv::CapabilityImage1D:
	case spv::CapabilitySampledBuffer:
	case spv::CapabilityImageBuffer:
	case spv::CapabilityImageQuery:
	case spv::CapabilityDerivativeControl:
	case spv::CapabilityStorageImageExtendedFormats:
	case spv::CapabilityDeviceGroup:
		return true;

	case spv::CapabilityGeometry:
		return features2.features.geometryShader == VK_TRUE;
	case spv::CapabilityTessellation:
		return features2.features.tessellationShader == VK_TRUE;
	case spv::CapabilityFloat64:
		return features2.features.shaderFloat64 == VK_TRUE;
	case spv::CapabilityInt64:
		return features2.features.shaderInt64 == VK_TRUE;
	case spv::CapabilityInt64Atomics:
		return features.atomic_int64.shaderBufferInt64Atomics == VK_TRUE ||
		       features.atomic_int64.shaderSharedInt64Atomics == VK_TRUE;
	case spv::CapabilityGroups:
		return enabled_extensions.count(VK_AMD_SHADER_BALLOT_EXTENSION_NAME);
	case spv::CapabilityInt16:
		return features2.features.shaderInt16 == VK_TRUE;
	case spv::CapabilityTessellationPointSize:
	case spv::CapabilityGeometryPointSize:
		return features2.features.shaderTessellationAndGeometryPointSize == VK_TRUE;
	case spv::CapabilityImageGatherExtended:
		return features2.features.shaderImageGatherExtended == VK_TRUE;
	case spv::CapabilityStorageImageMultisample:
		return features2.features.shaderStorageImageMultisample == VK_TRUE;
	case spv::CapabilityUniformBufferArrayDynamicIndexing:
		return features2.features.shaderUniformBufferArrayDynamicIndexing == VK_TRUE;
	case spv::CapabilitySampledImageArrayDynamicIndexing:
		return features2.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
	case spv::CapabilityStorageBufferArrayDynamicIndexing:
		return features2.features.shaderStorageBufferArrayDynamicIndexing == VK_TRUE;
	case spv::CapabilityStorageImageArrayDynamicIndexing:
		return features2.features.shaderStorageImageArrayDynamicIndexing == VK_TRUE;
	case spv::CapabilityClipDistance:
		return features2.features.shaderClipDistance == VK_TRUE;
	case spv::CapabilityCullDistance:
		return features2.features.shaderCullDistance == VK_TRUE;
	case spv::CapabilityImageCubeArray:
		return features2.features.imageCubeArray == VK_TRUE;
	case spv::CapabilitySampleRateShading:
		return features2.features.sampleRateShading == VK_TRUE;
	case spv::CapabilitySparseResidency:
		return features2.features.shaderResourceResidency == VK_TRUE;
	case spv::CapabilityMinLod:
		return features2.features.shaderResourceMinLod == VK_TRUE;
	case spv::CapabilitySampledCubeArray:
		return features2.features.imageCubeArray == VK_TRUE;
	case spv::CapabilityImageMSArray:
		return features2.features.shaderStorageImageMultisample == VK_TRUE;
	case spv::CapabilityInterpolationFunction:
		return features2.features.sampleRateShading == VK_TRUE;
	case spv::CapabilityStorageImageReadWithoutFormat:
		return features2.features.shaderStorageImageReadWithoutFormat == VK_TRUE;
	case spv::CapabilityStorageImageWriteWithoutFormat:
		return features2.features.shaderStorageImageWriteWithoutFormat == VK_TRUE;
	case spv::CapabilityMultiViewport:
		return features2.features.multiViewport == VK_TRUE;
	case spv::CapabilityDrawParameters:
		return features.draw_parameters.shaderDrawParameters == VK_TRUE ||
		       enabled_extensions.count(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME) != 0;
	case spv::CapabilityMultiView:
		return features.multiview.multiview == VK_TRUE;
	case spv::CapabilityVariablePointersStorageBuffer:
		return features.variable_pointers.variablePointersStorageBuffer == VK_TRUE;
	case spv::CapabilityVariablePointers:
		return features.variable_pointers.variablePointers == VK_TRUE;
	case spv::CapabilityShaderClockKHR:
		return enabled_extensions.count(VK_KHR_SHADER_CLOCK_EXTENSION_NAME) != 0;
	case spv::CapabilityStencilExportEXT:
		return enabled_extensions.count(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME) != 0;
	case spv::CapabilitySubgroupBallotKHR:
		return enabled_extensions.count(VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME) != 0;
	case spv::CapabilitySubgroupVoteKHR:
		return enabled_extensions.count(VK_EXT_SHADER_SUBGROUP_VOTE_EXTENSION_NAME) != 0;
	case spv::CapabilityImageReadWriteLodAMD:
		return enabled_extensions.count(VK_AMD_SHADER_IMAGE_LOAD_STORE_LOD_EXTENSION_NAME) != 0;
	case spv::CapabilityImageGatherBiasLodAMD:
		return enabled_extensions.count(VK_AMD_TEXTURE_GATHER_BIAS_LOD_EXTENSION_NAME) != 0;
	case spv::CapabilityFragmentMaskAMD:
		return enabled_extensions.count(VK_AMD_SHADER_FRAGMENT_MASK_EXTENSION_NAME) != 0;
	case spv::CapabilitySampleMaskOverrideCoverageNV:
		return enabled_extensions.count(VK_NV_SAMPLE_MASK_OVERRIDE_COVERAGE_EXTENSION_NAME) != 0;
	case spv::CapabilityGeometryShaderPassthroughNV:
		return enabled_extensions.count(VK_NV_GEOMETRY_SHADER_PASSTHROUGH_EXTENSION_NAME) != 0;
	case spv::CapabilityShaderViewportIndex:
	case spv::CapabilityShaderLayer:
		// Vulkan 1.2 feature struct. Validation layer complains when we use 1_2 feature struct along other similar structs.
		return false;
	case spv::CapabilityShaderViewportIndexLayerEXT:
		// NV version is a cloned enum.
		return enabled_extensions.count(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME) != 0 ||
		       enabled_extensions.count(VK_NV_VIEWPORT_ARRAY2_EXTENSION_NAME) != 0;
	case spv::CapabilityShaderViewportMaskNV:
		return enabled_extensions.count(VK_NV_VIEWPORT_ARRAY2_EXTENSION_NAME) != 0;
	case spv::CapabilityPerViewAttributesNV:
		return enabled_extensions.count(VK_NVX_MULTIVIEW_PER_VIEW_ATTRIBUTES_EXTENSION_NAME) != 0;
	case spv::CapabilityStorageBuffer16BitAccess:
		return features.storage_16bit.storageBuffer16BitAccess == VK_TRUE;
	case spv::CapabilityUniformAndStorageBuffer16BitAccess:
		return features.storage_16bit.uniformAndStorageBuffer16BitAccess == VK_TRUE;
	case spv::CapabilityStoragePushConstant16:
		return features.storage_16bit.storagePushConstant16 == VK_TRUE;
	case spv::CapabilityStorageInputOutput16:
		return features.storage_16bit.storageInputOutput16 == VK_TRUE;
	case spv::CapabilityGroupNonUniform:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT) != 0;
	case spv::CapabilityGroupNonUniformVote:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_VOTE_BIT) != 0;
	case spv::CapabilityGroupNonUniformArithmetic:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0;
	case spv::CapabilityGroupNonUniformBallot:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) != 0;
	case spv::CapabilityGroupNonUniformShuffle:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0;
	case spv::CapabilityGroupNonUniformShuffleRelative:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT) != 0;
	case spv::CapabilityGroupNonUniformClustered:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) != 0;
	case spv::CapabilityGroupNonUniformQuad:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT) != 0;
	case spv::CapabilityGroupNonUniformPartitionedNV:
		return (props.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_PARTITIONED_BIT_NV) != 0;
	case spv::CapabilitySampleMaskPostDepthCoverage:
		return enabled_extensions.count(VK_EXT_POST_DEPTH_COVERAGE_EXTENSION_NAME) != 0;
	case spv::CapabilityShaderNonUniform:
		return enabled_extensions.count(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) != 0 || api_version >= VK_API_VERSION_1_2;
	case spv::CapabilityRuntimeDescriptorArray:
		return features.descriptor_indexing.runtimeDescriptorArray == VK_TRUE;
	case spv::CapabilityInputAttachmentArrayDynamicIndexing:
		return features.descriptor_indexing.shaderInputAttachmentArrayDynamicIndexing == VK_TRUE;
	case spv::CapabilityUniformTexelBufferArrayDynamicIndexing:
		return features.descriptor_indexing.shaderUniformTexelBufferArrayDynamicIndexing == VK_TRUE;
	case spv::CapabilityStorageTexelBufferArrayDynamicIndexing:
		return features.descriptor_indexing.shaderStorageTexelBufferArrayDynamicIndexing == VK_TRUE;
	case spv::CapabilityUniformBufferArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderUniformBufferArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilitySampledImageArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityStorageBufferArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityStorageImageArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityInputAttachmentArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderInputAttachmentArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityStorageTexelBufferArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderStorageTexelBufferArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityFloat16:
		return features.float16_int8.shaderFloat16 == VK_TRUE || enabled_extensions.count(VK_AMD_GPU_SHADER_HALF_FLOAT_EXTENSION_NAME) == 0;
	case spv::CapabilityInt8:
		return features.float16_int8.shaderInt8 == VK_TRUE;
	case spv::CapabilityStorageBuffer8BitAccess:
		return features.storage_8bit.storageBuffer8BitAccess == VK_TRUE;
	case spv::CapabilityUniformAndStorageBuffer8BitAccess:
		return features.storage_8bit.uniformAndStorageBuffer8BitAccess == VK_TRUE;
	case spv::CapabilityStoragePushConstant8:
		return features.storage_8bit.storagePushConstant8 == VK_TRUE;
	case spv::CapabilityVulkanMemoryModel:
		return features.memory_model.vulkanMemoryModel == VK_TRUE;
	case spv::CapabilityVulkanMemoryModelDeviceScope:
		return features.memory_model.vulkanMemoryModelDeviceScope == VK_TRUE;
	case spv::CapabilityDenormPreserve:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return props.float_control.shaderDenormPreserveFloat16 == VK_TRUE ||
		       props.float_control.shaderDenormPreserveFloat32 == VK_TRUE ||
		       props.float_control.shaderDenormPreserveFloat64 == VK_TRUE;
	case spv::CapabilityDenormFlushToZero:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return props.float_control.shaderDenormFlushToZeroFloat16 == VK_TRUE ||
		       props.float_control.shaderDenormFlushToZeroFloat32 == VK_TRUE ||
		       props.float_control.shaderDenormFlushToZeroFloat64 == VK_TRUE;
	case spv::CapabilitySignedZeroInfNanPreserve:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return props.float_control.shaderSignedZeroInfNanPreserveFloat16 == VK_TRUE ||
		       props.float_control.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE ||
		       props.float_control.shaderSignedZeroInfNanPreserveFloat64 == VK_TRUE;
	case spv::CapabilityRoundingModeRTE:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return props.float_control.shaderRoundingModeRTEFloat16 == VK_TRUE ||
		       props.float_control.shaderRoundingModeRTEFloat32 == VK_TRUE ||
		       props.float_control.shaderRoundingModeRTEFloat64 == VK_TRUE;
	case spv::CapabilityRoundingModeRTZ:
		// Not sure if we have to inspect every possible type. Assume compiler won't barf if at least one property is set.
		return props.float_control.shaderRoundingModeRTZFloat16 == VK_TRUE ||
		       props.float_control.shaderRoundingModeRTZFloat32 == VK_TRUE ||
		       props.float_control.shaderRoundingModeRTZFloat64 == VK_TRUE;
	case spv::CapabilityComputeDerivativeGroupQuadsNV:
		return features.compute_shader_derivatives.computeDerivativeGroupQuads == VK_TRUE;
	case spv::CapabilityComputeDerivativeGroupLinearNV:
		return features.compute_shader_derivatives.computeDerivativeGroupLinear == VK_TRUE;
	case spv::CapabilityFragmentBarycentricNV:
		return features.barycentric_nv.fragmentShaderBarycentric == VK_TRUE;
	case spv::CapabilityImageFootprintNV:
		return features.image_footprint_nv.imageFootprint == VK_TRUE;
	case spv::CapabilityFragmentDensityEXT:
		// Spec mentions ShadingRateImageNV, but that does not appear to exist?
		return features.shading_rate_nv.shadingRateImage == VK_TRUE ||
		       features.fragment_density.fragmentDensityMap == VK_TRUE;
	case spv::CapabilityMeshShadingNV:
		return enabled_extensions.count(VK_NV_MESH_SHADER_EXTENSION_NAME) == VK_TRUE;
	case spv::CapabilityRayTracingNV:
		return enabled_extensions.count(VK_NV_RAY_TRACING_EXTENSION_NAME) == VK_TRUE;
	case spv::CapabilityTransformFeedback:
		return features.transform_feedback.transformFeedback == VK_TRUE;
	case spv::CapabilityGeometryStreams:
		return features.transform_feedback.geometryStreams == VK_TRUE;
	case spv::CapabilityPhysicalStorageBufferAddresses:
		// Apparently these are different types?
		return features.buffer_device_address.bufferDeviceAddress == VK_TRUE ||
		       features.buffer_device_address_ext.bufferDeviceAddress == VK_TRUE;
	case spv::CapabilityCooperativeMatrixNV:
		return features.cooperative_matrix_nv.cooperativeMatrix == VK_TRUE;
	case spv::CapabilityIntegerFunctions2INTEL:
		return features.integer_functions2_intel.shaderIntegerFunctions2 == VK_TRUE;
	case spv::CapabilityShaderSMBuiltinsNV:
		return features.sm_builtins_nv.shaderSMBuiltins == VK_TRUE;
	case spv::CapabilityFragmentShaderSampleInterlockEXT:
		return features.shader_interlock.fragmentShaderSampleInterlock == VK_TRUE;
	case spv::CapabilityFragmentShaderPixelInterlockEXT:
		return features.shader_interlock.fragmentShaderPixelInterlock == VK_TRUE;
	case spv::CapabilityFragmentShaderShadingRateInterlockEXT:
		return features.shader_interlock.fragmentShaderShadingRateInterlock == VK_TRUE ||
		       features.shading_rate_nv.shadingRateImage == VK_TRUE;
	case spv::CapabilityDemoteToHelperInvocationEXT:
		return features.demote_to_helper.shaderDemoteToHelperInvocation == VK_TRUE;

	default:
		LOGE("Unrecognized SPIR-V capability %u, treating as unsupported.\n", unsigned(cap));
		return false;
	}
}

bool FeatureFilter::Impl::validate_module_capabilities(const uint32_t *data, size_t size) const
{
	// Trivial SPIR-V parser, just need to poke at Capability opcodes.
	if (size & 3)
	{
		LOGE("SPIR-V module size is not aligned to 4 bytes.\n");
		return false;
	}

	if (size < 20)
	{
		LOGE("SPIR-V module size is impossibly small.\n");
		return false;
	}

	unsigned num_words = size >> 2;
	if (data[0] != spv::MagicNumber)
	{
		LOGE("Invalid magic number of module.\n");
		return false;
	}

	unsigned version = data[1];
	if (version > 0x10500)
	{
		LOGE("SPIR-V version above 1.5 not recognized.\n");
		return false;
	}
	else if (version == 0x10500)
	{
		if (api_version < VK_API_VERSION_1_2)
		{
			LOGE("SPIR-V 1.5 is only supported in Vulkan 1.2 and up.\n");
			return false;
		}
	}
	else if (version >= 0x10400)
	{
		if (api_version < VK_API_VERSION_1_2 && enabled_extensions.count(VK_KHR_SPIRV_1_4_EXTENSION_NAME) == 0)
		{
			LOGE("Need VK_KHR_spirv_1_4 or Vulkan 1.2 for SPIR-V 1.4.\n");
			return false;
		}
	}
	else if (version >= 0x10300)
	{
		if (api_version < VK_API_VERSION_1_1)
		{
			LOGE("Need Vulkan 1.1 for SPIR-V 1.3.\n");
			return false;
		}
	}

	unsigned offset = 5;
	while (offset < num_words)
	{
		auto op = static_cast<spv::Op>(data[offset] & 0xffff);
		unsigned count = (data[offset] >> 16) & 0xffff;

		if (count == 0)
		{
			LOGE("SPIR-V opcodes cannot consume 0 words.\n");
			return false;
		}

		if (offset + count > num_words)
		{
			LOGE("Opcode overflows module.\n");
			return false;
		}

		if (op == spv::OpCapability)
		{
			if (count != 2)
			{
				LOGE("Instruction length for OpCapability is wrong.\n");
				return false;
			}

			if (!validate_module_capability(static_cast<spv::Capability>(data[offset + 1])))
			{
				LOGE("Capability %u is not supported on this device, ignoring shader module.\n", data[offset + 1]);
				return false;
			}
		}
		else if (op == spv::OpFunction)
		{
			// We're now declaring code, so just stop parsing, there cannot be any capability ops after this.
			break;
		}
		offset += count;
	}

	return true;
}

bool FeatureFilter::Impl::shader_module_is_supported(const VkShaderModuleCreateInfo *info) const
{
	if (!validate_module_capabilities(info->pCode, info->codeSize))
		return false;
	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::render_pass_is_supported(const VkRenderPassCreateInfo *info) const
{
	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const
{
	if (info->pColorBlendState && !pnext_chain_is_supported(info->pColorBlendState->pNext))
		return false;
	if (info->pVertexInputState && !pnext_chain_is_supported(info->pVertexInputState->pNext))
		return false;
	if (info->pDepthStencilState && !pnext_chain_is_supported(info->pDepthStencilState->pNext))
		return false;
	if (info->pInputAssemblyState && !pnext_chain_is_supported(info->pInputAssemblyState->pNext))
		return false;
	if (info->pDynamicState && !pnext_chain_is_supported(info->pDynamicState->pNext))
		return false;
	if (info->pMultisampleState && !pnext_chain_is_supported(info->pMultisampleState->pNext))
		return false;
	if (info->pTessellationState && !pnext_chain_is_supported(info->pTessellationState->pNext))
		return false;
	if (info->pViewportState && !pnext_chain_is_supported(info->pViewportState->pNext))
		return false;
	if (info->pRasterizationState && !pnext_chain_is_supported(info->pRasterizationState->pNext))
		return false;

	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		if ((info->pStages[i].flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT) != 0 &&
		    features.subgroup_size_control.subgroupSizeControl == VK_FALSE)
		{
			return false;
		}

		if (!pnext_chain_is_supported(info->pStages[i].pNext))
			return false;
	}

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const
{
	if (!pnext_chain_is_supported(info->stage.pNext))
		return false;

	if ((info->stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT) != 0 &&
	    features.subgroup_size_control.computeFullSubgroups == VK_FALSE)
	{
		return false;
	}

	if ((info->stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT) != 0 &&
	    features.subgroup_size_control.subgroupSizeControl == VK_FALSE)
	{
		return false;
	}

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::sampler_is_supported(const VkSamplerCreateInfo *info) const
{
	return impl->sampler_is_supported(info);
}

bool FeatureFilter::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const
{
	return impl->descriptor_set_layout_is_supported(info);
}

bool FeatureFilter::pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const
{
	return impl->pipeline_layout_is_supported(info);
}

bool FeatureFilter::shader_module_is_supported(const VkShaderModuleCreateInfo *info) const
{
	return impl->shader_module_is_supported(info);
}

bool FeatureFilter::render_pass_is_supported(const VkRenderPassCreateInfo *info) const
{
	return impl->render_pass_is_supported(info);
}

bool FeatureFilter::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const
{
	return impl->graphics_pipeline_is_supported(info);
}

bool FeatureFilter::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const
{
	return impl->compute_pipeline_is_supported(info);
}

}
