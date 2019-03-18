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
#define VK_NO_PROTOTYPES // VALVE: So that vulkan_core.h definitions lacking dllexport don't conflict with our exported functions
#include "dispatch_helper.hpp"
#include "utils.hpp"
#include "device.hpp"
#include "instance.hpp"
#include <mutex>

// VALVE: do exports without .def file, see vk_layer.h for definition on non-Windows platforms
#ifdef _MSC_VER
#undef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#endif

extern "C"
{
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);
}

using namespace std;

namespace Fossilize
{

// Global data structures to remap VkInstance and VkDevice to internal data structures.
static mutex globalLock;
static InstanceTable instanceDispatch;
static DeviceTable deviceDispatch;
static unordered_map<void *, unique_ptr<Instance>> instanceData;
static unordered_map<void *, unique_ptr<Device>> deviceData;

static Device *get_device_layer(VkDevice device)
{
	// Need to hold a lock while querying the global hashmap, but not after it.
	Device *layer = nullptr;
	void *key = getDispatchKey(device);
	lock_guard<mutex> holder{ globalLock };
	layer = getLayerData(key, deviceData);
	return layer;
}

static Instance *get_instance_layer(VkPhysicalDevice gpu)
{
	lock_guard<mutex> holder{ globalLock };
	return getLayerData(getDispatchKey(gpu), instanceData);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	auto *layer = get_instance_layer(gpu);
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);

	auto fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpGetDeviceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	auto fpCreateDevice =
	    reinterpret_cast<PFN_vkCreateDevice>(fpGetInstanceProcAddr(layer->getInstance(), "vkCreateDevice"));
	if (!fpCreateDevice)
		return VK_ERROR_INITIALIZATION_FAILED;

	// Advance the link info for the next element on the chain
	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

