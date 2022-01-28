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

#include "fossilize_inttypes.h"
#include "fossilize_db.hpp"
#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "layer/utils.hpp"
#include "cli_parser.hpp"

using namespace Fossilize;
using namespace std;

static void print_help()
{
	LOGI("Usage: fossilize-rehash [--input-db path] [--output-db path] [--application hash]\n");
}

template <typename T>
static inline T fake_handle(uint64_t value)
{
	static_assert(sizeof(T) == sizeof(uint64_t), "Handle size is not 64-bit.");
	// reinterpret_cast does not work reliably on MSVC 2013 for Vulkan objects.
	return (T)value;
}

struct RehashReplayer : StateCreatorInterface
{
	StateRecorder *recorder = nullptr;
	Hash filter_application_hash = 0;
	bool has_set_application_info = false;
	bool should_filter_application_hash = false;

	void set_application_info(Hash hash, const VkApplicationInfo *info, const VkPhysicalDeviceFeatures2 *features) override
	{
		if (!should_filter_application_hash && has_set_application_info)
		{
			LOGE("There are multiple VkApplicationInfo in this database. All blobs in this input database will be assigned to the first application info.\n");
		}
		else if (!has_set_application_info && (!should_filter_application_hash || hash == filter_application_hash))
		{
			if (info)
				if (!recorder->record_application_info(*info))
					LOGE("Failed to record application info.\n");
			if (features)
				if (!recorder->record_physical_device_features(features))
					LOGE("Failed to record physical device features.\n");
			has_set_application_info = true;
		}
	}

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		*sampler = fake_handle<VkSampler>(hash);
		return recorder->record_sampler(*sampler, *create_info);
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		return recorder->record_descriptor_set_layout(*layout, *create_info);
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		*layout = fake_handle<VkPipelineLayout>(hash);
		return recorder->record_pipeline_layout(*layout, *create_info);
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		*module = fake_handle<VkShaderModule>(hash);
		return recorder->record_shader_module(*module, *create_info);
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return recorder->record_render_pass(*render_pass, *create_info);
	}

	bool enqueue_create_render_pass2(Hash hash, const VkRenderPassCreateInfo2 *create_info, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return recorder->record_render_pass2(*render_pass, *create_info);
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		return recorder->record_compute_pipeline(*pipeline, *create_info, nullptr, 0);
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		return recorder->record_graphics_pipeline(*pipeline, *create_info, nullptr, 0);
	}

	bool enqueue_create_raytracing_pipeline(Hash, const VkRayTracingPipelineCreateInfoKHR *, VkPipeline *) override
	{
		return false;
	}
};

int main(int argc, char *argv[])
{
	CLICallbacks cbs;
	string input_db_path;
	string output_db_path;

	unique_ptr<DatabaseInterface> output_db;

	StateRecorder recorder;
	recorder.set_database_enable_checksum(true);
	recorder.set_database_enable_compression(true);
	RehashReplayer rehash_replayer;
	rehash_replayer.recorder = &recorder;

	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--input-db", [&](CLIParser &parser) { input_db_path = parser.next_string(); });
	cbs.add("--output-db", [&](CLIParser &parser) { output_db_path = parser.next_string(); });
	cbs.add("--application", [&](CLIParser &parser) {
		rehash_replayer.filter_application_hash = strtoull(parser.next_string(), nullptr, 16);
		rehash_replayer.should_filter_application_hash = true;
	});

	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (input_db_path.empty() || output_db_path.empty())
	{
		print_help();
		return EXIT_FAILURE;
	}

	auto input_db = std::unique_ptr<DatabaseInterface>(create_database(input_db_path.c_str(), DatabaseMode::ReadOnly));
	output_db.reset(create_database(output_db_path.c_str(), DatabaseMode::OverWrite));
	if (!input_db || !input_db->prepare())
	{
		LOGE("Failed to load database: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	// Recording thread prepares.
	if (!output_db)
	{
		LOGE("Failed to open database for writing: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	StateReplayer replayer;

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO,
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

			if (!replayer.parse(rehash_replayer, input_db.get(), state_json.data(), state_json.size()))
				LOGE("Failed to parse blob (tag: %d, hash: 0x%" PRIx64 ").\n", tag, hash);
		}

		if (tag == RESOURCE_APPLICATION_INFO)
		{
			// Now we have set the application info, so we can start the recording thread now.
			recorder.init_recording_thread(output_db.get());
		}
	}
}


