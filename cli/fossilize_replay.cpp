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

#include "volk.h"
#include "device.hpp"
#include "fossilize.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "file.hpp"

#include <cinttypes>
#include <string>
#include <unordered_set>
#include <stdlib.h>
#include <filesystem>
#include <fstream>

using namespace Fossilize;
using namespace std;

struct DumbReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;
	};

	DumbReplayer(const VulkanDevice &device, const Options &opts,
	             const unordered_set<unsigned> &graphics,
	             const unordered_set<unsigned> &compute)
		: device(device), filter_graphics(graphics), filter_compute(compute)
	{
		if (opts.pipeline_cache)
		{
			VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
			vkCreatePipelineCache(device.get_device(), &info, nullptr, &pipeline_cache);
		}
	}

	~DumbReplayer()
	{
		if (pipeline_cache)
			vkDestroyPipelineCache(device.get_device(), pipeline_cache, nullptr);
		for (auto &sampler : samplers)
			if (sampler.second)
				vkDestroySampler(device.get_device(), sampler.second, nullptr);
		for (auto &layout : layouts)
			if (layout.second)
				vkDestroyDescriptorSetLayout(device.get_device(), layout.second, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout.second)
				vkDestroyPipelineLayout(device.get_device(), pipeline_layout.second, nullptr);
		for (auto &shader_module : shader_modules)
			if (shader_module.second)
				vkDestroyShaderModule(device.get_device(), shader_module.second, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass.second)
				vkDestroyRenderPass(device.get_device(), render_pass.second, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device.get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device.get_device(), pipeline.second, nullptr);
	}

	bool enqueue_create_sampler(Hash index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		if (vkCreateSampler(device.get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %0" PRIX64 " Failed!\n", index);
			return false;
		}
		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		if (vkCreateDescriptorSetLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating descriptor set layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		if (vkCreatePipelineLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating pipeline layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		pipeline_layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_shader_module(Hash index, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		if (vkCreateShaderModule(device.get_device(), create_info, nullptr, module) != VK_SUCCESS)
		{
			LOGE("Creating shader module %0" PRIX64 " Failed!\n", index);
			return false;
		}
		shader_modules[index] = *module;
		return true;
	}

	bool enqueue_create_render_pass(Hash index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		if (vkCreateRenderPass(device.get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
		{
			LOGE("Creating render pass %0" PRIX64 " Failed!\n", index);
			return false;
		}
		render_passes[index] = *render_pass;
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash index, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if ((filter_compute.empty() && filter_graphics.empty()) || filter_compute.count(index))
		{
			if (vkCreateComputePipelines(device.get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) !=
			    VK_SUCCESS)
			{
				LOGE("Creating compute pipeline %0" PRIX64 " Failed!\n", index);
				return false;
			}
		}
		else
			*pipeline = VK_NULL_HANDLE;

		compute_pipelines[index] = *pipeline;
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash index, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if ((filter_graphics.empty() && filter_compute.empty()) || filter_graphics.count(index))
		{
			if (vkCreateGraphicsPipelines(device.get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) !=
			    VK_SUCCESS)
			{
				LOGE("Creating graphics pipeline %0" PRIX64 " failed\n", index);
				return false;
			}
		}
		else
			*pipeline = VK_NULL_HANDLE;

		graphics_pipelines[index] = *pipeline;
		return true;
	}

	const VulkanDevice &device;
	const unordered_set<unsigned> &filter_graphics;
	const unordered_set<unsigned> &filter_compute;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> shader_modules;
	std::unordered_map<Hash, VkRenderPass> render_passes;
	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
};

static std::vector<uint8_t> load_buffer_from_path(std::filesystem::path path)
{
	if (std::filesystem::is_regular_file(path))
	{
		std::ifstream file(path, std::ios::binary);

		file.seekg(0, std::ios::end);
		auto file_size = file.tellg();
		file.seekg(0, std::ios::beg);

		std::vector<uint8_t> file_data(file_size);
		file.read(reinterpret_cast<char *>(file_data.data()), file_size);

		return file_data;
	}
	return {};
}

struct DirectoryResolver : ResolverInterface
{
	DirectoryResolver(const std::string &_directory) : directory(_directory) {};

	vector<uint8_t> resolve(Hash hash)
	{
		char filename[22];
		sprintf(filename, "%016" PRIX64 ".json", hash);
		auto path = directory / filename;

		return load_buffer_from_path(path);
	}

	std::filesystem::path directory;
};

static void print_help()
{
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--pipeline-cache]\n"
	     "\t[--filter-compute <index>]\n"
	     "\t[--filter-graphics <index>]\n"
	     "\t[--pipeline-cache]\n"
	     "\tstate.json\n");
}

int main(int argc, char *argv[])
{
	string json_path;
	VulkanDevice::Options opts;
	DumbReplayer::Options replayer_opts;

	unordered_set<unsigned> filter_graphics;
	unordered_set<unsigned> filter_compute;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { json_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
	cbs.add("--filter-compute", [&](CLIParser &parser) { filter_compute.insert(parser.next_uint()); });
	cbs.add("--filter-graphics", [&](CLIParser &parser) { filter_graphics.insert(parser.next_uint()); });
	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (json_path.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	if (!std::filesystem::is_directory(json_path))
	{
		LOGE("Invalid path to serialized state provided.\n");
		return EXIT_FAILURE;
	}

	auto start_time = chrono::steady_clock::now();

	VulkanDevice device;
	if (!device.init_device(opts))
		return EXIT_FAILURE;

	DumbReplayer replayer(device, replayer_opts, filter_graphics, filter_compute);
	DirectoryResolver resolver(json_path);
	StateReplayer state_replayer;

	for (auto& entry : std::filesystem::directory_iterator(json_path))
	{
		// check that filename is 16 char hex with json extension
		auto stem = entry.path().stem().string();
		if (stem.size() != 16)
			continue;
		for (auto c : stem)
		{
			if (!isxdigit(c))
				continue;
		}
		if (entry.path().extension() != ".json")
			continue;

		try
		{
			auto state_json = load_buffer_from_path(entry);
			if (state_json.empty())
			{
				LOGE("Failed to load %s from disk.\n", entry.path().string().c_str());
			}

			state_replayer.parse(replayer, resolver, state_json.data(), state_json.size());
		}
		catch (const exception &e)
		{
			LOGE("StateReplayer threw exception parsing %s: %s\n", entry.path().string().c_str(), e.what());
		}
	}

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.size() +
		replayer.render_passes.size() +
		replayer.compute_pipelines.size() +
		replayer.graphics_pipelines.size();

	unsigned long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Replayed %lu objects in %lu ms:\n", total_size, elapsed_ms);
	LOGI("  samplers:              %7lu\n", (unsigned long)replayer.samplers.size());
	LOGI("  descriptor set layouts:%7lu\n", (unsigned long)replayer.layouts.size());
	LOGI("  pipeline layouts:      %7lu\n", (unsigned long)replayer.pipeline_layouts.size());
	LOGI("  shader modules:        %7lu\n", (unsigned long)replayer.shader_modules.size());
	LOGI("  render passes:         %7lu\n", (unsigned long)replayer.render_passes.size());
	LOGI("  compute pipelines:     %7lu\n", (unsigned long)replayer.compute_pipelines.size());
	LOGI("  graphics pipelines:    %7lu\n", (unsigned long)replayer.graphics_pipelines.size());

	return EXIT_SUCCESS;
}