	auto res = fpCreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS)
		return res;

	// Build a physical device features 2 struct if we cannot find it in pCreateInfo.
	auto *pdf2 = static_cast<const VkPhysicalDeviceFeatures2 *>(findpNext(pCreateInfo, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
	VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	if (!pdf2)
	{
		pdf2 = &physicalDeviceFeatures2;
		if (pCreateInfo->pEnabledFeatures)
			physicalDeviceFeatures2.features = *pCreateInfo->pEnabledFeatures;
	}

	{
		lock_guard<mutex> holder{globalLock};
		auto *device = createLayerData(getDispatchKey(*pDevice), deviceData);
		device->init(gpu, *pDevice, layer, *pdf2, initDeviceTable(*pDevice, fpGetDeviceProcAddr, deviceDispatch));
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);

	auto fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fpGetInstanceProcAddr(nullptr, "vkCreateInstance"));
	if (!fpCreateInstance)
		return VK_ERROR_INITIALIZATION_FAILED;

	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
	auto res = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
	if (res != VK_SUCCESS)
		return res;

	{
		lock_guard<mutex> holder{globalLock};
		auto *layer = createLayerData(getDispatchKey(*pInstance), instanceData);
		layer->init(*pInstance, pCreateInfo->pApplicationInfo,
		            initInstanceTable(*pInstance, fpGetInstanceProcAddr, instanceDispatch), fpGetInstanceProcAddr);
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	lock_guard<mutex> holder{ globalLock };

	void *key = getDispatchKey(instance);
	auto *layer = getLayerData(key, instanceData);
	layer->getTable()->DestroyInstance(instance, pAllocator);
	destroyLayerData(key, instanceData);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                              uint32_t createInfoCount,
                                                              const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                              const VkAllocationCallbacks *pAllocator,
                                                              VkPipeline *pPipelines)
{
	auto *layer = get_device_layer(device);

	// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
	auto res = layer->getTable()->CreateGraphicsPipelines(layer->getDevice(), pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
	if (res != VK_SUCCESS)
		return res;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		try
		{
			// Record methods may throw on unexpected input. We cannot propagate this error up.
			layer->getRecorder().record_graphics_pipeline(pPipelines[i], pCreateInfos[i], pPipelines, createInfoCount);
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to record graphics pipeline: %s\n", e.what());
		}
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                             uint32_t createInfoCount,
                                                             const VkComputePipelineCreateInfo *pCreateInfos,
                                                             const VkAllocationCallbacks *pAllocator,
                                                             VkPipeline *pPipelines)
{
	auto *layer = get_device_layer(device);

	// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
	auto res = layer->getTable()->CreateComputePipelines(layer->getDevice(), pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
	if (res != VK_SUCCESS)
		return res;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		try
		{
			// Record methods may throw on unexpected input. We cannot propagate this error up.
			layer->getRecorder().record_compute_pipeline(pPipelines[i], pCreateInfos[i], pPipelines, createInfoCount);
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to record compute pipeline: %s\n", e.what());
		}
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device,
                                                           const VkPipelineLayoutCreateInfo *pCreateInfo,
                                                           const VkAllocationCallbacks *pAllocator,
                                                           VkPipelineLayout *pLayout)
{
	auto *layer = get_device_layer(device);

	VkResult result = layer->getTable()->CreatePipelineLayout(device, pCreateInfo, pAllocator, pLayout);

	if (result == VK_SUCCESS)
	{
		try
		{
			// Record methods may throw on unexpected input. We cannot propagate this error up.
			layer->getRecorder().record_pipeline_layout(*pLayout, *pCreateInfo);
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to record pipeline layout: %s\n", e.what());
		}
	}
	return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device,
                                                                const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                                                const VkAllocationCallbacks *pAllocator,
                                                                VkDescriptorSetLayout *pSetLayout)
{
	auto *layer = get_device_layer(device);

	VkResult result = layer->getTable()->CreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);

	if (result == VK_SUCCESS)
	{
		try
		{
			// Record methods may throw on unexpected input. We cannot propagate this error up.
			layer->getRecorder().record_descriptor_set_layout(*pSetLayout, *pCreateInfo);
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to record descriptor set layout: %s\n", e.what());
		}
	}
	return result;
}

static PFN_vkVoidFunction interceptCoreInstanceCommand(const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
	} coreInstanceCommands[] = {
		{ "vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(CreateInstance) },
		{ "vkDestroyInstance", reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance) },
		{ "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr) },
		{ "vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice) },
	};

	for (auto &cmd : coreInstanceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;
	return nullptr;
}

static VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator)
{
	lock_guard<mutex> holder{ globalLock };

	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	layer->getTable()->DestroyDevice(device, pAllocator);
	destroyLayerData(key, deviceData);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
                                                    const VkAllocationCallbacks *pCallbacks, VkSampler *pSampler)
{
	auto *layer = get_device_layer(device);
	auto res = layer->getTable()->CreateSampler(device, pCreateInfo, pCallbacks, pSampler);

	if (res == VK_SUCCESS)
	{
		try
		{
			// Record methods may throw on unexpected input. We cannot propagate this error up.
			layer->getRecorder().record_sampler(*pSampler, *pCreateInfo);
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to record sampler: %s\n", e.what());
		}
	}

	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
                                                         const VkAllocationCallbacks *pCallbacks,
                                                         VkShaderModule *pShaderModule)
{
	auto *layer = get_device_layer(device);

	*pShaderModule = VK_NULL_HANDLE;

	auto res = layer->getTable()->CreateShaderModule(device, pCreateInfo, pCallbacks, pShaderModule);

	if (res == VK_SUCCESS)
	{
		try
		{
			// Record methods may throw on unexpected input. We cannot propagate this error up.
			layer->getRecorder().record_shader_module(*pShaderModule, *pCreateInfo);
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to record shader module: %s\n", e.what());
		}
	}

	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
                                                       const VkAllocationCallbacks *pCallbacks, VkRenderPass *pRenderPass)
{
	auto *layer = get_device_layer(device);

	auto res = layer->getTable()->CreateRenderPass(device, pCreateInfo, pCallbacks, pRenderPass);

	if (res == VK_SUCCESS)
	{
		try
		{
			// Record methods may throw on unexpected input. We cannot propagate this error up.
			layer->getRecorder().record_render_pass(*pRenderPass, *pCreateInfo);
		}
		catch (const std::exception &e)
		{
			LOGE("Failed to record render pass: %s\n", e.what());
		}
	}
	return res;
}

static PFN_vkVoidFunction interceptCoreDeviceCommand(const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
	} coreDeviceCommands[] = {
		{ "vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr) },
		{ "vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice) },

		{ "vkCreateDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(CreateDescriptorSetLayout) },
		{ "vkCreatePipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(CreatePipelineLayout) },
		{ "vkCreateGraphicsPipelines", reinterpret_cast<PFN_vkVoidFunction>(CreateGraphicsPipelines) },
		{ "vkCreateComputePipelines", reinterpret_cast<PFN_vkVoidFunction>(CreateComputePipelines) },
		{ "vkCreateSampler", reinterpret_cast<PFN_vkVoidFunction>(CreateSampler) },
		{ "vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(CreateShaderModule) },
		{ "vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass) },
	};

	for (auto &cmd : coreDeviceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;
	return nullptr;
}
}

using namespace Fossilize;

extern "C"
{
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	auto proc = interceptCoreDeviceCommand(pName);
	if (proc)
		return proc;

	Device *layer = nullptr;
	{
		lock_guard<mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(device), deviceData);
	}

	return layer->getTable()->GetDeviceProcAddr(device, pName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	auto proc = interceptCoreInstanceCommand(pName);
	if (proc)
		return proc;

	proc = interceptCoreDeviceCommand(pName);
	if (proc)
		return proc;

	Instance *layer = nullptr;
	{
		lock_guard<mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(instance), instanceData);
	}

	return layer->getProcAddr(pName);
}

static const VkLayerProperties layerProps[] = {
	{ VK_LAYER_fossilize, VK_MAKE_VERSION(1, 1, 98), 1, "Fossilize capture layer" },
};

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
                                       VkExtensionProperties *pProperties)
{
	if (!pLayerName || strcmp(pLayerName, layerProps[0].layerName))
		return VK_ERROR_LAYER_NOT_PRESENT;

	if (pProperties && *pPropertyCount != 0)
	{
		*pPropertyCount = 0;
		return VK_INCOMPLETE;
	}

	*pPropertyCount = 0;
	return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *pLayerName,
                                     uint32_t *pPropertyCount,
                                     VkExtensionProperties *pProperties)
{
	if (pLayerName && !strcmp(pLayerName, layerProps[0].layerName))
	{
		if (pProperties && *pPropertyCount > 0)
			return VK_INCOMPLETE;
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}
	else
		return VK_ERROR_LAYER_NOT_PRESENT;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                                                                  VkLayerProperties *pProperties)
{
	if (pProperties)
	{
		uint32_t count = std::min(1u, *pPropertyCount);
		memcpy(pProperties, layerProps, count * sizeof(VkLayerProperties));
		VkResult res = count < *pPropertyCount ? VK_INCOMPLETE : VK_SUCCESS;
		*pPropertyCount = count;
		return res;
	}
	else
	{
		*pPropertyCount = sizeof(layerProps) / sizeof(VkLayerProperties);
		return VK_SUCCESS;
	}
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t *pPropertyCount,
                                 VkLayerProperties *pProperties)
{
	if (pProperties)
	{
		uint32_t count = std::min(1u, *pPropertyCount);
		memcpy(pProperties, layerProps, count * sizeof(VkLayerProperties));
		VkResult res = count < *pPropertyCount ? VK_INCOMPLETE : VK_SUCCESS;
		*pPropertyCount = count;
		return res;
	}
	else
	{
		*pPropertyCount = sizeof(layerProps) / sizeof(VkLayerProperties);
		return VK_SUCCESS;
	}
}
}
