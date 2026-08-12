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
#include "cli_parser.hpp"
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "logging.hpp"
#include "fossilize_sifter_utils.hpp"
#include <unistd.h>
#include <limits.h>
#include <memory>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>

using namespace Fossilize;

static void print_help()
{
	LOGI("fossilize-validate-cache-roundtrip\n"
		"\t[--help]\n"
		"\t[--fossilize-replay <custom path to replayer>]\n"
		"\t[--robustness]\n"
		"\t[--pipeline-binary-key]\n");
}

static constexpr VkApplicationInfo app_info = {
	VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
	"fossilize-validate-cache-roundtrip", 1,
	"fossilize-cli", 1,
	VK_API_VERSION_1_1,
};

// Basic idea, with a custom dumb device with minimal features enables, study if the full replayer roundtrips properly.
static VkInstance create_instance()
{
	if (volkInitialize() != VK_SUCCESS)
		return VK_NULL_HANDLE;

	uint32_t instance_version;
	if (vkEnumerateInstanceVersion(&instance_version) != VK_SUCCESS || instance_version < VK_API_VERSION_1_1)
		return VK_NULL_HANDLE;

	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_info.pApplicationInfo = &app_info;

	VkInstance instance;
	if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
	{
		LOGE("Failed to create instance.\n");
		return VK_NULL_HANDLE;
	}

	volkLoadInstance(instance);

	return instance;
}

static void pipeline_binary_key_to_str(char (&str)[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR * 2 + 1], const VkPipelineBinaryKeyKHR &key)
{
	str[0] = '\0';
	for (uint32_t i = 0; i < key.keySize; i++)
		sprintf(str + 2 * i, "%02x", key.key[i]);
}

static VkDevice create_device(VkInstance instance, const VkPhysicalDeviceFeatures2 &features2_base, bool pipeline_binary_key)
{
	// Just pick the first GPU. Could be improved if needed.
	uint32_t count = 1;
	VkPhysicalDevice gpu;
	if (vkEnumeratePhysicalDevices(instance, &count, &gpu) < 0)
		return VK_NULL_HANDLE;

	// We don't care about Vulkan 1.0 devices.
	VkPhysicalDeviceProperties props = {};
	vkGetPhysicalDeviceProperties(gpu, &props);
	if (props.apiVersion < VK_API_VERSION_1_1)
		return VK_NULL_HANDLE;

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

	static const char *extensions[] = {
		VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
		VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
		VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
		VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
		VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
		VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
	};

	if (pipeline_binary_key)
	{
		binary.pNext = features2.pNext;
		features2.pNext = &binary;

		device_info.ppEnabledExtensionNames = extensions;
		device_info.enabledExtensionCount = features2.features.robustBufferAccess ? 6 : 5;
	}
	else if (features2.features.robustBufferAccess)
	{
		device_info.ppEnabledExtensionNames = extensions + 5;
		device_info.enabledExtensionCount = 1;
	}

	VkDevice device;
	if (vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	volkLoadDevice(device);

	if (pipeline_binary_key)
	{
		VkPipelineBinaryKeyKHR key = { VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR };
		if (vkGetPipelineKeyKHR(device, nullptr, &key) != VK_SUCCESS)
		{
			LOGE("Failed to get the global pipeline binary key.\n");
			return VK_NULL_HANDLE;
		}

		char str[VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR * 2 + 1];
		pipeline_binary_key_to_str(str, key);
		LOGI("Global pipeline binary key: %s\n", str);
	}

	return device;
}

static bool record_foz_and_cache(VkDevice device, VkPipelineCache cache, StateRecorder &recorder)
{
	if (!create_compute_pipeline(device, cache, &recorder))
		return false;
	if (!create_graphics_pipeline(device, cache, &recorder))
		return false;

	return true;
}

static bool serialize_pipeline_cache(VkDevice device, VkPipelineCache cache, const char *path)
{
	auto data = serialize_pipeline_cache_to_data(device, cache);
	if (data.empty())
		return false;

	FILE *file = fopen(path, "wb");
	if (!file)
		return false;

	if (fwrite(data.data(), 1, data.size(), file) != data.size())
	{
		fclose(file);
		return false;
	}

	fclose(file);
	return true;
}

static bool check_replayer_roundtrip(const std::string &replayer, const char *foz_path, const char *cache_bin)
{
	int pipe_fd[2];
	if (pipe(pipe_fd) < 0)
		return false;
	pid_t pid = fork();

	if (pid)
	{
		close(pipe_fd[1]);
		FILE *file = fdopen(pipe_fd[0], "r");
		if (!file)
		{
			kill(pid, SIGKILL);
			LOGE("fdopen failed somehow.\n");
			return false;
		}

		unsigned hits = 0;
		unsigned misses = 0;

		char line[1024];
		while (fgets(line, sizeof(line), file))
		{
			const auto parse_number_end_of_line = [](const char *str)
			{
				const char *ptr = str + strlen(str);
				ptr--;
				if (*ptr == '\n')
					ptr--;
				while (isdigit(*ptr))
					ptr--;
				return strtoul(ptr, nullptr, 0);
			};

			// Crude parsing of console output.
			if (strstr(line, "Pipeline cache hits reported"))
				hits = parse_number_end_of_line(line);
			else if (strstr(line, "Pipeline cache misses reported"))
				misses = parse_number_end_of_line(line);
			else if (strncmp(line, "Fossilize ERROR:", strlen("Fossilize ERROR:")) == 0 ||
			         strstr(line, "Replayer global pipeline binary key:"))
			{
				fprintf(stderr, "%s", line);
			}
		}

		fclose(file);

		int wait_status;
		if (waitpid(pid, &wait_status, 0) < 0)
		{
			LOGE("Failed to wait for child process\n");
			return false;
		}

		if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0)
		{
			LOGE("Child process did not exit normally.\n");
			return false;
		}

		if (hits == 0 && misses != 0)
		{
			LOGE("Pipeline cache missed! Roundtrip does not work!\n");
			return false;
		}
		else if (hits != 0 && misses == 0)
		{
			LOGI("Roundtrip success :D\n");
		}
		else if (hits == 0 && misses == 0)
		{
			LOGE("No hits or misses reported. Something went wrong.\n");
			return false;
		}
	}
	else
	{
		close(pipe_fd[0]);
		if (dup2(pipe_fd[1], STDOUT_FILENO) < 0)
			_exit(EXIT_FAILURE);
		if (dup2(pipe_fd[1], STDERR_FILENO) < 0)
			_exit(EXIT_FAILURE);
		close(pipe_fd[1]);

		if (execlp(replayer.c_str(), replayer.c_str(),
		           foz_path, "--num-threads", "1", "--on-disk-pipeline-cache",
		           cache_bin, static_cast<char *>(nullptr)) < 0)
		{
			LOGE("Failed to spawn child process \"%s\".\n", replayer.c_str());
			_exit(EXIT_FAILURE);
		}
	}

	return true;
}

