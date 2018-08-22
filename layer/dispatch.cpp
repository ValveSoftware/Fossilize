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

#include "dispatch_helper.hpp"
#include "utils.hpp"
#include "device.hpp"
#include "instance.hpp"
#include <mutex>

#ifdef _MSC_VER // For SEH access violation handling.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <excpt.h>
#endif

using namespace std;

namespace Fossilize
{

// Global data structures to remap VkInstance and VkDevice to internal data structures.
static mutex globalLock;
static InstanceTable instanceDispatch;
static DeviceTable deviceDispatch;
static unordered_map<void *, unique_ptr<Instance>> instanceData;
static unordered_map<void *, unique_ptr<Device>> deviceData;

static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
	auto *layer = getLayerData(getDispatchKey(gpu), instanceData);
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

	auto *device = createLayerData(getDispatchKey(*pDevice), deviceData);
	device->init(gpu, *pDevice, layer->getTable(), initDeviceTable(*pDevice, fpGetDeviceProcAddr, deviceDispatch));
	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
	lock_guard<mutex> holder{ globalLock };
	auto *chainInfo = getChainInfo(pCreateInfo, VK_LAYER_LINK_INFO);

	auto fpGetInstanceProcAddr = chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	auto fpCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(fpGetInstanceProcAddr(nullptr, "vkCreateInstance"));
	if (!fpCreateInstance)
		return VK_ERROR_INITIALIZATION_FAILED;

	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
	auto res = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
	if (res != VK_SUCCESS)
		return res;

