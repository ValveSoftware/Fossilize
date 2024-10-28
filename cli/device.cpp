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

#include "device.hpp"
#include "logging.hpp"
#include <vector>
#include <algorithm>
#include <string.h>

using namespace std;

namespace Fossilize
{
static bool find_layer(const vector<VkLayerProperties> &layers, const char *layer)
{
	auto itr = find_if(begin(layers), end(layers), [&](const VkLayerProperties &prop) -> bool {
		return strcmp(layer, prop.layerName) == 0;
	});
	return itr != end(layers);
}

static bool find_extension(const vector<VkExtensionProperties> &exts, const char *ext)
{
	auto itr = find_if(begin(exts), end(exts), [&](const VkExtensionProperties &prop) -> bool {
		return strcmp(ext, prop.extensionName) == 0;
	});
	return itr != end(exts);
}

static bool filter_instance_extension(const char *ext, uint32_t api_version)
{
	if (api_version >= VK_API_VERSION_1_1)
		return true;

	static const char *vulkan_11_only_extensions[] = {
		VK_KHR_SURFACE_PROTECTED_CAPABILITIES_EXTENSION_NAME,
	};

	for (auto *candidate : vulkan_11_only_extensions)
		if (strcmp(candidate, ext) == 0)
			return false;

	return true;
}

static bool filter_extension(const char *ext, bool want_amd_shader_info,
                             const vector<VkExtensionProperties> &all_exts, uint32_t api_version)
{
	static const char *vulkan_11_only_extensions[] = {
		VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME,
		VK_KHR_SPIRV_1_4_EXTENSION_NAME,
		VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME,
		VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
		VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
		VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
		VK_KHR_RAY_QUERY_EXTENSION_NAME,
		VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
		VK_KHR_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_EXTENSION_NAME,
		VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
		VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME,
		VK_NV_SHADER_SUBGROUP_PARTITIONED_EXTENSION_NAME,
		VK_NV_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
	};

	bool ext_is_vulkan_11_only = false;
	for (auto *candidate : vulkan_11_only_extensions)
	{
		if (strcmp(candidate, ext) == 0)
		{
			ext_is_vulkan_11_only = true;
			break;
		}
	}

	if (strcmp(ext, VK_AMD_NEGATIVE_VIEWPORT_HEIGHT_EXTENSION_NAME) == 0 ||
	    strcmp(ext, VK_NV_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0)
	{
		// AMD_negative_viewport_height: Obsolete, illegal to enable along maintenance1.
		// NV_fragment_shader: Various enum conflicts,
		// not considered important enough to work around a vendor extension for this.
		// Don't want to enable both NV and KHR extensions together.
		return false;
	}
	else if (strcmp(ext, VK_NV_RAY_TRACING_EXTENSION_NAME) == 0)
	{
		// We never enable the features, and it is known to cause some shenanigans with pipeline replay.
		return false;
	}
	else if (strcmp(ext, VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0 &&
	         find_extension(all_exts, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
	{
		// Cannot enable both EXT and KHR versions, validation complains.
		return false;
	}
	else if (api_version < VK_API_VERSION_1_1 && ext_is_vulkan_11_only)
	{
		return false;
	}
	else if (strcmp(ext, VK_AMD_SHADER_INFO_EXTENSION_NAME) == 0 && !want_amd_shader_info)
	{
		// Mesa disables the pipeline cache when VK_AMD_shader_info is used, so disable this extension unless we need it.
		return false;
	}

	return true;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugReportFlagsEXT flags,
                                                     VkDebugReportObjectTypeEXT, uint64_t,
                                                     size_t, int32_t, const char *pLayerPrefix,
                                                     const char *pMessage, void *pUserData)
{
	if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT)
	{
		LOGE("[Layer]: Error: %s: %s\n", pLayerPrefix, pMessage);
		VulkanDevice *device = static_cast<VulkanDevice *>(pUserData);
		device->notify_validation_error();
	}

	return VK_FALSE;
}

static uint32_t major_minor_version(uint32_t version)
{
	return VK_MAKE_VERSION(VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), 0);
}

static bool application_info_promote_robustness2(const VkApplicationInfo *app_info)
{
	if (!app_info || !app_info->pEngineName)
		return false;

	static const char *promote_engine_names[] =
	{
		"DXVK",
		"vkd3d",
	};

	for (const char *name : promote_engine_names)
		if (strcmp(name, app_info->pEngineName) == 0)
			return true;

	return false;
}

static bool application_info_promote_fragment_shading_rate(const VkApplicationInfo *app_info)
{
	if (!app_info || !app_info->pEngineName)
		return false;

	if (strcmp("vkd3d", app_info->pEngineName) == 0)
		return true;

	return false;
}

bool VulkanDevice::init_device(const Options &opts)
{
	if (opts.null_device)
	{
		init_null_device();
		return true;
	}

	if (volkInitialize() != VK_SUCCESS)
	{
		LOGE("volkInitialize failed.\n");
		return false;
	}

	uint32_t instance_api_version = major_minor_version(volkGetInstanceVersion());
	if (instance_api_version == 0)
	{
		LOGE("Could not find loader.\n");
		return false;
	}

	uint32_t target_api_version = opts.application_info ? opts.application_info->apiVersion : instance_api_version;
	target_api_version = major_minor_version(target_api_version);

	if (target_api_version > instance_api_version)
	{
		LOGE("Database is targeting an API version which is unsupported by this Vulkan loader.\n");
		return false;
	}

	// Enable all extensions (FIXME: this is likely a problem).
	uint32_t ext_count = 0;
	if (vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, nullptr) != VK_SUCCESS)
		return false;
	vector<VkExtensionProperties> exts(ext_count);
	if (ext_count && vkEnumerateInstanceExtensionProperties(nullptr, &ext_count, exts.data()) != VK_SUCCESS)
		return false;

	vector<const char *> active_layers;
	if (opts.enable_validation)
	{
		uint32_t layer_count = 0;
		if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) != VK_SUCCESS)
			return false;
		vector<VkLayerProperties> layers(layer_count);
		if (vkEnumerateInstanceLayerProperties(&layer_count, layers.data()) != VK_SUCCESS)
			return false;

		if (find_layer(layers, "VK_LAYER_KHRONOS_validation"))
			active_layers.push_back("VK_LAYER_KHRONOS_validation");
		else
		{
			LOGE("Cannot find VK_LAYER_KHRONOS_validation layer.\n");
			return false;
		}
	}

