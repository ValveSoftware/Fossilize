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

#include "vulkan/vulkan.h"
#include "dispatch_helper.hpp"
#include "fossilize.hpp"
#include "fossilize_db.hpp"

namespace Fossilize
{
class Instance
{
public:
	Instance();

	static bool queryPrecompileQA();

	VkInstance getInstance()
	{
		return instance;
	}

	VkLayerInstanceDispatchTable *getTable()
	{
		return pTable;
	}

	void init(VkInstance instance, const VkApplicationInfo *pApp, VkLayerInstanceDispatchTable *pTable, PFN_vkGetInstanceProcAddr gpa);
	PFN_vkVoidFunction getProcAddr(const char *pName)
	{
		return gpa(instance, pName);
	}

	const VkApplicationInfo *getApplicationInfo() const
	{
		return pAppInfo;
	}

	StateRecorder *getStateRecorderForDevice(const VkPhysicalDeviceProperties2 *props,
	                                         const VkApplicationInfo *appInfo,
	                                         const void *device_pnext);

	bool capturesParanoid() const
	{
		return enableCrashHandler || synchronized;
	}

	bool capturesEagerly() const
	{
		return synchronized;
	}

	bool recordsImmutableSamplers() const
	{
		return shouldRecordImmutableSamplers;
	}

	bool enablesPrecompileQA() const
	{
		return enablePrecompileQA;
	}

	static void braceForGraphicsPipelineCrash(StateRecorder *recorder, const VkGraphicsPipelineCreateInfo *info);
	static void braceForComputePipelineCrash(StateRecorder *recorder, const VkComputePipelineCreateInfo *info);
	static void braceForRayTracingPipelineCrash(StateRecorder *recorder, const VkRayTracingPipelineCreateInfoKHR *info);
	static void completedPipelineCompilation();

private:
	ScratchAllocator alloc;
	VkApplicationInfo *pAppInfo = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkLayerInstanceDispatchTable *pTable = nullptr;
	PFN_vkGetInstanceProcAddr gpa = nullptr;
	ApplicationInfoFilter *infoFilter = nullptr;
	bool enableCrashHandler = false;
	bool synchronized = false;
	bool enablePrecompileQA = false;
	bool shouldRecordImmutableSamplers = true;
};
}
