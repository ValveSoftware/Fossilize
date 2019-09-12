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

#include "device.hpp"
#include <thread>

#ifdef __linux__
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#endif

using namespace Fossilize;

void test_thread(const VkApplicationInfo *info, const VkPhysicalDeviceFeatures2 *features)
{
	VulkanDevice::Options opts;
	opts.application_info = info;
	opts.features = features;
	opts.enable_validation = false;
	VulkanDevice device;
	if (!device.init_device(opts))
		return;

	// Create a lot of data so we can potentially expose some race conditions.
	for (unsigned i = 0; i < 10000; i++)
	{
		VkSamplerCreateInfo create_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		create_info.minLod = float(i);
		VkSampler sampler = VK_NULL_HANDLE;
		vkCreateSampler(device.get_device(), &create_info, nullptr, &sampler);
		if (sampler != VK_NULL_HANDLE)
			vkDestroySampler(device.get_device(), sampler, nullptr);
	}

}

int main()
{
#ifdef __linux__
	signal(SIGCHLD, SIG_IGN);
	// Stress multi-process.
	for (unsigned i = 0; i < 3; i++)
	{
		if (fork() <= 0)
			break;
	}
#endif

	// A simple test which creates multiple devices and instances. We should verify that this works as expected.
	std::thread threads[64];
	VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkApplicationInfo app_info_template = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app_info_template.pApplicationName = "Test App";
	app_info_template.pEngineName = "Fossilize";
	app_info_template.apiVersion = VK_API_VERSION_1_0;

	VkApplicationInfo app_infos[64];
	for (unsigned i = 0; i < 64; i++)
	{
		app_infos[i] = app_info_template;
		// Groups of 16 threads will end up with the same app-info hash.
		// We should see 4 unique databases being created with 10k samplers each.
		app_infos[i].applicationVersion = i & 3;
		threads[i] = std::thread(test_thread, &app_infos[i], &features);
	}

	for (unsigned i = 0; i < 64; i++)
		threads[i].join();

	// TODO: Automatically check here.
}
