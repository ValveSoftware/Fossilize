/* Copyright (c) 2021 Hans-Kristian Arntzen
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

#include <stdlib.h>
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <string.h>

// Crude hack, will probably just work on Linux,
// just for eye-balling that things work.
static void list_files(const char *tag)
{
	printf("\n======= %s ===========\n", tag);
	fflush(stdout);
	fflush(stderr);

	const char *env = getenv("FOSSILIZE_DUMP_PATH");
	if (!env)
		exit(EXIT_FAILURE);

	std::string cmd = "find ";
	cmd += env;
	cmd += "*";
	system(cmd.c_str());
	fflush(stdout);
	fflush(stderr);
	printf("=====================\n");
}

static void cleanup()
{
	const char *env = getenv("FOSSILIZE_DUMP_PATH");
	if (!env)
		exit(EXIT_FAILURE);

	std::string cmd = "rm -r ";
	cmd += env;
	cmd += "*";
	system(cmd.c_str());
}

static void run_app_info(const VkApplicationInfo *app_info, bool allow_bda = false)
{
	cleanup();

	VkInstanceCreateInfo instance_create_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_create_info.pApplicationInfo = app_info;

	VkInstance instance;
	if (vkCreateInstance(&instance_create_info, nullptr, &instance) != VK_SUCCESS)
		exit(EXIT_FAILURE);

	uint32_t gpu_count;
	vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
	if (gpu_count == 0)
		exit(EXIT_FAILURE);
	std::vector<VkPhysicalDevice> gpus(gpu_count);
	vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());

	const char *bda_ext = nullptr;
	if (allow_bda)
	{
		uint32_t ext_count = 0;
		vkEnumerateDeviceExtensionProperties(gpus.front(), nullptr, &ext_count, nullptr);
		std::vector<VkExtensionProperties> exts(ext_count);
		vkEnumerateDeviceExtensionProperties(gpus.front(), nullptr, &ext_count, exts.data());
		for (auto &ext : exts)
		{
			if (strcmp(ext.extensionName, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) == 0)
			{
				bda_ext = VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME;
				break;
			}
		}
	}

	VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPhysicalDeviceBufferDeviceAddressFeatures bda_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
	VkDeviceCreateInfo device_create_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	features2.pNext = &bda_features;
	if (bda_ext)
	{
		vkGetPhysicalDeviceFeatures2(gpus.front(), &features2);
		device_create_info.pNext = &features2;
		device_create_info.enabledExtensionCount = 1;
		device_create_info.ppEnabledExtensionNames = &bda_ext;
	}

	VkDevice device;
	if (vkCreateDevice(gpus.front(), &device_create_info, nullptr, &device) != VK_SUCCESS)
		exit(EXIT_FAILURE);

	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
}

static void record_data()
{
	VkApplicationInfo app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info.apiVersion = VK_API_VERSION_1_1;

	list_files("Initial");

	run_app_info(nullptr);
	list_files("Blank appinfo");

	run_app_info(&app_info);
	list_files("Blank names");

	// Test blacklists for engine and app names.
	app_info.pApplicationName = "A";
	run_app_info(&app_info);
	list_files("Ignore A");

	app_info.pApplicationName = "AA";
	run_app_info(&app_info);
	list_files("Don't ignore AA");

	app_info.pEngineName = "D";
	run_app_info(&app_info);
	list_files("Ignore D");

	app_info.pEngineName = "DD";
	run_app_info(&app_info);
	list_files("Don't ignore DD");

	// Test block env.
	app_info.pEngineName = "X";
	run_app_info(&app_info);
	list_files("X is blocked by BLOCK_ENV");

	// Test custom dependencies, depends on Major and Minor version only, not patch.
	app_info.pEngineName = "Y";
	app_info.engineVersion = VK_MAKE_VERSION(2, 1, 0);
	run_app_info(&app_info);
	list_files("Y engine version 2.1");

	app_info.pEngineName = "Y";
	app_info.engineVersion = VK_MAKE_VERSION(3, 2, 0);
	run_app_info(&app_info);
	list_files("Y engine version 3.2");

	app_info.pEngineName = "Y";
	app_info.engineVersion = VK_MAKE_VERSION(3, 2, 1);
	run_app_info(&app_info);
	list_files("Y engine version 3.2.1, same as 3.2");

	// Test that we can depend on vendor ID, and enabled feature state
	app_info.pEngineName = "Z";
	app_info.engineVersion = VK_MAKE_VERSION(3, 2, 1);
	run_app_info(&app_info);
	list_files("Z engine version 3.2.1, also depends on VendorID, BDA = off");

	// Test that we can depend on vendor ID, and enabled feature state
	app_info.pEngineName = "Z";
	app_info.engineVersion = VK_MAKE_VERSION(3, 2, 1);
	run_app_info(&app_info, true);
	list_files("Z engine version 3.2.1, also depends on VendorID, BDA = on");

	// Test different buckets for different engine/app combinations.
	app_info.pApplicationName = "default";
	app_info.pEngineName = "default";
	run_app_info(&app_info);
	list_files("default, default");

	app_info.pApplicationName = "default";
	app_info.pEngineName = nullptr;
	run_app_info(&app_info);
	list_files("default, NULL");

	app_info.pApplicationName = nullptr;
	app_info.pEngineName = "default";
	run_app_info(&app_info);
	list_files("NULL, default");
}

// Reference output (stdout only).
// This is just for sanity checking.
// TODO: Is there an easy way to automate this ...
#if 0
======= Initial ===========
=====================

======= Blank appinfo ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25077f9dcd57a7
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25077f9dcd57a7/4c8e285519af45f4.1.foz
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25077f9dcd57a7/TOUCH
=====================

======= Blank names ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25077f9dcd57a7
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25077f9dcd57a7/1555c5d3856de7f2.1.foz
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25077f9dcd57a7/TOUCH
=====================

======= Ignore A ===========
=====================

======= Do not ignore AA ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.15924cf8084faf71
/home/maister/git/Fossilize/cmake-build-debug/test-dump.15924cf8084faf71/TOUCH
/home/maister/git/Fossilize/cmake-build-debug/test-dump.15924cf8084faf71/8229507ec1d8bba6.1.foz
=====================

======= Ignore D ===========
=====================

======= Do not ignore DD ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.96d89db75c36bfe1
/home/maister/git/Fossilize/cmake-build-debug/test-dump.96d89db75c36bfe1/TOUCH
/home/maister/git/Fossilize/cmake-build-debug/test-dump.96d89db75c36bfe1/7175664a8dde98e0.1.foz
=====================

======= X is blocked by BLOCK_ENV ===========
=====================

======= Y engine version 2.1 ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25747f9dce108e
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25747f9dce108e/TOUCH
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25747f9dce108e/ddcbd730790c3fc7.1.foz
=====================

======= Y engine version 3.2 ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25737f9dce0ede
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25737f9dce0ede/22f489f5f55b3ab3.1.foz
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25737f9dce0ede/TOUCH
=====================

======= Y engine version 3.2.1, same as 3.2 ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25737f9dce0ede
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25737f9dce0ede/TOUCH
/home/maister/git/Fossilize/cmake-build-debug/test-dump.4d25737f9dce0ede/eb432fbcb89f7731.1.foz
=====================

======= Z engine version 3.2.1, also depends on VendorID, BDA = off ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.bdff55fa1b066828
/home/maister/git/Fossilize/cmake-build-debug/test-dump.bdff55fa1b066828/306ceabce294dba0.1.foz
/home/maister/git/Fossilize/cmake-build-debug/test-dump.bdff55fa1b066828/TOUCH
=====================

======= Z engine version 3.2.1, also depends on VendorID, BDA = on ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.bdff55fa1b066829
/home/maister/git/Fossilize/cmake-build-debug/test-dump.bdff55fa1b066829/306cebbce294d5d3.1.foz
/home/maister/git/Fossilize/cmake-build-debug/test-dump.bdff55fa1b066829/TOUCH
=====================

======= default, default ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.3fbe857283b87271
/home/maister/git/Fossilize/cmake-build-debug/test-dump.3fbe857283b87271/TOUCH
/home/maister/git/Fossilize/cmake-build-debug/test-dump.3fbe857283b87271/d1e39251c9fbf006.1.foz
=====================

======= default, NULL ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.9022e7589277e5ea
/home/maister/git/Fossilize/cmake-build-debug/test-dump.9022e7589277e5ea/44bf69d6def94e03.1.foz
/home/maister/git/Fossilize/cmake-build-debug/test-dump.9022e7589277e5ea/TOUCH
=====================

======= NULL, default ===========
/home/maister/git/Fossilize/cmake-build-debug/test-dump.28c9de13b540236a
/home/maister/git/Fossilize/cmake-build-debug/test-dump.28c9de13b540236a/12e7f9b68dee5d3f.1.foz
/home/maister/git/Fossilize/cmake-build-debug/test-dump.28c9de13b540236a/TOUCH
=====================
#endif

int main()
{
	record_data();
	cleanup();
}