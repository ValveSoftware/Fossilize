/* Copyright (c) 2026 Hans-Kristian Arntzen for Valve Corporation
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

#include "volk.h"
#include "fossilize_feature_sifter.hpp"
#include "fossilize_sifter_utils.hpp"

#include <algorithm>
#include <vector>
#include "cli_parser.hpp"
#include "logging.hpp"
#include <string.h>

using namespace Fossilize;

static void print_help()
{
	LOGI("fossilize-feature-sifter\n"
		"\t[--help]\n"
		"\t[--block-extension <ext>]\n");
}

struct Instance
{
	VkInstance instance;
	std::vector<VkExtensionProperties> props;
	std::vector<const char *> extensions;
};

static bool create_instance(Instance &instance)
{
	if (volkInitialize() != VK_SUCCESS)
		return false;

	uint32_t instance_version;
	if (vkEnumerateInstanceVersion(&instance_version) != VK_SUCCESS || instance_version < VK_API_VERSION_1_1)
		return false;

	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };

	const VkApplicationInfo app_info = {
		VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
		"fossilize-sifter", 1,
		"fossilize-cli", 1,
		instance_version,
	};

	instance_info.pApplicationInfo = &app_info;

	uint32_t count;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	instance.props.resize(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, instance.props.data());
	instance.extensions.reserve(count);
	for (auto &prop : instance.props)
		instance.extensions.push_back(prop.extensionName);

	instance_info.enabledExtensionCount = count;
	instance_info.ppEnabledExtensionNames = instance.extensions.data();

	if (vkCreateInstance(&instance_info, nullptr, &instance.instance) != VK_SUCCESS)
	{
		LOGE("Failed to create instance.\n");
		return false;
	}

	volkLoadInstance(instance.instance);
	return true;
}

static void pipeline_binary_key_to_str(char (&str)[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR * 2 + 1], const VkPipelineBinaryKeyKHR &key)
{
	str[0] = '\0';
	for (uint32_t i = 0; i < key.keySize; i++)
		sprintf(str + 2 * i, "%02x", key.key[i]);
}

static void print_pipeline_binary_key(const char *tag, const VkPipelineBinaryKeyKHR &key)
{
	char str[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR * 2 + 1];
	pipeline_binary_key_to_str(str, key);
	LOGI("Pipeline key for \"%s\": %s\n", tag, str);
}

static bool compare_pipeline_key(const VkPipelineBinaryKeyKHR &a, const VkPipelineBinaryKeyKHR &b)
{
	return a.keySize == b.keySize && memcmp(a.key, b.key, a.keySize) == 0;
}

struct CacheResults
{
	std::vector<uint8_t> cache;
	bool valid = false;
};

static bool get_pipeline_key(VkPhysicalDevice gpu, const std::vector<const char *> &extensions,
                             const VkPhysicalDeviceFeatures2 &features2_base,
                             VkPipelineBinaryKeyKHR &key, CacheResults &cache)
{
	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkDeviceQueueCreateInfo queue_create_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	const float queue_prio = 1.0f;
	queue_create_info.queueCount = 1;
	// Don't care what this queue does.
	queue_create_info.queueFamilyIndex = 0;
	queue_create_info.pQueuePriorities = &queue_prio;
	device_info.pQueueCreateInfos = &queue_create_info;
	device_info.queueCreateInfoCount = 1;

	VkPhysicalDevicePipelineBinaryFeaturesKHR binary = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, nullptr, VK_TRUE
	};

	auto features2 = features2_base;
	device_info.pNext = &features2;

	auto tmp_extensions = extensions;
	tmp_extensions.push_back(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME);
	tmp_extensions.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);

	binary.pNext = features2.pNext;
	features2.pNext = &binary;
	device_info.ppEnabledExtensionNames = tmp_extensions.data();
	device_info.enabledExtensionCount = uint32_t(tmp_extensions.size());

	VkDevice device;
	auto vr = vkCreateDevice(gpu, &device_info, nullptr, &device);
	if (vr != VK_SUCCESS)
	{
		LOGE("Failed to create device, vr %d.\n", vr);
		return false;
	}

	if (vkGetPipelineKeyKHR(device, nullptr, &key) != VK_SUCCESS)
	{
		LOGE("Failed to get the global pipeline binary key.\n");
		vkDestroyDevice(device, nullptr);
		return false;
	}

	VkPipelineCache pipeline_cache = create_pipeline_cache(device, cache.cache.data(), cache.cache.size());
	if (pipeline_cache == VK_NULL_HANDLE)
	{
		LOGE("Failed to create pipeline cache.\n");
		vkDestroyDevice(device, nullptr);
		return false;
	}

	cache.valid = true;
	if (!create_graphics_pipeline(device, pipeline_cache, nullptr, !cache.cache.empty()))
		cache.valid = false;
	if (!create_compute_pipeline(device, pipeline_cache, nullptr, !cache.cache.empty()))
		cache.valid = false;

	if (cache.cache.empty())
		cache.cache = serialize_pipeline_cache_to_data(device, pipeline_cache);

	vkDestroyPipelineCache(device, pipeline_cache, nullptr);
	vkDestroyDevice(device, nullptr);
	return true;
}

int main(int argc, char **argv)
{
	std::vector<std::string> ban_list;
	CLICallbacks cbs;

	cbs.add("--help", [&](CLIParser &parser) { parser.end(); });
	cbs.add("--block-extension", [&](CLIParser &parser) { ban_list.emplace_back(parser.next_string()); });

	CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
	{
		print_help();
		return EXIT_FAILURE;
	}
	else if (parser.is_ended_state())
	{
		print_help();
		return EXIT_SUCCESS;
	}

	Instance instance;
	if (!create_instance(instance))
		return EXIT_FAILURE;

	VkPhysicalDevice gpu;
	uint32_t count = 1;
	if (vkEnumeratePhysicalDevices(instance.instance, &count, &gpu) < 0)
	{
		LOGE("Failed to enumerate GPU.\n");
		return EXIT_FAILURE;
	}

	VkPhysicalDeviceFeatures features = {};
	vkGetPhysicalDeviceFeatures(gpu, &features);

	VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPipelineBinaryKeyKHR baseline_key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
	VkPipelineBinaryKeyKHR full_feature_key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };

	CacheResults baseline_cache = {};
	CacheResults full_feature_cache = {};

	if (!get_pipeline_key(gpu, {}, features2, baseline_key, baseline_cache))
	{
		LOGE("Failed to get pipeline key.\n");
		return EXIT_FAILURE;
	}

	print_pipeline_binary_key("Baseline", baseline_key);

	features2.features = features;
	if (!get_pipeline_key(gpu, {}, features2, full_feature_key, full_feature_cache))
	{
		LOGE("Failed to get pipeline key.\n");
		return EXIT_FAILURE;
	}
	print_pipeline_binary_key("Full 1.0 feature key", full_feature_key);

	// First, sift through all base features.
	for (size_t i = 0; i < sizeof(features) / sizeof(VkBool32); i++)
	{
		if (reinterpret_cast<VkBool32 *>(&features)[i])
		{
			features2.features = {};
			reinterpret_cast<VkBool32 *>(&features2.features)[i] = VK_TRUE;

			VkPipelineBinaryKeyKHR key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
			if (!get_pipeline_key(gpu, {}, features2, key, baseline_cache))
			{
				LOGE("Failed to get pipeline key.\n");
				return EXIT_FAILURE;
			}

			if (!compare_pipeline_key(baseline_key, key))
			{
				LOGI("Found key delta for VkPhysicalDeviceFeature feature number %zu ...\n", i);
				print_pipeline_binary_key("Delta", key);
			}

			if (!baseline_cache.valid)
				LOGE("Found cache miss for VkPhysicalDeviceFeature feature number %zu ...\n", i);
		}
	}

	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> extensions(count);
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions.data());

	std::vector<const char *> enabled_extensions;
	ExistingFeatureStructs feature_structs;

	const auto supports_device_extension = [&](const char *e)
	{
		return std::find_if(extensions.begin(), extensions.end(),
			[&](const VkExtensionProperties &prop) { return strcmp(prop.extensionName, e) == 0; }) != extensions.end();
	};

	const auto has_banned_extension = [&](const std::vector<const char *> &candidates)
	{
		for (auto *candidate: candidates)
		{
			if (std::find_if(ban_list.begin(), ban_list.end(), [&](const std::string &banned) {
				return banned == candidate; }) != ban_list.end())
			{
				return true;
			}
		}
		return false;
	};

	for (auto &ext : extensions)
	{
		if (strcmp(ext.extensionName, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) == 0)
			continue;
		if (strcmp(ext.extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME) == 0)
			continue;

		feature_structs.reset_pnext();
		enabled_extensions.clear();

		features2.pNext = nullptr;
		enabled_extensions.push_back(ext.extensionName);

		ExtensionRequirements reqs = {};
		if (!feature_structs.get_extension_requirements(ext.extensionName, reqs))
		{
			LOGE("Extension %s does not have extension requirements in LUT. Skipping.\n", ext.extensionName);
			continue;
		}

		features2.pNext = reqs.primary_feature;
		if (reqs.secondary_feature)
		{
			static_cast<VkBaseOutStructure *>(reqs.primary_feature)->pNext =
				static_cast<VkBaseOutStructure *>(reqs.secondary_feature);
		}

		const auto add_unique_device_ext = [&](const char *candidate) -> bool
		{
			auto itr = std::find_if(
					instance.extensions.begin(), instance.extensions.end(),
					[&](const char *instance_ext) { return strcmp(instance_ext, candidate) == 0; });

			if (itr != instance.extensions.end())
				return false;

			for (auto *enabled : enabled_extensions)
				if (strcmp(enabled, candidate) == 0)
					return false;
			enabled_extensions.push_back(candidate);
			return true;
		};

		std::vector<const char *> current_exts;
		bool new_extensions;

		do
		{
			new_extensions = false;
			current_exts = enabled_extensions;
			for (auto *current_ext : current_exts)
				if (feature_structs.get_extension_requirements(current_ext, reqs))
					for (uint32_t i = 0; i < reqs.num_extension_dependencies; i++)
						if (add_unique_device_ext(reqs.extension_dependencies[i]))
							new_extensions = true;
		} while(new_extensions);

		// The dependency list might contain extensions that aren't actually supported if they are promoted exts to core.
		// Filter them out.
		auto itr = std::remove_if(enabled_extensions.begin(), enabled_extensions.end(),
			[&](const char *candidate)
			{
				return !supports_device_extension(candidate);
			});
		enabled_extensions.erase(itr, enabled_extensions.end());

		if (has_banned_extension(enabled_extensions))
		{
			LOGI("Skipping extension %s due to banned extensions.\n", ext.extensionName);
			continue;
		}

		// Make sure dependent features also get enabled.
		if (strcmp(ext.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0)
		{
			feature_structs.mFragmentShadingRateFeaturesKHR.pNext = features2.pNext;
			features2.pNext = &feature_structs.mFragmentShadingRateFeaturesKHR;
			feature_structs.mMultiviewFeatures.pNext = features2.pNext;
			features2.pNext = &feature_structs.mMultiviewFeatures;
		}

		vkGetPhysicalDeviceFeatures2(gpu, &features2);

		VkPipelineBinaryKeyKHR key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
		if (!get_pipeline_key(gpu, enabled_extensions, features2, key, full_feature_cache))
		{
			LOGE("Failed to get pipeline key for extension %s.\n", ext.extensionName);
			continue;
		}

		if (!full_feature_cache.valid)
			LOGE("Pipeline cache roundtrip failed for extension %s.\n", ext.extensionName);

		if (!compare_pipeline_key(full_feature_key, key))
			print_pipeline_binary_key(ext.extensionName, key);
	}

	vkDestroyInstance(instance.instance, nullptr);
}