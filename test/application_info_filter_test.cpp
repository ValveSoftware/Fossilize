/* Copyright (c) 2019 Hans-Kristian Arntzen
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

#include "fossilize_application_filter.hpp"
#include "layer/utils.hpp"
#include "vulkan/vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>

static bool write_string_to_file(const char *path, const char *str)
{
	FILE *file = fopen(path, "w");
	if (!file)
		return false;

	fprintf(file, "%s\n", str);
	fclose(file);
	return true;
}

int main()
{
	const char *test_json =
R"delim(
{
	"asset": "FossilizeApplicationInfoFilter",
	"version" : 2,
	"blacklistedApplicationNames" : [ "A",  "B", "C" ],
	"blacklistedEngineNames" : [ "D", "E", "F" ],
	"applicationFilters" : {
		"test1" : { "minimumApplicationVersion" : 10 },
		"test2" : { "minimumApplicationVersion" : 10, "minimumEngineVersion" : 1000 },
		"test3" : { "minimumApiVersion" : 50 },
		"test4" : {
			"blacklistedEnvironments" : {
				"TEST_ENV" : { "contains" : "foo", "equals" : "bar" },
				"TEST_ENV" : { "equals" : "bar2", "contains": "" },
				"TEST" : { "nonnull" : true }
			}
		},
		"test5" : { "recordImmutableSamplers" : true },
		"test6" : { "recordImmutableSamplers" : false }
	},
	"engineFilters" : {
		"test1" : {
			"minimumEngineVersion" : 10,
			"bucketVariantDependencies" : [
				"BindlessUBO",
				"VendorID",
				"MutableDescriptorType",
				"BufferDeviceAddress",
				"DummyIgnored",
				"ApplicationName",
				"FragmentShadingRate",
				"DynamicRendering"
			]
		},
		"variance" : {
			"bucketVariantDependencies" : [ "VendorID" ]
		},
		"variance2" : {
			"bucketVariantDependencies" : [ "VendorID" ],
			"bucketVariantFeatureDependencies" : [
				"BindlessUBO",
				"MutableDescriptorType",
				"BufferDeviceAddress",
				"DummyIgnored",
				"FragmentShadingRate",
				"DynamicRendering",
				"DescriptorBuffer"
			]
		},
		"test2" : { "minimumEngineVersion" : 10, "minimumApplicationVersion" : 1000 },
		"test3" : { "minimumApiVersion" : 50 },
		"test4" : {
			"blacklistedEnvironments" : {
				"TEST_ENV" : { "contains" : "foo", "equals" : "bar" },
				"TEST_ENV" : { "equals" : "bar2", "contains": "" },
				"TEST" : { "nonnull" : true }
			}
		},
		"test5" : { "recordImmutableSamplers" : false },
		"test6" : { "recordImmutableSamplers" : true }
	},
	"defaultBucketVariantDependencies" : [
		"ApplicationName",
		"EngineName"
	],
	"defaultBucketVariantFeatureDependencies" : [
		"DescriptorBuffer"
	]
}
)delim";

	if (!write_string_to_file(".__test_appinfo.json", test_json))
		return EXIT_FAILURE;

	struct UserData
	{
		const char *env = nullptr;
		const char *data = nullptr;
	} data = {};

	const auto getenv_wrapper = +[](const char *env, void *userdata) -> const char *
	{
		auto *env_data = static_cast<const UserData *>(userdata);
		if (env_data->env && strcmp(env_data->env, env) == 0)
			return env_data->data;
		else
			return nullptr;
	};

	std::unique_ptr<Fossilize::ApplicationInfoFilter> filter_handle;
	filter_handle.reset(Fossilize::ApplicationInfoFilter::parse(".__test_appinfo.json",
																getenv_wrapper, &data));

	if (!filter_handle)
	{
		LOGE("Parsing did not complete successfully.\n");
		return EXIT_FAILURE;
	}

	auto &filter = *filter_handle;

	VkApplicationInfo appinfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };

	if (!filter.test_application_info(nullptr))
		return EXIT_FAILURE;

	// Test blacklists
	appinfo.pApplicationName = "A";
	appinfo.pEngineName = "G";
	if (filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	appinfo.pApplicationName = "D";
	appinfo.pEngineName = "A";
	if (!filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	appinfo.pApplicationName = "H";
	appinfo.pEngineName = "E";
	if (filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	// Test application version filtering
	appinfo.pApplicationName = "test1";
	appinfo.pEngineName = nullptr;
	appinfo.applicationVersion = 9;
	if (filter.test_application_info(&appinfo))
		return EXIT_FAILURE;
	appinfo.applicationVersion = 10;
	if (!filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	// Engine version should be ignored for appinfo filters.
	appinfo.pApplicationName = "test2";
	if (!filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	appinfo.pApplicationName = "test3";
	appinfo.applicationVersion = 0;
	appinfo.apiVersion = 49;
	if (filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	appinfo.apiVersion = 50;
	if (!filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	// Test engine version filtering
	appinfo.pApplicationName = nullptr;
	appinfo.pEngineName = "test1";
	appinfo.engineVersion = 9;
	if (filter.test_application_info(&appinfo))
		return EXIT_FAILURE;
	appinfo.engineVersion = 10;
	if (!filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	// Engine version should be ignored for appinfo filters.
	appinfo.pEngineName = "test2";
	if (!filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	appinfo.pEngineName = "test3";
	appinfo.engineVersion = 0;
	appinfo.apiVersion = 49;
	if (filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	appinfo.apiVersion = 50;
	if (!filter.test_application_info(&appinfo))
		return EXIT_FAILURE;

	appinfo.engineVersion = 0;
	appinfo.applicationVersion = 0;

	for (unsigned i = 0; i < 2; i++)
	{
		data = {};

		// Test env blacklisting (application)
		if (i == 0)
		{
			appinfo.pApplicationName = "test4";
			appinfo.pEngineName = nullptr;
		}
		else
		{
			appinfo.pEngineName = "test4";
			appinfo.pApplicationName = nullptr;
		}

		if (!filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		data.env = "TEST_FOO";
		data.data = "foo";

		if (!filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		// Contains test, should fail
		data.env = "TEST_ENV";

		data.data = "foo";
		if (filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		data.data = "Afoo";
		if (filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		data.data = "fooA";
		if (filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		// Equals bar, should fail
		data.data = "bar";
		if (filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		// Equals bar2, should fail
		data.data = "bar2";
		if (filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		// Should not fail
		data.data = "bar3";
		if (!filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		// Should not fail
		data.env = "TEST";
		data.data = nullptr;
		if (!filter.test_application_info(&appinfo))
			return EXIT_FAILURE;

		// Should fail
		data.data = "";
		if (filter.test_application_info(&appinfo))
			return EXIT_FAILURE;
	}

	// Test bucket variant filter.
	appinfo.pEngineName = nullptr;
	appinfo.pApplicationName = "test1";
	if (!filter.needs_buckets(&appinfo))
		return EXIT_FAILURE;

	appinfo.pEngineName = "test1";
	appinfo.pApplicationName = nullptr;
	if (!filter.needs_buckets(&appinfo))
		return EXIT_FAILURE;

	{
		// Make sure this doesn't crash.
		filter.get_bucket_hash(nullptr, nullptr, nullptr);

		auto hash0 = filter.get_bucket_hash(nullptr, &appinfo, nullptr);
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		props2.properties.vendorID = 1;
		auto hash1 = filter.get_bucket_hash(&props2, &appinfo, nullptr);
		if (hash0 == hash1)
			return EXIT_FAILURE;

		VkPhysicalDeviceBufferDeviceAddressFeatures bda_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
		VkPhysicalDeviceVulkan12Features vulkan12_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan13Features vulkan13_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceDescriptorIndexingFeatures indexing_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
		VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
		VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
		VkPhysicalDeviceDynamicRenderingFeatures dynamic_rendering_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };

		bda_features.pNext = &indexing_features;
		indexing_features.pNext = &mutable_features;
		VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		features2.pNext = &bda_features;

		auto hash2 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash1 != hash2)
			return EXIT_FAILURE;

		// Try to enable one feature at a time and verify it's the same.
		bda_features.bufferDeviceAddress = VK_TRUE;
		auto hash3 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash2 == hash3)
			return EXIT_FAILURE;

		indexing_features.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
		auto hash4 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash3 == hash4)
			return EXIT_FAILURE;

		mutable_features.mutableDescriptorType = VK_TRUE;
		auto hash5 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash4 == hash5)
			return EXIT_FAILURE;

		// Verify that the 1.2 structs can also be used.
		mutable_features.pNext = &vulkan12_features;
		features2.pNext = &mutable_features;
		vulkan12_features.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
		vulkan12_features.bufferDeviceAddress = VK_TRUE;
		auto hash6 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash5 != hash6)
			return EXIT_FAILURE;

		features2.pNext = nullptr;
		auto hash7 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		features2.pNext = &vrs_features;
		auto hash8 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash7 != hash8)
			return EXIT_FAILURE;
		vrs_features.primitiveFragmentShadingRate = VK_TRUE;
		auto hash9 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash8 == hash9)
			return EXIT_FAILURE;
		vrs_features.primitiveFragmentShadingRate = VK_FALSE;
		vrs_features.pipelineFragmentShadingRate = VK_TRUE;
		auto hash10 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash9 == hash10)
			return EXIT_FAILURE;
		vrs_features.pipelineFragmentShadingRate = VK_FALSE;
		vrs_features.attachmentFragmentShadingRate = VK_TRUE;
		auto hash11 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash10 == hash11)
			return EXIT_FAILURE;

		features2.pNext = nullptr;
		auto hash12 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		features2.pNext = &dynamic_rendering_features;
		auto hash13 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash12 != hash13)
			return EXIT_FAILURE;
		dynamic_rendering_features.dynamicRendering = VK_TRUE;
		auto hash14 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash13 == hash14)
			return EXIT_FAILURE;
		vulkan13_features.dynamicRendering = VK_TRUE;
		features2.pNext = &vulkan13_features;
		auto hash15 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash14 != hash15)
			return EXIT_FAILURE;

		// Spot check for ApplicationName.
		appinfo.pApplicationName = "foo";
		auto hash16 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash16 == hash15)
			return EXIT_FAILURE;

		// Check that the default variant hash is used.
		appinfo.pApplicationName = "blah";
		appinfo.pEngineName = "blah2";
		auto hash17 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		auto hash18 = filter.get_bucket_hash(&props2, &appinfo, nullptr);
		if (hash17 != hash18)
			return EXIT_FAILURE;
	}

	// Test that feature hashing works as intended.
	{
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		VkPhysicalDeviceBufferDeviceAddressFeatures bda_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
		VkPhysicalDeviceVulkan12Features vulkan12_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan13Features vulkan13_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceDescriptorIndexingFeatures indexing_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
		VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
		VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
		VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };

		props2.properties.vendorID = 1;
		appinfo.pEngineName = "variance";
		appinfo.pApplicationName = nullptr;

		auto hash0 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		appinfo.pEngineName = "variance2";
		auto hash1 = filter.get_bucket_hash(&props2, &appinfo, &features2);

		if (hash0 != hash1)
			return EXIT_FAILURE;

		// Ensure that hashing disabled structs does not change anything either.
		features2.pNext = &descriptor_buffer_features;
		if (filter.get_bucket_hash(&props2, &appinfo, &features2) != hash1)
			return EXIT_FAILURE;

		features2.pNext = &bda_features;
		if (filter.get_bucket_hash(&props2, &appinfo, &features2) != hash1)
			return EXIT_FAILURE;

		features2.pNext = &vulkan12_features;
		if (filter.get_bucket_hash(&props2, &appinfo, &features2) != hash1)
			return EXIT_FAILURE;

		features2.pNext = &vulkan13_features;
		if (filter.get_bucket_hash(&props2, &appinfo, &features2) != hash1)
			return EXIT_FAILURE;

		features2.pNext = &indexing_features;
		if (filter.get_bucket_hash(&props2, &appinfo, &features2) != hash1)
			return EXIT_FAILURE;

		features2.pNext = &mutable_features;
		if (filter.get_bucket_hash(&props2, &appinfo, &features2) != hash1)
			return EXIT_FAILURE;

		features2.pNext = &vrs_features;
		if (filter.get_bucket_hash(&props2, &appinfo, &features2) != hash1)
			return EXIT_FAILURE;

		features2.pNext = &descriptor_buffer_features;
		descriptor_buffer_features.descriptorBuffer = VK_TRUE;
		auto hash2 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		descriptor_buffer_features.descriptorBufferPushDescriptors = VK_TRUE;
		auto hash3 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash1 == hash2 || hash2 == hash3)
			return EXIT_FAILURE;

		features2.pNext = &bda_features;
		bda_features.bufferDeviceAddress = VK_TRUE;
		hash2 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash1 == hash2)
			return EXIT_FAILURE;
		features2.pNext = &vulkan12_features;
		vulkan12_features.bufferDeviceAddress = VK_TRUE;
		hash3 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		if (hash2 != hash3)
			return EXIT_FAILURE;
	}

	// Test that we can pick up defaultFeatureVariantFilter.
	{
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		VkPhysicalDeviceBufferDeviceAddressFeatures bda_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
		VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptor_buffer_features =
				{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };

		props2.properties.vendorID = 1;
		appinfo.pEngineName = nullptr;
		appinfo.pApplicationName = nullptr;

		auto hash0 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		features2.pNext = &bda_features;
		bda_features.bufferDeviceAddress = VK_TRUE;
		auto hash1 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		features2.pNext = &descriptor_buffer_features;
		auto hash2 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		descriptor_buffer_features.descriptorBuffer = VK_TRUE;
		auto hash3 = filter.get_bucket_hash(&props2, &appinfo, &features2);
		descriptor_buffer_features.descriptorBufferPushDescriptors = VK_TRUE;
		auto hash4 = filter.get_bucket_hash(&props2, &appinfo, &features2);

		if (hash0 != hash1 || hash1 != hash2)
			return EXIT_FAILURE;
		if (hash2 == hash3 || hash3 == hash4)
			return EXIT_FAILURE;
	}

	{
		appinfo.pApplicationName = "test5";
		appinfo.pEngineName = nullptr;
		if (!filter.should_record_immutable_samplers(&appinfo))
			return EXIT_FAILURE;
		appinfo.pApplicationName = "test6";
		if (filter.should_record_immutable_samplers(&appinfo))
			return EXIT_FAILURE;
		appinfo.pApplicationName = nullptr;
		appinfo.pEngineName = "test5";
		if (filter.should_record_immutable_samplers(&appinfo))
			return EXIT_FAILURE;
		appinfo.pEngineName = "test6";
		if (!filter.should_record_immutable_samplers(&appinfo))
			return EXIT_FAILURE;
	}

	remove(".__test_appinfo.json");
}