	bool use_debug_callback = find_extension(exts, VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.apiVersion = instance_api_version;
	app.pApplicationName = "Fossilize Replayer";
	app.pEngineName = "Fossilize";

	instance_info.enabledLayerCount = uint32_t(active_layers.size());
	instance_info.ppEnabledLayerNames = active_layers.empty() ? nullptr : active_layers.data();
	instance_info.pApplicationInfo = opts.application_info ? opts.application_info : &app;

	for (uint32_t i = 0; i < instance_info.enabledLayerCount; i++)
		LOGI("Enabling instance layer: %s\n", instance_info.ppEnabledLayerNames[i]);
	for (uint32_t i = 0; i < instance_info.enabledExtensionCount; i++)
		LOGI("Enabling instance extension: %s\n", instance_info.ppEnabledExtensionNames[i]);

	vector<const char *> active_exts;
	for (auto &ext : exts)
		if (filter_instance_extension(ext.extensionName, instance_info.pApplicationInfo->apiVersion))
			active_exts.push_back(ext.extensionName);

	instance_info.enabledExtensionCount = uint32_t(active_exts.size());
	instance_info.ppEnabledExtensionNames = active_exts.empty() ? nullptr : active_exts.data();

	if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
	{
		LOGE("Failed to create instance.\n");
		return false;
	}

	volkLoadInstance(instance);

	if (use_debug_callback)
	{
		VkDebugReportCallbackCreateInfoEXT cb_info = { VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT };
		cb_info.pfnCallback = debug_callback;
		cb_info.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT;
		cb_info.pUserData = this;

		if (vkCreateDebugReportCallbackEXT(instance, &cb_info, nullptr, &callback) != VK_SUCCESS)
			return false;
	}

	uint32_t gpu_count = 0;
	if (vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr) != VK_SUCCESS)
		return false;
	vector<VkPhysicalDevice> gpus(gpu_count);