int main(int argc, char **argv)
{
	std::string fossilize_replay = "fossilize-replay";
	bool pipeline_binary_key = false;
	bool robustness = false;
	CLICallbacks cbs;

	cbs.add("--help", [&](CLIParser &parser) { parser.end(); });
	cbs.add("--fossilize-replay", [&](CLIParser &parser) { fossilize_replay = parser.next_string(); });
	cbs.add("--pipeline-binary-key", [&](CLIParser &) { pipeline_binary_key = true; });
	cbs.add("--robustness", [&](CLIParser &) { robustness = true; });

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

	VkInstance instance = create_instance();

	VkPhysicalDeviceRobustness2FeaturesKHR robustness2 = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR, nullptr,
		VK_TRUE, VK_TRUE, VK_TRUE,
	};
	VkPhysicalDeviceFeatures2 features2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, robustness ? &robustness2 : nullptr, { robustness } };

	VkDevice device = create_device(instance, features2, pipeline_binary_key);
	VkPipelineCache cache = create_pipeline_cache(device);

	char tmp_foz[PATH_MAX];
	char tmp_bin[PATH_MAX];
	snprintf(tmp_foz, sizeof(tmp_foz), "/tmp/tmp_pipeline_cache.%u.foz", getpid());
	snprintf(tmp_bin, sizeof(tmp_bin), "/tmp/tmp_pipeline_cache.%u.bin", getpid());

	bool ret;
	{
		std::unique_ptr<DatabaseInterface> db(create_database(tmp_foz, DatabaseMode::OverWrite));
		StateRecorder recorder;
		recorder.init_recording_synchronized(db.get());
		recorder.set_database_enable_application_feature_links(false);

		if (!recorder.record_application_info(app_info))
			return EXIT_FAILURE;
		if (!recorder.record_physical_device_features(&features2))
			return EXIT_FAILURE;

		ret = record_foz_and_cache(device, cache, recorder);
	}

	if (ret)
		ret = serialize_pipeline_cache(device, cache, tmp_bin);

	vkDestroyPipelineCache(device, cache, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);

	if (ret)
		ret = check_replayer_roundtrip(fossilize_replay, tmp_foz, tmp_bin);

	unlink(tmp_foz);
	unlink(tmp_bin);

	if (!ret)
		return EXIT_FAILURE;
}
