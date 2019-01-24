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

#include "fossilize.hpp"
#include "logging.hpp"
#include "cli_parser.hpp"
#include "file.hpp"
#include "spirv-tools/optimizer.hpp"

using namespace std;
using namespace Fossilize;

template <typename T>
static inline T fake_handle(uint64_t v)
{
	return (T)v;
}

struct OptimizeReplayer : StateCreatorInterface
{
	StateRecorder recorder;

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		*sampler = fake_handle<VkSampler>(hash);
		recorder.record_sampler(*sampler, *create_info);
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		recorder.record_descriptor_set_layout(*layout, *create_info);
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		*layout = fake_handle<VkPipelineLayout>(hash);
		recorder.record_pipeline_layout(*layout, *create_info);
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		vector<uint32_t> compiled_spirv;
		spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);
		optimizer.RegisterPerformancePasses();
		if (!optimizer.Run(create_info->pCode, create_info->codeSize / sizeof(uint32_t), &compiled_spirv))
			return false;
		auto info = *create_info;
		info.pCode = compiled_spirv.data();
		info.codeSize = compiled_spirv.size() * sizeof(uint32_t);

		*module = fake_handle<VkShaderModule>(hash);
		recorder.record_shader_module(*module, info);
		return true;
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		recorder.record_render_pass(*render_pass, *create_info);
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		recorder.record_compute_pipeline(*pipeline, *create_info);
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		recorder.record_graphics_pipeline(*pipeline, *create_info);
		return true;
	}
};

static void print_help()
{
	LOGI("fossilize-opt\n"
	     "\t[--help]\n"
	     "\t[--input state.json]\n"
	     "\t[--output state.json]\n");
}

int main(int argc, char *argv[])
{
	string json_path;
	string json_output_path;
	CLICallbacks cbs;

	cbs.default_handler = [&](const char *arg) { json_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--input", [&](CLIParser &parser) { json_path = parser.next_string(); });
	cbs.add("--output", [&](CLIParser &parser) { json_output_path = parser.next_string(); });
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

	if (json_output_path.empty())
	{
		LOGE("No output path provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	try
	{
		OptimizeReplayer replayer;
		StateReplayer state_replayer;
		auto state_json = load_buffer_from_file(json_path.c_str());
		if (state_json.empty())
		{
			LOGE("Failed to load state JSON from disk.\n");
			return EXIT_FAILURE;
		}

		state_replayer.parse(replayer, nullptr, state_json.data(), state_json.size());

		uint8_t *serialized;
		size_t serialized_size;
		if (replayer.recorder.serialize(&serialized, &serialized_size))
		{
			if (!write_buffer_to_file(json_output_path.c_str(), serialized, serialized_size))
			{
				LOGE("Failed to write buffer to file: %s.\n", json_output_path.c_str());
				return EXIT_FAILURE;
			}
			StateRecorder::free_serialized(serialized);
		}
	}
	catch (const exception &e)
	{
		LOGE("StateReplayer threw exception: %s\n", e.what());
		return EXIT_FAILURE;
	}
}