	if (!gpu_count)
	{
		LOGE("No physical devices.\n");
		return false;
	}

	if (vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data()) != VK_SUCCESS)
		return false;

	for (uint32_t i = 0; i < gpu_count; i++)
	{
		vkGetPhysicalDeviceProperties(gpus[i], &gpu_props);
		LOGI("Enumerated GPU #%u:\n", i);
		LOGI("  name: %s\n", gpu_props.deviceName);
		LOGI("  apiVersion: %u.%u.%u\n",
		     VK_VERSION_MAJOR(gpu_props.apiVersion),
		     VK_VERSION_MINOR(gpu_props.apiVersion),
		     VK_VERSION_PATCH(gpu_props.apiVersion));
	}

	if (opts.device_index >= 0)
	{
		if (size_t(opts.device_index) >= gpus.size())
		{
			LOGE("Device index %d is out of range, only %u devices on system.\n",
			     opts.device_index, unsigned(gpus.size()));
			return false;
		}
		gpu = gpus[opts.device_index];
	}
	else
		gpu = gpus.front();

	vkGetPhysicalDeviceProperties(gpu, &gpu_props);
	LOGI("Chose GPU:\n");
	LOGI("  name: %s\n", gpu_props.deviceName);
	LOGI("  apiVersion: %u.%u.%u\n",
	     VK_VERSION_MAJOR(gpu_props.apiVersion),
	     VK_VERSION_MINOR(gpu_props.apiVersion),
	     VK_VERSION_PATCH(gpu_props.apiVersion));
	LOGI("  vendorID: 0x%x\n", gpu_props.vendorID);
	LOGI("  deviceID: 0x%x\n", gpu_props.deviceID);

	uint32_t gpu_api_version = major_minor_version(gpu_props.apiVersion);
	if (!opts.application_info)
		target_api_version = instance_api_version < gpu_api_version ? instance_api_version : gpu_api_version;

	if (target_api_version > gpu_api_version)
	{
		LOGE("Selected GPU does not support desired Vulkan API version.\n");
		return false;
	}

	api_version = target_api_version;

	vector<const char *> active_device_extensions;
	uint32_t device_ext_count = 0;
	if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &device_ext_count, nullptr) != VK_SUCCESS)
		return false;
	vector<VkExtensionProperties> device_ext_props(device_ext_count);
	if (device_ext_count && vkEnumerateDeviceExtensionProperties(gpu, nullptr, &device_ext_count, device_ext_props.data()) != VK_SUCCESS)
		return false;

	for (auto &ext : device_ext_props)
		if (filter_extension(ext.extensionName, opts.want_amd_shader_info, device_ext_props, api_version))
			active_device_extensions.push_back(ext.extensionName);

	bool has_device_features2 = find_extension(exts, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

	VkPhysicalDeviceFeatures2 gpu_features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR stats_feature = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR
	};
	gpu_features2.pNext = &stats_feature;
	if (has_device_features2)
	{
		stats_feature.pNext = build_pnext_chain(features, api_version,
		                                        active_device_extensions.data(),
		                                        uint32_t(active_device_extensions.size()));
		vkGetPhysicalDeviceFeatures2KHR(gpu, &gpu_features2);

		pipeline_stats = stats_feature.pipelineExecutableInfo;
		if (pipeline_stats && opts.want_pipeline_stats)
			stats_feature.pipelineExecutableInfo = VK_TRUE;
		else
			stats_feature.pipelineExecutableInfo = VK_FALSE;
	}
	else
		vkGetPhysicalDeviceFeatures(gpu, &gpu_features2.features);

	VkPhysicalDeviceProperties2 gpu_props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	if (has_device_features2)
	{
		gpu_props2.pNext = build_pnext_chain(props, api_version,
		                                     active_device_extensions.data(),
		                                     uint32_t(active_device_extensions.size()));
		vkGetPhysicalDeviceProperties2KHR(gpu, &gpu_props2);
	}
	else
		vkGetPhysicalDeviceProperties(gpu, &gpu_props2.properties);

	// A fairly ugly, but important workaround.
	// When replaying dxvk/vkd3d, we expect robustness2, but this was not captured on earlier databases,
	// which means we get different shader hashes when replaying.
	// For now, it's pragmatic to just enable robustness2 until old Fossils have been retired.
	// New fossils will capture robustness2.
	const auto *requested_pdf2 = opts.features;
	VkPhysicalDeviceFeatures2 replacement_pdf2;
	VkPhysicalDeviceRobustness2FeaturesEXT replacement_robustness2;
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR replacement_fragment_shading_rate;

	const auto begin_replacement = [&](void *pnext) {
		if (&replacement_pdf2 != requested_pdf2)
		{
			replacement_pdf2 = *requested_pdf2;
			requested_pdf2 = &replacement_pdf2;
		}

		static_cast<VkBaseOutStructure *>(pnext)->pNext =
				static_cast<VkBaseOutStructure *>(replacement_pdf2.pNext);
		replacement_pdf2.pNext = pnext;
	};

	if (opts.features)
	{
		if (application_info_promote_robustness2(opts.application_info) &&
		    find_pnext<VkPhysicalDeviceRobustness2FeaturesEXT>(
				    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
				    opts.features->pNext) == nullptr)
		{
			replacement_robustness2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
			begin_replacement(&replacement_robustness2);

			replacement_robustness2.robustBufferAccess2 = opts.features->features.robustBufferAccess;
			replacement_robustness2.robustImageAccess2 = opts.features->features.robustBufferAccess;
			replacement_robustness2.nullDescriptor = VK_TRUE;
		}

		if (application_info_promote_fragment_shading_rate(opts.application_info) &&
		    find_pnext<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(
				    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
				    opts.features->pNext) == nullptr)
		{
			replacement_fragment_shading_rate = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
			begin_replacement(&replacement_fragment_shading_rate);
			reset_features(replacement_fragment_shading_rate, VK_TRUE);
		}
	}

	size_t active_extension_count = active_device_extensions.size();
	filter_feature_enablement(gpu_features2, features, requested_pdf2,
	                          active_device_extensions.data(), &active_extension_count);
	active_device_extensions.resize(active_extension_count);

	// Just pick one graphics queue.
	// FIXME: Does shader compilation depend on which queues we have enabled?
	// FIXME: Potentially separate code-gen if COMPUTE queue needs different optimizations, etc ...
	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
	vector<VkQueueFamilyProperties> queue_props(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, queue_props.data());

	VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	static float one = 1.0f;
	queue_info.pQueuePriorities = &one;
	queue_info.queueCount = 1;
	for (auto &queue_prop : queue_props)
	{
		if ((queue_prop.queueCount > 0) && (queue_prop.queueFlags & VK_QUEUE_GRAPHICS_BIT))
		{
			queue_info.queueFamilyIndex = uint32_t(&queue_prop - queue_props.data());
			break;
		}
	}

	vector<const char *> active_device_layers;
	if (opts.enable_validation)
	{
		uint32_t device_layer_count = 0;
		if (vkEnumerateDeviceLayerProperties(gpu, &device_layer_count, nullptr) != VK_SUCCESS)
			return false;
		vector<VkLayerProperties> device_layers(device_layer_count);
		if (device_layer_count && vkEnumerateDeviceLayerProperties(gpu, &device_layer_count, device_layers.data()) != VK_SUCCESS)
			return false;

		if (find_layer(device_layers, "VK_LAYER_KHRONOS_validation"))
		{
			active_device_layers.push_back("VK_LAYER_KHRONOS_validation");

			uint32_t validation_ext_count = 0;
			vkEnumerateDeviceExtensionProperties(gpu, "VK_LAYER_KHRONOS_validation", &validation_ext_count, nullptr);
			vector<VkExtensionProperties> validation_extensions(validation_ext_count);
			vkEnumerateDeviceExtensionProperties(gpu, "VK_LAYER_KHRONOS_validation", &validation_ext_count, validation_extensions.data());
			validation_cache = find_extension(validation_extensions, VK_EXT_VALIDATION_CACHE_EXTENSION_NAME);
			if (validation_cache)
				active_device_extensions.push_back(VK_EXT_VALIDATION_CACHE_EXTENSION_NAME);
		}
		else
		{
			LOGE("Cannot find VK_LAYER_KHRONOS_validation layer.\n");
			return false;
		}
	}

	supports_pipeline_feedback = find_if(begin(active_device_extensions), end(active_device_extensions), [](const char *ext) {
		return strcmp(ext, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0;
	}) != end(active_device_extensions);

	amd_shader_info = find_if(begin(active_device_extensions), end(active_device_extensions), [](const char *ext) {
		return strcmp(ext, VK_AMD_SHADER_INFO_EXTENSION_NAME) == 0;
	}) != end(active_device_extensions);

	supports_module_identifiers = features.shader_module_identifier.shaderModuleIdentifier == VK_TRUE;

	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	device_info.pNext = has_device_features2 ? &gpu_features2 : nullptr;
	device_info.pEnabledFeatures = has_device_features2 ? nullptr : &gpu_features2.features;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.queueCreateInfoCount = 1;
	device_info.enabledLayerCount = uint32_t(active_device_layers.size());
	device_info.ppEnabledLayerNames = active_device_layers.empty() ? nullptr : active_device_layers.data();
	device_info.enabledExtensionCount = uint32_t(active_device_extensions.size());
	device_info.ppEnabledExtensionNames = active_device_extensions.empty() ? nullptr : active_device_extensions.data();

	if (device_info.ppEnabledLayerNames)
		for (uint32_t i = 0; i < device_info.enabledLayerCount; i++)
			LOGI("Enabling device layer: %s\n", device_info.ppEnabledLayerNames[i]);

	if (device_info.ppEnabledExtensionNames)
		for (uint32_t i = 0; i < device_info.enabledExtensionCount; i++)
			LOGI("Enabling device extension: %s\n", device_info.ppEnabledExtensionNames[i]);

	if (vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_SUCCESS)
	{
		LOGE("Failed to create device.\n");
		return false;
	}

	if (!feature_filter.init(api_version, active_device_extensions.data(), active_device_extensions.size(),
	                         &gpu_features2, &gpu_props2))
	{
		LOGE("Failed to init feature filter.\n");
		return false;
	}

	feature_filter.set_device_query_interface(this);

	return true;
}

