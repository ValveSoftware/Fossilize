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

#include <string>
#include <stdlib.h>

using namespace Fossilize;
using namespace std;

struct DumbReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;
	};

	DumbReplayer(const VulkanDevice &device, const Options &opts)
		: device(device)
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
			if (sampler)
				vkDestroySampler(device.get_device(), sampler, nullptr);
		for (auto &layout : layouts)
			if (layout)
				vkDestroyDescriptorSetLayout(device.get_device(), layout, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout)
				vkDestroyPipelineLayout(device.get_device(), pipeline_layout, nullptr);
		for (auto &shader_module : shader_modules)
			if (shader_module)
				vkDestroyShaderModule(device.get_device(), shader_module, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass)
				vkDestroyRenderPass(device.get_device(), render_pass, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline)
				vkDestroyPipeline(device.get_device(), pipeline, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline)
				vkDestroyPipeline(device.get_device(), pipeline, nullptr);
	}

	bool set_num_samplers(unsigned count) override
	{
		samplers.resize(count);
		return true;
	}

	bool set_num_descriptor_set_layouts(unsigned count) override
	{
		layouts.resize(count);
		return true;
	}

	bool set_num_pipeline_layouts(unsigned count) override
	{
		pipeline_layouts.resize(count);
		return true;
	}

	bool set_num_shader_modules(unsigned count) override
	{
		shader_modules.resize(count);
		return true;
	}

	bool set_num_render_passes(unsigned count) override
	{
		render_passes.resize(count);
		return true;
	}

	bool set_num_compute_pipelines(unsigned count) override
	{
		compute_pipelines.resize(count);
		return true;
	}

	bool set_num_graphics_pipelines(unsigned count) override
	{
		graphics_pipelines.resize(count);
		return true;
	}

	bool enqueue_create_sampler(Hash, unsigned index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		LOGI("Creating sampler #%u\n", index);
		if (vkCreateSampler(device.get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE(" ... Failed!\n");
			return false;
		}
		samplers[index] = *sampler;
		LOGI(" ... Succeeded!\n");
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash, unsigned index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		LOGI("Creating descriptor set layout #%u\n", index);
		if (vkCreateDescriptorSetLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE(" ... Failed!\n");
			return false;
		}
		layouts[index] = *layout;
		LOGI(" ... Succeeded!\n");
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash, unsigned index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		LOGI("Creating pipeline layout #%u\n", index);
		if (vkCreatePipelineLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE(" ... Failed!\n");
			return false;
		}
		pipeline_layouts[index] = *layout;
		LOGI(" ... Succeeded!\n");
		return true;
	}

	bool enqueue_create_shader_module(Hash, unsigned index, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		LOGI("Creating shader module #%u\n", index);
		if (vkCreateShaderModule(device.get_device(), create_info, nullptr, module) != VK_SUCCESS)
		{
			LOGE(" ... Failed!\n");
			return false;
		}
		shader_modules[index] = *module;
		LOGI(" ... Succeeded!\n");
		return true;
	}

	bool enqueue_create_render_pass(Hash, unsigned index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		LOGI("Creating render pass #%u\n", index);
		if (vkCreateRenderPass(device.get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
		{
			LOGE(" ... Failed!\n");
			return false;
		}
		render_passes[index] = *render_pass;
		LOGI(" ... Succeeded!\n");
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash, unsigned index, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		LOGI("Creating compute pipeline #%u\n", index);
		if (vkCreateComputePipelines(device.get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) != VK_SUCCESS)
		{
			LOGE(" ... Failed!\n");
			return false;
		}
		compute_pipelines[index] = *pipeline;
		LOGI(" ... Succeeded!\n");
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash, unsigned index, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		LOGI("Creating graphics pipeline #%u\n", index);
		if (vkCreateGraphicsPipelines(device.get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) != VK_SUCCESS)
		{
			LOGE(" ... Failed!\n");
			return false;
		}
		graphics_pipelines[index] = *pipeline;
		LOGI(" ... Succeeded!\n");
		return true;
	}

	const VulkanDevice &device;

	vector<VkSampler> samplers;
	vector<VkDescriptorSetLayout> layouts;
	vector<VkPipelineLayout> pipeline_layouts;
	vector<VkShaderModule> shader_modules;
	vector<VkRenderPass> render_passes;
	vector<VkPipeline> compute_pipelines;
	vector<VkPipeline> graphics_pipelines;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
};

static void print_help()
{
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--pipeline-cache]\n"
	     "\tstate.json\n");
}

int main(int argc, char *argv[])
{
	string json_path;
	VulkanDevice::Options opts;
	DumbReplayer::Options replayer_opts;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { json_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
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

	try
	{
		VulkanDevice device;
		if (!device.init_device(opts))
			return EXIT_FAILURE;

		DumbReplayer replayer(device, replayer_opts);
		StateReplayer state_replayer;
		auto state_json = load_json_from_file(json_path.c_str());
		if (state_json.empty())
		{
			LOGE("Failed to load state JSON from disk.\n");
			return EXIT_FAILURE;
		}

		state_replayer.parse(replayer, state_json.data(), state_json.size());
	}
	catch (const exception &e)
	{
		LOGE("StateReplayer threw exception: %s\n", e.what());
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}