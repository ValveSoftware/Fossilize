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
#include <unordered_map>
#include <string>
#include <algorithm>
#include <vector>
#include <mutex>

namespace Fossilize
{
void *build_pnext_chain(VulkanFeatures &features, uint32_t api_version,
                        const char **enabled_extensions, uint32_t extension_count)
{
	features = {};
	void *pNext = nullptr;
	void **ppNext = nullptr;

	std::unordered_set<std::string> enabled_extension_set;
	for (uint32_t i = 0; i < extension_count; i++)
		enabled_extension_set.insert(enabled_extensions[i]);

#define CHAIN(struct_type, member, min_api_version, required_extension, required_extension_alias) \
	do { \
        bool is_minimum_api_version = api_version >= min_api_version; \
		bool supports_extension = enabled_extension_set.count(required_extension) != 0; \
        if (!supports_extension && required_extension_alias) \
			supports_extension = enabled_extension_set.count(required_extension_alias) != 0; \
		if (is_minimum_api_version && supports_extension) { \
			member.sType = struct_type; \
			if (!pNext) pNext = &member; \
			if (ppNext) *ppNext = &member; \
			ppNext = &member.pNext; \
		} \
	} while (0)

#define F(struct_type, member, minimum_api_version, required_extension) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES, features.member, \
	VK_API_VERSION_##minimum_api_version, VK_##required_extension##_EXTENSION_NAME, nullptr)
#define FE(struct_type, member, ext) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext, features.member, \
	VK_API_VERSION_1_0, VK_##ext##_##struct_type##_EXTENSION_NAME, nullptr)
#define FE_ALIAS(struct_type, member, ext, ext_alias) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext, features.member, \
	VK_API_VERSION_1_0, VK_##ext##_##struct_type##_EXTENSION_NAME, VK_##ext_alias##_##struct_type##_EXTENSION_NAME)

#include "fossilize_feature_filter_features.inc"

#undef CHAIN
#undef F
#undef FE
#undef FE_ALIAS

	return pNext;
}

static void filter_feature_enablement(
		VkPhysicalDeviceFeatures2 &pdf, VulkanFeatures &features,
		const VkPhysicalDeviceFeatures2 *target_features)
{
	// These feature bits conflict according to validation layers.
	if (features.fragment_shading_rate.pipelineFragmentShadingRate == VK_TRUE ||
	    features.fragment_shading_rate.attachmentFragmentShadingRate == VK_TRUE ||
	    features.fragment_shading_rate.primitiveFragmentShadingRate == VK_TRUE)
	{
		features.shading_rate_nv.shadingRateImage = VK_FALSE;
		features.shading_rate_nv.shadingRateCoarseSampleOrder = VK_FALSE;
		features.fragment_density.fragmentDensityMap = VK_FALSE;
	}

	// Only enable robustness if requested since it affects compilation on most implementations.
	if (target_features)
	{
		pdf.features.robustBufferAccess =
				pdf.features.robustBufferAccess && target_features->features.robustBufferAccess;

		const auto *robustness2 = find_pnext<VkPhysicalDeviceRobustness2FeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
				target_features->pNext);

		if (robustness2)
		{
			features.robustness2.robustBufferAccess2 =
					features.robustness2.robustBufferAccess2 && robustness2->robustBufferAccess2;
			features.robustness2.robustImageAccess2 =
					features.robustness2.robustImageAccess2 && robustness2->robustImageAccess2;
			features.robustness2.nullDescriptor =
					features.robustness2.nullDescriptor && robustness2->nullDescriptor;
		}
		else
		{
			reset_features(features.robustness2, VK_FALSE);
		}

		const auto *image_robustness = find_pnext<VkPhysicalDeviceImageRobustnessFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT,
				target_features->pNext);

		if (image_robustness)
		{
			features.image_robustness.robustImageAccess =
					features.image_robustness.robustImageAccess && image_robustness->robustImageAccess;
		}
		else
		{
			reset_features(features.image_robustness, VK_FALSE);
		}

		const auto *fragment_shading_rate_enums = find_pnext<VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV,
				target_features->pNext);

		if (fragment_shading_rate_enums)
		{
			features.fragment_shading_rate_enums.fragmentShadingRateEnums =
					features.fragment_shading_rate_enums.fragmentShadingRateEnums &&
					fragment_shading_rate_enums->fragmentShadingRateEnums;

			features.fragment_shading_rate_enums.noInvocationFragmentShadingRates =
					features.fragment_shading_rate_enums.noInvocationFragmentShadingRates &&
					fragment_shading_rate_enums->noInvocationFragmentShadingRates;

			features.fragment_shading_rate_enums.supersampleFragmentShadingRates =
					features.fragment_shading_rate_enums.supersampleFragmentShadingRates &&
					fragment_shading_rate_enums->supersampleFragmentShadingRates;
		}
		else
		{
			reset_features(features.fragment_shading_rate_enums, VK_FALSE);
		}

		const auto *fragment_shading_rate = find_pnext<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
				target_features->pNext);

		if (fragment_shading_rate)
		{
			features.fragment_shading_rate.pipelineFragmentShadingRate =
					features.fragment_shading_rate.pipelineFragmentShadingRate &&
					fragment_shading_rate->pipelineFragmentShadingRate;
			features.fragment_shading_rate.primitiveFragmentShadingRate =
					features.fragment_shading_rate.primitiveFragmentShadingRate &&
					fragment_shading_rate->primitiveFragmentShadingRate;
			features.fragment_shading_rate.attachmentFragmentShadingRate =
					features.fragment_shading_rate.attachmentFragmentShadingRate &&
					fragment_shading_rate->attachmentFragmentShadingRate;
		}
		else
		{
			reset_features(features.fragment_shading_rate, VK_FALSE);
		}

		const auto *mesh_shader = find_pnext<VkPhysicalDeviceMeshShaderFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
				target_features->pNext);

		if (mesh_shader)
		{
			features.mesh_shader.taskShader =
					features.mesh_shader.taskShader && mesh_shader->taskShader;
			features.mesh_shader.meshShader =
					features.mesh_shader.meshShader && mesh_shader->meshShader;
			features.mesh_shader.multiviewMeshShader =
					features.mesh_shader.multiviewMeshShader && mesh_shader->multiviewMeshShader;
			features.mesh_shader.meshShaderQueries =
					features.mesh_shader.meshShaderQueries && mesh_shader->meshShaderQueries;
			features.mesh_shader.primitiveFragmentShadingRateMeshShader =
					features.mesh_shader.primitiveFragmentShadingRateMeshShader &&
					features.fragment_shading_rate.primitiveFragmentShadingRate &&
					mesh_shader->primitiveFragmentShadingRateMeshShader;
		}
		else
		{
			reset_features(features.mesh_shader, VK_FALSE);
		}

		const auto *mesh_shader_nv = find_pnext<VkPhysicalDeviceMeshShaderFeaturesNV>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV,
				target_features->pNext);

		if (mesh_shader_nv)
		{
			features.mesh_shader_nv.taskShader =
					features.mesh_shader_nv.taskShader && mesh_shader->taskShader;
			features.mesh_shader_nv.meshShader =
					features.mesh_shader_nv.meshShader && mesh_shader->meshShader;
		}
		else
		{
			reset_features(features.mesh_shader_nv, VK_FALSE);
		}

		const auto *descriptor_buffer = find_pnext<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
				target_features->pNext);

		if (descriptor_buffer)
		{
			features.descriptor_buffer.descriptorBuffer =
					features.descriptor_buffer.descriptorBuffer &&
					descriptor_buffer->descriptorBuffer;
			features.descriptor_buffer.descriptorBufferCaptureReplay =
					features.descriptor_buffer.descriptorBufferCaptureReplay &&
					descriptor_buffer->descriptorBufferCaptureReplay;
			features.descriptor_buffer.descriptorBufferImageLayoutIgnored =
					features.descriptor_buffer.descriptorBufferImageLayoutIgnored &&
					descriptor_buffer->descriptorBufferImageLayoutIgnored;
			features.descriptor_buffer.descriptorBufferPushDescriptors =
					features.descriptor_buffer.descriptorBufferPushDescriptors &&
					descriptor_buffer->descriptorBufferPushDescriptors;
		}
		else
		{
			reset_features(features.descriptor_buffer, VK_FALSE);
		}
	}
	else
	{
		pdf.features.robustBufferAccess = VK_FALSE;
		reset_features(features.robustness2, VK_FALSE);
		reset_features(features.image_robustness, VK_FALSE);
		reset_features(features.fragment_shading_rate_enums, VK_FALSE);
		reset_features(features.fragment_shading_rate, VK_FALSE);
		reset_features(features.mesh_shader, VK_FALSE);
		reset_features(features.mesh_shader_nv, VK_FALSE);
		reset_features(features.descriptor_buffer, VK_FALSE);
	}
}

static void remove_extension(const char **active_extensions, size_t *out_extension_count, const char *ext)
{
	size_t count = *out_extension_count;
	for (size_t i = 0; i < count; i++)
	{
		if (strcmp(active_extensions[i], ext) == 0)
		{
			active_extensions[i] = active_extensions[--count];
			break;
		}
	}

	*out_extension_count = count;
}