VulkanDevice::~VulkanDevice()
{
	for (auto &ycbcr : ycbcr_conversions)
		vkDestroySamplerYcbcrConversionKHR(device, ycbcr, nullptr);
	if (!is_null_device && device)
		vkDestroyDevice(device, nullptr);
	if (callback)
		vkDestroyDebugReportCallbackEXT(instance, callback, nullptr);
	if (instance)
		vkDestroyInstance(instance, nullptr);
}

void VulkanDevice::set_validation_error_callback(void (*cb)(void *), void *userdata)
{
	validation_callback = cb;
	validation_callback_userdata = userdata;
}

void VulkanDevice::notify_validation_error()
{
	if (validation_callback)
		validation_callback(validation_callback_userdata);
}

bool VulkanDevice::format_is_supported(VkFormat format, VkFormatFeatureFlags format_features)
{
	if (is_null_device)
		return true;

	VkFormatProperties format_props = {};
	vkGetPhysicalDeviceFormatProperties(gpu, format, &format_props);
	VkFormatFeatureFlags supported =
			format_props.linearTilingFeatures |
			format_props.optimalTilingFeatures |
			format_props.bufferFeatures;
	return (format_features & supported) == format_features;
}

bool VulkanDevice::descriptor_set_layout_is_supported(const VkDescriptorSetLayoutCreateInfo *info)
{
	if (is_null_device)
		return true;

	VkDescriptorSetLayoutSupport set_layout_support = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };

	vkGetDescriptorSetLayoutSupportKHR(device, info, &set_layout_support);
	return set_layout_support.supported;
}

