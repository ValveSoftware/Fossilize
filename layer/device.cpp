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
#include "instance.hpp"
#include "path.hpp"
#include "utils.hpp"
#include <cinttypes>
#include <stdlib.h>

namespace Fossilize
{
Device::Device()
{
	precompileQAFailCount.store(0, std::memory_order_relaxed);
	precompileQASuccessCount.store(0, std::memory_order_relaxed);
}

void Device::registerPrecompileQASuccess(uint32_t numPipelines)
{
	uint64_t totalSuccess = precompileQASuccessCount.fetch_add(numPipelines, std::memory_order_relaxed) + numPipelines;
	LOGI("QA: Successfully created total of %llu pipelines without compilation.\n",
	     static_cast<unsigned long long>(totalSuccess));
}

void Device::registerPrecompileQAFailure(uint32_t numPipelines)
{
	uint64_t totalFailure = precompileQAFailCount.fetch_add(numPipelines, std::memory_order_relaxed) + numPipelines;
	LOGI("QA: Required fallback compilation for a total of %llu pipelines.\n",
	     static_cast<unsigned long long>(totalFailure));
}

void Device::init(VkPhysicalDevice gpu_, VkDevice device_, Instance *pInstance_,
                  const void *device_pnext,
                  VkLayerDispatchTable *pTable_)
{
	gpu = gpu_;
	device = device_;
	pInstance = pInstance_;
	pInstanceTable = pInstance->getTable();
	pTable = pTable_;

	// Need to know the UUID hash, so we can write module identifiers to appropriate path.
	auto *identifier =
			static_cast<const VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *>(
					findpNext(device_pnext,
					          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT));

	VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT identifierProps =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT };

	// Only bother if the application is actually using shader module identifiers.
	if (identifier && identifier->shaderModuleIdentifier)
	{
		props2.pNext = &identifierProps;
		if (pInstanceTable->GetPhysicalDeviceProperties2)
			pInstanceTable->GetPhysicalDeviceProperties2(gpu, &props2);
		else if (pInstanceTable->GetPhysicalDeviceProperties2KHR)
			pInstanceTable->GetPhysicalDeviceProperties2KHR(gpu, &props2);
		else
			pInstanceTable->GetPhysicalDeviceProperties(gpu, &props2.properties);

		usesModuleIdentifiers = true;
	}
	else
	{
		pInstanceTable->GetPhysicalDeviceProperties(gpu, &props2.properties);
	}

	recorder = pInstance->getStateRecorderForDevice(&props2, pInstance->getApplicationInfo(), device_pnext);
}
}
