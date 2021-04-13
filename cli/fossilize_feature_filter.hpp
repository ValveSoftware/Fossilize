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

#pragma once

#ifndef VULKAN_CORE_H_
#error "Must include Vulkan headers before including fossilize_feature_filter.hpp"
#endif

namespace Fossilize
{
struct VulkanFeatures
{
	VkPhysicalDevice16BitStorageFeatures storage_16bit;
	VkPhysicalDeviceMultiviewFeatures multiview;
	VkPhysicalDeviceVariablePointersFeatures variable_pointers;
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_conversion;
	VkPhysicalDeviceShaderDrawParametersFeatures draw_parameters;
	VkPhysicalDevice8BitStorageFeatures storage_8bit;
	VkPhysicalDeviceShaderAtomicInt64Features atomic_int64;
	VkPhysicalDeviceShaderFloat16Int8Features float16_int8;
	VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing;
	VkPhysicalDeviceVulkanMemoryModelFeatures memory_model;
	VkPhysicalDeviceUniformBufferStandardLayoutFeatures ubo_standard_layout;
	VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures subgroup_extended_types;
	VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures separate_ds_layout;
	VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address;
	VkPhysicalDeviceShaderClockFeaturesKHR shader_clock;
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragment_shading_rate;
	VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback;
	VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip;
	VkPhysicalDeviceInlineUniformBlockFeaturesEXT inline_uniform_block;
	VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT blend_operation_advanced;
	VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT attribute_divisor;
	VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote_to_helper;
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT shader_interlock;
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT fragment_density;
	VkPhysicalDeviceBufferDeviceAddressFeaturesEXT buffer_device_address_ext;
	VkPhysicalDeviceLineRasterizationFeaturesEXT line_rasterization;
	VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroup_size_control;
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extended_dynamic_state;
	VkPhysicalDeviceComputeShaderDerivativesFeaturesNV compute_shader_derivatives;
	VkPhysicalDeviceFragmentShaderBarycentricFeaturesNV barycentric_nv;
	VkPhysicalDeviceShaderImageFootprintFeaturesNV image_footprint_nv;
	VkPhysicalDeviceShadingRateImageFeaturesNV shading_rate_nv;
	VkPhysicalDeviceCooperativeMatrixFeaturesNV cooperative_matrix_nv;
	VkPhysicalDeviceShaderSMBuiltinsFeaturesNV sm_builtins_nv;
	VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL integer_functions2_intel;
	VkPhysicalDeviceMutableDescriptorTypeFeaturesVALVE mutable_descriptor_type_valve;
};

void *build_pnext_chain(VulkanFeatures &features);

struct VulkanProperties
{
	VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing;
	VkPhysicalDeviceSubgroupProperties subgroup;
	VkPhysicalDeviceFloatControlsProperties float_control;
	VkPhysicalDeviceSubgroupSizeControlPropertiesEXT subgroup_size_control;
	VkPhysicalDeviceInlineUniformBlockPropertiesEXT inline_uniform_block;
	VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT attribute_divisor;
	VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragment_shading_rate;
	VkPhysicalDeviceDepthStencilResolveProperties ds_resolve;
	VkPhysicalDeviceMultiviewProperties multiview;
};

void *build_pnext_chain(VulkanProperties &properties);

class DeviceQueryInterface
{
public:
	virtual ~DeviceQueryInterface() = default;
	virtual bool format_is_supported(VkFormat format, VkFormatFeatureFlags features) = 0;
};

class FeatureFilter
{
public:
	FeatureFilter();
	~FeatureFilter();
	void operator=(const FeatureFilter &) = delete;
	FeatureFilter(const FeatureFilter &) = delete;

	bool init(uint32_t api_version, const char **device_exts, unsigned count,
	          const VkPhysicalDeviceFeatures2 *enabled_features,
	          const VkPhysicalDeviceProperties2 *props);

	bool init_null_device();

	// Adds a query interface which lets the feature filter
	// do more detailed checks.
	// The pointer is not owned by FeatureFilter.
	// If not present, all relevant features are assumed to return true.
	void set_device_query_interface(DeviceQueryInterface *iface);

	bool sampler_is_supported(const VkSamplerCreateInfo *info) const;
	bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const;
	bool pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const;
	bool shader_module_is_supported(const VkShaderModuleCreateInfo *info) const;
	bool render_pass_is_supported(const VkRenderPassCreateInfo *info) const;
	bool render_pass2_is_supported(const VkRenderPassCreateInfo2 *info) const;
	bool graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const;
	bool compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const;

	bool supports_scalar_block_layout() const;

private:
	struct Impl;
	Impl *impl;
};
}