void VulkanDevice::physical_device_feature_query(VkPhysicalDeviceFeatures2 *pdf2)
{
	if (is_null_device)
		return;

	if (vkGetPhysicalDeviceFeatures2)
		vkGetPhysicalDeviceFeatures2(gpu, pdf2);
	else if (vkGetPhysicalDeviceFeatures)
		vkGetPhysicalDeviceFeatures(gpu, &pdf2->features);
}

template <typename T>
static T allocate_dummy(size_t size)
{
	// Make sure to write something so the memory is forced to be resident.
	void *ptr = malloc(size);
	memset(ptr, 0xab, size);
	return (T)ptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_sampler(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *sampler)
{
	*sampler = allocate_dummy<VkSampler>(64);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL destroy_sampler(VkDevice, VkSampler sampler, const VkAllocationCallbacks *)
{
	free((void *) sampler);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_sampler_ycbcr_conversion_khr(VkDevice, const VkSamplerYcbcrConversionCreateInfo *,
                                    const VkAllocationCallbacks *, VkSamplerYcbcrConversion *conv)
{
	*conv = allocate_dummy<VkSamplerYcbcrConversion>(64);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_sampler_ycbcr_conversion_khr(VkDevice, VkSamplerYcbcrConversion conv, const VkAllocationCallbacks *)
{
	free((void *)conv);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_set_layout(VkDevice, const VkDescriptorSetLayoutCreateInfo *, const VkAllocationCallbacks *,
                  VkDescriptorSetLayout *layout)
{
	*layout = allocate_dummy<VkDescriptorSetLayout>(256);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_set_layout(VkDevice, VkDescriptorSetLayout layout, const VkAllocationCallbacks *)
{
	free((void *) layout);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_pipeline_layout(VkDevice, const VkPipelineLayoutCreateInfo *, const VkAllocationCallbacks *,
                       VkPipelineLayout *layout)
{
	*layout = allocate_dummy<VkPipelineLayout>(256);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_pipeline_layout(VkDevice, VkPipelineLayout layout, const VkAllocationCallbacks *)
{
	free((void *) layout);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_render_pass(VkDevice, const VkRenderPassCreateInfo *, const VkAllocationCallbacks *,
                   VkRenderPass *pass)
{
	*pass = allocate_dummy<VkRenderPass>(1024);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_render_pass2(VkDevice, const VkRenderPassCreateInfo2 *, const VkAllocationCallbacks *,
                    VkRenderPass *pass)
{
	*pass = allocate_dummy<VkRenderPass>(1024);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_render_pass(VkDevice, VkRenderPass pass, const VkAllocationCallbacks *)
{
	free((void *) pass);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_shader_module(VkDevice, const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *,
                     VkShaderModule *module)
{
	*module = allocate_dummy<VkShaderModule>(info->codeSize);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_shader_module(VkDevice, VkShaderModule module, const VkAllocationCallbacks *)
{
	free((void *) module);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_graphics_pipelines(VkDevice, VkPipelineCache, uint32_t count, const VkGraphicsPipelineCreateInfo *, const VkAllocationCallbacks *,
                          VkPipeline *pipelines)
{
	for (uint32_t i = 0; i < count; i++)
		pipelines[i] = allocate_dummy<VkPipeline>(4096);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_compute_pipelines(VkDevice, VkPipelineCache, uint32_t count, const VkComputePipelineCreateInfo *, const VkAllocationCallbacks *,
                         VkPipeline *pipelines)
{
	for (uint32_t i = 0; i < count; i++)
		pipelines[i] = allocate_dummy<VkPipeline>(4096);
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_raytracing_pipelines_khr(VkDevice, VkDeferredOperationKHR, VkPipelineCache,
                                uint32_t count, const VkRayTracingPipelineCreateInfoKHR *,
                                const VkAllocationCallbacks *,
                                VkPipeline *pipelines)
{
	for (uint32_t i = 0; i < count; i++)
		pipelines[i] = allocate_dummy<VkPipeline>(64 * 1024);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_pipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks *)
{
	free((void *) pipeline);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_pipeline_cache(VkDevice, const VkPipelineCacheCreateInfo *, const VkAllocationCallbacks *, VkPipelineCache *cache)
{
	*cache = allocate_dummy<VkPipelineCache>(1024);
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_pipeline_cache(VkDevice, VkPipelineCache cache, const VkAllocationCallbacks *)
{
	free((void *) cache);
}

static VKAPI_ATTR VkResult VKAPI_CALL
get_pipeline_cache_data(VkDevice, VkPipelineCache, size_t *, void *)
{
	return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VKAPI_ATTR void VKAPI_CALL
get_physical_device_properties(VkPhysicalDevice, VkPhysicalDeviceProperties *props)
{
	*props = {};
	props->apiVersion = VK_API_VERSION_1_1;
}

VkResult VulkanDevice::create_sampler_with_ycbcr_remap(const VkSamplerCreateInfo *create_info, VkSampler *sampler)
{
	VkSamplerYcbcrConversion conv = VK_NULL_HANDLE;
	VkResult vr;

	// Kinda hacky. Resolve Ycbcr sampler objects.
	// Replace the create info inline.
	const auto *next = static_cast<const VkBaseInStructure *>(create_info->pNext);
	while (next)
	{
		if (next->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO)
		{
			auto ycbcr = *reinterpret_cast<const VkSamplerYcbcrConversionCreateInfo *>(next);
			ycbcr.pNext = nullptr;

			if ((vr = vkCreateSamplerYcbcrConversionKHR(device, &ycbcr, nullptr, &conv)) != VK_SUCCESS)
				return vr;

			ycbcr_conversions.push_back(conv);

			// Kinda icky, but we know the conversion info is smaller than the create info.
			// It's also safe to mutate the input structs we get from enqueue_create_sampler().
			auto *mut_next = const_cast<VkBaseInStructure *>(next);
			mut_next->sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
			reinterpret_cast<VkSamplerYcbcrConversionInfo *>(mut_next)->conversion = conv;
		}

		next = static_cast<const VkBaseInStructure *>(next->pNext);
	}

	if ((vr = vkCreateSampler(device, create_info, nullptr, sampler)) != VK_SUCCESS)
		return vr;

	return VK_SUCCESS;
}

void VulkanDevice::init_null_device()
{
	LOGI("Creating null device.\n");
	device = reinterpret_cast<VkDevice>(uintptr_t(1));
	gpu = reinterpret_cast<VkPhysicalDevice>(uintptr_t(2));
	api_version = VK_API_VERSION_1_1;

	vkCreateSampler = create_sampler;
	vkDestroySampler = destroy_sampler;
	vkCreateDescriptorSetLayout = create_set_layout;
	vkDestroyDescriptorSetLayout = destroy_set_layout;
	vkCreatePipelineLayout = create_pipeline_layout;
	vkDestroyPipelineLayout = destroy_pipeline_layout;
	vkCreateRenderPass = create_render_pass;
	vkCreateRenderPass2 = create_render_pass2;
	vkCreateRenderPass2KHR = create_render_pass2;
	vkDestroyRenderPass = destroy_render_pass;
	vkCreateShaderModule = create_shader_module;
	vkDestroyShaderModule = destroy_shader_module;
	vkCreateGraphicsPipelines = create_graphics_pipelines;
	vkCreateComputePipelines = create_compute_pipelines;
	vkDestroyPipeline = destroy_pipeline;
	vkCreatePipelineCache = create_pipeline_cache;
	vkDestroyPipelineCache = destroy_pipeline_cache;
	vkGetPipelineCacheData = get_pipeline_cache_data;
	vkGetPhysicalDeviceProperties = get_physical_device_properties;
	vkCreateRayTracingPipelinesKHR = create_raytracing_pipelines_khr;
	vkCreateSamplerYcbcrConversionKHR = create_sampler_ycbcr_conversion_khr;
	vkDestroySamplerYcbcrConversionKHR = destroy_sampler_ycbcr_conversion_khr;
	is_null_device = true;

	feature_filter.init_null_device();
}
}