static void filter_active_extensions(VkPhysicalDeviceFeatures2 &pdf,
                                     const char **active_extensions, size_t *out_extension_count)
{
	// If we end up disabling features deliberately, we should remove the extension enablement as well,
	// and remove the feature pNext.
	// Some implementations will end up changing their PSO keys even though no features are actually enabled.
	auto *pNext = static_cast<VkBaseOutStructure *>(pdf.pNext);

	pdf.pNext = nullptr;
	void **ppNext = &pdf.pNext;

	while (pNext)
	{
		bool accept = true;
		auto *s = pNext;
		pNext = s->pNext;

		switch (s->sType)
		{
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV *>(s);
			if (feature->fragmentShadingRateEnums == VK_FALSE &&
			    feature->noInvocationFragmentShadingRates == VK_FALSE &&
			    feature->supersampleFragmentShadingRates == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count,
				                 VK_NV_FRAGMENT_SHADING_RATE_ENUMS_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceFragmentShadingRateFeaturesKHR *>(s);
			if (feature->attachmentFragmentShadingRate == VK_FALSE &&
			    feature->pipelineFragmentShadingRate == VK_FALSE &&
			    feature->primitiveFragmentShadingRate == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceRobustness2FeaturesEXT *>(s);
			if (feature->nullDescriptor == VK_FALSE &&
			    feature->robustBufferAccess2 == VK_FALSE &&
			    feature->robustImageAccess2 == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceImageRobustnessFeaturesEXT *>(s);
			if (feature->robustImageAccess == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceMeshShaderFeaturesEXT *>(s);
			if (feature->meshShader == VK_FALSE &&
			    feature->taskShader == VK_FALSE &&
			    feature->multiviewMeshShader == VK_FALSE &&
			    feature->primitiveFragmentShadingRateMeshShader == VK_FALSE &&
			    feature->meshShaderQueries == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_MESH_SHADER_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceMeshShaderFeaturesNV *>(s);
			if (feature->meshShader == VK_FALSE &&
			    feature->taskShader == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_NV_MESH_SHADER_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
		{
			auto *feature = reinterpret_cast<VkPhysicalDeviceDescriptorBufferFeaturesEXT *>(s);
			if (feature->descriptorBuffer == VK_FALSE &&
			    feature->descriptorBufferCaptureReplay == VK_FALSE &&
			    feature->descriptorBufferImageLayoutIgnored == VK_FALSE &&
			    feature->descriptorBufferPushDescriptors == VK_FALSE)
			{
				remove_extension(active_extensions, out_extension_count, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
				accept = false;
			}
			break;
		}

		default:
			break;
		}

		if (accept)
		{
			*ppNext = s;
			ppNext = reinterpret_cast<void **>(&s->pNext);
			*ppNext = nullptr;
		}
	}
}

void filter_feature_enablement(VkPhysicalDeviceFeatures2 &pdf,
                               VulkanFeatures &features,
                               const VkPhysicalDeviceFeatures2 *target_features,
							   const char **active_extensions, size_t *out_extension_count)
{
	filter_feature_enablement(pdf, features, target_features);
	filter_active_extensions(pdf, active_extensions, out_extension_count);
}

void *build_pnext_chain(VulkanProperties &props, uint32_t api_version,
                        const char **enabled_extensions, uint32_t extension_count)
{
	props = {};
	void *pNext = nullptr;
	void **ppNext = nullptr;

	std::unordered_set<std::string> enabled_extension_set;
	for (uint32_t i = 0; i < extension_count; i++)
		enabled_extension_set.insert(enabled_extensions[i]);

#define CHAIN(struct_type, member, min_api_version, required_extension) \
	do { \
		if ((required_extension == nullptr || enabled_extension_set.count(required_extension) != 0) && api_version >= min_api_version) { \
			member.sType = struct_type; \
			if (!pNext) pNext = &member; \
			if (ppNext) *ppNext = &member; \
			ppNext = &member.pNext; \
		} \
	} while (0)

#define P(struct_type, member, minimum_api_version, required_extension) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES, props.member, \
	VK_API_VERSION_##minimum_api_version, VK_##required_extension##_EXTENSION_NAME)
#define P_CORE(struct_type, member, minimum_api_version) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES, props.member, \
	VK_API_VERSION_##minimum_api_version, nullptr)
#define PE(struct_type, member, ext) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext, props.member, \
	VK_API_VERSION_1_0, VK_##ext##_##struct_type##_EXTENSION_NAME)

#include "fossilize_feature_filter_properties.inc"

#undef CHAIN
#undef P
#undef P_CORE
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
	bool register_shader_module_info(VkShaderModule module, const VkShaderModuleCreateInfo *info);
	void unregister_shader_module_info(VkShaderModule module);
	bool render_pass_is_supported(const VkRenderPassCreateInfo *info) const;
	bool render_pass2_is_supported(const VkRenderPassCreateInfo2 *info) const;
	bool graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const;
	bool compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const;
	bool raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info) const;

	bool attachment_reference_is_supported(const VkAttachmentReference &ref) const;
	bool attachment_reference2_is_supported(const VkAttachmentReference2 &ref) const;
	bool attachment_description_is_supported(const VkAttachmentDescription &desc,
	                                         VkFormatFeatureFlags format_features) const;
	bool attachment_description2_is_supported(const VkAttachmentDescription2 &desc,
	                                          VkFormatFeatureFlags format_features) const;
	bool subpass_description_is_supported(const VkSubpassDescription &sub) const;
	bool subpass_description2_is_supported(const VkSubpassDescription2 &sub) const;
	bool subpass_dependency_is_supported(const VkSubpassDependency &dep) const;
	bool subpass_dependency2_is_supported(const VkSubpassDependency2 &dep) const;
	bool dependency_flags_is_supported(VkDependencyFlags deps) const;

	bool subgroup_size_control_is_supported(const VkPipelineShaderStageCreateInfo &stage) const;

	bool multiview_mask_is_supported(uint32_t mask) const;
	bool image_layout_is_supported(VkImageLayout layout) const;
	bool format_is_supported(VkFormat, VkFormatFeatureFlags format_features) const;

	bool stage_limits_are_supported(const VkPipelineShaderStageCreateInfo &info) const;

	std::unordered_set<std::string> enabled_extensions;

	DeviceQueryInterface *query = nullptr;

	uint32_t api_version = 0;
	VkPhysicalDeviceProperties2 props2 = {};
	VkPhysicalDeviceFeatures2 features2 = {};
	VulkanFeatures features = {};
	VulkanProperties props = {};
	bool null_device = false;

	struct DeferredEntryPoint
	{
		spv::Id id;
		spv::Id wg_size_literal[3];
		spv::Id wg_size_id[3];
		spv::Id constant_id[3];
		bool has_constant_id[3];
		std::string name;
		spv::ExecutionModel execution_model;
		uint32_t max_vertices;
		uint32_t max_primitives;
	};

	struct ModuleInfo
	{
		std::vector<DeferredEntryPoint> deferred_entry_points;
	};

	mutable std::mutex module_to_info_lock;
	std::unordered_map<VkShaderModule, ModuleInfo> module_to_info;

	void init_features(const void *pNext);
	void init_properties(const void *pNext);
	bool pnext_chain_is_supported(const void *pNext) const;
	bool validate_module_capabilities(const uint32_t *data, size_t size) const;
	bool parse_module_info(const uint32_t *data, size_t size, ModuleInfo &info);
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

#define F(struct_type, member, ...) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

#define FE(struct_type, member, ext) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

#define FE_ALIAS(struct_type, member, ext, ext_alias) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_##ext: \
		memcpy(&features.member, base, sizeof(features.member)); \
		features.member.pNext = nullptr; \
		break

#define FEATURE(struct_name, core_struct, feature) if (core_struct.feature) features.struct_name.feature = VK_TRUE
		switch (base->sType)
		{
#include "fossilize_feature_filter_features.inc"
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
		{
			auto &vk11 = *reinterpret_cast<const VkPhysicalDeviceVulkan11Features *>(base);
			FEATURE(storage_16bit, vk11, storageBuffer16BitAccess);
			FEATURE(storage_16bit, vk11, uniformAndStorageBuffer16BitAccess);
			FEATURE(storage_16bit, vk11, storagePushConstant16);
			FEATURE(storage_16bit, vk11, storageInputOutput16);
			FEATURE(multiview, vk11, multiview);
			FEATURE(multiview, vk11, multiviewGeometryShader);
			FEATURE(multiview, vk11, multiviewTessellationShader);
			FEATURE(variable_pointers, vk11, variablePointersStorageBuffer);
			FEATURE(variable_pointers, vk11, variablePointers);
			// protected memory
			FEATURE(ycbcr_conversion, vk11, samplerYcbcrConversion);
			FEATURE(draw_parameters, vk11, shaderDrawParameters);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
		{
			auto &vk12 = *reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(base);
			//samplerMirrorClampToEdge
			//drawIndirectCount
			FEATURE(storage_8bit, vk12, storageBuffer8BitAccess);
			FEATURE(storage_8bit, vk12, uniformAndStorageBuffer8BitAccess);
			FEATURE(storage_8bit, vk12, storagePushConstant8);
			FEATURE(atomic_int64, vk12, shaderBufferInt64Atomics);
			FEATURE(atomic_int64, vk12, shaderSharedInt64Atomics);
			FEATURE(float16_int8, vk12, shaderFloat16);
			FEATURE(float16_int8, vk12, shaderInt8);
			FEATURE(vk12, vk12, descriptorIndexing);
			FEATURE(descriptor_indexing, vk12, shaderInputAttachmentArrayDynamicIndexing);
			FEATURE(descriptor_indexing, vk12, shaderUniformTexelBufferArrayDynamicIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageTexelBufferArrayDynamicIndexing);
			FEATURE(descriptor_indexing, vk12, shaderUniformBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderSampledImageArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageImageArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderInputAttachmentArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderUniformTexelBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, shaderStorageTexelBufferArrayNonUniformIndexing);
			FEATURE(descriptor_indexing, vk12, descriptorBindingUniformBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingSampledImageUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingStorageImageUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingStorageBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingUniformTexelBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingStorageTexelBufferUpdateAfterBind);
			FEATURE(descriptor_indexing, vk12, descriptorBindingUpdateUnusedWhilePending);
			FEATURE(descriptor_indexing, vk12, descriptorBindingPartiallyBound);
			FEATURE(descriptor_indexing, vk12, descriptorBindingVariableDescriptorCount);
			FEATURE(descriptor_indexing, vk12, runtimeDescriptorArray);
			FEATURE(vk12, vk12, samplerFilterMinmax);
			FEATURE(vk12, vk12, scalarBlockLayout);
			//imagelessFramebuffer;
			//uniformBufferStandardLayout;
			FEATURE(subgroup_extended_types, vk12, shaderSubgroupExtendedTypes);
			FEATURE(separate_ds_layout, vk12, separateDepthStencilLayouts);
			//hostQueryReset;
			//timelineSemaphore;
			FEATURE(buffer_device_address, vk12, bufferDeviceAddress);
			FEATURE(buffer_device_address, vk12, bufferDeviceAddressCaptureReplay);
			FEATURE(buffer_device_address, vk12, bufferDeviceAddressMultiDevice);
			FEATURE(memory_model, vk12, vulkanMemoryModel);
			FEATURE(memory_model, vk12, vulkanMemoryModelDeviceScope);
			FEATURE(memory_model, vk12, vulkanMemoryModelAvailabilityVisibilityChains);
			FEATURE(vk12, vk12, shaderOutputViewportIndex);
			FEATURE(vk12, vk12, shaderOutputLayer);
			//subgroupBroadcastDynamicId;
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
		{
			auto &vk13 = *reinterpret_cast<const VkPhysicalDeviceVulkan13Features *>(base);
			FEATURE(image_robustness, vk13, robustImageAccess);
			FEATURE(inline_uniform_block, vk13, inlineUniformBlock);
			FEATURE(inline_uniform_block, vk13, descriptorBindingInlineUniformBlockUpdateAfterBind);
			//pipelineCreationCacheControl;
			//privateData;
			FEATURE(demote_to_helper, vk13, shaderDemoteToHelperInvocation);
			FEATURE(vk13, vk13, shaderTerminateInvocation);
			FEATURE(subgroup_size_control, vk13, subgroupSizeControl);
			FEATURE(subgroup_size_control, vk13, computeFullSubgroups);
			FEATURE(synchronization2, vk13, synchronization2);
			//textureCompressionASTC_HDR;
			FEATURE(zero_initialize_workgroup_memory, vk13, shaderZeroInitializeWorkgroupMemory);
			FEATURE(dynamic_rendering, vk13, dynamicRendering);
			FEATURE(shader_integer_dot_product, vk13, shaderIntegerDotProduct);
			FEATURE(maintenance4, vk13, maintenance4);
			break;
		}

		default:
			break;
		}

#undef F
#undef FE
#undef FE_ALIAS
#undef FEATURE

		pNext = base->pNext;
	}
}

void FeatureFilter::Impl::init_properties(const void *pNext)
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);

#define P(struct_type, member, ...) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES: \
		memcpy(&props.member, base, sizeof(props.member)); \
		props.member.pNext = nullptr; \
		break

#define P_CORE(struct_type, member, ...) P(struct_type, member, __VA_ARGS__)

#define PE(struct_type, member, ext) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_PROPERTIES_##ext: \
		memcpy(&props.member, base, sizeof(props.member)); \
		props.member.pNext = nullptr; \
		break

#define PROP(struct_name, core_struct, prop) props.struct_name.prop = core_struct.prop
		switch (base->sType)
		{
#include "fossilize_feature_filter_properties.inc"

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
		{
			auto &vk11 = *reinterpret_cast<const VkPhysicalDeviceVulkan11Properties *>(base);
			PROP(subgroup, vk11, subgroupSize);
			props.subgroup.supportedStages = vk11.subgroupSupportedStages;
			props.subgroup.supportedOperations = vk11.subgroupSupportedOperations;
			props.subgroup.quadOperationsInAllStages = vk11.subgroupQuadOperationsInAllStages;
			//pointClippingBehavior;
			PROP(multiview, vk11, maxMultiviewViewCount);
			PROP(multiview, vk11, maxMultiviewInstanceIndex);
			//protectedNoFault;
			//maxPerSetDescriptors;
			//maxMemoryAllocationSize;
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
		{
			auto &vk12 = *reinterpret_cast<const VkPhysicalDeviceVulkan12Properties *>(base);
			//conformanceVersion;
			PROP(float_control, vk12, denormBehaviorIndependence);
			PROP(float_control, vk12, roundingModeIndependence);
			PROP(float_control, vk12, shaderSignedZeroInfNanPreserveFloat16);
			PROP(float_control, vk12, shaderSignedZeroInfNanPreserveFloat32);
			PROP(float_control, vk12, shaderSignedZeroInfNanPreserveFloat64);
			PROP(float_control, vk12, shaderDenormPreserveFloat16);
			PROP(float_control, vk12, shaderDenormPreserveFloat32);
			PROP(float_control, vk12, shaderDenormPreserveFloat64);
			PROP(float_control, vk12, shaderDenormFlushToZeroFloat16);
			PROP(float_control, vk12, shaderDenormFlushToZeroFloat32);
			PROP(float_control, vk12, shaderDenormFlushToZeroFloat64);
			PROP(float_control, vk12, shaderRoundingModeRTEFloat16);
			PROP(float_control, vk12, shaderRoundingModeRTEFloat32);
			PROP(float_control, vk12, shaderRoundingModeRTEFloat64);
			PROP(float_control, vk12, shaderRoundingModeRTZFloat16);
			PROP(float_control, vk12, shaderRoundingModeRTZFloat32);
			PROP(float_control, vk12, shaderRoundingModeRTZFloat64);
			PROP(descriptor_indexing, vk12, maxUpdateAfterBindDescriptorsInAllPools);
			PROP(descriptor_indexing, vk12, shaderUniformBufferArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderSampledImageArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderStorageBufferArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderStorageImageArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, shaderInputAttachmentArrayNonUniformIndexingNative);
			PROP(descriptor_indexing, vk12, robustBufferAccessUpdateAfterBind);
			PROP(descriptor_indexing, vk12, quadDivergentImplicitLod);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindSamplers);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindUniformBuffers);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindStorageBuffers);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindSampledImages);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindStorageImages);
			PROP(descriptor_indexing, vk12, maxPerStageDescriptorUpdateAfterBindInputAttachments);
			PROP(descriptor_indexing, vk12, maxPerStageUpdateAfterBindResources);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindSamplers);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindUniformBuffers);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindUniformBuffersDynamic);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindStorageBuffers);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindStorageBuffersDynamic);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindSampledImages);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindStorageImages);
			PROP(descriptor_indexing, vk12, maxDescriptorSetUpdateAfterBindInputAttachments);
			PROP(ds_resolve, vk12, supportedDepthResolveModes);
			PROP(ds_resolve, vk12, supportedStencilResolveModes);
			PROP(ds_resolve, vk12, independentResolveNone);
			PROP(ds_resolve, vk12, independentResolve);
			//filterMinmaxSingleComponentFormats;
			//filterMinmaxImageComponentMapping;
			//maxTimelineSemaphoreValueDifference;
			//framebufferIntegerColorSampleCounts;
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES:
		{
			auto &vk13 = *reinterpret_cast<const VkPhysicalDeviceVulkan13Properties *>(base);
			PROP(subgroup_size_control, vk13, minSubgroupSize);
			PROP(subgroup_size_control, vk13, maxSubgroupSize);
			PROP(subgroup_size_control, vk13, maxComputeWorkgroupSubgroups);
			PROP(subgroup_size_control, vk13, requiredSubgroupSizeStages);
			PROP(inline_uniform_block, vk13, maxInlineUniformBlockSize);
			PROP(inline_uniform_block, vk13, maxPerStageDescriptorInlineUniformBlocks);
			PROP(inline_uniform_block, vk13, maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks);
			PROP(inline_uniform_block, vk13, maxDescriptorSetInlineUniformBlocks);
			PROP(inline_uniform_block, vk13, maxDescriptorSetUpdateAfterBindInlineUniformBlocks);
			PROP(vk13, vk13, maxInlineUniformTotalSize);
			// integer dot product hints
			//storageTexelBufferOffsetAlignmentBytes;
			//storageTexelBufferOffsetSingleTexelAlignment;
			//uniformTexelBufferOffsetAlignmentBytes;
			//uniformTexelBufferOffsetSingleTexelAlignment;
			//maxBufferSize;
			break;
		}

		default:
			break;
		}

