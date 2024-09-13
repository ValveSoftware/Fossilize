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

namespace Fossilize
{
void layerInitDeviceDispatchTable(VkDevice device, VkLayerDispatchTable *table, PFN_vkGetDeviceProcAddr gpa)
{
	*table = {};
	table->GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gpa(device, "vkGetDeviceProcAddr");
	table->DestroyDevice = (PFN_vkDestroyDevice)gpa(device, "vkDestroyDevice");
	table->CreateShaderModule = (PFN_vkCreateShaderModule)gpa(device, "vkCreateShaderModule");
	table->CreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)gpa(device, "vkCreateGraphicsPipelines");
	table->CreateComputePipelines = (PFN_vkCreateComputePipelines)gpa(device, "vkCreateComputePipelines");
	table->DestroyPipeline = (PFN_vkDestroyPipeline)gpa(device, "vkDestroyPipeline");
	table->CreatePipelineLayout = (PFN_vkCreatePipelineLayout)gpa(device, "vkCreatePipelineLayout");
	table->CreateSampler = (PFN_vkCreateSampler)gpa(device, "vkCreateSampler");
	table->CreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout)gpa(device, "vkCreateDescriptorSetLayout");
	table->CreateRenderPass = (PFN_vkCreateRenderPass)gpa(device, "vkCreateRenderPass");
	table->CreateRenderPass2 = (PFN_vkCreateRenderPass2)gpa(device, "vkCreateRenderPass2");
	table->CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR)gpa(device, "vkCreateRenderPass2KHR");
	table->CreateSamplerYcbcrConversion = (PFN_vkCreateSamplerYcbcrConversion)gpa(device, "vkCreateSamplerYcbcrConversion");
	table->CreateSamplerYcbcrConversionKHR = (PFN_vkCreateSamplerYcbcrConversionKHR)gpa(device, "vkCreateSamplerYcbcrConversionKHR");
	table->CreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)gpa(device, "vkCreateRayTracingPipelinesKHR");
	table->GetShaderModuleIdentifierEXT = (PFN_vkGetShaderModuleIdentifierEXT)gpa(device, "vkGetShaderModuleIdentifierEXT");
	table->GetShaderModuleCreateInfoIdentifierEXT = (PFN_vkGetShaderModuleCreateInfoIdentifierEXT)gpa(device, "vkGetShaderModuleCreateInfoIdentifierEXT");
	table->CreatePipelineBinariesKHR = (PFN_vkCreatePipelineBinariesKHR)gpa(device, "vkCreatePipelineBinariesKHR");
}

void layerInitInstanceDispatchTable(VkInstance instance, VkLayerInstanceDispatchTable *table,
                                    PFN_vkGetInstanceProcAddr gpa)
{
	*table = {};
	table->DestroyInstance = (PFN_vkDestroyInstance)gpa(instance, "vkDestroyInstance");
	table->GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)gpa(instance, "vkGetPhysicalDeviceProperties");
	table->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)gpa(instance, "vkGetPhysicalDeviceProperties2");
	table->GetPhysicalDeviceProperties2KHR = (PFN_vkGetPhysicalDeviceProperties2KHR)gpa(instance, "vkGetPhysicalDeviceProperties2KHR");
}
}
