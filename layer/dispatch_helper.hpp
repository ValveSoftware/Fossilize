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

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <memory>
#include <string.h>
#include <unordered_map>
#include <algorithm>
#include "vulkan/vk_layer.h"
#include "vulkan/vulkan.h"

namespace Fossilize
{
// Isolate to just what we need. Should improve layer init time.

// Instance function pointer dispatch table
struct VkLayerInstanceDispatchTable
{
	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
	PFN_vkGetPhysicalDeviceProperties2KHR GetPhysicalDeviceProperties2KHR;
};

// Device function pointer dispatch table
struct VkLayerDispatchTable
{
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkCreateShaderModule CreateShaderModule;
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkCreateComputePipelines CreateComputePipelines;
	PFN_vkDestroyPipeline DestroyPipeline;
	PFN_vkCreatePipelineLayout CreatePipelineLayout;
	PFN_vkCreateSampler CreateSampler;
	PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
	PFN_vkCreateRenderPass CreateRenderPass;
	PFN_vkCreateRenderPass2 CreateRenderPass2;
	PFN_vkCreateRenderPass2KHR CreateRenderPass2KHR;
	PFN_vkCreateSamplerYcbcrConversion CreateSamplerYcbcrConversion;
	PFN_vkCreateSamplerYcbcrConversionKHR CreateSamplerYcbcrConversionKHR;
	PFN_vkCreateRayTracingPipelinesKHR CreateRayTracingPipelinesKHR;
	PFN_vkGetShaderModuleIdentifierEXT GetShaderModuleIdentifierEXT;
	PFN_vkGetShaderModuleCreateInfoIdentifierEXT GetShaderModuleCreateInfoIdentifierEXT;
	PFN_vkCreatePipelineBinariesKHR CreatePipelineBinariesKHR;
};

using InstanceTable = std::unordered_map<void *, std::unique_ptr<VkLayerInstanceDispatchTable>>;
using DeviceTable = std::unordered_map<void *, std::unique_ptr<VkLayerDispatchTable>>;

static inline VkLayerDeviceCreateInfo *getChainInfo(const VkInstanceCreateInfo *pCreateInfo, VkLayerFunction func)
{
	auto *chain_info = static_cast<const VkLayerDeviceCreateInfo *>(pCreateInfo->pNext);
	while (chain_info &&
	       !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain_info->function == func))
		chain_info = static_cast<const VkLayerDeviceCreateInfo *>(chain_info->pNext);
	return const_cast<VkLayerDeviceCreateInfo *>(chain_info);
}

static inline VkLayerDeviceCreateInfo *getChainInfo(const VkDeviceCreateInfo *pCreateInfo, VkLayerFunction func)
{
	auto *chain_info = static_cast<const VkLayerDeviceCreateInfo *>(pCreateInfo->pNext);
	while (chain_info &&
	       !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain_info->function == func))
		chain_info = static_cast<const VkLayerDeviceCreateInfo *>(chain_info->pNext);
	return const_cast<VkLayerDeviceCreateInfo *>(chain_info);
}

static inline const void *findpNext(const void *pCreateInfo, VkStructureType sType)
{
	auto *s = static_cast<const VkBaseInStructure *>(pCreateInfo);
	while (s && s->sType != sType)
		s = s->pNext;
	return s;
}

static inline void *findpNext(void *pCreateInfo, VkStructureType sType)
{
	auto *s = static_cast<VkBaseOutStructure *>(pCreateInfo);
	while (s && s->sType != sType)
		s = s->pNext;
	return s;
}

void layerInitDeviceDispatchTable(VkDevice device, VkLayerDispatchTable *table, PFN_vkGetDeviceProcAddr gpa);

void layerInitInstanceDispatchTable(VkInstance instance, VkLayerInstanceDispatchTable *table,
                                    PFN_vkGetInstanceProcAddr gpa);

static inline void *getDispatchKey(void *ptr)
{
	return *static_cast<void **>(ptr);
}

template <typename T>
static inline T *getLayerData(void *key, const std::unordered_map<void *, std::unique_ptr<T>> &m)
{
	auto itr = m.find(key);
	if (itr != end(m))
		return itr->second.get();
	else
		return nullptr;
}

template <typename T, typename... TArgs>
static inline T *createLayerData(void *key, std::unordered_map<void *, std::unique_ptr<T>> &m, TArgs &&... args)
{
	auto *ptr = new T(std::forward<TArgs>(args)...);
	m[key] = std::unique_ptr<T>(ptr);
	return ptr;
}

template <typename T>
static inline void destroyLayerData(void *key, std::unordered_map<void *, std::unique_ptr<T>> &m)
{
	auto itr = m.find(key);
	m.erase(itr);
}

static inline VkLayerInstanceDispatchTable *initInstanceTable(VkInstance instance, const PFN_vkGetInstanceProcAddr gpa,
                                                              InstanceTable &table)
{
	auto key = getDispatchKey(instance);
	auto itr = table.find(key);
	VkLayerInstanceDispatchTable *pTable = nullptr;
	if (itr == end(table))
	{
		pTable = new VkLayerInstanceDispatchTable;
		table[key] = std::unique_ptr<VkLayerInstanceDispatchTable>(pTable);
	}
	else
		pTable = itr->second.get();

	layerInitInstanceDispatchTable(instance, pTable, gpa);
	return pTable;
}

static inline VkLayerDispatchTable *initDeviceTable(VkDevice device, const PFN_vkGetDeviceProcAddr gpa,
                                                    DeviceTable &table)
{
	auto key = getDispatchKey(device);
	auto itr = table.find(key);
	VkLayerDispatchTable *pTable = nullptr;
	if (itr == end(table))
	{
		pTable = new VkLayerDispatchTable;
		table[key] = std::unique_ptr<VkLayerDispatchTable>(pTable);
	}
	else
		pTable = itr->second.get();

	layerInitDeviceDispatchTable(device, pTable, gpa);
	return pTable;
}
}