#undef P
#undef PE
#undef PROP

		pNext = base->pNext;
	}
}

bool FeatureFilter::Impl::init(uint32_t api_version_, const char **device_exts, unsigned count,
                               const VkPhysicalDeviceFeatures2 *enabled_features,
                               const VkPhysicalDeviceProperties2 *properties)
{
	for (unsigned i = 0; i < count; i++)
	{
		enabled_extensions.insert(device_exts[i]);
		if (strcmp(device_exts[i], VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME) == 0)
			features.vk12.scalarBlockLayout = VK_TRUE;
	}

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

bool FeatureFilter::init_null_device()
{
	impl->null_device = true;
	return true;
}

bool FeatureFilter::Impl::multiview_mask_is_supported(uint32_t mask) const
{
	const uint32_t allowed_mask =
			(props.multiview.maxMultiviewViewCount >= 32 ? 0u : (1u << props.multiview.maxMultiviewViewCount)) - 1u;

	return (mask & allowed_mask) == mask;
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
			if (!enabled_extensions.count(VK_KHR_MAINTENANCE2_EXTENSION_NAME) && api_version < VK_API_VERSION_1_1)
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
				return false;

			auto *divisor = static_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT *>(pNext);
			bool use_zero_divisor = false;
			bool use_non_identity_divisor = false;
			uint32_t max_divisor = 0;

			for (uint32_t i = 0; i < divisor->vertexBindingDivisorCount; i++)
			{
				if (divisor->pVertexBindingDivisors[i].divisor != 1)
					use_non_identity_divisor = true;
				if (divisor->pVertexBindingDivisors[i].divisor == 0)
					use_zero_divisor = true;
				max_divisor = (std::max)(max_divisor, divisor->pVertexBindingDivisors[i].divisor);
			}

			if (max_divisor > props.attribute_divisor.maxVertexAttribDivisor)
				return false;
			if (use_zero_divisor && !features.attribute_divisor.vertexAttributeInstanceRateZeroDivisor)
				return false;
			if (use_non_identity_divisor && !features.attribute_divisor.vertexAttributeInstanceRateDivisor)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
		{
			auto *clip = static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(pNext);
			if (clip->depthClipEnable && !features.depth_clip.depthClipEnable)
				return false;
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
			if (features.transform_feedback.geometryStreams == VK_FALSE)
				return false;
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
		{
			if (features.multiview.multiview == VK_FALSE)
				return false;

			auto *multiview = static_cast<const VkRenderPassMultiviewCreateInfo *>(pNext);

			for (uint32_t i = 0; i < multiview->subpassCount; i++)
			{
				if (!multiview_mask_is_supported(multiview->pViewMasks[i]))
					return false;
			}

			for (uint32_t i = 0; i < multiview->correlationMaskCount; i++)
			{
				if (!multiview_mask_is_supported(multiview->pCorrelationMasks[i]))
					return false;
			}

			break;
		}

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT:
		{
			auto *flags = static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo *>(pNext);
			VkDescriptorBindingFlagsEXT flag_union = 0;
			for (uint32_t i = 0; i < flags->bindingCount; i++)
				flag_union |= flags->pBindingFlags[i];

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

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
			if (features.blend_operation_advanced.advancedBlendCoherentOperations == VK_FALSE)
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
			if (!enabled_extensions.count(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME))
				return false;
			break;

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

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT:
			// The details are validated explicitly elsewhere since we need to know the shader create info to deduce correctness.
			break;

		case VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT:
		{
			if (features.mutable_descriptor_type.mutableDescriptorType == VK_FALSE)
				return false;

			auto *lists = static_cast<const VkMutableDescriptorTypeCreateInfoEXT *>(pNext);
			for (uint32_t i = 0; i < lists->mutableDescriptorTypeListCount; i++)
			{
				for (uint32_t j = 0; j < lists->pMutableDescriptorTypeLists[i].descriptorTypeCount; j++)
				{
					switch (lists->pMutableDescriptorTypeLists[i].pDescriptorTypes[j])
					{
					case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
					case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
					case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
					case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
					case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
					case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
					case VK_DESCRIPTOR_TYPE_SAMPLER:
						break;

					default:
						// Implementation can theoretically support beyond this (and we'd have to query support),
						// but validate against what is required.
						return false;
					}
				}
			}

			break;
		}

		case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
		{
			if (!features.separate_ds_layout.separateDepthStencilLayouts)
				return false;

			auto *layout = static_cast<const VkAttachmentDescriptionStencilLayout *>(pNext);
			if (!image_layout_is_supported(layout->stencilInitialLayout))
				return false;
			if (!image_layout_is_supported(layout->stencilFinalLayout))
				return false;
			break;
		}

		case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
		{
			if (!features.separate_ds_layout.separateDepthStencilLayouts)
				return false;

			auto *layout = static_cast<const VkAttachmentReferenceStencilLayout *>(pNext);
			if (!image_layout_is_supported(layout->stencilLayout))
				return false;
			break;
		}

		case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
		{
			if (api_version < VK_API_VERSION_1_2 && !enabled_extensions.count(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
				return false;

			auto *resolve = static_cast<const VkSubpassDescriptionDepthStencilResolve *>(pNext);

			if (!resolve->pDepthStencilResolveAttachment)
				break;

			if (!attachment_reference2_is_supported(*resolve->pDepthStencilResolveAttachment))
				return false;

			if (resolve->depthResolveMode &&
			    (props.ds_resolve.supportedDepthResolveModes & resolve->depthResolveMode) == 0)
				return false;

			if (resolve->stencilResolveMode &&
			    (props.ds_resolve.supportedStencilResolveModes & resolve->stencilResolveMode) == 0)
				return false;

			if (resolve->depthResolveMode != resolve->stencilResolveMode)
			{
				bool use_zero = resolve->depthResolveMode == VK_RESOLVE_MODE_NONE ||
				                resolve->stencilResolveMode == VK_RESOLVE_MODE_NONE;

				auto cond = use_zero ?
				            props.ds_resolve.independentResolveNone :
				            props.ds_resolve.independentResolve;
				if (!cond)
					return false;
			}

			break;
		}

		case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
		{
			if (!features.fragment_shading_rate.attachmentFragmentShadingRate)
				return false;

			auto *attachment = static_cast<const VkFragmentShadingRateAttachmentInfoKHR *>(pNext);

			if (!attachment->pFragmentShadingRateAttachment)
				break;

			if (!attachment_reference2_is_supported(*attachment->pFragmentShadingRateAttachment))
				return false;

			uint32_t width = attachment->shadingRateAttachmentTexelSize.width;
			uint32_t height = attachment->shadingRateAttachmentTexelSize.height;

			if (width == 0 || height == 0)
				return false;

			if (width < props.fragment_shading_rate.minFragmentShadingRateAttachmentTexelSize.width)
				return false;
			if (width > props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSize.width)
				return false;
			if (height < props.fragment_shading_rate.minFragmentShadingRateAttachmentTexelSize.height)
				return false;
			if (height > props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSize.height)
				return false;

			uint32_t higher = (std::max)(width, height);
			uint32_t lower = (std::min)(width, height);
			uint32_t aspect = higher / lower;
			if (aspect > props.fragment_shading_rate.maxFragmentShadingRateAttachmentTexelSizeAspectRatio)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR:
		{
			if (features.dynamic_rendering.dynamicRendering == VK_FALSE)
				return false;

			auto *info = static_cast<const VkPipelineRenderingCreateInfoKHR *>(pNext);

			for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
			{
				if (info->pColorAttachmentFormats[i] != VK_FORMAT_UNDEFINED &&
				    !format_is_supported(info->pColorAttachmentFormats[i], VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
				{
					return false;
				}
			}

			if (info->depthAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    !format_is_supported(info->depthAttachmentFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
			{
				return false;
			}

			if (info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    !format_is_supported(info->stencilAttachmentFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
			{
				return false;
			}

			if (info->depthAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED &&
			    info->depthAttachmentFormat != info->stencilAttachmentFormat)
			{
				return false;
			}

			if (info->viewMask && !multiview_mask_is_supported(info->viewMask))
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT:
		{
			if (!features.color_write_enable.colorWriteEnable)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME))
				return false;

			auto *info = static_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *>(pNext);

			if (info->provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT &&
			    !features.provoking_vertex.provokingVertexLast)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))
				return false;

			auto *info = static_cast<const VkSamplerCustomBorderColorCreateInfoEXT *>(pNext);

			if (info->format == VK_FORMAT_UNDEFINED &&
			    !features.custom_border_color.customBorderColorWithoutFormat)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO:
		{
			if (!enabled_extensions.count(VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME) && !features.vk12.samplerFilterMinmax)
				return false;
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
		{
			if (!enabled_extensions.count(VK_KHR_MAINTENANCE_2_EXTENSION_NAME) && api_version < VK_API_VERSION_1_1)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_DISCARD_RECTANGLES_EXTENSION_NAME))
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR:
		{
			if (!features.synchronization2.synchronization2)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
		{
			if (!features.fragment_shading_rate.pipelineFragmentShadingRate)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO:
		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO:
		{
			// Should also validate formats and all sorts of feature bits, but
			// consider that TODO for now.
			// YcbcrConversionCreateInfo is inlined into a VkSamplerCreateInfo when replaying from a Fossilize archive.
			// Normally, it's not a pNext, but we pretend it is to make capture and replay
			// a bit more sane.
			if (!features.ycbcr_conversion.samplerYcbcrConversion)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
		{
			if (features.graphics_pipeline_library.graphicsPipelineLibrary == VK_FALSE)
				return false;

			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT:
		{
			if (!enabled_extensions.count(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME))
				return false;

			auto *info = static_cast<const VkPipelineSampleLocationsStateCreateInfoEXT *>(pNext);
			// If count is 0, this is part of dynamic state, so just ignore it.
			if (!info->sampleLocationsEnable || info->sampleLocationsInfo.sampleLocationsCount == 0)
				break;

			if (info->sampleLocationsInfo.sampleLocationGridSize.width > props.sample_locations.maxSampleLocationGridSize.width ||
			    info->sampleLocationsInfo.sampleLocationGridSize.height > props.sample_locations.maxSampleLocationGridSize.height)
			{
				return false;
			}

			if ((props.sample_locations.sampleLocationSampleCounts & info->sampleLocationsInfo.sampleLocationsPerPixel) == 0)
				return false;

			// Sample positions are clamped by implementation.
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
		{
			if (!features.depth_clip_control.depthClipControl)
			{
				return false;
			}

			break;
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
	// Only allow flags we recognize and validate.
	constexpr VkSamplerCreateFlags supported_flags = VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT;
	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if ((info->flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT) != 0)
		if (!features.non_seamless_cube_map.nonSeamlessCubeMap)
			return false;

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const
{
	bool must_check_set_layout_before_accept = false;

	// Only allow flags we recognize and validate.
	constexpr VkDescriptorSetLayoutCreateFlags supported_flags =
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
			VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT;

	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0)
	{
		if (enabled_extensions.count(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0)
			return false;
	}

	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0)
	{
		// There doesn't seem to be a specific feature bit for this flag, key it on extension being enabled.
		// For specific descriptor types, we check the individual features.
		if (enabled_extensions.count(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0 && !features.vk12.descriptorIndexing)
			return false;
	}

	if ((info->flags & (VK_DESCRIPTOR_SET_LAYOUT_CREATE_EMBEDDED_IMMUTABLE_SAMPLERS_BIT_EXT |
	                    VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT)) != 0)
	{
		if (!features.descriptor_buffer.descriptorBuffer)
			return false;

		if (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
		{
			if (!features.descriptor_buffer.descriptorBufferPushDescriptors)
				return false;
		}
	}

	struct DescriptorCounts
	{
		uint32_t sampled_image;
		uint32_t storage_image;
		uint32_t ssbo;
		uint32_t ubo;
		uint32_t input_attachment;
		uint32_t sampler;
		uint32_t ubo_dynamic;
		uint32_t ssbo_dynamic;
		uint32_t acceleration_structure;
	};
	DescriptorCounts counts = {};
	uint32_t total_count = 0;

	auto *flags = find_pnext<VkDescriptorSetLayoutBindingFlagsCreateInfo>(
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			info->pNext);

	auto *mutable_info = find_pnext<VkMutableDescriptorTypeCreateInfoEXT>(
			VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
			info->pNext);

	bool pool_is_update_after_bind = (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0;

	for (unsigned i = 0; i < info->bindingCount; i++)
	{
		bool binding_is_update_after_bind =
				flags && i < flags->bindingCount &&
				(flags->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0;

		uint32_t *count = nullptr;

		switch (info->pBindings[i].descriptorType)
		{
		case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT:
			if (features.inline_uniform_block.inlineUniformBlock == VK_FALSE)
				return false;
			// TODO: maxInlineUniformTotalSize, but this shouldn't matter when we use the EXT.
			if (info->pBindings[i].descriptorCount > props.inline_uniform_block.maxInlineUniformBlockSize)
				return false;
			if (binding_is_update_after_bind && features.inline_uniform_block.descriptorBindingInlineUniformBlockUpdateAfterBind == VK_FALSE)
				return false;
			break;

		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind == VK_FALSE)
				return false;
			count = &counts.ssbo;
			break;

		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind == VK_FALSE)
				return false;
			count = &counts.ubo;
			break;

		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind == VK_FALSE)
				return false;
			count = &counts.sampled_image;
			break;

		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_FALSE)
				return false;
			count = &counts.sampled_image;
			break;

		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_FALSE)
				return false;
			count = &counts.storage_image;
			break;

		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind == VK_FALSE)
				return false;
			count = &counts.storage_image;
			break;

		case VK_DESCRIPTOR_TYPE_SAMPLER:
			if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind == VK_FALSE)
				return false;
			count = &counts.sampler;
			break;

		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			if (binding_is_update_after_bind)
				return false;
			count = &counts.input_attachment;
			break;

		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
			if (binding_is_update_after_bind)
				return false;
			count = &counts.ubo_dynamic;
			break;

		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			if (binding_is_update_after_bind)
				return false;
			count = &counts.ssbo_dynamic;
			break;

		case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
			if (binding_is_update_after_bind &&
			    features.acceleration_structure.descriptorBindingAccelerationStructureUpdateAfterBind == VK_FALSE)
				return false;
			if (features.acceleration_structure.accelerationStructure == VK_FALSE)
				return false;
			count = &counts.acceleration_structure;
			break;

		case VK_DESCRIPTOR_TYPE_MUTABLE_EXT:
		{
			DescriptorCounts mutable_counts = {};
			if (features.mutable_descriptor_type.mutableDescriptorType == VK_FALSE)
				return false;
			if (!mutable_info || i >= mutable_info->mutableDescriptorTypeListCount)
				return false;

			auto &list = mutable_info->pMutableDescriptorTypeLists[i];

			for (uint32_t j = 0; j < list.descriptorTypeCount; j++)
			{
				switch (list.pDescriptorTypes[j])
				{
				case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageBufferUpdateAfterBind == VK_FALSE)
						return false;
					mutable_counts.ssbo = 1;
					break;

				case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformBufferUpdateAfterBind == VK_FALSE)
						return false;
					mutable_counts.ubo = 1;
					break;

				case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingSampledImageUpdateAfterBind == VK_FALSE)
						return false;
					mutable_counts.sampled_image = 1;
					break;

				case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_FALSE)
						return false;
					mutable_counts.sampled_image = 1;
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageImageUpdateAfterBind == VK_FALSE)
						return false;
					mutable_counts.storage_image = 1;
					break;

				case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
					if (binding_is_update_after_bind && features.descriptor_indexing.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_FALSE)
						return false;
					mutable_counts.storage_image = 1;
					break;

				case VK_DESCRIPTOR_TYPE_SAMPLER:
					// Sampler is a non-standard type for mutable
					mutable_counts.sampled_image = 1;
					must_check_set_layout_before_accept = true;
					break;

				default:
					return false;
				}
			}

			counts.sampled_image += info->pBindings[i].descriptorCount * mutable_counts.sampled_image;
			counts.storage_image += info->pBindings[i].descriptorCount * mutable_counts.storage_image;
			counts.ubo += info->pBindings[i].descriptorCount * mutable_counts.ubo;
			counts.ssbo += info->pBindings[i].descriptorCount * mutable_counts.ssbo;
			break;
		}

		default:
			return false;
		}

		if (count)
			*count += info->pBindings[i].descriptorCount;
		total_count += info->pBindings[i].descriptorCount;
	}

	if (pool_is_update_after_bind)
	{
		if (counts.ubo_dynamic > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic)
			return false;
		if (counts.ssbo_dynamic > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic)
			return false;
		if (counts.ubo > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindUniformBuffers)
			return false;
		if (counts.ssbo > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffers)
			return false;
		if (counts.sampled_image > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindSampledImages)
			return false;
		if (counts.storage_image > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindStorageBuffers)
			return false;
		if (counts.sampler > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindSamplers)
			return false;
		if (counts.input_attachment > props.descriptor_indexing.maxDescriptorSetUpdateAfterBindInputAttachments)
			return false;
		if (counts.acceleration_structure > props.acceleration_structure.maxDescriptorSetUpdateAfterBindAccelerationStructures)
			return false;
	}
	else
	{
		if (counts.ubo_dynamic > props2.properties.limits.maxDescriptorSetUniformBuffersDynamic)
			return false;
		if (counts.ssbo_dynamic > props2.properties.limits.maxDescriptorSetStorageBuffersDynamic)
			return false;
		if (counts.ubo > props2.properties.limits.maxDescriptorSetUniformBuffers)
			return false;
		if (counts.ssbo > props2.properties.limits.maxDescriptorSetStorageBuffers)
			return false;
		if (counts.sampled_image > props2.properties.limits.maxDescriptorSetSampledImages)
			return false;
		if (counts.storage_image > props2.properties.limits.maxDescriptorSetStorageImages)
			return false;
		if (counts.sampler > props2.properties.limits.maxDescriptorSetSamplers)
			return false;
		if (counts.input_attachment > props2.properties.limits.maxDescriptorSetInputAttachments)
			return false;
		if (counts.acceleration_structure > props.acceleration_structure.maxDescriptorSetAccelerationStructures)
			return false;
	}

	if ((info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0 &&
	    total_count > props.push_descriptor.maxPushDescriptors)
	{
		return false;
	}

	if (must_check_set_layout_before_accept)
	{
		if (query && !query->descriptor_set_layout_is_supported(info))
			return false;
	}

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineLayoutCreateFlags supported_flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if ((info->flags & VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT) != 0 &&
	    features.graphics_pipeline_library.graphicsPipelineLibrary == VK_FALSE)
	{
		return false;
	}

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
		return true;

	case spv::CapabilityDeviceGroup:
		return api_version >= VK_API_VERSION_1_1;

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
		       features.atomic_int64.shaderSharedInt64Atomics == VK_TRUE ||
		       features.shader_image_atomic_int64.shaderImageInt64Atomics == VK_TRUE;
	case spv::CapabilityAtomicFloat16AddEXT:
		return features.shader_atomic_float2.shaderBufferFloat16AtomicAdd == VK_TRUE ||
		       features.shader_atomic_float2.shaderSharedFloat16AtomicAdd == VK_TRUE;
	case spv::CapabilityAtomicFloat32AddEXT:
		return features.shader_atomic_float.shaderBufferFloat32AtomicAdd == VK_TRUE ||
		       features.shader_atomic_float.shaderSharedFloat32AtomicAdd == VK_TRUE ||
		       features.shader_atomic_float.shaderImageFloat32AtomicAdd == VK_TRUE;
	case spv::CapabilityAtomicFloat64AddEXT:
		return features.shader_atomic_float.shaderBufferFloat64AtomicAdd == VK_TRUE ||
		       features.shader_atomic_float.shaderSharedFloat64AtomicAdd == VK_TRUE;
	case spv::CapabilityAtomicFloat16MinMaxEXT:
		return features.shader_atomic_float2.shaderBufferFloat16AtomicMinMax == VK_TRUE ||
		       features.shader_atomic_float2.shaderSharedFloat16AtomicMinMax == VK_TRUE;
	case spv::CapabilityAtomicFloat32MinMaxEXT:
		return features.shader_atomic_float2.shaderBufferFloat32AtomicMinMax == VK_TRUE ||
		       features.shader_atomic_float2.shaderSharedFloat32AtomicMinMax == VK_TRUE;
	case spv::CapabilityAtomicFloat64MinMaxEXT:
		return features.shader_atomic_float2.shaderBufferFloat64AtomicMinMax == VK_TRUE ||
		       features.shader_atomic_float2.shaderSharedFloat64AtomicMinMax == VK_TRUE;
	case spv::CapabilityInt64ImageEXT:
		return features.shader_image_atomic_int64.shaderImageInt64Atomics == VK_TRUE;
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
		return features2.features.shaderStorageImageReadWithoutFormat == VK_TRUE ||
		       api_version >= VK_API_VERSION_1_3 ||
		       enabled_extensions.count(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) != 0;
	case spv::CapabilityStorageImageWriteWithoutFormat:
		return features2.features.shaderStorageImageWriteWithoutFormat == VK_TRUE ||
		       api_version >= VK_API_VERSION_1_3 ||
		       enabled_extensions.count(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME) != 0;
	case spv::CapabilityMultiViewport:
		return features2.features.multiViewport == VK_TRUE;
	case spv::CapabilityDrawParameters:
		return features.draw_parameters.shaderDrawParameters == VK_TRUE;
	case spv::CapabilityMultiView:
		return features.multiview.multiview == VK_TRUE;
	case spv::CapabilityVariablePointersStorageBuffer:
		return features.variable_pointers.variablePointersStorageBuffer == VK_TRUE;
	case spv::CapabilityVariablePointers:
		return features.variable_pointers.variablePointers == VK_TRUE;
	case spv::CapabilityShaderClockKHR:
		// There aren't two separate capabilities, so we'd have to analyze all opcodes to deduce this.
		// Just gate this on both feature bits being supported to be safe.
		return features.shader_clock.shaderDeviceClock == VK_TRUE &&
		       features.shader_clock.shaderSubgroupClock == VK_TRUE;
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
		return features.vk12.shaderOutputViewportIndex;
	case spv::CapabilityShaderLayer:
		return features.vk12.shaderOutputLayer;
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
	case spv::CapabilityUniformTexelBufferArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderUniformTexelBufferArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityStorageBufferArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityStorageImageArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderStorageImageArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityInputAttachmentArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderInputAttachmentArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityStorageTexelBufferArrayNonUniformIndexing:
		return features.descriptor_indexing.shaderStorageTexelBufferArrayNonUniformIndexing == VK_TRUE;
	case spv::CapabilityFloat16:
		return features.float16_int8.shaderFloat16 == VK_TRUE || enabled_extensions.count(VK_AMD_GPU_SHADER_HALF_FLOAT_EXTENSION_NAME) != 0;
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
	case spv::CapabilityComputeDerivativeGroupQuadsKHR:
		return features.compute_shader_derivatives.computeDerivativeGroupQuads == VK_TRUE;
	case spv::CapabilityComputeDerivativeGroupLinearKHR:
		return features.compute_shader_derivatives.computeDerivativeGroupLinear == VK_TRUE;
	case spv::CapabilityFragmentBarycentricKHR:
		return features.barycentric.fragmentShaderBarycentric == VK_TRUE;
	case spv::CapabilityImageFootprintNV:
		return features.image_footprint_nv.imageFootprint == VK_TRUE;
	case spv::CapabilityFragmentDensityEXT:
		// Spec mentions ShadingRateImageNV, but that does not appear to exist?
		return features.shading_rate_nv.shadingRateImage == VK_TRUE ||
		       features.fragment_density.fragmentDensityMap == VK_TRUE;
	case spv::CapabilityMeshShadingNV:
		return features.mesh_shader_nv.meshShader == VK_TRUE;
	case spv::CapabilityRayTracingNV:
		return enabled_extensions.count(VK_NV_RAY_TRACING_EXTENSION_NAME) != 0;
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
	case spv::CapabilityFragmentShadingRateKHR:
		return features.fragment_shading_rate.primitiveFragmentShadingRate == VK_TRUE;
	case spv::CapabilityRayQueryKHR:
		return features.ray_query.rayQuery == VK_TRUE;
	case spv::CapabilityRayTracingKHR:
		return features.ray_tracing_pipeline.rayTracingPipeline == VK_TRUE;
	case spv::CapabilityRayTraversalPrimitiveCullingKHR:
		return features.ray_tracing_pipeline.rayTraversalPrimitiveCulling == VK_TRUE ||
		       features.ray_query.rayQuery == VK_TRUE;
	case spv::CapabilityWorkgroupMemoryExplicitLayoutKHR:
		return features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout == VK_TRUE;
	case spv::CapabilityWorkgroupMemoryExplicitLayout8BitAccessKHR:
		return features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout8BitAccess == VK_TRUE;
	case spv::CapabilityWorkgroupMemoryExplicitLayout16BitAccessKHR:
		return features.workgroup_memory_explicit_layout.workgroupMemoryExplicitLayout16BitAccess == VK_TRUE;
	case spv::CapabilityDotProductKHR:
	case spv::CapabilityDotProductInputAllKHR:
	case spv::CapabilityDotProductInput4x8BitKHR:
	case spv::CapabilityDotProductInput4x8BitPackedKHR:
		return features.shader_integer_dot_product.shaderIntegerDotProduct == VK_TRUE;
	case spv::CapabilityMeshShadingEXT:
		return features.mesh_shader.meshShader == VK_TRUE;
	case spv::CapabilityFragmentFullyCoveredEXT:
		return enabled_extensions.count(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME) != 0;
	// AtomicFloat16VectorNV does not seem to exist.
	case spv::CapabilityRayCullMaskKHR:
		return features.ray_tracing_maintenance1.rayTracingMaintenance1 == VK_TRUE;
	case spv::CapabilityRayTracingMotionBlurNV:
		return features.ray_tracing_motion_blur_nv.rayTracingMotionBlur == VK_TRUE;
	case spv::CapabilityRayTracingOpacityMicromapEXT:
		return enabled_extensions.count(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME) != 0;
	case spv::CapabilityTextureSampleWeightedQCOM:
		return features.image_processing_qcom.textureSampleWeighted == VK_TRUE;
	case spv::CapabilityTextureBoxFilterQCOM:
		return features.image_processing_qcom.textureBoxFilter == VK_TRUE;
	case spv::CapabilityTextureBlockMatchQCOM:
		return features.image_processing_qcom.textureBlockMatch == VK_TRUE;
	case spv::CapabilityTextureBlockMatch2QCOM:
		return features.image_processing2_qcom.textureBlockMatch2 == VK_TRUE;
	case spv::CapabilityCoreBuiltinsARM:
		return features.shader_core_builtins_arm.shaderCoreBuiltins == VK_TRUE;
	case spv::CapabilityShaderInvocationReorderNV:
		return enabled_extensions.count(VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) != 0;
	// Ignore CapabilityClusterCullingShadingHUAWEI. It does not exist in SPIR-V headers.
	case spv::CapabilityRayTracingPositionFetchKHR:
	case spv::CapabilityRayQueryPositionFetchKHR:
		return features.ray_tracing_position_fetch.rayTracingPositionFetch == VK_TRUE;
	case spv::CapabilityTileImageColorReadAccessEXT:
		return features.shader_tile_image.shaderTileImageColorReadAccess == VK_TRUE;
	case spv::CapabilityTileImageDepthReadAccessEXT:
		return features.shader_tile_image.shaderTileImageDepthReadAccess == VK_TRUE;
	case spv::CapabilityTileImageStencilReadAccessEXT:
		return features.shader_tile_image.shaderTileImageStencilReadAccess == VK_TRUE;
	case spv::CapabilityCooperativeMatrixKHR:
		return features.cooperative_matrix.cooperativeMatrix == VK_TRUE;
	// Ignore ShaderEnqueueAMDX, it's a beta extension.
	case spv::CapabilityGroupNonUniformRotateKHR:
		return features.shader_subgroup_rotate.shaderSubgroupRotate == VK_TRUE;
	case spv::CapabilityExpectAssumeKHR:
		return features.expect_assume.shaderExpectAssume == VK_TRUE;
	case spv::CapabilityFloatControls2:
		return features.shader_float_controls2.shaderFloatControls2 == VK_TRUE;
	case spv::CapabilityQuadControlKHR:
		return features.shader_quad_control.shaderQuadControl == VK_TRUE;
	case spv::CapabilityRawAccessChainsNV:
		return features.raw_access_chains_nv.shaderRawAccessChains == VK_TRUE;
	case spv::CapabilityReplicatedCompositesEXT:
		return features.shader_replicated_composites.shaderReplicatedComposites == VK_TRUE;

	default:
		LOGE("Unrecognized SPIR-V capability %u, treating as unsupported.\n", unsigned(cap));
		return false;
	}
}

static std::string extract_string(const uint32_t *words, uint32_t num_words)
{
	std::string ret;
	for (uint32_t i = 0; i < num_words; i++)
	{
		uint32_t w = words[i];

		for (uint32_t j = 0; j < 4; j++, w >>= 8)
		{
			auto c = char(w & 0xff);
			if (c == '\0')
				return ret;
			ret += c;
		}
	}
	return ret;
}

bool FeatureFilter::Impl::parse_module_info(const uint32_t *data, size_t size, ModuleInfo &info)
{
	// We've already validated the module, now parse information which is related to API limits.
	auto num_words = unsigned(size >> 2);
	spv::Id global_workgroup_size_id = 0;

	struct Constant
	{
		spv::Id id;
		uint32_t literal;
		spv::Id constant_id;
		bool has_constant_id;
	};
	std::vector<Constant> constants;

	const auto add_constant = [&](spv::Id id, uint32_t literal)
	{
		for (auto &c : constants)
		{
			if (c.id == id)
			{
				c.literal = literal;
				return;
			}
		}

		constants.push_back({ id, literal, 0, false });
	};

	const auto find_constant = [&](spv::Id id) -> std::vector<Constant>::const_iterator
	{
		return std::find_if(constants.begin(), constants.end(), [id](const Constant &c) { return c.id == id; });
	};

	unsigned offset = 5;
	while (offset < num_words)
	{
		auto op = static_cast<spv::Op>(data[offset] & 0xffff);
		unsigned count = (data[offset] >> 16) & 0xffff;

		if (op == spv::OpFunction)
		{
			// We're now declaring code, so just stop parsing, there cannot be any capability ops after this.
			break;
		}
		else if (op == spv::OpEntryPoint && count >= 4)
		{
			// Must come first.
			auto execution_model = spv::ExecutionModel(data[offset + 1]);
			spv::Id id = data[offset + 2];

			// We only care about compute-like stages here since we need to validate workgroup sizes.
			if (execution_model == spv::ExecutionModelGLCompute ||
			    execution_model == spv::ExecutionModelTaskEXT ||
			    execution_model == spv::ExecutionModelMeshEXT)
			{
				DeferredEntryPoint entry = {};
				entry.name = extract_string(&data[offset + 3], num_words - (offset + 3));
				entry.execution_model = execution_model;
				entry.id = id;
				info.deferred_entry_points.push_back(entry);
			}
		}
		else if (op == spv::OpExecutionMode || op == spv::OpExecutionModeId)
		{
			// Now we can get execution mode.
			spv::Id id = data[offset + 1];
			auto mode = spv::ExecutionMode(data[offset + 2]);

			auto itr = std::find_if(info.deferred_entry_points.begin(), info.deferred_entry_points.end(),
			                        [id](const DeferredEntryPoint &entry)
			                        { return entry.id == id; });

			if (itr != info.deferred_entry_points.end())
			{
				if ((mode == spv::ExecutionModeLocalSize || mode == spv::ExecutionModeLocalSizeId) && count >= 6)
				{
					if (mode == spv::ExecutionModeLocalSizeId)
					{
						for (unsigned i = 0; i < 3; i++)
							itr->wg_size_id[i] = data[offset + 3 + i];
					}
					else
					{
						for (unsigned i = 0; i < 3; i++)
							itr->wg_size_literal[i] = data[offset + 3 + i];
					}
				}
				else if (op == spv::OpExecutionMode && mode == spv::ExecutionModeOutputPrimitivesEXT && count >= 4)
				{
					itr->max_primitives = data[offset + 3];
				}
				else if (op == spv::OpExecutionMode && mode == spv::ExecutionModeOutputVertices && count >= 4)
				{
					itr->max_vertices = data[offset + 3];
				}
			}
		}
		else if (op == spv::OpDecorate && count >= 4)
		{
			// Now we can get decorations.
			spv::Id target_id = data[offset + 1];
			auto dec = spv::Decoration(data[offset + 2]);
			if (dec == spv::DecorationBuiltIn && spv::BuiltIn(data[offset + 3]) == spv::BuiltInWorkgroupSize)
			{
				global_workgroup_size_id = target_id;
			}
			else if (dec == spv::DecorationSpecId)
			{
				Constant c = {};
				c.id = target_id;
				c.constant_id = data[offset + 3];
				c.has_constant_id = true;
				constants.push_back(c);
			}
		}
		else if ((op == spv::OpConstant || op == spv::OpSpecConstant) && count >= 4)
		{
			spv::Id target_id = data[offset + 2];
			if (count == 4)
				add_constant(target_id, data[offset + 3]);
		}
		else if ((op == spv::OpSpecConstantComposite || op == spv::OpConstantComposite) && count >= 6)
		{
			spv::Id target_id = data[offset + 2];
			if (target_id == global_workgroup_size_id)
			{
				// BuiltInWorkgroupSize overrides all execution modes if it exists (deprecated in SPIR-V 1.6).
				for (auto &entry : info.deferred_entry_points)
					for (unsigned i = 0; i < 3; i++)
						entry.wg_size_id[i] = data[offset + 3 + i];
			}
		}

		offset += count;
	}

	for (auto &entry : info.deferred_entry_points)
	{
		for (unsigned i = 0; i < 3; i++)
		{
			if (entry.wg_size_id[i] != 0)
			{
				auto itr = find_constant(entry.wg_size_id[i]);
				if (itr != constants.end())
				{
					entry.wg_size_literal[i] = itr->literal;
					entry.constant_id[i] = itr->constant_id;
					entry.has_constant_id[i] = itr->has_constant_id;
				}
				else
					return false;
			}
		}
	}

	// If we can statically pass an entry point now, go ahead. Remove it from consideration to save on storage.
	auto itr = std::remove_if(info.deferred_entry_points.begin(), info.deferred_entry_points.end(), [&](const DeferredEntryPoint &e)
	{
		// We have no control over workgroup limits.
		if (e.has_constant_id[0] || e.has_constant_id[1] || e.has_constant_id[2])
			return false;

		// It's possible that we have to fail validate based on FULL_SUBGROUPS usage.
		if (props.subgroup_size_control.maxSubgroupSize &&
		    (e.wg_size_literal[0] % props.subgroup_size_control.maxSubgroupSize) != 0)
		{
			return false;
		}

		// Might have to fail validation based on MaxVertices/MaxPrimitives.
		if (e.execution_model == spv::ExecutionModelMeshEXT &&
		    (e.max_vertices > props.mesh_shader.maxMeshOutputVertices ||
		     e.max_primitives > props.mesh_shader.maxMeshOutputPrimitives))
		{
			return false;
		}

		uint32_t wg_limit[3] = {};
		uint32_t wg_invocations = {};
		switch (e.execution_model)
		{
		case spv::ExecutionModelGLCompute:
			memcpy(wg_limit, props2.properties.limits.maxComputeWorkGroupSize, sizeof(wg_limit));
			wg_invocations = props2.properties.limits.maxComputeWorkGroupInvocations;
			break;

		case spv::ExecutionModelMeshEXT:
			memcpy(wg_limit, props.mesh_shader.maxMeshWorkGroupSize, sizeof(wg_limit));
			wg_invocations = props.mesh_shader.maxMeshWorkGroupInvocations;
			break;

		case spv::ExecutionModelTaskEXT:
			memcpy(wg_limit, props.mesh_shader.maxTaskWorkGroupSize, sizeof(wg_limit));
			wg_invocations = props.mesh_shader.maxTaskWorkGroupInvocations;
			break;

		default:
			break;
		}

		// If anyone tries to create this entry point, we have to fail it.
		for (unsigned i = 0; i < 3; i++)
			if (e.wg_size_literal[i] > wg_limit[i])
				return false;

		if (e.wg_size_literal[0] * e.wg_size_literal[1] * e.wg_size_literal[2] > wg_invocations)
			return false;

		return true;
	});

	info.deferred_entry_points.erase(itr, info.deferred_entry_points.end());

	return true;
}

bool FeatureFilter::Impl::register_shader_module_info(VkShaderModule module, const VkShaderModuleCreateInfo *info)
{
	ModuleInfo module_info;
	if (parse_module_info(info->pCode, info->codeSize, module_info))
	{
		if (!module_info.deferred_entry_points.empty())
		{
			std::lock_guard<std::mutex> holder{module_to_info_lock};
			module_to_info[module] = std::move(module_info);
		}
		return true;
	}
	else
		return false;
}

void FeatureFilter::Impl::unregister_shader_module_info(VkShaderModule module)
{
	std::lock_guard<std::mutex> holder{module_to_info_lock};
	auto itr = module_to_info.find(module);
	if (itr != module_to_info.end())
		module_to_info.erase(itr);
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

	auto num_words = unsigned(size >> 2);
	if (data[0] != spv::MagicNumber)
	{
		LOGE("Invalid magic number of module.\n");
		return false;
	}

	unsigned version = data[1];
	if (version > 0x10600)
	{
		LOGE("SPIR-V version above 1.6 not recognized.\n");
		return false;
	}
	else if (version == 0x10600)
	{
		if (api_version < VK_API_VERSION_1_3)
		{
			LOGE("SPIR-V 1.6 is only supported in Vulkan 1.3 and up.\n");
			return false;
		}
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
	else if (version > 0x10000)
	{
		if (api_version < VK_API_VERSION_1_1)
		{
			LOGE("Need Vulkan 1.1 for SPIR-V 1.1 or 1.2.\n");
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
	// Only allow flags we recognize and validate.
	constexpr VkShaderModuleCreateFlags supported_flags = 0;
	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if (!validate_module_capabilities(info->pCode, info->codeSize))
		return false;

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::render_pass_is_supported(const VkRenderPassCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkRenderPassCreateFlags supported_flags = 0;
	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if (!pnext_chain_is_supported(info->pNext))
		return false;

	for (uint32_t i = 0; i < info->attachmentCount; i++)
	{
		VkFormatFeatureFlags format_features = 0;

		for (uint32_t j = 0; j < info->subpassCount; j++)
		{
			for (uint32_t k = 0; k < info->pSubpasses[j].colorAttachmentCount; k++)
			{
				if (info->pSubpasses[j].pColorAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

				if (info->pSubpasses[j].pResolveAttachments &&
				    info->pSubpasses[j].pResolveAttachments[k].attachment == i)
				{
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
				}
			}

			for (uint32_t k = 0; k < info->pSubpasses[j].inputAttachmentCount; k++)
				if (info->pSubpasses[j].pInputAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

			if (info->pSubpasses[j].pDepthStencilAttachment &&
			    info->pSubpasses[j].pDepthStencilAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}
		}

		if (!attachment_description_is_supported(info->pAttachments[i], format_features))
			return false;
	}

	for (uint32_t i = 0; i < info->subpassCount; i++)
	{
		if (!subpass_description_is_supported(info->pSubpasses[i]))
			return false;
	}

	for (uint32_t i = 0; i < info->dependencyCount; i++)
	{
		if (!subpass_dependency_is_supported(info->pDependencies[i]))
			return false;
	}

	return true;
}

bool FeatureFilter::Impl::format_is_supported(VkFormat format, VkFormatFeatureFlags format_features) const
{
	if (!query)
		return true;

	return query->format_is_supported(format, format_features);
}

bool FeatureFilter::Impl::image_layout_is_supported(VkImageLayout layout) const
{
	switch (layout)
	{
	case VK_IMAGE_LAYOUT_UNDEFINED:
	case VK_IMAGE_LAYOUT_GENERAL:
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
	case VK_IMAGE_LAYOUT_PREINITIALIZED:
		return true;

	case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
		return enabled_extensions.count(VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0;

	case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
		return enabled_extensions.count(VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME) != 0;

	case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
		return api_version >= VK_API_VERSION_1_1 || enabled_extensions.count(VK_KHR_MAINTENANCE2_EXTENSION_NAME) != 0;

	case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
	case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
	case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
		return features.separate_ds_layout.separateDepthStencilLayouts == VK_TRUE;

	case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:
		return features.fragment_shading_rate.attachmentFragmentShadingRate == VK_TRUE;

	case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL_KHR:
	case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL_KHR:
		return features.synchronization2.synchronization2 == VK_TRUE;

	case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
		return features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout == VK_TRUE;

	default:
		return false;
	}
}

bool FeatureFilter::Impl::attachment_reference_is_supported(const VkAttachmentReference &ref) const
{
	if (!image_layout_is_supported(ref.layout))
		return false;

	return true;
}

bool FeatureFilter::Impl::attachment_reference2_is_supported(const VkAttachmentReference2 &ref) const
{
	if (!pnext_chain_is_supported(ref.pNext))
		return false;
	if (!image_layout_is_supported(ref.layout))
		return false;

	return true;
}

bool FeatureFilter::Impl::attachment_description_is_supported(const VkAttachmentDescription &desc,
                                                              VkFormatFeatureFlags format_features) const
{
	if (!image_layout_is_supported(desc.initialLayout))
		return false;
	if (!image_layout_is_supported(desc.finalLayout))
		return false;
	if (format_features && !format_is_supported(desc.format, format_features))
		return false;

	return true;
}

bool FeatureFilter::Impl::attachment_description2_is_supported(const VkAttachmentDescription2 &desc,
                                                               VkFormatFeatureFlags format_features) const
{
	if (!pnext_chain_is_supported(desc.pNext))
		return false;
	if (!image_layout_is_supported(desc.initialLayout))
		return false;
	if (!image_layout_is_supported(desc.finalLayout))
		return false;
	if (format_features && !format_is_supported(desc.format, format_features))
		return false;

	return true;
}

bool FeatureFilter::Impl::subpass_description_is_supported(const VkSubpassDescription &sub) const
{
	for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
	{
		if (!attachment_reference_is_supported(sub.pColorAttachments[j]))
			return false;
		if (sub.pResolveAttachments && !attachment_reference_is_supported(sub.pResolveAttachments[j]))
			return false;
	}

	for (uint32_t j = 0; j < sub.inputAttachmentCount; j++)
		if (!attachment_reference_is_supported(sub.pInputAttachments[j]))
			return false;

	if (sub.pDepthStencilAttachment && !attachment_reference_is_supported(*sub.pDepthStencilAttachment))
		return false;

	return true;
}

bool FeatureFilter::Impl::subpass_description2_is_supported(const VkSubpassDescription2 &sub) const
{
	if (!pnext_chain_is_supported(sub.pNext))
		return false;

	for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
	{
		if (!attachment_reference2_is_supported(sub.pColorAttachments[j]))
			return false;
		if (sub.pResolveAttachments && !attachment_reference2_is_supported(sub.pResolveAttachments[j]))
			return false;
	}

	for (uint32_t j = 0; j < sub.inputAttachmentCount; j++)
		if (!attachment_reference2_is_supported(sub.pInputAttachments[j]))
			return false;

	if (sub.pDepthStencilAttachment && !attachment_reference2_is_supported(*sub.pDepthStencilAttachment))
		return false;

	if (sub.viewMask && !multiview_mask_is_supported(sub.viewMask))
		return false;

	return true;
}

bool FeatureFilter::Impl::dependency_flags_is_supported(VkDependencyFlags deps) const
{
	constexpr VkDependencyFlags supported_flags = VK_DEPENDENCY_BY_REGION_BIT |
	                                              VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT |
	                                              VK_DEPENDENCY_VIEW_LOCAL_BIT;

	if ((deps & ~supported_flags) != 0)
		return false;

	if ((deps & VK_DEPENDENCY_FEEDBACK_LOOP_BIT_EXT) != 0)
		if (!features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout)
			return false;

	if ((deps & VK_DEPENDENCY_VIEW_LOCAL_BIT) != 0)
		if (!features.multiview.multiview)
			return false;

	return true;
}

bool FeatureFilter::Impl::subpass_dependency_is_supported(const VkSubpassDependency &dep) const
{
	if (!dependency_flags_is_supported(dep.dependencyFlags))
		return false;
	return true;
}

bool FeatureFilter::Impl::subpass_dependency2_is_supported(const VkSubpassDependency2 &dep) const
{
	if (!dependency_flags_is_supported(dep.dependencyFlags))
		return false;
	if (!pnext_chain_is_supported(dep.pNext))
		return false;

	return true;
}

bool FeatureFilter::Impl::stage_limits_are_supported(const VkPipelineShaderStageCreateInfo &info) const
{
	spv::ExecutionModel target_model;
	uint32_t limit_workgroup_size[3];
	uint32_t limit_invocations;

	if (!info.pName)
		return false;

	switch (info.stage)
	{
	case VK_SHADER_STAGE_COMPUTE_BIT:
		target_model = spv::ExecutionModelGLCompute;
		memcpy(limit_workgroup_size, props2.properties.limits.maxComputeWorkGroupSize, sizeof(limit_workgroup_size));
		limit_invocations = props2.properties.limits.maxComputeWorkGroupInvocations;
		break;

	case VK_SHADER_STAGE_TASK_BIT_EXT:
		target_model = spv::ExecutionModelTaskEXT;
		memcpy(limit_workgroup_size, props.mesh_shader.maxTaskWorkGroupSize, sizeof(limit_workgroup_size));
		limit_invocations = props.mesh_shader.maxTaskWorkGroupInvocations;
		break;

	case VK_SHADER_STAGE_MESH_BIT_EXT:
		target_model = spv::ExecutionModelMeshEXT;
		memcpy(limit_workgroup_size, props.mesh_shader.maxMeshWorkGroupSize, sizeof(limit_workgroup_size));
		limit_invocations = props.mesh_shader.maxMeshWorkGroupInvocations;
		break;

	default:
		// Shouldn't happen. Only happens for invalid input.
		return true;
	}

	std::lock_guard<std::mutex> holder{module_to_info_lock};

	// If we cannot find anything, assume that we're statically okay.
	auto itr = module_to_info.find(info.module);
	if (itr == module_to_info.end())
		return true;

	const DeferredEntryPoint *deferred_entry_point = nullptr;
	for (auto &entry : itr->second.deferred_entry_points)
	{
		if (target_model == entry.execution_model && entry.name == info.pName)
		{
			deferred_entry_point = &entry;
			break;
		}
	}

	if (deferred_entry_point)
	{
		auto &entry = *deferred_entry_point;
		uint32_t wg_size[3] = {};

		if (entry.execution_model == spv::ExecutionModelMeshEXT)
		{
			if (entry.max_primitives > props.mesh_shader.maxMeshOutputPrimitives)
				return false;
			if (entry.max_vertices > props.mesh_shader.maxMeshOutputVertices)
				return false;
		}

		for (unsigned i = 0; i < 3; i++)
		{
			wg_size[i] = entry.wg_size_literal[i];
			if (entry.has_constant_id[i] && info.pSpecializationInfo)
			{
				auto *data = static_cast<const uint8_t *>(info.pSpecializationInfo->pData);
				for (unsigned j = 0; j < info.pSpecializationInfo->mapEntryCount; j++)
				{
					auto &map_entry = info.pSpecializationInfo->pMapEntries[j];
					if (map_entry.constantID == entry.constant_id[i] &&
					    map_entry.size == sizeof(uint32_t) &&
					    map_entry.offset + sizeof(uint32_t) <= info.pSpecializationInfo->dataSize)
					{
						memcpy(&wg_size[i], data + map_entry.offset, sizeof(uint32_t));
						break;
					}
				}
			}

			if (wg_size[i] > limit_workgroup_size[i])
				return false;
		}

		if (wg_size[0] * wg_size[1] * wg_size[2] > limit_invocations)
			return false;

		if (info.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT)
		{
			auto *required = find_pnext<VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT>(
					VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
					info.pNext);

			uint32_t target_subgroup_size = required ?
					required->requiredSubgroupSize : props.subgroup_size_control.maxSubgroupSize;

			if (!target_subgroup_size)
				return false;

			if (wg_size[0] % target_subgroup_size != 0)
			{
				// Can only use full subgroups if we're guaranteed maxSubgroupSize is contained.
				// If we're using required size, we can statically validate it.
				return false;
			}
		}
	}

	return true;
}

bool FeatureFilter::Impl::subgroup_size_control_is_supported(const VkPipelineShaderStageCreateInfo &stage) const
{
	constexpr VkPipelineShaderStageCreateFlags supported_flags =
			VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT |
			VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;

	if ((stage.flags & ~supported_flags) != 0)
		return false;

	if ((stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT) != 0 &&
	    features.subgroup_size_control.subgroupSizeControl == VK_FALSE)
	{
		return false;
	}

	bool is_compute_like_stage =
			stage.stage == VK_SHADER_STAGE_MESH_BIT_EXT ||
			stage.stage == VK_SHADER_STAGE_TASK_BIT_EXT ||
			stage.stage == VK_SHADER_STAGE_COMPUTE_BIT;

	if ((stage.flags & VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT) != 0 &&
	    (!is_compute_like_stage ||
	     features.subgroup_size_control.computeFullSubgroups == VK_FALSE ||
	     features.subgroup_size_control.subgroupSizeControl == VK_FALSE))
	{
		return false;
	}

	auto *required = find_pnext<VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT>(
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
			stage.pNext);

	if (required)
	{
		if (features.subgroup_size_control.subgroupSizeControl == VK_FALSE ||
		    (props.subgroup_size_control.requiredSubgroupSizeStages & stage.stage) == 0 ||
		    required->requiredSubgroupSize > props.subgroup_size_control.maxSubgroupSize ||
		    required->requiredSubgroupSize < props.subgroup_size_control.minSubgroupSize)
		{
			return false;
		}
	}

	return true;
}

bool FeatureFilter::Impl::render_pass2_is_supported(const VkRenderPassCreateInfo2 *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkRenderPassCreateFlags supported_flags = 0;
	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if (!enabled_extensions.count(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) && api_version < VK_API_VERSION_1_2)
		return false;

	if (!pnext_chain_is_supported(info->pNext))
		return false;

	for (uint32_t i = 0; i < info->attachmentCount; i++)
	{
		VkFormatFeatureFlags format_features = 0;

		for (uint32_t j = 0; j < info->subpassCount; j++)
		{
			for (uint32_t k = 0; k < info->pSubpasses[j].colorAttachmentCount; k++)
			{
				if (info->pSubpasses[j].pColorAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

				if (info->pSubpasses[j].pResolveAttachments &&
				    info->pSubpasses[j].pResolveAttachments[k].attachment == i)
				{
					format_features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
				}
			}

			for (uint32_t k = 0; k < info->pSubpasses[j].inputAttachmentCount; k++)
				if (info->pSubpasses[j].pInputAttachments[k].attachment == i)
					format_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

			if (info->pSubpasses[j].pDepthStencilAttachment &&
			    info->pSubpasses[j].pDepthStencilAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}

			auto *ds_resolve = find_pnext<VkSubpassDescriptionDepthStencilResolve>(
					VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE, info->pSubpasses[j].pNext);
			if (ds_resolve &&
			    ds_resolve->pDepthStencilResolveAttachment &&
			    ds_resolve->pDepthStencilResolveAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}

			auto *rate_attachment = find_pnext<VkFragmentShadingRateAttachmentInfoKHR>(
					VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR, info->pSubpasses[j].pNext);
			if (rate_attachment &&
			    rate_attachment->pFragmentShadingRateAttachment &&
			    rate_attachment->pFragmentShadingRateAttachment->attachment == i)
			{
				format_features |= VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
			}
		}

		if (!attachment_description2_is_supported(info->pAttachments[i], format_features))
			return false;
	}

	for (uint32_t i = 0; i < info->subpassCount; i++)
	{
		if (!subpass_description2_is_supported(info->pSubpasses[i]))
			return false;
	}

	for (uint32_t i = 0; i < info->dependencyCount; i++)
	{
		if (!subpass_dependency2_is_supported(info->pDependencies[i]))
			return false;
	}

	for (uint32_t i = 0; i < info->correlatedViewMaskCount; i++)
		if (!multiview_mask_is_supported(info->pCorrelatedViewMasks[i]))
			return false;

	return true;
}

bool FeatureFilter::Impl::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineCreateFlags supported_flags =
			VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
			VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
			VK_PIPELINE_CREATE_DERIVATIVE_BIT |
			VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT |
			VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT |
			VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
			VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
			VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT |
			VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT |
			VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
			VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;

	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if ((info->flags & VK_PIPELINE_CREATE_VIEW_INDEX_FROM_DEVICE_INDEX_BIT) != 0 &&
	    api_version < VK_API_VERSION_1_1)
	{
		return false;
	}

	if ((info->flags & VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR) != 0 &&
	    (enabled_extensions.count(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0 ||
	     enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0))
	{
		return false;
	}

	if ((info->flags & VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT) != 0 &&
	    (enabled_extensions.count(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) == 0 ||
	     enabled_extensions.count(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0))
	{
		return false;
	}

	if ((info->flags & (VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
	                    VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT |
	                    VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT)) != 0 &&
	    features.graphics_pipeline_library.graphicsPipelineLibrary == VK_FALSE)
	{
		return false;
	}

	if ((info->flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
		if (!features.descriptor_buffer.descriptorBuffer)
			return false;

	if ((info->flags & (VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT |
	                    VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)) != 0)
	{
		if (!features.attachment_feedback_loop_layout.attachmentFeedbackLoopLayout)
			return false;
	}

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

	if (info->renderPass == VK_NULL_HANDLE && features.dynamic_rendering.dynamicRendering == VK_FALSE)
		return false;

	if (info->pVertexInputState)
	{
		for (uint32_t i = 0; i < info->pVertexInputState->vertexAttributeDescriptionCount; i++)
		{
			if (!format_is_supported(info->pVertexInputState->pVertexAttributeDescriptions[i].format,
			                         VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
				return false;
		}
	}

	if (info->pDynamicState)
	{
		auto &dyn = *info->pDynamicState;
		for (uint32_t i = 0; i < dyn.dynamicStateCount; i++)
		{
			switch (dyn.pDynamicStates[i])
			{
			case VK_DYNAMIC_STATE_CULL_MODE_EXT:
			case VK_DYNAMIC_STATE_FRONT_FACE_EXT:
			case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY_EXT:
			case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT:
			case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT:
			case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT:
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT:
			case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT:
			case VK_DYNAMIC_STATE_STENCIL_OP_EXT:
				if (!features.extended_dynamic_state.extendedDynamicState && api_version < VK_API_VERSION_1_3)
					return false;
				break;

			case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT:
				if (!features.extended_dynamic_state2.extendedDynamicState2PatchControlPoints)
					return false;
				break;

			case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
				if (!features.extended_dynamic_state2.extendedDynamicState2LogicOp)
					return false;
				break;

			case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT:
			case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT:
			case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT:
				if (!features.extended_dynamic_state2.extendedDynamicState2 && api_version < VK_API_VERSION_1_3)
					return false;
				break;

			case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT:
				if (!features.color_write_enable.colorWriteEnable)
					return false;
				break;

			case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
				if (!features.vertex_input_dynamic_state.vertexInputDynamicState)
					return false;
				break;

			case VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR:
				// Only support dynamic fragment shading rate for now.
				// pNext variant needs to validate against vkGetPhysicalDeviceFragmentShadingRatesKHR on top.
				if (features.fragment_shading_rate.pipelineFragmentShadingRate == VK_FALSE)
					return false;
				break;

			case VK_DYNAMIC_STATE_VIEWPORT_W_SCALING_NV:
				if (!enabled_extensions.count(VK_NV_CLIP_SPACE_W_SCALING_EXTENSION_NAME))
					return false;
				break;

			case VK_DYNAMIC_STATE_DISCARD_RECTANGLE_EXT:
				if (!enabled_extensions.count(VK_EXT_DISCARD_RECTANGLES_EXTENSION_NAME))
					return false;
				break;

			case VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT:
				if (!enabled_extensions.count(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME))
					return false;
				break;

			case VK_DYNAMIC_STATE_VIEWPORT_SHADING_RATE_PALETTE_NV:
				if (!features.shading_rate_nv.shadingRateImage)
					return false;
				break;

			case VK_DYNAMIC_STATE_VIEWPORT_COARSE_SAMPLE_ORDER_NV:
				if (!features.shading_rate_nv.shadingRateCoarseSampleOrder)
					return false;
				break;

			case VK_DYNAMIC_STATE_EXCLUSIVE_SCISSOR_NV:
				if (!enabled_extensions.count(VK_NV_SCISSOR_EXCLUSIVE_EXTENSION_NAME))
					return false;
				break;

			case VK_DYNAMIC_STATE_LINE_STIPPLE_EXT:
				if (!enabled_extensions.count(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME))
					return false;
				break;

#define DYN_STATE3(state, member) case VK_DYNAMIC_STATE_##state: \
	if (!enabled_extensions.count(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) || \
			features.extended_dynamic_state3.extendedDynamicState3##member == VK_FALSE) \
		return false; \
	break

			DYN_STATE3(TESSELLATION_DOMAIN_ORIGIN_EXT, TessellationDomainOrigin);
			DYN_STATE3(DEPTH_CLAMP_ENABLE_EXT, DepthClampEnable);
			DYN_STATE3(POLYGON_MODE_EXT, PolygonMode);
			DYN_STATE3(RASTERIZATION_SAMPLES_EXT, RasterizationSamples);
			DYN_STATE3(SAMPLE_MASK_EXT, SampleMask);
			DYN_STATE3(ALPHA_TO_COVERAGE_ENABLE_EXT, AlphaToCoverageEnable);
			DYN_STATE3(ALPHA_TO_ONE_ENABLE_EXT, AlphaToOneEnable);
			DYN_STATE3(LOGIC_OP_ENABLE_EXT, LogicOpEnable);
			DYN_STATE3(COLOR_BLEND_ENABLE_EXT, ColorBlendEnable);
			DYN_STATE3(COLOR_BLEND_EQUATION_EXT, ColorBlendEquation);
			DYN_STATE3(COLOR_WRITE_MASK_EXT, ColorWriteMask);
			DYN_STATE3(RASTERIZATION_STREAM_EXT, RasterizationStream);
			DYN_STATE3(CONSERVATIVE_RASTERIZATION_MODE_EXT, ConservativeRasterizationMode);
			DYN_STATE3(EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT, ExtraPrimitiveOverestimationSize);
			DYN_STATE3(DEPTH_CLIP_ENABLE_EXT, DepthClipEnable);
			DYN_STATE3(SAMPLE_LOCATIONS_ENABLE_EXT, SampleLocationsEnable);
			DYN_STATE3(COLOR_BLEND_ADVANCED_EXT, ColorBlendAdvanced);
			DYN_STATE3(PROVOKING_VERTEX_MODE_EXT, ProvokingVertexMode);
			DYN_STATE3(LINE_RASTERIZATION_MODE_EXT, LineRasterizationMode);
			DYN_STATE3(LINE_STIPPLE_ENABLE_EXT, LineStippleEnable);
			DYN_STATE3(DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT, DepthClipNegativeOneToOne);
			DYN_STATE3(VIEWPORT_W_SCALING_ENABLE_NV, ViewportWScalingEnable);
			DYN_STATE3(VIEWPORT_SWIZZLE_NV, ViewportSwizzle);
			DYN_STATE3(COVERAGE_TO_COLOR_ENABLE_NV, CoverageToColorEnable);
			DYN_STATE3(COVERAGE_TO_COLOR_LOCATION_NV, CoverageToColorLocation);
			DYN_STATE3(COVERAGE_MODULATION_MODE_NV, CoverageModulationMode);
			DYN_STATE3(COVERAGE_MODULATION_TABLE_ENABLE_NV, CoverageModulationTableEnable);
			DYN_STATE3(COVERAGE_MODULATION_TABLE_NV, CoverageModulationTable);
			DYN_STATE3(SHADING_RATE_IMAGE_ENABLE_NV, ShadingRateImageEnable);
			DYN_STATE3(REPRESENTATIVE_FRAGMENT_TEST_ENABLE_NV, RepresentativeFragmentTestEnable);
			DYN_STATE3(COVERAGE_REDUCTION_MODE_NV, CoverageReductionMode);
#undef DYN_STATE3

			case VK_DYNAMIC_STATE_VIEWPORT:
			case VK_DYNAMIC_STATE_SCISSOR:
			case VK_DYNAMIC_STATE_LINE_WIDTH:
			case VK_DYNAMIC_STATE_DEPTH_BIAS:
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
				// Part of core.
				break;

			default:
				// Unrecognized dynamic state, we almost certainly have not enabled the feature.
				return false;
			}
		}
	}

	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		switch (info->pStages[i].stage)
		{
		case VK_SHADER_STAGE_VERTEX_BIT:
		case VK_SHADER_STAGE_FRAGMENT_BIT:
			break;

		case VK_SHADER_STAGE_COMPUTE_BIT:
			return false;

		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			if (!features2.features.tessellationShader)
				return false;
			break;

		case VK_SHADER_STAGE_GEOMETRY_BIT:
			if (!features2.features.geometryShader)
				return false;
			break;

		case VK_SHADER_STAGE_MESH_BIT_EXT:
			if (!features.mesh_shader.meshShader && !features.mesh_shader_nv.meshShader)
				return false;
			if (!stage_limits_are_supported(info->pStages[i]))
				return false;
			break;

		case VK_SHADER_STAGE_TASK_BIT_EXT:
			if (!features.mesh_shader.taskShader && !features.mesh_shader_nv.taskShader)
				return false;
			if (!stage_limits_are_supported(info->pStages[i]))
				return false;
			break;

		default:
			return false;
		}

		if (!subgroup_size_control_is_supported(info->pStages[i]))
			return false;
		if (!pnext_chain_is_supported(info->pStages[i].pNext))
			return false;
	}

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineCreateFlags supported_flags =
			VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
			VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
			VK_PIPELINE_CREATE_DERIVATIVE_BIT |
			VK_PIPELINE_CREATE_DISPATCH_BASE_BIT |
			VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if ((info->flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
		if (!features.descriptor_buffer.descriptorBuffer)
			return false;

	if ((info->flags & VK_PIPELINE_CREATE_DISPATCH_BASE_BIT) != 0 &&
	    api_version < VK_API_VERSION_1_1)
	{
		return false;
	}

	if (!subgroup_size_control_is_supported(info->stage))
		return false;

	if (!stage_limits_are_supported(info->stage))
		return false;

	return pnext_chain_is_supported(info->pNext);
}

bool FeatureFilter::Impl::raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info) const
{
	// Only allow flags we recognize and validate.
	constexpr VkPipelineCreateFlags supported_flags =
			VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT |
			VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT |
			VK_PIPELINE_CREATE_DERIVATIVE_BIT |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_ANY_HIT_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_CLOSEST_HIT_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_INTERSECTION_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_NO_NULL_MISS_SHADERS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_SKIP_AABBS_BIT_KHR |
			VK_PIPELINE_CREATE_RAY_TRACING_SKIP_TRIANGLES_BIT_KHR |
			VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
			VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
			VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;

	if ((info->flags & ~supported_flags) != 0)
		return false;

	if (null_device)
		return true;

	if ((info->flags & (VK_PIPELINE_CREATE_RAY_TRACING_SKIP_AABBS_BIT_KHR |
	                    VK_PIPELINE_CREATE_RAY_TRACING_SKIP_TRIANGLES_BIT_KHR)) != 0 &&
	    features.ray_tracing_pipeline.rayTraversalPrimitiveCulling == VK_FALSE)
	{
		return false;
	}

	if ((info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0 &&
	    !enabled_extensions.count(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME))
	{
		return false;
	}

	if ((info->flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0)
		if (!features.descriptor_buffer.descriptorBuffer)
			return false;

	if (features.ray_tracing_pipeline.rayTracingPipeline == VK_FALSE)
		return false;

	if (info->maxPipelineRayRecursionDepth > props.ray_tracing_pipeline.maxRayRecursionDepth)
		return false;

	if (info->pDynamicState)
	{
		for (uint32_t i = 0; i < info->pDynamicState->dynamicStateCount; i++)
			if (info->pDynamicState->pDynamicStates[i] != VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR)
				return false;
		if (!pnext_chain_is_supported(info->pDynamicState->pNext))
			return false;
	}

	if ((info->pLibraryInfo || info->pLibraryInterface) &&
	    !enabled_extensions.count(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME))
		return false;

	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		if (!subgroup_size_control_is_supported(info->pStages[i]))
			return false;
		if (!pnext_chain_is_supported(info->pStages[i].pNext))
			return false;
	}

	if (info->pLibraryInterface)
	{
		if (info->pLibraryInterface->maxPipelineRayHitAttributeSize > props.ray_tracing_pipeline.maxRayHitAttributeSize)
			return false;
		if (!pnext_chain_is_supported(info->pLibraryInterface->pNext))
			return false;
	}

	if (info->pLibraryInfo)
	{
		if (!pnext_chain_is_supported(info->pLibraryInfo->pNext))
			return false;
	}

	for (uint32_t i = 0; i < info->groupCount; i++)
	{
		if (!pnext_chain_is_supported(info->pGroups[i].pNext))
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

bool FeatureFilter::register_shader_module_info(VkShaderModule module, const VkShaderModuleCreateInfo *info)
{
	return impl->register_shader_module_info(module, info);
}

void FeatureFilter::unregister_shader_module_info(VkShaderModule module)
{
	impl->unregister_shader_module_info(module);
}

bool FeatureFilter::render_pass_is_supported(const VkRenderPassCreateInfo *info) const
{
	return impl->render_pass_is_supported(info);
}

bool FeatureFilter::render_pass2_is_supported(const VkRenderPassCreateInfo2 *info) const
{
	return impl->render_pass2_is_supported(info);
}

bool FeatureFilter::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const
{
	return impl->graphics_pipeline_is_supported(info);
}

bool FeatureFilter::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const
{
	return impl->compute_pipeline_is_supported(info);
}

bool FeatureFilter::raytracing_pipeline_is_supported(const VkRayTracingPipelineCreateInfoKHR *info) const
{
	return impl->raytracing_pipeline_is_supported(info);
}

bool FeatureFilter::supports_scalar_block_layout() const
{
	return impl->null_device || impl->features.vk12.scalarBlockLayout;
}

void FeatureFilter::set_device_query_interface(DeviceQueryInterface *iface)
{
	impl->query = iface;
}
}
