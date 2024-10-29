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
#include "fossilize_errors.hpp"
#include <mutex>
#include <memory>

// VALVE: do exports without .def file, see vk_layer.h for definition on non-Windows platforms
#ifdef _MSC_VER
#if defined(_WIN32) && !defined(_WIN64)
// Josh: We need to match the export names up to the functions to avoid stdcall aliasing
#pragma comment(linker, "/EXPORT:VK_LAYER_fossilize_GetInstanceProcAddr=_VK_LAYER_fossilize_GetInstanceProcAddr@8")
#pragma comment(linker, "/EXPORT:VK_LAYER_fossilize_GetDeviceProcAddr=_VK_LAYER_fossilize_GetDeviceProcAddr@8")
#endif
#undef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#endif

extern "C"
{
#ifdef ANDROID
#define VK_LAYER_fossilize_GetInstanceProcAddr vkGetInstanceProcAddr
#define VK_LAYER_fossilize_GetDeviceProcAddr vkGetDeviceProcAddr
#endif
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_fossilize_GetInstanceProcAddr(VkInstance instance, const char *pName);
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_fossilize_GetDeviceProcAddr(VkDevice device, const char *pName);
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

static bool shallowCopyPnextChain(ScratchAllocator &alloc, const void *pNext, const void **outpNext);

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

	// Safely overwrite the device creation infos to support pipeline cache control when using QA mode.
	// We don't care about fallbacks since we want to fail if device doesn't support it.
	VkPhysicalDevicePipelineCreationCacheControlFeatures cacheControlFeatures;
	auto tmpCreateInfo = *pCreateInfo;
	std::unique_ptr<const char * []> enabledExtensions;
	ScratchAllocator alloc;

	if (layer->enablesPrecompileQA())
	{
		// Make sure the relevant EXT is enabled.
		// Just assume it works, since precompile QA is developer-only feature and everyone supports it since it's core 1.3.
		uint32_t i;
		for (i = 0; i < tmpCreateInfo.enabledExtensionCount; i++)
			if (strcmp(tmpCreateInfo.ppEnabledExtensionNames[i], VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME) == 0)
				break;

		if (i == tmpCreateInfo.enabledExtensionCount)
		{
			enabledExtensions.reset(new const char *[tmpCreateInfo.enabledExtensionCount + 1]);
			memcpy(enabledExtensions.get(), tmpCreateInfo.ppEnabledExtensionNames,
			       tmpCreateInfo.enabledExtensionCount * sizeof(const char *));
			enabledExtensions[tmpCreateInfo.enabledExtensionCount++] = VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME;
			tmpCreateInfo.ppEnabledExtensionNames = enabledExtensions.get();
		}

		if (!shallowCopyPnextChain(alloc, tmpCreateInfo.pNext, &tmpCreateInfo.pNext))
		{
			LOGE_LEVEL("Failed to shallow copy pNext chain.");
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		if (void *vk13 = findpNext(const_cast<void *>(tmpCreateInfo.pNext), VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES))
		{
			static_cast<VkPhysicalDeviceVulkan13Features *>(vk13)->pipelineCreationCacheControl = VK_TRUE;
		}
		else if (void *ccf = findpNext(const_cast<void *>(tmpCreateInfo.pNext), VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES))
		{
			static_cast<VkPhysicalDevicePipelineCreationCacheControlFeatures *>(ccf)->pipelineCreationCacheControl = VK_TRUE;
		}
		else
		{
			cacheControlFeatures = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES,
				const_cast<void *>(tmpCreateInfo.pNext),
				VK_TRUE
			};

			tmpCreateInfo.pNext = &cacheControlFeatures;
		}

		// We shallow copied this too, make sure we update the right thing.
		chainInfo = getChainInfo(&tmpCreateInfo, VK_LAYER_LINK_INFO);
	}

	// Advance the link info for the next element on the chain
	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;

	auto res = fpCreateDevice(gpu, &tmpCreateInfo, pAllocator, pDevice);
	if (res != VK_SUCCESS)
		return res;

	// Build a physical device features 2 struct if we cannot find it in pCreateInfo.
	auto *pdf2 = static_cast<const VkPhysicalDeviceFeatures2 *>(findpNext(pCreateInfo, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2));
	VkPhysicalDeviceFeatures2 physicalDeviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	const void *device_pnext;

	if (!pdf2)
	{
		pdf2 = &physicalDeviceFeatures2;
		if (pCreateInfo->pEnabledFeatures)
			physicalDeviceFeatures2.features = *pCreateInfo->pEnabledFeatures;

		// When DeviceCreateInfo::pNext is found, chain it to serialize
		// other physical device features 2 struct.
		device_pnext = &physicalDeviceFeatures2;
		physicalDeviceFeatures2.pNext = const_cast<void *>(pCreateInfo->pNext);
	}
	else
		device_pnext = pCreateInfo->pNext;

	{
		lock_guard<mutex> holder{globalLock};
		auto *device = createLayerData(getDispatchKey(*pDevice), deviceData);
		device->init(gpu, *pDevice, layer, device_pnext, initDeviceTable(*pDevice, fpGetDeviceProcAddr, deviceDispatch));
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

	auto tmpCreateInfo = *pCreateInfo;
	std::unique_ptr<const char * []> enabledExtensions;

	if (Instance::queryPrecompileQA())
	{
		// Need GDP2 for pipeline cache control.
		// Only relevant when using Vulkan 1.1 instance.
		uint32_t i;
		for (i = 0; i < tmpCreateInfo.enabledExtensionCount; i++)
			if (strcmp(tmpCreateInfo.ppEnabledExtensionNames[i], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0)
				break;

		if (i == tmpCreateInfo.enabledExtensionCount)
		{
			enabledExtensions.reset(new const char *[tmpCreateInfo.enabledExtensionCount + 1]);
			memcpy(enabledExtensions.get(), tmpCreateInfo.ppEnabledExtensionNames,
			       tmpCreateInfo.enabledExtensionCount * sizeof(const char *));
			enabledExtensions[tmpCreateInfo.enabledExtensionCount++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
			tmpCreateInfo.ppEnabledExtensionNames = enabledExtensions.get();
		}
	}

	chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
	auto res = fpCreateInstance(&tmpCreateInfo, pAllocator, pInstance);
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

static void fixup_props2_chain(VkPhysicalDeviceProperties2 *props2)
{
	auto *binary_props = static_cast<VkPhysicalDevicePipelineBinaryPropertiesKHR *>(
			findpNext(props2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR));

	// If we're using Fossilize, we prefer to use internal caches when possible.
	// Gently asks that applications do not try to be clever and let the layer do its thing.
	if (binary_props && binary_props->pipelineBinaryInternalCache)
	{
		binary_props->pipelineBinaryPrefersInternalCache = VK_TRUE;
		binary_props->pipelineBinaryPrecompiledInternalCache = VK_TRUE;
	}
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2 *props2)
{
	Instance *layer;
	{
		void *key = getDispatchKey(gpu);
		lock_guard<mutex> holder{ globalLock };
		layer = getLayerData(key, instanceData);
	}

	layer->getTable()->GetPhysicalDeviceProperties2(gpu, props2);
	fixup_props2_chain(props2);
}

static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2KHR(VkPhysicalDevice gpu, VkPhysicalDeviceProperties2 *props2)
{
	Instance *layer;
	{
		void *key = getDispatchKey(gpu);
		lock_guard<mutex> holder{ globalLock };
		layer = getLayerData(key, instanceData);
	}

	layer->getTable()->GetPhysicalDeviceProperties2KHR(gpu, props2);
	fixup_props2_chain(props2);
}

static VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator)
{
	lock_guard<mutex> holder{ globalLock };

	void *key = getDispatchKey(instance);
	auto *layer = getLayerData(key, instanceData);
	layer->getTable()->DestroyInstance(instance, pAllocator);
	destroyLayerData(key, instanceData);
}

template <typename T>
static VkPipelineCreateFlags2KHR getEffectivePipelineFlags(const T &info)
{
	auto *flags2 = static_cast<const VkPipelineCreateFlags2CreateInfoKHR *>(
			findpNext(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR));
	return flags2 ? flags2->flags : info.flags;
}

template <typename T>
static bool pipelineCreationIsNonBlocking(const T &info)
{
	auto flags = getEffectivePipelineFlags(info);

	auto *libraries = static_cast<const VkPipelineLibraryCreateInfoKHR *>(
			findpNext(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR));

	auto *binaries = static_cast<const VkPipelineBinaryInfoKHR *>(
			findpNext(info.pNext, VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR));

	// Creating pipelines from binaries are always non-blocking.
	if (binaries && binaries->binaryCount)
		return true;

	// Fast link pipelines are expected to be non-blocking always, and at least RADV does not cache it.
	// Don't bother checking if this PSO is in cache, since there's no reason for it to be.
	if (libraries && libraries->libraryCount && (flags & VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT) == 0)
		return true;

	return (flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) != 0;
}

template <typename T>
static bool shouldAttemptNonBlockingCreation(Device *layer, uint32_t createInfoCount, const T *pCreateInfos)
{
	if (layer->getInstance()->enablesPrecompileQA() && createInfoCount)
	{
		// If app is already trying non-blocking compile, the app will handle fallback case.
		for (uint32_t i = 0; i < createInfoCount; i++)
			if (pipelineCreationIsNonBlocking(pCreateInfos[i]))
				return false;

		return true;
	}
	else
	{
		return false;
	}
}

static size_t getPnextStructSize(VkStructureType type)
{
	// Table autogenerated by extract_vulkan_extensions.py
	static const struct
	{
		VkStructureType type;
		size_t size;
	} sizes[] = {
		{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_INDIRECT_BUFFER_INFO_NV, sizeof(VkComputePipelineIndirectBufferInfoNV) },
		{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, sizeof(VkPipelineCreateFlags2CreateInfoKHR) },
		{ VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, sizeof(VkPipelineBinaryInfoKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV, sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV, sizeof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV) },
		{ VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO, sizeof(VkDevicePrivateDataCreateInfo) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, sizeof(VkPhysicalDevicePrivateDataFeatures) },
		{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_SHADER_GROUPS_CREATE_INFO_NV, sizeof(VkGraphicsPipelineShaderGroupsCreateInfoNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, sizeof(VkPhysicalDeviceFeatures2) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, sizeof(VkPhysicalDeviceVariablePointersFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, sizeof(VkPhysicalDeviceMultiviewFeatures) },
		{ VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO, sizeof(VkDeviceGroupDeviceCreateInfo) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, sizeof(VkPhysicalDevicePresentIdFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, sizeof(VkPhysicalDevicePresentWaitFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT, sizeof(VkPipelineDiscardRectangleStateCreateInfoEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, sizeof(VkPhysicalDevice16BitStorageFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, sizeof(VkPhysicalDeviceProtectedMemoryFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT, sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT, sizeof(VkPhysicalDeviceMultiDrawFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, sizeof(VkPhysicalDeviceInlineUniformBlockFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, sizeof(VkPhysicalDeviceMaintenance4Features) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, sizeof(VkPhysicalDeviceMaintenance5FeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR, sizeof(VkPhysicalDeviceMaintenance6FeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR, sizeof(VkPhysicalDeviceMaintenance7FeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, sizeof(VkPhysicalDeviceShaderDrawParametersFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, sizeof(VkPhysicalDeviceShaderFloat16Int8Features) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, sizeof(VkPhysicalDeviceHostQueryResetFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES_KHR, sizeof(VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT, sizeof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT, sizeof(VkDeviceDeviceMemoryReportCreateInfoEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, sizeof(VkPhysicalDeviceDescriptorIndexingFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, sizeof(VkPhysicalDevice8BitStorageFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT, sizeof(VkPhysicalDeviceConditionalRenderingFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, sizeof(VkPhysicalDeviceShaderAtomicInt64Features) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, sizeof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT, sizeof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, sizeof(VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT, sizeof(VkPhysicalDeviceASTCDecodeFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, sizeof(VkPhysicalDeviceTransformFeedbackFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_REPRESENTATIVE_FRAGMENT_TEST_FEATURES_NV, sizeof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV, sizeof(VkPipelineRepresentativeFragmentTestStateCreateInfoNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXCLUSIVE_SCISSOR_FEATURES_NV, sizeof(VkPhysicalDeviceExclusiveScissorFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CORNER_SAMPLED_IMAGE_FEATURES_NV, sizeof(VkPhysicalDeviceCornerSampledImageFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR, sizeof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_FOOTPRINT_FEATURES_NV, sizeof(VkPhysicalDeviceShaderImageFootprintFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEDICATED_ALLOCATION_IMAGE_ALIASING_FEATURES_NV, sizeof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV, sizeof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_NV, sizeof(VkPhysicalDeviceMemoryDecompressionFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV, sizeof(VkPhysicalDeviceShadingRateImageFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INVOCATION_MASK_FEATURES_HUAWEI, sizeof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV, sizeof(VkPhysicalDeviceMeshShaderFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, sizeof(VkPhysicalDeviceMeshShaderFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, sizeof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, sizeof(VkPhysicalDeviceRayQueryFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR, sizeof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR) },
		{ VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD, sizeof(VkDeviceMemoryOverallocationCreateInfoAMD) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT, sizeof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_FEATURES_EXT, sizeof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_FEATURES_QCOM, sizeof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT, sizeof(VkPhysicalDeviceMemoryPriorityFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT, sizeof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT, sizeof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, sizeof(VkPhysicalDeviceImagelessFramebufferFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, sizeof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_NV, sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT, sizeof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, sizeof(VkPipelineCreationFeedbackCreateInfo) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_BARRIER_FEATURES_NV, sizeof(VkPhysicalDevicePresentBarrierFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR, sizeof(VkPhysicalDevicePerformanceQueryFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV, sizeof(VkPhysicalDeviceCoverageReductionModeFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_FUNCTIONS_2_FEATURES_INTEL, sizeof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderClockFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR, sizeof(VkPhysicalDeviceIndexTypeUint8FeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV, sizeof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, sizeof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT, sizeof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, sizeof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, sizeof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT, sizeof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures) },
		{ VK_STRUCTURE_TYPE_SUBPASS_SHADING_PIPELINE_CREATE_INFO_HUAWEI, sizeof(VkSubpassShadingPipelineCreateInfoHUAWEI) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR, sizeof(VkPhysicalDeviceLineRasterizationFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, sizeof(VkPhysicalDeviceVulkan11Features) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, sizeof(VkPhysicalDeviceVulkan12Features) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, sizeof(VkPhysicalDeviceVulkan13Features) },
		{ VK_STRUCTURE_TYPE_PIPELINE_COMPILER_CONTROL_CREATE_INFO_AMD, sizeof(VkPipelineCompilerControlCreateInfoAMD) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD, sizeof(VkPhysicalDeviceCoherentMemoryFeaturesAMD) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT, sizeof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, sizeof(VkPipelineLibraryCreateInfoKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV, sizeof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV) },
		{ VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV, sizeof(VkDeviceDiagnosticsConfigCreateInfoNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, sizeof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, sizeof(VkPhysicalDeviceRobustness2FeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, sizeof(VkPhysicalDeviceImageRobustnessFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR, sizeof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, sizeof(VkPhysicalDevice4444FormatsFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_SHADING_FEATURES_HUAWEI, sizeof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_CULLING_SHADER_FEATURES_HUAWEI, sizeof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, sizeof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR, sizeof(VkPipelineFragmentShadingRateStateCreateInfoKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, sizeof(VkPhysicalDeviceShaderTerminateInvocationFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV, sizeof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV, sizeof(VkPipelineFragmentShadingRateEnumStateCreateInfoNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT, sizeof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_SLICED_VIEW_OF_3D_FEATURES_EXT, sizeof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT, sizeof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT, sizeof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, sizeof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, sizeof(VkPhysicalDeviceDepthClipControlFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT, sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_CONTROL_FEATURES_EXT, sizeof(VkPhysicalDeviceDepthClampControlFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, sizeof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_RDMA_FEATURES_NV, sizeof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_RELAXED_EXTENDED_INSTRUCTION_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT, sizeof(VkPhysicalDeviceColorWriteEnableFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, sizeof(VkPhysicalDeviceSynchronization2Features) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, sizeof(VkPhysicalDeviceHostImageCopyFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT, sizeof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_DITHERING_FEATURES_EXT, sizeof(VkPhysicalDeviceLegacyDitheringFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT, sizeof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROTECTED_ACCESS_FEATURES_EXT, sizeof(VkPhysicalDevicePipelineProtectedAccessFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR, sizeof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INHERITED_VIEWPORT_SCISSOR_FEATURES_NV, sizeof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_2_PLANE_444_FORMATS_FEATURES_EXT, sizeof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, sizeof(VkPhysicalDeviceProvokingVertexFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT, sizeof(VkPhysicalDeviceDescriptorBufferFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR, sizeof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV, sizeof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV, sizeof(VkPhysicalDeviceRayTracingValidationFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RGBA10X6_FORMATS_FEATURES_EXT, sizeof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, sizeof(VkPipelineRenderingCreateInfo) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, sizeof(VkPhysicalDeviceDynamicRenderingFeatures) },
		{ VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD, sizeof(VkAttachmentSampleCountInfoAMD) },
		{ VK_STRUCTURE_TYPE_MULTIVIEW_PER_VIEW_ATTRIBUTES_INFO_NVX, sizeof(VkMultiviewPerViewAttributesInfoNVX) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT, sizeof(VkPhysicalDeviceImageViewMinLodFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT, sizeof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINEAR_COLOR_ATTACHMENT_FEATURES_NV, sizeof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT, sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, sizeof(VkPhysicalDevicePipelineBinaryFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_DEVICE_PIPELINE_BINARY_INTERNAL_CACHE_CONTROL_KHR, sizeof(VkDevicePipelineBinaryInternalCacheControlKHR) },
		{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, sizeof(VkGraphicsPipelineLibraryCreateInfoEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_SET_HOST_MAPPING_FEATURES_VALVE, sizeof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_FEATURES_EXT, sizeof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, sizeof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT, sizeof(VkPhysicalDeviceImageCompressionControlFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_FEATURES_EXT, sizeof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_MERGE_FEEDBACK_FEATURES_EXT, sizeof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT, sizeof(VkPhysicalDeviceOpacityMicromapFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT, sizeof(VkPhysicalDevicePipelinePropertiesFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EARLY_AND_LATE_FRAGMENT_TESTS_FEATURES_AMD, sizeof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT, sizeof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT, sizeof(VkPhysicalDevicePipelineRobustnessFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT, sizeof(VkPipelineRobustnessCreateInfoEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM, sizeof(VkPhysicalDeviceImageProcessingFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_PROPERTIES_FEATURES_QCOM, sizeof(VkPhysicalDeviceTilePropertiesFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_AMIGO_PROFILING_FEATURES_SEC, sizeof(VkPhysicalDeviceAmigoProfilingFeaturesSEC) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT, sizeof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_EXT, sizeof(VkPhysicalDeviceDepthClampZeroOneFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT, sizeof(VkPhysicalDeviceAddressBindingReportFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV, sizeof(VkPhysicalDeviceOpticalFlowFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, sizeof(VkPhysicalDeviceFaultFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT, sizeof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_BUILTINS_FEATURES_ARM, sizeof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT, sizeof(VkPhysicalDeviceFrameBoundaryFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT, sizeof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT, sizeof(VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT, sizeof(VkPhysicalDeviceDepthBiasControlFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV, sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_SPARSE_ADDRESS_SPACE_FEATURES_NV, sizeof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_VIEWPORTS_FEATURES_QCOM, sizeof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR, sizeof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_RENDER_AREAS_FEATURES_QCOM, sizeof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, sizeof(VkPhysicalDeviceShaderObjectFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT, sizeof(VkPhysicalDeviceShaderTileImageFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD, sizeof(VkPhysicalDeviceAntiLagFeaturesAMD) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_CLAMP_FEATURES_QCOM, sizeof(VkPhysicalDeviceCubicClampFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_DEGAMMA_FEATURES_QCOM, sizeof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_WEIGHTS_FEATURES_QCOM, sizeof(VkPhysicalDeviceCubicWeightsFeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_2_FEATURES_QCOM, sizeof(VkPhysicalDeviceImageProcessing2FeaturesQCOM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_POOL_OVERALLOCATION_FEATURES_NV, sizeof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV, sizeof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_FEATURES_NV, sizeof(VkPhysicalDeviceCudaKernelLaunchFeaturesNV) },
		{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_SHADER_CORE_CONTROL_CREATE_INFO_ARM, sizeof(VkDeviceQueueShaderCoreControlCreateInfoARM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCHEDULING_CONTROLS_FEATURES_ARM, sizeof(VkPhysicalDeviceSchedulingControlsFeaturesARM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG, sizeof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RENDER_PASS_STRIPED_FEATURES_ARM, sizeof(VkPhysicalDeviceRenderPassStripedFeaturesARM) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderSubgroupRotateFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderExpectAssumeFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderFloatControls2FeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, sizeof(VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR, sizeof(VkRenderingAttachmentLocationInfoKHR) },
		{ VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR, sizeof(VkRenderingInputAttachmentIndexInfoKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR, sizeof(VkPhysicalDeviceShaderQuadControlFeaturesKHR) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT16_VECTOR_FEATURES_NV, sizeof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, sizeof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV, sizeof(VkPhysicalDeviceRawAccessChainsFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMMAND_BUFFER_INHERITANCE_FEATURES_NV, sizeof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ALIGNMENT_CONTROL_FEATURES_MESA, sizeof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_REPLICATED_COMPOSITES_FEATURES_EXT, sizeof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_EXT, sizeof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesEXT) },
		{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV, sizeof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV) },
	};

	for (auto &siz : sizes)
		if (siz.type == type)
			return siz.size;

	return 0;
}

static bool shallowCopyPnextChain(ScratchAllocator &alloc, const void *pNext, const void **outpNext)
{
	VkBaseInStructure new_pnext = {};
	const VkBaseInStructure **ppNext = &new_pnext.pNext;

	while (pNext)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);

		size_t copy_size;

		// Magic pNext type which only exists in loader, not XML.
		if (pin->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
			copy_size = sizeof(VkLayerDeviceCreateInfo);
		else
			copy_size = getPnextStructSize(pin->sType);

		if (!copy_size)
		{
			LOGE_LEVEL("Cannot shallow copy unknown pNext sType: %d.\n", int(pin->sType));
			return false;
		}

		auto *buffer = alloc.allocate_raw(copy_size, 16);
		memcpy(buffer, pin, copy_size);
		*ppNext = static_cast<VkBaseInStructure *>(buffer);

		pNext = pin->pNext;
		ppNext = const_cast<const VkBaseInStructure **>(&(*ppNext)->pNext);
		*ppNext = nullptr;
	}

	*outpNext = new_pnext.pNext;
	return true;
}

template <typename CreateInfo, typename CompileFunc, typename PipelineRecorder>
static VkResult compileNonBlockingPipelines(Device *layer, uint32_t createInfoCount, const CreateInfo *pCreateInfos,
                                            VkPipeline *pPipelines, const VkAllocationCallbacks *pAllocator,
                                            const CompileFunc &compileFunc, const PipelineRecorder &rec)
{
	ScratchAllocator alloc;
	auto *modifiedCreateInfos = alloc.allocate_n<CreateInfo>(createInfoCount);
	memcpy(modifiedCreateInfos, pCreateInfos, createInfoCount * sizeof(*modifiedCreateInfos));

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		modifiedCreateInfos[i].flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;

		if (!shallowCopyPnextChain(alloc, modifiedCreateInfos[i].pNext, &modifiedCreateInfos[i].pNext))
			return VK_PIPELINE_COMPILE_REQUIRED;

		auto *flags2 = static_cast<VkPipelineCreateFlags2CreateInfoKHR *>(const_cast<void *>(
				findpNext(modifiedCreateInfos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)));

		if (flags2)
			flags2->flags |= VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR;
	}

	VkResult res = compileFunc(modifiedCreateInfos);

	if (res == VK_PIPELINE_COMPILE_REQUIRED)
	{
		for (uint32_t i = 0; i < createInfoCount; i++)
		{
			if (pPipelines[i] == VK_NULL_HANDLE)
			{
				auto *flags2 = static_cast<VkPipelineCreateFlags2CreateInfoKHR *>(const_cast<void *>(
						findpNext(pCreateInfos[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)));
				VkPipelineCreateFlags2KHR flags = flags2 ? flags2->flags : pCreateInfos[i].flags;
				LOGW_LEVEL("QA: Pipeline compilation required for pipeline, flags %08x'%08x.\n",
				           uint32_t(flags >> 32), uint32_t(flags));
				layer->registerPrecompileQAFailure(1);
				// Record all entries first in case we have derived pipeline references which went through unharmed.
				rec(i);
			}
			else
			{
				layer->registerPrecompileQASuccess(1);
			}
		}

		for (uint32_t i = 0; i < createInfoCount; i++)
		{
			layer->getTable()->DestroyPipeline(layer->getDevice(), pPipelines[i], pAllocator);
			pPipelines[i] = VK_NULL_HANDLE;
		}
	}
	else
	{
		layer->registerPrecompileQASuccess(createInfoCount);
	}

	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelinesNormal(Device *layer,
                                                                    VkDevice device, VkPipelineCache pipelineCache,
                                                                    uint32_t createInfoCount,
                                                                    const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                                    const VkAllocationCallbacks *pAllocator,
                                                                    VkPipeline *pPipelines)
{
	VkResult res = VK_PIPELINE_COMPILE_REQUIRED;
	bool shouldRecord = true;

	if (shouldAttemptNonBlockingCreation(layer, createInfoCount, pCreateInfos))
	{
		// Explicitly ignore pipeline cache since we want to test the internal cache for hits.
		const auto compileFunc = [&](const VkGraphicsPipelineCreateInfo *modifiedInfos) {
			return layer->getTable()->CreateGraphicsPipelines(
					device, VK_NULL_HANDLE, createInfoCount, modifiedInfos, pAllocator, pPipelines);
		};

		const auto recordFunc = [&](uint32_t index) {
			if (!layer->getRecorder().record_graphics_pipeline(
					pPipelines[index], pCreateInfos[index], pPipelines, createInfoCount, 0,
					device,
					layer->requiresModuleIdentifiers()
					? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
			{
				LOGW_LEVEL("Recording graphics pipeline failed, usually caused by unsupported pNext.\n");
			}
		};

		res = compileNonBlockingPipelines(layer, createInfoCount, pCreateInfos, pPipelines, pAllocator, compileFunc, recordFunc);

		// Only record the pipelines which failed to compile so we can debug why.
		shouldRecord = false;
		for (uint32_t i = 0; i < createInfoCount; i++)
		{
			// If we're creating a library, there might be future pipelines which depend on this pipeline to record properly,
			// so just record it anyway.
			if (getEffectivePipelineFlags(pCreateInfos[i]) & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)
			{
				shouldRecord = true;
				break;
			}
		}
	}

	// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
	if (res == VK_PIPELINE_COMPILE_REQUIRED)
		res = layer->getTable()->CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);

	// If pipeline compile fails due to pipeline cache control we get a null handle for pipeline, so we need to
	// treat it as a failure.
	if (res != VK_SUCCESS || !shouldRecord)
		return res;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		if (!layer->getRecorder().record_graphics_pipeline(
				pPipelines[i], pCreateInfos[i], pPipelines, createInfoCount, 0,
				device,
				layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Recording graphics pipeline failed, usually caused by unsupported pNext.\n");
		}
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelinesParanoid(Device *layer,
                                                                      VkDevice device, VkPipelineCache pipelineCache,
                                                                      uint32_t createInfoCount,
                                                                      const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                                      const VkAllocationCallbacks *pAllocator,
                                                                      VkPipeline *pPipelines)
{
	VkResult final_res = VK_SUCCESS;

	// If we return early due to pipeline compile required exit, a NULL handle needs to signal no pipeline.
	for (uint32_t i = 0; i < createInfoCount; i++)
		pPipelines[i] = VK_NULL_HANDLE;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		// Fixup base pipeline index since we unroll the Create call.
		auto info = pCreateInfos[i];
		if ((info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
		    info.basePipelineHandle == VK_NULL_HANDLE &&
		    info.basePipelineIndex >= 0)
		{
			info.basePipelineHandle = pPipelines[info.basePipelineIndex];
			info.basePipelineIndex = -1;
		}

		bool eager = layer->getInstance()->capturesEagerly();
		if (eager && !layer->getRecorder().record_graphics_pipeline(
				VK_NULL_HANDLE, info, nullptr, 0, 0,
				device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to capture eagerly.\n");
		}

		// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
		// Write arguments in TLS in-case we crash here.
		Instance::braceForGraphicsPipelineCrash(&layer->getRecorder(), &info);
		auto res = layer->getTable()->CreateGraphicsPipelines(device, pipelineCache, 1, &info,
		                                                      pAllocator, &pPipelines[i]);
		Instance::completedPipelineCompilation();

		// Record failing pipelines for repro.
		if (!layer->getRecorder().record_graphics_pipeline(
				res == VK_SUCCESS ? pPipelines[i] : VK_NULL_HANDLE, info, nullptr, 0, 0,
				device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to record graphics pipeline, usually caused by unsupported pNext.\n");
		}

		if (res == VK_PIPELINE_COMPILE_REQUIRED_EXT)
			final_res = res;

		if (res < 0)
		{
			for (uint32_t j = 0; j < i; j++)
				layer->getTable()->DestroyPipeline(device, pPipelines[j], pAllocator);
			return res;
		}
		else if (res == VK_PIPELINE_COMPILE_REQUIRED_EXT &&
		         (info.flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT) != 0)
		{
			break;
		}
	}

	return final_res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                              uint32_t createInfoCount,
                                                              const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                              const VkAllocationCallbacks *pAllocator,
                                                              VkPipeline *pPipelines)
{
	auto *layer = get_device_layer(device);

	if (layer->getInstance()->capturesParanoid())
		return CreateGraphicsPipelinesParanoid(layer, device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
	else
		return CreateGraphicsPipelinesNormal(layer, device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelinesNormal(Device *layer,
                                                                   VkDevice device, VkPipelineCache pipelineCache,
                                                                   uint32_t createInfoCount,
                                                                   const VkComputePipelineCreateInfo *pCreateInfos,
                                                                   const VkAllocationCallbacks *pAllocator,
                                                                   VkPipeline *pPipelines)
{
	VkResult res = VK_PIPELINE_COMPILE_REQUIRED;
	bool shouldRecord = true;

	if (shouldAttemptNonBlockingCreation(layer, createInfoCount, pCreateInfos))
	{
		const auto compileFunc = [&](const VkComputePipelineCreateInfo *modifiedInfos) {
			// Explicitly ignore pipeline cache since we want to test the internal cache for hits.
			return layer->getTable()->CreateComputePipelines(
					device, VK_NULL_HANDLE, createInfoCount, modifiedInfos, pAllocator, pPipelines);
		};

		const auto recordFunc = [&](uint32_t index) {
			if (!layer->getRecorder().record_compute_pipeline(
					pPipelines[index], pCreateInfos[index], pPipelines, createInfoCount, 0,
					device,
					layer->requiresModuleIdentifiers()
					? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
			{
				LOGW_LEVEL("Recording graphics pipeline failed, usually caused by unsupported pNext.\n");
			}
		};

		res = compileNonBlockingPipelines(layer, createInfoCount, pCreateInfos, pPipelines, pAllocator, compileFunc, recordFunc);
		// Only record the pipelines which failed to compile so we can debug why.
		shouldRecord = false;
	}

	// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
	if (res == VK_PIPELINE_COMPILE_REQUIRED)
		res = layer->getTable()->CreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);

	// If pipeline compile fails due to pipeline cache control we get a null handle for pipeline, so we need to
	// treat it as a failure.
	if (res != VK_SUCCESS || !shouldRecord)
		return res;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		if (!layer->getRecorder().record_compute_pipeline(
				pPipelines[i], pCreateInfos[i], pPipelines, createInfoCount, 0,
				device,
				layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to record compute pipeline, usually caused by unsupported pNext.\n");
		}
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelinesParanoid(Device *layer,
                                                                     VkDevice device, VkPipelineCache pipelineCache,
                                                                     uint32_t createInfoCount,
                                                                     const VkComputePipelineCreateInfo *pCreateInfos,
                                                                     const VkAllocationCallbacks *pAllocator,
                                                                     VkPipeline *pPipelines)
{
	VkResult final_res = VK_SUCCESS;

	// If we return early due to pipeline compile required exit, a NULL handle needs to signal no pipeline.
	for (uint32_t i = 0; i < createInfoCount; i++)
		pPipelines[i] = VK_NULL_HANDLE;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		// Fixup base pipeline index since we unroll the Create call.
		auto info = pCreateInfos[i];
		if ((info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
		    info.basePipelineHandle == VK_NULL_HANDLE &&
		    info.basePipelineIndex >= 0)
		{
			info.basePipelineHandle = pPipelines[info.basePipelineIndex];
			info.basePipelineIndex = -1;
		}

		bool eager = layer->getInstance()->capturesEagerly();
		if (eager && !layer->getRecorder().record_compute_pipeline(
				VK_NULL_HANDLE, info, nullptr, 0, 0,
				device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to capture eagerly.\n");
		}

		// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
		// Write arguments in TLS in-case we crash here.
		Instance::braceForComputePipelineCrash(&layer->getRecorder(), &info);
		auto res = layer->getTable()->CreateComputePipelines(device, pipelineCache, 1, &info,
		                                                     pAllocator, &pPipelines[i]);
		Instance::completedPipelineCompilation();

		// Record failing pipelines for repro.
		if (!layer->getRecorder().record_compute_pipeline(
				res == VK_SUCCESS ? pPipelines[i] : VK_NULL_HANDLE, info, nullptr, 0, 0,
				device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to record compute pipeline, usually caused by unsupported pNext.\n");
		}

		if (res == VK_PIPELINE_COMPILE_REQUIRED_EXT)
			final_res = res;

		if (res < 0)
		{
			for (uint32_t j = 0; j < i; j++)
				layer->getTable()->DestroyPipeline(device, pPipelines[j], pAllocator);
			return res;
		}
		else if (res == VK_PIPELINE_COMPILE_REQUIRED_EXT &&
		         (info.flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT) != 0)
		{
			break;
		}
	}

	return final_res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                                             uint32_t createInfoCount,
                                                             const VkComputePipelineCreateInfo *pCreateInfos,
                                                             const VkAllocationCallbacks *pAllocator,
                                                             VkPipeline *pPipelines)
{
	auto *layer = get_device_layer(device);

	if (layer->getInstance()->capturesParanoid())
		return CreateComputePipelinesParanoid(layer, device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
	else
		return CreateComputePipelinesNormal(layer, device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesNormal(Device *layer,
                                                                      VkDevice device, VkDeferredOperationKHR deferredOperation,
                                                                      VkPipelineCache pipelineCache,
                                                                      uint32_t createInfoCount,
                                                                      const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
                                                                      const VkAllocationCallbacks *pAllocator,
                                                                      VkPipeline *pPipelines)
{
	bool shouldRecord = true;

	if (shouldAttemptNonBlockingCreation(layer, createInfoCount, pCreateInfos))
	{
		const auto compileFunc = [&](const VkRayTracingPipelineCreateInfoKHR *modifiedInfos) {
			// Explicitly ignore pipeline cache since we want to test the internal cache for hits.
			return layer->getTable()->CreateRayTracingPipelinesKHR(
					device, VK_NULL_HANDLE, VK_NULL_HANDLE, createInfoCount, modifiedInfos, pAllocator, pPipelines);
		};

		const auto recordFunc = [&](uint32_t index) {
			if (!layer->getRecorder().record_raytracing_pipeline(
					pPipelines[index], pCreateInfos[index], pPipelines, createInfoCount, 0,
					device,
					layer->requiresModuleIdentifiers()
					? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
			{
				LOGW_LEVEL("Recording graphics pipeline failed, usually caused by unsupported pNext.\n");
			}
		};

		auto res = compileNonBlockingPipelines(layer, createInfoCount, pCreateInfos, pPipelines, pAllocator, compileFunc, recordFunc);
		// Only record the pipelines which failed to compile so we can debug why.
		shouldRecord = false;

		for (uint32_t i = 0; i < createInfoCount; i++)
		{
			// If we're creating a library, there might be future pipelines which depend on this pipeline to record properly,
			// so just record it anyway.
			if (getEffectivePipelineFlags(pCreateInfos[i]) & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR)
			{
				shouldRecord = true;
				break;
			}
		}

		// If app asked for deferredOperations, need to recreate the PSO, but with proper deferred operations this time.
		if (res == VK_SUCCESS && deferredOperation != VK_NULL_HANDLE)
		{
			for (uint32_t i = 0; i < createInfoCount; i++)
			{
				layer->getTable()->DestroyPipeline(device, pPipelines[i], pAllocator);
				pPipelines[i] = VK_NULL_HANDLE;
			}
		}
	}

	// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
	auto res = layer->getTable()->CreateRayTracingPipelinesKHR(
			device, deferredOperation, pipelineCache,
			createInfoCount, pCreateInfos, pAllocator, pPipelines);

	// If pipeline compile fails due to pipeline cache control we get a null handle for pipeline, so we need to
	// treat it as a failure.
	if (res != VK_SUCCESS || !shouldRecord)
		return res;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		if (!layer->getRecorder().record_raytracing_pipeline(
				pPipelines[i], pCreateInfos[i], pPipelines, createInfoCount, 0,
				device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to record compute pipeline, usually caused by unsupported pNext.\n");
		}
	}

	return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesParanoid(Device *layer,
                                                                        VkDevice device, VkDeferredOperationKHR deferredOperation,
                                                                        VkPipelineCache pipelineCache,
                                                                        uint32_t createInfoCount,
                                                                        const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
                                                                        const VkAllocationCallbacks *pAllocator,
                                                                        VkPipeline *pPipelines)
{
	VkResult final_res = VK_SUCCESS;

	// If we return early due to pipeline compile required exit, a NULL handle needs to signal no pipeline.
	for (uint32_t i = 0; i < createInfoCount; i++)
		pPipelines[i] = VK_NULL_HANDLE;

	for (uint32_t i = 0; i < createInfoCount; i++)
	{
		// Fixup base pipeline index since we unroll the Create call.
		auto info = pCreateInfos[i];
		if ((info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
		    info.basePipelineHandle == VK_NULL_HANDLE &&
		    info.basePipelineIndex >= 0)
		{
			info.basePipelineHandle = pPipelines[info.basePipelineIndex];
			info.basePipelineIndex = -1;
		}

		bool eager = layer->getInstance()->capturesEagerly();
		if (eager && !layer->getRecorder().record_raytracing_pipeline(
				VK_NULL_HANDLE, info, nullptr, 0, 0,
				device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to capture eagerly.\n");
		}

		// Have to create all pipelines here, in case the application makes use of basePipelineIndex.
		// Write arguments in TLS in-case we crash here.
		Instance::braceForRayTracingPipelineCrash(&layer->getRecorder(), &info);
		// FIXME: Can we meaningfully deal with deferredOperation here?
		auto res = layer->getTable()->CreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, 1, &info,
		                                                           pAllocator, &pPipelines[i]);
		Instance::completedPipelineCompilation();

		// Record failing pipelines for repro.
		if (!layer->getRecorder().record_raytracing_pipeline(
				res == VK_SUCCESS ? pPipelines[i] : VK_NULL_HANDLE, info, nullptr, 0, 0,
				device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
		{
			LOGW_LEVEL("Failed to record compute pipeline, usually caused by unsupported pNext.\n");
		}

		if (res == VK_PIPELINE_COMPILE_REQUIRED_EXT)
			final_res = res;

		if (res < 0)
		{
			for (uint32_t j = 0; j < i; j++)
				layer->getTable()->DestroyPipeline(device, pPipelines[j], pAllocator);
			return res;
		}
		else if (res == VK_PIPELINE_COMPILE_REQUIRED_EXT &&
		         (info.flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT) != 0)
		{
			break;
		}
	}

	return final_res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(
		VkDevice device, VkDeferredOperationKHR deferredOperation,
		VkPipelineCache pipelineCache, uint32_t createInfoCount,
		const VkRayTracingPipelineCreateInfoKHR *pCreateInfos,
		const VkAllocationCallbacks *pAllocator,
		VkPipeline *pPipelines)
{
	auto *layer = get_device_layer(device);

	if (layer->getInstance()->capturesParanoid())
	{
		return CreateRayTracingPipelinesParanoid(layer, device, deferredOperation, pipelineCache,
		                                         createInfoCount, pCreateInfos, pAllocator, pPipelines);
	}
	else
	{
		return CreateRayTracingPipelinesNormal(layer, device, deferredOperation, pipelineCache,
		                                       createInfoCount, pCreateInfos, pAllocator, pPipelines);
	}
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
		if (!layer->getRecorder().record_pipeline_layout(*pLayout, *pCreateInfo))
			LOGW_LEVEL("Failed to record pipeline layout, usually caused by unsupported pNext.\n");
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

	// No point in recording a host only layout since we will never be able to use it in a pipeline layout.
	if (result == VK_SUCCESS && (pCreateInfo->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_HOST_ONLY_POOL_BIT_EXT) == 0)
	{
		if (!layer->getRecorder().record_descriptor_set_layout(*pSetLayout, *pCreateInfo))
			LOGW_LEVEL("Failed to record descriptor set layout, usually caused by unsupported pNext.\n");
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
		{ "vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_fossilize_GetInstanceProcAddr) },
		{ "vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice) },
	};

	for (auto &cmd : coreInstanceCommands)
		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;
	return nullptr;
}

static PFN_vkVoidFunction interceptInstanceCommand(const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
	} instanceCommands[] = {
		{ "vkGetPhysicalDeviceProperties2", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2) },
		{ "vkGetPhysicalDeviceProperties2KHR", reinterpret_cast<PFN_vkVoidFunction>(GetPhysicalDeviceProperties2KHR) },
	};

	for (auto &cmd : instanceCommands)
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
		if (!layer->getRecorder().record_sampler(*pSampler, *pCreateInfo))
			LOGW_LEVEL("Failed to record sampler, usually caused by unsupported pNext.\n");
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
		// Pass along the module identifier for later reference.
		VkPipelineShaderStageModuleIdentifierCreateInfoEXT identifierCreateInfo =
				{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT };
		VkShaderModuleIdentifierEXT identifier =
				{ VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT };
		VkShaderModuleCreateInfo tmpCreateInfo;

		if (layer->requiresModuleIdentifiers())
		{
			layer->getTable()->GetShaderModuleIdentifierEXT(device, *pShaderModule, &identifier);
			identifierCreateInfo.pIdentifier = identifier.identifier;
			identifierCreateInfo.identifierSize = identifier.identifierSize;
			tmpCreateInfo = *pCreateInfo;
			identifierCreateInfo.pNext = tmpCreateInfo.pNext;
			tmpCreateInfo.pNext = &identifierCreateInfo;
			pCreateInfo = &tmpCreateInfo;
		}

		if (!layer->getRecorder().record_shader_module(*pShaderModule, *pCreateInfo))
			LOGW_LEVEL("Failed to record shader module, usually caused by unsupported pNext.\n");
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
		if (!layer->getRecorder().record_render_pass(*pRenderPass, *pCreateInfo))
			LOGW_LEVEL("Failed to record render pass, usually caused by unsupported pNext.\n");
	}
	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo,
                                                        const VkAllocationCallbacks *pCallbacks, VkRenderPass *pRenderPass)
{
	auto *layer = get_device_layer(device);

	// Split calls since 2 and KHR variants might not be present even if the other one is.
	auto res = layer->getTable()->CreateRenderPass2(device, pCreateInfo, pCallbacks, pRenderPass);

	if (res == VK_SUCCESS)
	{
		if (!layer->getRecorder().record_render_pass2(*pRenderPass, *pCreateInfo))
			LOGW_LEVEL("Failed to record render pass, usually caused by unsupported pNext.\n");
	}
	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2 *pCreateInfo,
                                                           const VkAllocationCallbacks *pCallbacks, VkRenderPass *pRenderPass)
{
	auto *layer = get_device_layer(device);

	// Split calls since 2 and KHR variants might not be present even if the other one is.
	auto res = layer->getTable()->CreateRenderPass2KHR(device, pCreateInfo, pCallbacks, pRenderPass);

	if (res == VK_SUCCESS)
	{
		if (!layer->getRecorder().record_render_pass2(*pRenderPass, *pCreateInfo))
			LOGW_LEVEL("Failed to record render pass, usually caused by unsupported pNext.\n");
	}
	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSamplerYcbcrConversion(
		VkDevice device, const VkSamplerYcbcrConversionCreateInfo *pCreateInfo,
		const VkAllocationCallbacks *pCallbacks,
		VkSamplerYcbcrConversion *pConversion)
{
	auto *layer = get_device_layer(device);

	auto res = layer->getTable()->CreateSamplerYcbcrConversion(device, pCreateInfo, pCallbacks, pConversion);

	if (res == VK_SUCCESS)
	{
		if (!layer->getRecorder().record_ycbcr_conversion(*pConversion, *pCreateInfo))
			LOGW_LEVEL("Failed to record YCbCr conversion, usually caused by unsupported pNext.\n");
	}

	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateSamplerYcbcrConversionKHR(
		VkDevice device, const VkSamplerYcbcrConversionCreateInfo *pCreateInfo,
		const VkAllocationCallbacks *pCallbacks,
		VkSamplerYcbcrConversion *pConversion)
{
	auto *layer = get_device_layer(device);

	auto res = layer->getTable()->CreateSamplerYcbcrConversionKHR(device, pCreateInfo, pCallbacks, pConversion);

	if (res == VK_SUCCESS)
	{
		if (!layer->getRecorder().record_ycbcr_conversion(*pConversion, *pCreateInfo))
			LOGW_LEVEL("Failed to record YCbCr conversion, usually caused by unsupported pNext.\n");
	}

	return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineBinariesKHR(
		VkDevice                                    device,
		const VkPipelineBinaryCreateInfoKHR*        pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkPipelineBinaryHandlesInfoKHR*             pBinaries)
{
	auto *layer = get_device_layer(device);
	auto res = layer->getTable()->CreatePipelineBinariesKHR(device, pCreateInfo, pAllocator, pBinaries);

	if (res == VK_SUCCESS && pCreateInfo->pPipelineCreateInfo)
	{
		// If we successfully created binaries from a pipeline create info (internal cache),
		// this should be seen as compiling the pipeline early.
		// Later, we will see a pipeline compile with binary, which will not be possible to record.

		if (pCreateInfo->pPipelineCreateInfo->pNext)
		{
			const void *pnext = pCreateInfo->pPipelineCreateInfo->pNext;
			auto *create_info = static_cast<const VkBaseInStructure *>(pnext);

			switch (create_info->sType)
			{
			case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:
				// We don't have a concrete VkPipeline.
				if (!layer->getRecorder().record_graphics_pipeline(
						VK_NULL_HANDLE, *static_cast<const VkGraphicsPipelineCreateInfo *>(pnext), nullptr, 0, 0,
						device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
					LOGW_LEVEL("Recording graphics pipeline failed, usually caused by unsupported pNext.\n");
				break;

			case VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:
				if (!layer->getRecorder().record_compute_pipeline(
						VK_NULL_HANDLE, *static_cast<const VkComputePipelineCreateInfo *>(pnext), nullptr, 0, 0,
						device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
					LOGW_LEVEL("Recording compute pipeline failed, usually caused by unsupported pNext.\n");
				break;

			case VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR:
				if (!layer->getRecorder().record_raytracing_pipeline(
						VK_NULL_HANDLE, *static_cast<const VkRayTracingPipelineCreateInfoKHR *>(pnext), nullptr, 0, 0,
						device, layer->requiresModuleIdentifiers() ? layer->getTable()->GetShaderModuleCreateInfoIdentifierEXT : nullptr))
					LOGW_LEVEL("Recording ray tracing pipeline failed, usually caused by unsupported pNext.\n");
				break;

			default:
				break;
			}
		}
	}

	return res;
}

static PFN_vkVoidFunction interceptDeviceCommand(Instance *instance, const char *pName)
{
	static const struct
	{
		const char *name;
		PFN_vkVoidFunction proc;
		bool is_sampler;
	} coreDeviceCommands[] = {
		{ "vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(VK_LAYER_fossilize_GetDeviceProcAddr) },
		{ "vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice) },

		{ "vkCreateDescriptorSetLayout", reinterpret_cast<PFN_vkVoidFunction>(CreateDescriptorSetLayout) },
		{ "vkCreatePipelineLayout", reinterpret_cast<PFN_vkVoidFunction>(CreatePipelineLayout) },
		{ "vkCreateGraphicsPipelines", reinterpret_cast<PFN_vkVoidFunction>(CreateGraphicsPipelines) },
		{ "vkCreateComputePipelines", reinterpret_cast<PFN_vkVoidFunction>(CreateComputePipelines) },
		{ "vkCreateSampler", reinterpret_cast<PFN_vkVoidFunction>(CreateSampler), true },
		{ "vkCreateShaderModule", reinterpret_cast<PFN_vkVoidFunction>(CreateShaderModule) },
		{ "vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass) },
		{ "vkCreateRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2) },
		{ "vkCreateRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2KHR) },
		{ "vkCreateSamplerYcbcrConversion", reinterpret_cast<PFN_vkVoidFunction>(CreateSamplerYcbcrConversion), true },
		{ "vkCreateSamplerYcbcrConversionKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateSamplerYcbcrConversionKHR), true },
		{ "vkCreateRayTracingPipelinesKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateRayTracingPipelinesKHR) },
		{ "vkCreatePipelineBinariesKHR", reinterpret_cast<PFN_vkVoidFunction>(CreatePipelineBinariesKHR) },
	};

	for (auto &cmd : coreDeviceCommands)
	{
		if (cmd.is_sampler && !instance->recordsImmutableSamplers())
			continue;

		if (strcmp(cmd.name, pName) == 0)
			return cmd.proc;
	}

	return nullptr;
}
}

using namespace Fossilize;

extern "C"
{
VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_fossilize_GetDeviceProcAddr(VkDevice device, const char *pName)
{
	Device *layer;
	{
		lock_guard<mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(device), deviceData);
	}

	auto proc = layer->getTable()->GetDeviceProcAddr(device, pName);

	// If the underlying implementation returns nullptr, we also need to return nullptr.
	// This means we never expose wrappers which will end up dispatching into nullptr.
	if (proc)
	{
		auto wrapped_proc = interceptDeviceCommand(layer->getInstance(), pName);
		if (wrapped_proc)
			proc = wrapped_proc;
	}

	return proc;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL VK_LAYER_fossilize_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
	if (!pName)
		return nullptr;

	// We only wrap core Vulkan 1.0 instance commands, no need to check for availability of underlying implementation.
	auto proc = interceptCoreInstanceCommand(pName);
	if (proc)
		return proc;

	// For global instance functions, the assumption is that we cannot call down the chain.
	if (!instance)
		return nullptr;

	Instance *layer;
	{
		lock_guard<mutex> holder{globalLock};
		layer = getLayerData(getDispatchKey(instance), instanceData);
	}

	proc = layer->getProcAddr(pName);

	if (proc)
	{
		auto wrapped_proc = interceptInstanceCommand(pName);
		if (wrapped_proc)
			return wrapped_proc;
	}

	// If the underlying implementation returns nullptr, we also need to return nullptr.
	// This means we never expose wrappers which will end up dispatching into nullptr.
	if (proc)
	{
		auto wrapped_proc = interceptDeviceCommand(layer, pName);
		if (wrapped_proc)
			return wrapped_proc;
	}

	return proc;
}

#ifdef ANDROID
static const VkLayerProperties layerProps[] = {
	{ VK_LAYER_fossilize, VK_MAKE_VERSION(1, 3, 136), 1, "Fossilize capture layer" },
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
#endif
}
