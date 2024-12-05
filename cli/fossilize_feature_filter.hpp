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
	VkPhysicalDeviceRayQueryFeaturesKHR ray_query;
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing_pipeline;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure;
	VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR workgroup_memory_explicit_layout;
	VkPhysicalDeviceShaderIntegerDotProductFeaturesKHR shader_integer_dot_product;
	VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2;
	VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering;
	VkPhysicalDeviceMaintenance4FeaturesKHR maintenance4;
	VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5;
	VkPhysicalDeviceMaintenance7FeaturesKHR maintenance7;
	VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeaturesKHR zero_initialize_workgroup_memory;
	VkPhysicalDeviceTransformFeedbackFeaturesEXT transform_feedback;
	VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip;
	VkPhysicalDeviceInlineUniformBlockFeaturesEXT inline_uniform_block;
	VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT blend_operation_advanced;
	VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR attribute_divisor;
	VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demote_to_helper;
	VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT shader_interlock;
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT fragment_density;
	VkPhysicalDeviceBufferDeviceAddressFeaturesEXT buffer_device_address_ext;
	VkPhysicalDeviceLineRasterizationFeaturesKHR line_rasterization;
	VkPhysicalDeviceSubgroupSizeControlFeaturesEXT subgroup_size_control;
	VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extended_dynamic_state;
	VkPhysicalDeviceExtendedDynamicState2FeaturesEXT extended_dynamic_state2;
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT extended_dynamic_state3;
	VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_input_dynamic_state;
	VkPhysicalDeviceColorWriteEnableFeaturesEXT color_write_enable;
	VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT shader_image_atomic_int64;
	VkPhysicalDeviceShaderAtomicFloatFeaturesEXT shader_atomic_float;
	VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT shader_atomic_float2;
	VkPhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex;
	VkPhysicalDeviceCustomBorderColorFeaturesEXT custom_border_color;
	VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR compute_shader_derivatives;
	VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric;
	VkPhysicalDeviceShaderImageFootprintFeaturesNV image_footprint_nv;
	VkPhysicalDeviceShadingRateImageFeaturesNV shading_rate_nv;
	VkPhysicalDeviceCooperativeMatrixFeaturesNV cooperative_matrix_nv;
	VkPhysicalDeviceShaderSMBuiltinsFeaturesNV sm_builtins_nv;
	VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL integer_functions2_intel;
	VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_descriptor_type;
	VkPhysicalDeviceRobustness2FeaturesEXT robustness2;
	VkPhysicalDeviceImageRobustnessFeaturesEXT image_robustness;
	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT graphics_pipeline_library;
	VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV fragment_shading_rate_enums;
	VkPhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control;
	VkPhysicalDeviceMeshShaderFeaturesNV mesh_shader_nv;
	VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader;
	VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer;
	VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT non_seamless_cube_map;
	VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT attachment_feedback_loop_layout;
	VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT shader_module_identifier;
	VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR ray_tracing_maintenance1;
	VkPhysicalDeviceRayTracingMotionBlurFeaturesNV ray_tracing_motion_blur_nv;
	VkPhysicalDeviceImageProcessingFeaturesQCOM image_processing_qcom;
	VkPhysicalDeviceImageProcessing2FeaturesQCOM image_processing2_qcom;
	VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM shader_core_builtins_arm;
	VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR ray_tracing_position_fetch;
	VkPhysicalDeviceShaderTileImageFeaturesEXT shader_tile_image;
	VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperative_matrix;
	VkPhysicalDeviceShaderSubgroupRotateFeaturesKHR shader_subgroup_rotate;
	VkPhysicalDeviceShaderExpectAssumeFeaturesKHR expect_assume;
	VkPhysicalDeviceShaderFloatControls2FeaturesKHR shader_float_controls2;
	VkPhysicalDeviceShaderQuadControlFeaturesKHR shader_quad_control;
	VkPhysicalDeviceRawAccessChainsFeaturesNV raw_access_chains_nv;
	VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT shader_replicated_composites;
	VkPhysicalDevicePipelineRobustnessFeaturesEXT pipeline_robustness;
	VkPhysicalDeviceConditionalRenderingFeaturesEXT conditional_rendering;
	VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR dynamic_rendering_local_read;
	VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT rasterization_order_attachment_access;
	VkPhysicalDeviceOpacityMicromapFeaturesEXT opacity_micromap;
	VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV device_generated_commands_nv;
	VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV device_generated_commands_compute_nv;
	VkPhysicalDeviceOpticalFlowFeaturesNV optical_flow_nv;
	VkPhysicalDeviceLegacyDitheringFeaturesEXT legacy_dithering;
	VkPhysicalDevicePipelineProtectedAccessFeaturesEXT pipeline_protected_access;
	VkPhysicalDevicePerStageDescriptorSetFeaturesNV per_stage_descriptor_set_nv;
	VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT attachment_feedback_loop_dynamic_state;
	VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT subpass_merge_feedback;
	VkPhysicalDeviceBorderColorSwizzleFeaturesEXT border_color_swizzle;
	VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT multisampled_render_to_single_sampled;
	VkPhysicalDeviceDepthBiasControlFeaturesEXT depth_bias_control;
	VkPhysicalDeviceDepthClampControlFeaturesEXT depth_clamp_control;
	VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT device_generated_commands;
	VkPhysicalDeviceShaderObjectFeaturesEXT shader_object;
	VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT primitives_generated_query;
	VkPhysicalDeviceImage2DViewOf3DFeaturesEXT image_2d_view_of_3d;

	struct
	{
		VkBool32 shaderOutputViewportIndex;
		VkBool32 shaderOutputLayer;
		VkBool32 samplerFilterMinmax;
		VkBool32 scalarBlockLayout;
		VkBool32 descriptorIndexing;
	} vk12;

	struct
	{
		VkBool32 shaderTerminateInvocation;
	} vk13;

	struct
	{
		VkBool32 pushDescriptor;
	} vk14;
};

