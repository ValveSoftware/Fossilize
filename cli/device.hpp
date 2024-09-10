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

#pragma once

#include "volk.h"
#include "fossilize_feature_filter.hpp"
#include <vector>

namespace Fossilize
{
class VulkanDevice : public DeviceQueryInterface
{
public:
	struct Options
	{
		bool enable_validation = false;
		bool want_amd_shader_info = false;
		bool null_device = false;
		bool want_pipeline_stats = false;
		int device_index = -1;
		const VkApplicationInfo *application_info = nullptr;
		const VkPhysicalDeviceFeatures2 *features = nullptr;
	};
	bool init_device(const Options &opts);

	VulkanDevice() = default;
	~VulkanDevice();
	VulkanDevice(VulkanDevice &&) = delete;
	void operator=(VulkanDevice &&) = delete;

	VkDevice get_device() const
	{
		return device;
	}

	VkPhysicalDevice get_gpu() const
	{
		return gpu;
	}

	uint32_t get_api_version() const
	{
		return api_version;
	}

	void set_validation_error_callback(void (*callback)(void *), void *userdata);
	void notify_validation_error();

	bool pipeline_feedback_enabled() const
	{
		return supports_pipeline_feedback;
	}

	bool module_identifiers_enabled() const
	{
		return supports_module_identifiers;
	}

	const VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT &get_module_identifier_properties() const
	{
		return props.shader_module_identifier;
	}

	bool has_pipeline_stats() const
	{
		return pipeline_stats;
	}

	bool has_validation_cache() const
	{
		return validation_cache;
	}

	bool has_amd_shader_info() const
	{
		return amd_shader_info;
	}

	FeatureFilter &get_feature_filter()
	{
		return feature_filter;
	}

	const VkPhysicalDeviceProperties &get_gpu_properties() const
	{
		return gpu_props;
	}

	// Special helper which deals with SAMPLER_YCBCR_CONVERSION_CREATE_INFO.
	// Prefer using this instead of vkCreateSampler directly.
	// The relevant pNext will be mutated into CONVERSION_INFO in-place if it exists.
	// Should only be called from enqueue_create_sampler().
	VkResult create_sampler_with_ycbcr_remap(const VkSamplerCreateInfo *create_info, VkSampler *sampler);

private:
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkDebugReportCallbackEXT callback = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties gpu_props = {};
	uint32_t api_version = 0;

	void (*validation_callback)(void *) = nullptr;
	void *validation_callback_userdata = nullptr;
	bool supports_pipeline_feedback = false;
	bool supports_module_identifiers = false;

	void init_null_device();
	bool is_null_device = false;
	bool pipeline_stats = false;
	bool validation_cache = false;
	bool amd_shader_info = false;

	VulkanFeatures features = {};
	VulkanProperties props = {};
	FeatureFilter feature_filter;

	std::vector<VkSamplerYcbcrConversion> ycbcr_conversions;

	bool format_is_supported(VkFormat format, VkFormatFeatureFlags features) override;
	bool descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info) override;
	void physical_device_feature_query(VkPhysicalDeviceFeatures2 *pdf2) override;
};
}
