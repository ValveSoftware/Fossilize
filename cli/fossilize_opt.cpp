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

#include "fossilize_inttypes.h"
#include "fossilize.hpp"
#include "logging.hpp"
#include "cli_parser.hpp"
#include "fossilize_db.hpp"
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
	bool optimize_size = false;

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		*sampler = fake_handle<VkSampler>(hash);
		return recorder.record_sampler(*sampler, *create_info, hash);
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		return recorder.record_descriptor_set_layout(*layout, *create_info, hash);
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		*layout = fake_handle<VkPipelineLayout>(hash);
		return recorder.record_pipeline_layout(*layout, *create_info, hash);
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		vector<uint32_t> compiled_spirv;
		spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_1);

		if (optimize_size)
			optimizer.RegisterSizePasses();
		else
			optimizer.RegisterPerformancePasses();

		if (!optimizer.Run(create_info->pCode, create_info->codeSize / sizeof(uint32_t), &compiled_spirv))
		{
			LOGE("Failed to optimize shader module %016" PRIx64 ". Using original module.\n", hash);
			*module = fake_handle<VkShaderModule>(hash);
			return recorder.record_shader_module(*module, *create_info, hash);
		}
		else
		{
			auto info = *create_info;
			info.pCode = compiled_spirv.data();
			info.codeSize = compiled_spirv.size() * sizeof(uint32_t);

			*module = fake_handle<VkShaderModule>(hash);
			return recorder.record_shader_module(*module, info, hash);
		}
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return recorder.record_render_pass(*render_pass, *create_info, hash);
	}

	bool enqueue_create_render_pass2(Hash hash, const VkRenderPassCreateInfo2 *create_info, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return recorder.record_render_pass2(*render_pass, *create_info, hash);
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		return recorder.record_compute_pipeline(*pipeline, *create_info, nullptr, 0, hash);
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		return recorder.record_graphics_pipeline(*pipeline, *create_info, nullptr, 0, hash);
	}

	bool enqueue_create_raytracing_pipeline(Hash, const VkRayTracingPipelineCreateInfoKHR *, VkPipeline *) override
	{
		return false;
	}
};

static void print_help()
{
	LOGI("fossilize-opt\n"
	     "\t[--help]\n"
	     "\t[--optimize-size]\n"
	     "\t[--input-db <path>]\n"
	     "\t[--output-db <path>]\n");
}

int main(int argc, char *argv[])
{
	string input_db_path;
	string output_db_path;
	CLICallbacks cbs;
	bool optimize_size = false;

	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--input-db", [&](CLIParser &parser) { input_db_path = parser.next_string(); });
	cbs.add("--output-db", [&](CLIParser &parser) { output_db_path = parser.next_string(); });
	cbs.add("--optimize-size", [&](CLIParser &) { optimize_size = true; });
	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (input_db_path.empty())
	{
		LOGE("No input database provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	if (output_db_path.empty())
	{
		LOGE("No output database provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	auto input_db = std::unique_ptr<DatabaseInterface>(create_database(input_db_path.c_str(), DatabaseMode::ReadOnly));
	auto output_db = std::unique_ptr<DatabaseInterface>(create_database(output_db_path.c_str(), DatabaseMode::OverWrite));

	OptimizeReplayer optimize_replayer;
	optimize_replayer.optimize_size = optimize_size;

	StateReplayer replayer;
	optimize_replayer.recorder.set_database_enable_checksum(true);
	optimize_replayer.recorder.set_database_enable_compression(true);

	if (!input_db || !input_db->prepare())
	{
		LOGE("Failed to load database: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (!output_db)
	{
		LOGE("Failed to open database for writing: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	// Recording thread prepares.
	optimize_replayer.recorder.init_recording_thread(output_db.get());

	static const ResourceTag playback_order[] = {
		RESOURCE_SHADER_MODULE,
		RESOURCE_SAMPLER,
		RESOURCE_DESCRIPTOR_SET_LAYOUT,
		RESOURCE_PIPELINE_LAYOUT,
		RESOURCE_RENDER_PASS,
		RESOURCE_GRAPHICS_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
	};

	vector<uint8_t> state_json;
	for (auto &tag : playback_order)
	{
		size_t hash_count = 0;
		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
		{
			LOGE("Failed to get hashes.\n");
			return EXIT_FAILURE;
		}

		vector<Hash> hashes(hash_count);

		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
		{
			LOGE("Failed to get shader module hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto hash : hashes)
		{
			size_t state_json_size;
			if (!input_db->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			state_json.resize(state_json_size);

			if (!input_db->read_entry(tag, hash, &state_json_size, state_json.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			if (!replayer.parse(optimize_replayer, input_db.get(), state_json.data(), state_json.size()))
				LOGE("Failed to parse blob (tag: %d, hash: 0x%" PRIx64 ").\n", tag, hash);
		}
	}
}