void *build_pnext_chain(VulkanFeatures &features, uint32_t api_version,
                        const char **enabled_extensions, uint32_t num_extensions);
void filter_feature_enablement(
		VkPhysicalDeviceFeatures2 &pdf2,
		VulkanFeatures &features,
		const VkPhysicalDeviceFeatures2 *target_features,
		const char **extensions,
		size_t *extension_count);

struct VulkanProperties
{
	VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing;
	VkPhysicalDeviceSubgroupProperties subgroup;
	VkPhysicalDeviceFloatControlsProperties float_control;
	VkPhysicalDeviceSubgroupSizeControlPropertiesEXT subgroup_size_control;
	VkPhysicalDeviceInlineUniformBlockPropertiesEXT inline_uniform_block;
	VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR attribute_divisor;
	VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragment_shading_rate;
	VkPhysicalDeviceDepthStencilResolveProperties ds_resolve;
	VkPhysicalDeviceMultiviewProperties multiview;
	VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline;
	VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure;
	VkPhysicalDevicePushDescriptorPropertiesKHR push_descriptor;
	VkPhysicalDeviceSampleLocationsPropertiesEXT sample_locations;
	VkPhysicalDeviceExtendedDynamicState3PropertiesEXT extended_dynamic_state3;
	VkPhysicalDeviceMeshShaderPropertiesEXT mesh_shader;
	VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT shader_module_identifier;
	VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT blend_operation_advanced;
	VkPhysicalDeviceMaintenance7PropertiesKHR maintenance7;

	struct
	{
		uint32_t maxInlineUniformTotalSize;
	} vk13;
};

void *build_pnext_chain(VulkanProperties &properties, uint32_t api_version,
                        const char **enabled_extensions, uint32_t num_extensions);

class DeviceQueryInterface
{
public:
	virtual ~DeviceQueryInterface() = default;
	virtual bool format_is_supported(VkFormat format, VkFormatFeatureFlags features) = 0;
	virtual bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) = 0;
	virtual void physical_device_feature_query(VkPhysicalDeviceFeatures2 *pdf2) = 0;
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
	bool raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info) const;

	bool register_shader_module_info(VkShaderModule module, const VkShaderModuleCreateInfo *info);
	void unregister_shader_module_info(VkShaderModule module);

	bool supports_scalar_block_layout() const;

private:
	struct Impl;
	Impl *impl;
};

template <typename T>
static inline const T *find_pnext(VkStructureType type, const void *pNext)
{
	while (pNext != nullptr)
	{
		auto *sin = static_cast<const VkBaseInStructure *>(pNext);
		if (sin->sType == type)
			return static_cast<const T*>(pNext);

		pNext = sin->pNext;
	}

	return nullptr;
}

static inline void reset_features(VkPhysicalDeviceMeshShaderFeaturesEXT &features, VkBool32 state)
{
	features.taskShader = state;
	features.meshShader = state;
	features.multiviewMeshShader = state;
	features.primitiveFragmentShadingRateMeshShader = state;
	features.meshShaderQueries = state;
}

static inline void reset_features(VkPhysicalDeviceMeshShaderFeaturesNV &features, VkBool32 state)
{
	features.taskShader = state;
	features.meshShader = state;
}

static inline void reset_features(VkPhysicalDeviceRobustness2FeaturesEXT &features, VkBool32 state)
{
	features.nullDescriptor = state;
	features.robustBufferAccess2 = state;
	features.robustImageAccess2 = state;
}

static inline void reset_features(VkPhysicalDeviceImageRobustnessFeaturesEXT &features, VkBool32 state)
{
	features.robustImageAccess = state;
}

static inline void reset_features(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV &features, VkBool32 state)
{
	features.fragmentShadingRateEnums = state;
	features.noInvocationFragmentShadingRates = state;
	features.supersampleFragmentShadingRates = state;
}

static inline void reset_features(VkPhysicalDeviceFragmentShadingRateFeaturesKHR &features, VkBool32 state)
{
	features.pipelineFragmentShadingRate = state;
	features.primitiveFragmentShadingRate = state;
	features.attachmentFragmentShadingRate = state;
}

static inline void reset_features(VkPhysicalDeviceDescriptorBufferFeaturesEXT &features, VkBool32 state)
{
	features.descriptorBuffer = state;
	features.descriptorBufferCaptureReplay = state;
	features.descriptorBufferImageLayoutIgnored = state;
	features.descriptorBufferPushDescriptors = state;
}

static inline void reset_features(VkPhysicalDeviceShaderObjectFeaturesEXT &features, VkBool32 state)
{
	features.shaderObject = state;
}

static inline void reset_features(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT &features, VkBool32 state)
{
	features.primitivesGeneratedQuery = state;
	features.primitivesGeneratedQueryWithRasterizerDiscard = state;
	features.primitivesGeneratedQueryWithNonZeroStreams = state;
}

static inline void reset_features(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT &features, VkBool32 state)
{
	features.image2DViewOf3D = state;
	features.sampler2DViewOf3D = state;
}
}