	auto *layer = createLayerData(getDispatchKey(*pInstance), instanceData);
	layer->init(*pInstance, initInstanceTable(*pInstance, fpGetInstanceProcAddr, instanceDispatch),
	            fpGetInstanceProcAddr);

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

#ifdef _MSC_VER
static int filterSEHException(int code)
{
	return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
#endif

static VkResult createGraphicsPipeline(Device *device, VkPipelineCache pipelineCache,
                                       const VkGraphicsPipelineCreateInfo *pCreateInfo,
                                       const VkAllocationCallbacks *pCallbacks,
                                       VkPipeline *pipeline)
{
#ifdef _MSC_VER // Dunno how to do SIGSEGV handling on Windows, so ad-hoc SEH it is. This isn't supported on MinGW, so just MSVC for now.
	__try
#endif
	{
		return device->getTable()->CreateGraphicsPipelines(device->getDevice(), pipelineCache, 1, pCreateInfo, pCallbacks, pipeline);
	}
#ifdef _MSC_VER
	__except (filterSEHException(GetExceptionCode()))
	{
		LOGE("Caught access violation in vkCreateGraphicsPipelines(), safety serialization before terminating ...\n");
		bool success = device->serializeToPath(device->getSerializationPath());

		if (success)
			MessageBoxA(nullptr, "vkCreateGraphicsPipelines() triggered an access violation, the offending state has been serialized.", "CreateGraphicsPipeline Access Violation", 0);
		else
			MessageBoxA(nullptr, "vkCreateGraphicsPipelines() triggered an access violation, but the offending state failed to be serialized.", "CreateGraphicsPipeline Access Violation", 0);
		std::terminate();
	}
#endif
}

static VkResult createComputePipeline(Device *device, VkPipelineCache pipelineCache,
                                      const VkComputePipelineCreateInfo *pCreateInfo,
                                      const VkAllocationCallbacks *pCallbacks,
                                      VkPipeline *pipeline)
{
#ifdef _MSC_VER // Dunno how to do SIGSEGV handling on Windows, so ad-hoc SEH it is. This isn't supported on MinGW, so just MSVC for now.
	__try
#endif
	{
		return device->getTable()->CreateComputePipelines(device->getDevice(), pipelineCache, 1, pCreateInfo, pCallbacks, pipeline);
	}
#ifdef _MSC_VER
	__except (filterSEHException(GetExceptionCode()))
	{
		LOGE("Caught access violation in vkCreateComputePipelines(), safety serialization before terminating ...\n");
		bool success = device->serializeToPath(device->getSerializationPath());

		if (success)
			MessageBoxA(nullptr, "vkCreateComputePipelines() triggered an access violation, the offending state has been serialized.", "CreateComputePipeline Access Violation", 0);
		else
			MessageBoxA(nullptr, "vkCreateComputePipelines() triggered an access violation, but the offending state failed to be serialized.", "GraphicsComputePipeline Access Violation", 0);
		std::terminate();
	}
#endif
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                              uint32_t createInfoCount,
                                                              const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                              const VkAllocationCallbacks *pAllocator,
                                                              VkPipeline *pPipelines)
{
	lock_guard<mutex> holder{ globalLock };
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		bool registerHandle = false;
		Hash index;
		try
		{
			index = layer->getRecorder().register_graphics_pipeline(
					Hashing::compute_hash_graphics_pipeline(layer->getRecorder(), pCreateInfos[i]), pCreateInfos[i]);
			registerHandle = true;
		}
		catch (const std::exception &e)
		{
			LOGE("Exception caught: %s\n", e.what());
		}
		pPipelines[i] = VK_NULL_HANDLE;

		if (layer->isParanoid())
			layer->serializeToPath(layer->getSerializationPath());

		auto res = createGraphicsPipeline(layer, pipelineCache, &pCreateInfos[i], pAllocator, &pPipelines[i]);

		if (res != VK_SUCCESS)
		{
			LOGE("Failed to create graphics pipeline, safety serialization ...\n");
			layer->serializeToPath(layer->getSerializationPath());
			return res;
		}

		if (registerHandle) {
			layer->getRecorder().set_compute_pipeline_handle(index, pPipelines[i]);
			layer->serializeGraphicsPipeline(index);
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
	lock_guard<mutex> holder{ globalLock };
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		bool registerHandle = false;
		Hash index;
		try
		{
			index = layer->getRecorder().register_compute_pipeline(
					Hashing::compute_hash_compute_pipeline(layer->getRecorder(), pCreateInfos[i]), pCreateInfos[i]);
			registerHandle = true;
		}
		catch (const std::exception &e)
		{
			LOGE("Exception caught: %s\n", e.what());
		}

		if (layer->isParanoid())
			layer->serializeToPath(layer->getSerializationPath());

		pPipelines[i] = VK_NULL_HANDLE;

		auto res = createComputePipeline(layer, pipelineCache, &pCreateInfos[i], pAllocator, &pPipelines[i]);

		if (res != VK_SUCCESS)
		{
			LOGE("Failed to create compute pipeline, safety serialization ...\n");
			layer->serializeToPath(layer->getSerializationPath());
			return res;
		}

		if (registerHandle) {
			layer->getRecorder().set_compute_pipeline_handle(index, pPipelines[i]);
			layer->serializeComputePipeline(index);
		}
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice device,
                                                           const VkPipelineLayoutCreateInfo *pCreateInfo,
                                                           const VkAllocationCallbacks *pAllocator,
                                                           VkPipelineLayout *pLayout)
{
	lock_guard<mutex> holder{ globalLock };
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	bool registerHandle = false;
	Hash index;
	try
	{
		index = layer->getRecorder().register_pipeline_layout(
				Hashing::compute_hash_pipeline_layout(layer->getRecorder(), *pCreateInfo), *pCreateInfo);
		registerHandle = true;
	}
	catch (const std::exception &e)
	{
		LOGE("Exception caught: %s\n", e.what());
	}
	*pLayout = VK_NULL_HANDLE;

	VkResult result = layer->getTable()->CreatePipelineLayout(device, pCreateInfo, pAllocator, pLayout);

	if (registerHandle)
		layer->getRecorder().set_pipeline_layout_handle(index, *pLayout);
	return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorSetLayout(VkDevice device,
                                                                const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                                                const VkAllocationCallbacks *pAllocator,
                                                                VkDescriptorSetLayout *pSetLayout)
{
	lock_guard<mutex> holder{ globalLock };
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	bool registerHandle = false;
	Hash index;
	try
	{
		index = layer->getRecorder().register_descriptor_set_layout(
				Hashing::compute_hash_descriptor_set_layout(layer->getRecorder(), *pCreateInfo), *pCreateInfo);
		registerHandle = true;
	}
	catch (const std::exception &e)
	{
		LOGE("Exception caught: %s\n", e.what());
	}
	*pSetLayout = VK_NULL_HANDLE;

	VkResult result = layer->getTable()->CreateDescriptorSetLayout(device, pCreateInfo, pAllocator, pSetLayout);

	if (registerHandle)
		layer->getRecorder().set_descriptor_set_layout_handle(index, *pSetLayout);
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

	layer->serializeToPath(layer->getSerializationPath());

	layer->getTable()->DestroyDevice(device, pAllocator);
	destroyLayerData(key, deviceData);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice device, const VkSamplerCreateInfo *pCreateInfo,
                                                    const VkAllocationCallbacks *pCallbacks, VkSampler *pSampler)
{
	lock_guard<mutex> holder{ globalLock };
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	bool registerHandle = false;
	Hash index;
	try
	{
		index = layer->getRecorder().register_sampler(
				Hashing::compute_hash_sampler(layer->getRecorder(), *pCreateInfo), *pCreateInfo);
		registerHandle = true;
	}
	catch (const std::exception &e)
	{
		LOGE("Exception caught: %s\n", e.what());
	}
	*pSampler = VK_NULL_HANDLE;

	auto res = layer->getTable()->CreateSampler(device, pCreateInfo, pCallbacks, pSampler);

	if (registerHandle)
		layer->getRecorder().set_sampler_handle(index, *pSampler);
	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
                                                         const VkAllocationCallbacks *pCallbacks,
                                                         VkShaderModule *pShaderModule)
{
	lock_guard<mutex> holder{ globalLock };
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	bool registerHandle = false;
	Hash index;
	try
	{
		index = layer->getRecorder().register_shader_module(
				Hashing::compute_hash_shader_module(layer->getRecorder(), *pCreateInfo), *pCreateInfo);
		registerHandle = true;
	}
	catch (const std::exception &e)
	{
		LOGE("Exception caught: %s\n", e.what());
	}
	*pShaderModule = VK_NULL_HANDLE;

	auto res = layer->getTable()->CreateShaderModule(device, pCreateInfo, pCallbacks, pShaderModule);

	if (registerHandle)
		layer->getRecorder().set_shader_module_handle(index, *pShaderModule);
	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
                                                       const VkAllocationCallbacks *pCallbacks, VkRenderPass *pRenderPass)
{
	lock_guard<mutex> holder{ globalLock };
	void *key = getDispatchKey(device);
	auto *layer = getLayerData(key, deviceData);

	bool registerHandle = false;
	Hash index;
	try
	{
		index = layer->getRecorder().register_render_pass(
				Hashing::compute_hash_render_pass(layer->getRecorder(), *pCreateInfo), *pCreateInfo);
		registerHandle = true;
	}
	catch (const std::exception &e)
	{
		LOGE("Exception caught: %s\n", e.what());
	}
	*pRenderPass = VK_NULL_HANDLE;

	auto res = layer->getTable()->CreateRenderPass(device, pCreateInfo, pCallbacks, pRenderPass);

	if (registerHandle)
		layer->getRecorder().set_render_pass_handle(index, *pRenderPass);
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
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName)
{
	lock_guard<mutex> holder{ globalLock };

	auto proc = interceptCoreDeviceCommand(pName);
	if (proc)
		return proc;

	auto *layer = getLayerData(getDispatchKey(device), deviceData);
	return layer->getTable()->GetDeviceProcAddr(device, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName)
{
	lock_guard<mutex> holder{ globalLock };

	auto proc = interceptCoreInstanceCommand(pName);
	if (proc)
		return proc;

	proc = interceptCoreDeviceCommand(pName);
	if (proc)
		return proc;

	auto *layer = getLayerData(getDispatchKey(instance), instanceData);
	return layer->getProcAddr(pName);
}

static const VkLayerProperties layerProps[] = {
	{ VK_LAYER_fossilize, VK_MAKE_VERSION(1, 0, 70), 1, "Fossilize capture layer" },
};

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount,
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

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *pLayerName,
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

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
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

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice, uint32_t *pPropertyCount,
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
