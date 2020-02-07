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
#include <unordered_set>
#include <string.h>

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
#define F_EXT(struct_type, member) \
	CHAIN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_EXT, features.member)

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
	F_EXT(TRANSFORM_FEEDBACK, transform_feedback);
	F_EXT(DEPTH_CLIP_ENABLE, depth_clip);
	F_EXT(INLINE_UNIFORM_BLOCK, inline_uniform_block);
	F_EXT(BLEND_OPERATION_ADVANCED, blend_operation_advanced);
	F_EXT(VERTEX_ATTRIBUTE_DIVISOR, attribute_divisor);
	F_EXT(SHADER_DEMOTE_TO_HELPER_INVOCATION, demote_to_helper);
	F_EXT(FRAGMENT_SHADER_INTERLOCK, shader_interlock);

#undef CHAIN
#undef F
#undef F_EXT

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

	P(DESCRIPTOR_INDEXING, descriptor_indexing);

#undef CHAIN
#undef P

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

#define F_EXT(struct_type, member) case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_##struct_type##_FEATURES_EXT: \
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
		F_EXT(TRANSFORM_FEEDBACK, transform_feedback);
		F_EXT(DEPTH_CLIP_ENABLE, depth_clip);
		F_EXT(INLINE_UNIFORM_BLOCK, inline_uniform_block);
		F_EXT(BLEND_OPERATION_ADVANCED, blend_operation_advanced);
		F_EXT(VERTEX_ATTRIBUTE_DIVISOR, attribute_divisor);
		F_EXT(SHADER_DEMOTE_TO_HELPER_INVOCATION, demote_to_helper);
		F_EXT(FRAGMENT_SHADER_INTERLOCK, shader_interlock);
		default:
			break;
		}

#undef F
#undef F_EXT

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

		switch (base->sType)
		{
		P(DESCRIPTOR_INDEXING, descriptor_indexing);
		default:
			break;
		}

#undef P

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

bool FeatureFilter::Impl::sampler_is_supported(const VkSamplerCreateInfo *info) const
{
	return true;
}

bool FeatureFilter::Impl::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) const
{
	return true;
}

bool FeatureFilter::Impl::pipeline_layout_is_supported(const VkPipelineLayoutCreateInfo *info) const
{
	return true;
}

bool FeatureFilter::Impl::shader_module_is_supported(const VkShaderModuleCreateInfo *info) const
{
	return true;
}

bool FeatureFilter::Impl::render_pass_is_supported(const VkRenderPassCreateInfo *info) const
{
	return true;
}

bool FeatureFilter::Impl::graphics_pipeline_is_supported(const VkGraphicsPipelineCreateInfo *info) const
{
	return true;
}

bool FeatureFilter::Impl::compute_pipeline_is_supported(const VkComputePipelineCreateInfo *info) const
{
	return true;
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
