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

static bool filter_extension(const char *ext, bool need_disasm)
{
	// Ban certain extensions, because they conflict with others.
	if (strcmp(ext, VK_AMD_NEGATIVE_VIEWPORT_HEIGHT_EXTENSION_NAME) == 0)
		return false;
	else if (strcmp(ext, VK_AMD_SHADER_INFO_EXTENSION_NAME) == 0 && !need_disasm)
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

	vector<const char *> active_exts;
	for (auto &ext : exts)
		active_exts.push_back(ext.extensionName);

	vector<const char *> active_layers;
	if (opts.enable_validation)
	{
		uint32_t layer_count = 0;
		if (vkEnumerateInstanceLayerProperties(&layer_count, nullptr) != VK_SUCCESS)
			return false;
		vector<VkLayerProperties> layers(layer_count);
		if (vkEnumerateInstanceLayerProperties(&layer_count, layers.data()) != VK_SUCCESS)
			return false;

		// FIXME: This will not work on Android, use "shopping list of layers" method. :(
		if (find_layer(layers, "VK_LAYER_LUNARG_standard_validation"))
			active_layers.push_back("VK_LAYER_LUNARG_standard_validation");
	}

	bool use_debug_callback = find_extension(exts, VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.apiVersion = instance_api_version;
	app.pApplicationName = "Fossilize Replayer";
	app.pEngineName = "Fossilize";

	instance_info.enabledExtensionCount = uint32_t(active_exts.size());
	instance_info.ppEnabledExtensionNames = active_exts.empty() ? nullptr : active_exts.data();
	instance_info.enabledLayerCount = uint32_t(active_layers.size());
	instance_info.ppEnabledLayerNames = active_layers.empty() ? nullptr : active_layers.data();
	instance_info.pApplicationInfo = opts.application_info ? opts.application_info : &app;

	for (uint32_t i = 0; i < instance_info.enabledLayerCount; i++)
		LOGI("Enabling instance layer: %s\n", instance_info.ppEnabledLayerNames[i]);
	for (uint32_t i = 0; i < instance_info.enabledExtensionCount; i++)
		LOGI("Enabling instance extension: %s\n", instance_info.ppEnabledExtensionNames[i]);

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
		VkPhysicalDeviceProperties gpu_props = {};
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

	VkPhysicalDeviceProperties gpu_props = {};
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
	bool has_device_features2 = api_version >= VK_MAKE_VERSION(1, 1, 0);

	// FIXME: There are arbitrary features we can request here from physical_device_features2.
	VkPhysicalDeviceFeatures2 gpu_features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPhysicalDeviceFeatures *gpu_features = &gpu_features2.features;
	VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR stats_feature = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };
	gpu_features2.pNext = &stats_feature;
	if (has_device_features2)
	{
		vkGetPhysicalDeviceFeatures2(gpu, &gpu_features2);

		pipeline_stats = stats_feature.pipelineExecutableInfo;
		if (pipeline_stats && opts.want_pipeline_stats)
			stats_feature.pipelineExecutableInfo = VK_TRUE;
		else
			stats_feature.pipelineExecutableInfo = VK_FALSE;
	}
	else
		vkGetPhysicalDeviceFeatures(gpu, gpu_features);

	// FIXME: Have some way to enable the right features that a repro-capture may want to use.
	// FIXME: It is unlikely any feature other than robust access has any real impact on code-gen, but who knows.
	if (gpu_features->robustBufferAccess && opts.features && opts.features->features.robustBufferAccess)
		gpu_features->robustBufferAccess = VK_TRUE;
	else
		gpu_features->robustBufferAccess = VK_FALSE;

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
	for (auto &props : queue_props)
	{
		if ((props.queueCount > 0) && (props.queueFlags & VK_QUEUE_GRAPHICS_BIT))
		{
			queue_info.queueFamilyIndex = uint32_t(&props - queue_props.data());
			break;
		}
	}

	vector<const char *> active_device_layers;
	vector<const char *> active_device_extensions;
	if (opts.enable_validation)
	{
		uint32_t device_layer_count = 0;
		if (vkEnumerateDeviceLayerProperties(gpu, &device_layer_count, nullptr) != VK_SUCCESS)
			return false;
		vector<VkLayerProperties> device_layers(device_layer_count);
		if (device_layer_count && vkEnumerateDeviceLayerProperties(gpu, &device_layer_count, device_layers.data()) != VK_SUCCESS)
			return false;

		// FIXME: This will not work on Android, use "shopping list of layers" method. :(
		if (find_layer(device_layers, "VK_LAYER_LUNARG_standard_validation"))
			active_device_layers.push_back("VK_LAYER_LUNARG_standard_validation");
	}

	uint32_t device_ext_count = 0;
	if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &device_ext_count, nullptr) != VK_SUCCESS)
		return false;
	vector<VkExtensionProperties> device_ext_props(device_ext_count);
	if (device_ext_count && vkEnumerateDeviceExtensionProperties(gpu, nullptr, &device_ext_count, device_ext_props.data()) != VK_SUCCESS)
		return false;

	for (auto &ext : device_ext_props)
	{
		if (filter_extension(ext.extensionName, opts.need_disasm))
			active_device_extensions.push_back(ext.extensionName);
	}

	supports_pipeline_feedback = find_if(begin(active_device_extensions), end(active_device_extensions), [](const char *ext) {
		return strcmp(ext, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0;
	}) != end(active_device_extensions);

	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	device_info.pNext = has_device_features2 ? &gpu_features2 : nullptr;
	device_info.pEnabledFeatures = has_device_features2 ? nullptr : gpu_features;
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

	if (vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_NULL_HANDLE)
	{
		LOGE("Failed to create device.\n");
		return false;
	}

	return true;
}

VulkanDevice::~VulkanDevice()
{
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
	is_null_device = true;
}
}
