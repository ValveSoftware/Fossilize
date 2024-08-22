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
#include <stdlib.h>
#include <memory>
#include <vector>
#include <time.h>
#include <unordered_set>
#include <unordered_map>
#include "layer/utils.hpp"
#include "cli_parser.hpp"

using namespace Fossilize;
using namespace std;

static void print_help()
{
	LOGI("Usage: fossilize-prune\n"
	     "\t[--input-db path]\n"
	     "\t[--output-db path]\n"
	     "\t[--filter-application hash]\n"
	     "\t[--filter-graphics hash]\n"
	     "\t[--filter-compute hash]\n"
	     "\t[--filter-raytracing hash]\n"
	     "\t[--filter-module hash]\n"
	     "\t[--filter-timestamp path seconds] (seconds is relative to current time. E.g., if 100, any entry made more than 100 seconds ago are pruned)\n"
	     "\t[--skip-graphics hash]\n"
	     "\t[--skip-compute hash]\n"
	     "\t[--skip-raytracing hash]\n"
	     "\t[--skip-module hash]\n"
	     "\t[--skip-application-info-links]\n"
	     "\t[--whitelist whitelist.foz]\n"
	     "\t[--blacklist blacklist.foz]\n"
	     "\t[--invert-module-pruning]\n");
}

template <typename T>
static inline T fake_handle(uint64_t v)
{
	return (T)v;
}

template <typename T>
static inline const T *find_pnext(VkStructureType type, const void *pNext)
{
	while (pNext != nullptr)
	{
		auto *sin = static_cast<const VkBaseInStructure *>(pNext);
		if (sin->sType == type)
			return static_cast<const T*>(pNext);

		pNext = sin->pNext;
	}

	return nullptr;
}

struct PruneReplayer : StateCreatorInterface
{
	unordered_set<Hash> accessed_samplers;
	unordered_set<Hash> accessed_descriptor_sets;
	unordered_set<Hash> accessed_pipeline_layouts;
	unordered_set<Hash> accessed_shader_modules;
	unordered_set<Hash> accessed_render_passes;
	unordered_set<Hash> accessed_graphics_pipelines;
	unordered_set<Hash> accessed_compute_pipelines;
	unordered_set<Hash> accessed_raytracing_pipelines;
	unordered_set<Hash> filter_graphics;
	unordered_set<Hash> filter_compute;
	unordered_set<Hash> filter_raytracing;
	unordered_set<Hash> filter_modules;
	unordered_set<Hash> banned_graphics;
	unordered_set<Hash> banned_compute;
	unordered_set<Hash> banned_raytracing;
	unordered_set<Hash> banned_modules;

	unordered_map<Hash, const VkDescriptorSetLayoutCreateInfo *> descriptor_sets;
	unordered_map<Hash, const VkPipelineLayoutCreateInfo *> pipeline_layouts;
	unordered_map<Hash, const VkRayTracingPipelineCreateInfoKHR *> raytracing_pipelines;
	unordered_map<Hash, const VkGraphicsPipelineCreateInfo *> graphics_pipelines;
	unordered_map<Hash, const VkRayTracingPipelineCreateInfoKHR *> library_raytracing_pipelines;
	unordered_map<Hash, const VkGraphicsPipelineCreateInfo *> library_graphics_pipelines;

	unordered_set<Hash> filtered_blob_hashes[RESOURCE_COUNT];

	std::unique_ptr<DatabaseInterface> timestamp_db;
	uint64_t timestamp_minimum_accept = 0;

	Hash filter_application_hash = 0;
	bool should_filter_application_hash = false;

	Hash application_info_blob = 0;
	bool has_application_info_for_blob = false;
	bool blob_belongs_to_application_info = false;

	bool skip_application_info_links = false;

	void set_application_info(Hash hash, const VkApplicationInfo *app,
	                          const VkPhysicalDeviceFeatures2 *) override
	{
		LOGI("Available application feature hash: %016" PRIx64 "\n", hash);

		if (app)
		{
			LOGI("  applicationInfo: engineName = %s, applicationName = %s, engineVersion = %u, appVersion = %u\n",
			     app->pEngineName ? app->pEngineName : "N/A", app->pApplicationName ? app->pApplicationName : "N/A",
			     app->engineVersion, app->applicationVersion);
		}
	}

	void set_current_application_info(Hash hash) override
	{
		application_info_blob = hash;
		has_application_info_for_blob = true;
		blob_belongs_to_application_info = !should_filter_application_hash || (hash == filter_application_hash);
	}

	void notify_application_info_link(Hash link_hash, Hash app_hash, ResourceTag tag, Hash hash) override
	{
		if (skip_application_info_links)
			return;

		if (!filter_timestamp(RESOURCE_APPLICATION_BLOB_LINK, link_hash, nullptr))
			return;

		if (should_filter_application_hash && app_hash == filter_application_hash)
		{
			filtered_blob_hashes[tag].insert(hash);
			filtered_blob_hashes[RESOURCE_APPLICATION_BLOB_LINK].insert(link_hash);
		}
		else if (!should_filter_application_hash)
			filtered_blob_hashes[RESOURCE_APPLICATION_BLOB_LINK].insert(link_hash);
	}

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *, VkSampler *sampler) override
	{
		*sampler = fake_handle<VkSampler>(hash);
		return true;
	}

	void access_sampler(Hash hash)
	{
		accessed_samplers.insert(hash);
	}

	void access_descriptor_set(Hash hash)
	{
		if (!hash)
			return;
		if (accessed_descriptor_sets.count(hash))
			return;
		accessed_descriptor_sets.insert(hash);

		auto *create_info = descriptor_sets[hash];
		if (!create_info)
			return;

		for (uint32_t binding = 0; binding < create_info->bindingCount; binding++)
		{
			auto &bind = create_info->pBindings[binding];
			if (bind.pImmutableSamplers && bind.descriptorCount != 0)
			{
				for (uint32_t i = 0; i < bind.descriptorCount; i++)
					if (bind.pImmutableSamplers[i] != VK_NULL_HANDLE)
						access_sampler((Hash)bind.pImmutableSamplers[i]);
			}
		}
	}

	void access_pipeline_layout(Hash hash)
	{
		if (!hash)
			return;
		if (accessed_pipeline_layouts.count(hash))
			return;
		accessed_pipeline_layouts.insert(hash);

		auto *create_info = pipeline_layouts[hash];
		if (!create_info)
			return;

		for (uint32_t layout = 0; layout < create_info->setLayoutCount; layout++)
			access_descriptor_set((Hash)create_info->pSetLayouts[layout]);
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		descriptor_sets[hash] = create_info;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		*layout = fake_handle<VkPipelineLayout>(hash);
		pipeline_layouts[hash] = create_info;
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *, VkShaderModule *module) override
	{
		*module = fake_handle<VkShaderModule>(hash);
		return true;
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return true;
	}

	bool enqueue_create_render_pass2(Hash hash, const VkRenderPassCreateInfo2 *, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return true;
	}

	bool filter_timestamp(ResourceTag tag, Hash hash, uint64_t *ts) const
	{
		if (!timestamp_db)
			return true;

		uint64_t timestamp = 0;
		bool ret = true;

		size_t timestamp_size = sizeof(timestamp);
		if (!timestamp_db->read_entry(tag, hash, &timestamp_size, &timestamp, PAYLOAD_READ_NO_FLAGS) ||
		    timestamp_size != sizeof(timestamp))
		{
			ret = false;
		}

		if (timestamp < timestamp_minimum_accept)
			ret = false;

		if (ts)
			*ts = timestamp;

		return ret;
	}

	bool filter_object(ResourceTag tag, Hash hash) const
	{
		if (!filter_timestamp(tag, hash, nullptr))
			return false;

		bool hash_filtering = !(filter_compute.empty() && filter_graphics.empty() && filter_raytracing.empty());
		if (tag == RESOURCE_COMPUTE_PIPELINE)
		{
			if (banned_compute.count(hash))
				return false;
			if (hash_filtering && !filter_compute.count(hash))
				return false;
		}
		else if (tag == RESOURCE_GRAPHICS_PIPELINE)
		{
			if (banned_graphics.count(hash))
				return false;
			if (hash_filtering && !filter_graphics.count(hash))
				return false;
		}
		else if (tag == RESOURCE_RAYTRACING_PIPELINE)
		{
			if (banned_raytracing.count(hash))
				return false;
			if (hash_filtering && !filter_raytracing.count(hash))
				return false;
		}

		return blob_belongs_to_application_info || !should_filter_application_hash || (filtered_blob_hashes[tag].count(hash) != 0);
	}

	bool filter_shader_module(Hash hash) const
	{
		if (banned_modules.count(hash))
			return false;
		if (filter_modules.empty())
			return true;

		return filter_modules.count(hash) != 0;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);

		if (filter_object(RESOURCE_COMPUTE_PIPELINE, hash))
		{
			bool allow_pipeline = filter_shader_module((Hash)create_info->stage.module);

			if (allow_pipeline)
			{
				access_pipeline_layout((Hash) create_info->layout);
				accessed_shader_modules.insert((Hash) create_info->stage.module);
				accessed_compute_pipelines.insert(hash);
			}
		}
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		bool allow_pipeline = false;

		if (filter_object(RESOURCE_GRAPHICS_PIPELINE, hash))
		{
			for (uint32_t i = 0; i < create_info->stageCount; i++)
			{
				if (filter_shader_module((Hash) create_info->pStages[i].module))
				{
					allow_pipeline = true;
					break;
				}
			}

			if (create_info->stageCount == 0)
				allow_pipeline = true;

			// Need to test this as well, if there is at least one banned module used, we don't allow the pipeline.
			for (uint32_t i = 0; i < create_info->stageCount; i++)
			{
				if (banned_modules.count((Hash) create_info->pStages[i].module))
				{
					allow_pipeline = false;
					break;
				}
			}

			auto *library_info =
					find_pnext<VkPipelineLibraryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
					                                           create_info->pNext);

			if (library_info)
			{
				bool has_default_allowed_library = false;
				for (uint32_t i = 0; i < library_info->libraryCount; i++)
				{
					if (banned_graphics.count((Hash) library_info->pLibraries[i]))
					{
						allow_pipeline = false;
						break;
					}

					if (!has_default_allowed_library)
					{
						// Only consider libraries which contain modules.
						auto itr = graphics_pipelines.find((Hash) library_info->pLibraries[i]);
						if (itr != graphics_pipelines.end() && itr->second->stageCount != 0)
							has_default_allowed_library = true;
					}
				}

				// At least one of the dependent libraries must be considered allowed to pass.
				if (create_info->stageCount == 0 && allow_pipeline)
					allow_pipeline = has_default_allowed_library;
			}

			// Never include libraries by default unless they contain real code.
			if ((create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0 && create_info->stageCount == 0)
				allow_pipeline = false;
		}

		// Need to defer this since we need to access pipeline libraries.
		if (allow_pipeline)
			graphics_pipelines[hash] = create_info;
		else if ((create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0)
			library_graphics_pipelines[hash] = create_info;

		return true;
	}

	void access_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info)
	{
		if (accessed_graphics_pipelines.count(hash))
			return;
		accessed_graphics_pipelines.insert(hash);

		access_pipeline_layout((Hash) create_info->layout);
		if (create_info->renderPass != VK_NULL_HANDLE)
			accessed_render_passes.insert((Hash) create_info->renderPass);
		for (uint32_t stage = 0; stage < create_info->stageCount; stage++)
			accessed_shader_modules.insert((Hash) create_info->pStages[stage].module);

		auto *library_info =
				find_pnext<VkPipelineLibraryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
				                                           create_info->pNext);

		if (library_info)
		{
			for (uint32_t i = 0; i < library_info->libraryCount; i++)
			{
				// Only need to recurse if a pipeline was only allowed due to having CREATE_LIBRARY_KHR flag.
				auto lib_itr = library_graphics_pipelines.find((Hash) library_info->pLibraries[i]);
				if (lib_itr != library_graphics_pipelines.end())
					access_graphics_pipeline(lib_itr->first, lib_itr->second);
			}
		}
	}

	void access_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info)
	{
		if (accessed_raytracing_pipelines.count(hash))
			return;
		accessed_raytracing_pipelines.insert(hash);

		access_pipeline_layout((Hash) create_info->layout);
		for (uint32_t stage = 0; stage < create_info->stageCount; stage++)
			accessed_shader_modules.insert((Hash) create_info->pStages[stage].module);

		if (create_info->pLibraryInfo)
		{
			for (uint32_t i = 0; i < create_info->pLibraryInfo->libraryCount; i++)
			{
				// Only need to recurse if a pipeline was only allowed due to having CREATE_LIBRARY_KHR flag.
				auto lib_itr = library_raytracing_pipelines.find((Hash) create_info->pLibraryInfo->pLibraries[i]);
				if (lib_itr != library_raytracing_pipelines.end())
					access_raytracing_pipeline(lib_itr->first, lib_itr->second);
			}
		}
	}

	void access_graphics_pipelines()
	{
		for (auto &pipe : graphics_pipelines)
			access_graphics_pipeline(pipe.first, pipe.second);
	}

	void access_raytracing_pipelines()
	{
		for (auto &pipe : raytracing_pipelines)
			access_raytracing_pipeline(pipe.first, pipe.second);
	}

	bool enqueue_create_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);
		bool allow_pipeline = false;

		if (filter_object(RESOURCE_RAYTRACING_PIPELINE, hash))
		{
			for (uint32_t i = 0; i < create_info->stageCount; i++)
			{
				if (filter_shader_module((Hash) create_info->pStages[i].module))
				{
					allow_pipeline = true;
					break;
				}
			}

			if (create_info->stageCount == 0)
				allow_pipeline = true;

			// Need to test this as well, if there is at least one banned module used, we don't allow the pipeline.
			for (uint32_t i = 0; i < create_info->stageCount; i++)
			{
				if (banned_modules.count((Hash) create_info->pStages[i].module))
				{
					allow_pipeline = false;
					break;
				}
			}

			if (create_info->pLibraryInfo)
			{
				bool has_default_allowed_library = false;
				for (uint32_t i = 0; i < create_info->pLibraryInfo->libraryCount; i++)
				{
					if (banned_raytracing.count((Hash) create_info->pLibraryInfo->pLibraries[i]))
					{
						allow_pipeline = false;
						break;
					}

					if (!has_default_allowed_library &&
					    raytracing_pipelines.count((Hash) create_info->pLibraryInfo->pLibraries[i]))
					{
						has_default_allowed_library = true;
					}

					// At least one of the dependent libraries must be considered allowed to pass.
					if (create_info->stageCount == 0 && allow_pipeline)
						allow_pipeline = has_default_allowed_library;
				}
			}

			// Never include libraries by default unless they contain real code.
			if ((create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0 && create_info->stageCount == 0)
				allow_pipeline = false;
		}

		// Need to defer this since we need to access pipeline libraries.
		if (allow_pipeline)
			raytracing_pipelines[hash] = create_info;
		else if ((create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0)
			library_raytracing_pipelines[hash] = create_info;

		return true;
	}
};

static bool copy_accessed_types(DatabaseInterface &input_db,
                                DatabaseInterface &output_db,
                                vector<uint8_t> &state_json,
                                const unordered_set<Hash> &accessed,
                                ResourceTag tag,
                                unsigned *per_tag_written)
{
	per_tag_written[tag] = accessed.size();
	for (auto hash : accessed)
	{
		size_t compressed_size = 0;
		if (!input_db.read_entry(tag, hash, &compressed_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
		{
			if (tag == RESOURCE_SHADER_MODULE)
			{
				// We did not resolve shader module references, so we might hit an error here, but that's fine.
				LOGE("Reference shader module %016" PRIx64 " does not exist in database.\n", hash);
				continue;
			}
			else
				return false;
		}

		state_json.resize(compressed_size);

		if (!input_db.read_entry(tag, hash, &compressed_size, state_json.data(), PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
			return false;
		if (!output_db.write_entry(tag, hash, state_json.data(), state_json.size(), PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT))
			return false;
	}
	return true;
}

int main(int argc, char *argv[])
{
	CLICallbacks cbs;
	string input_db_path;
	string output_db_path;
	string whitelist, blacklist, timestamp;
	unsigned timestamp_seconds = 0;
	Hash application_hash = 0;
	bool should_filter_application_hash = false;
	bool skip_application_info_links = false;
	bool invert_module_pruning = false;

	unordered_set<Hash> filter_graphics;
	unordered_set<Hash> filter_compute;
	unordered_set<Hash> filter_raytracing;
	unordered_set<Hash> filter_modules;

	unordered_set<Hash> banned_graphics;
	unordered_set<Hash> banned_compute;
	unordered_set<Hash> banned_raytracing;
	unordered_set<Hash> banned_modules;

	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--input-db", [&](CLIParser &parser) { input_db_path = parser.next_string(); });
	cbs.add("--output-db", [&](CLIParser &parser) { output_db_path = parser.next_string(); });
	cbs.add("--filter-application", [&](CLIParser &parser) {
		application_hash = strtoull(parser.next_string(), nullptr, 16);
		should_filter_application_hash = true;
	});
	cbs.add("--filter-graphics", [&](CLIParser &parser) {
		filter_graphics.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--filter-compute", [&](CLIParser &parser) {
		filter_compute.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--filter-raytracing", [&](CLIParser &parser) {
		filter_raytracing.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--filter-module", [&](CLIParser &parser) {
		filter_modules.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--filter-timestamp", [&](CLIParser &parser) {
		timestamp = parser.next_string();
		timestamp_seconds = parser.next_uint();
	});
	cbs.add("--skip-graphics", [&](CLIParser &parser) {
		banned_graphics.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--skip-compute", [&](CLIParser &parser) {
		banned_compute.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--skip-raytracing", [&](CLIParser &parser) {
		banned_raytracing.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--skip-module", [&](CLIParser &parser) {
		banned_modules.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--skip-application-info-links", [&](CLIParser &) {
		skip_application_info_links = true;
	});
	cbs.add("--invert-module-pruning", [&](CLIParser &) {
		invert_module_pruning = true;
	});
	cbs.add("--whitelist", [&](CLIParser &parser) {
		whitelist = parser.next_string();
	});
	cbs.add("--blacklist", [&](CLIParser &parser) {
		blacklist = parser.next_string();
	});
	cbs.error_handler = [] { print_help(); };

	CLIParser parser(std::move(cbs), argc - 1, argv + 1);
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
	auto output_db = std::unique_ptr<DatabaseInterface>(create_database(output_db_path.c_str(), DatabaseMode::OverWrite));

	if (input_db && !whitelist.empty())
	{
		if (!input_db->load_whitelist_database(whitelist.c_str()))
		{
			LOGE("Failed to install whitelist database %s.\n", whitelist.c_str());
			return EXIT_FAILURE;
		}
	}

	if (input_db && !blacklist.empty())
	{
		if (!input_db->load_blacklist_database(blacklist.c_str()))
		{
			LOGE("Failed to install blacklist database %s.\n", blacklist.c_str());
			return EXIT_FAILURE;
		}
	}

	if (!input_db || !input_db->prepare())
	{
		LOGE("Failed to load database: %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	if (!output_db || !output_db->prepare())
	{
		LOGE("Failed to open database for writing: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	StateReplayer replayer;
	PruneReplayer prune_replayer;

	replayer.set_resolve_shader_module_handles(false);

	if (should_filter_application_hash)
	{
		prune_replayer.should_filter_application_hash = true;
		prune_replayer.filter_application_hash = application_hash;
	}

	prune_replayer.filter_graphics = std::move(filter_graphics);
	prune_replayer.filter_compute = std::move(filter_compute);
	prune_replayer.filter_raytracing = std::move(filter_raytracing);
	prune_replayer.filter_modules = std::move(filter_modules);
	prune_replayer.banned_graphics = std::move(banned_graphics);
	prune_replayer.banned_compute = std::move(banned_compute);
	prune_replayer.banned_raytracing = std::move(banned_raytracing);
	prune_replayer.banned_modules = std::move(banned_modules);
	prune_replayer.skip_application_info_links = skip_application_info_links;

	if (!timestamp.empty())
	{
		prune_replayer.timestamp_db.reset(create_stream_archive_database(timestamp.c_str(), DatabaseMode::ReadOnly));
		if (!prune_replayer.timestamp_db->prepare())
		{
			LOGE("Failed to open timestamp DB.\n");
			return EXIT_FAILURE;
		}
		prune_replayer.timestamp_minimum_accept = time(nullptr);
		prune_replayer.timestamp_minimum_accept -=
				std::min<uint64_t>(prune_replayer.timestamp_minimum_accept, timestamp_seconds);
	}

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO,
		RESOURCE_APPLICATION_BLOB_LINK,
		RESOURCE_SHADER_MODULE,
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Implicitly pulls in dependent samplers.
		RESOURCE_PIPELINE_LAYOUT,
		RESOURCE_RENDER_PASS,
		RESOURCE_GRAPHICS_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
		RESOURCE_RAYTRACING_PIPELINE,
	};

	unsigned per_tag_read[RESOURCE_COUNT] = {};
	unsigned per_tag_written[RESOURCE_COUNT] = {};

	static const char *tag_names[] = {
		"AppInfo",
		"Sampler",
		"Descriptor Set Layout",
		"Pipeline Layout",
		"Shader Module",
		"Render Pass",
		"Graphics Pipeline",
		"Compute Pipeline",
		"Application Blob Link",
		"Raytracing Pipeline",
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

		per_tag_read[tag] = hash_count;

		// No need to resolve this type.
		if (tag == RESOURCE_SHADER_MODULE)
			continue;

		vector<Hash> hashes(hash_count);
		if (!input_db->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
		{
			LOGE("Failed to get shader module hashes.\n");
			return EXIT_FAILURE;
		}

		// Filter application infos as well,
		// but avoid a situation where we omit all application infos since these are very important in replay.
		if (tag == RESOURCE_APPLICATION_INFO && prune_replayer.timestamp_db)
		{
			vector<Hash> accepted_hashes;
			uint64_t latest_ts = 0;
			Hash latest_hash = 0;

			for (auto hash : hashes)
			{
				uint64_t ts = 0;
				if (hash == application_hash || prune_replayer.filter_timestamp(RESOURCE_APPLICATION_INFO, hash, &ts))
					accepted_hashes.push_back(hash);

				if (ts > latest_ts)
				{
					latest_hash = hash;
					latest_ts = ts;
				}
			}

			if (accepted_hashes.empty())
				accepted_hashes.push_back(latest_hash);

			hashes = std::move(accepted_hashes);
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

			prune_replayer.has_application_info_for_blob = false;
			prune_replayer.blob_belongs_to_application_info = false;
			if (!replayer.parse(prune_replayer, input_db.get(), state_json.data(), state_json.size()))
				LOGE("Failed to parse blob (tag: %d, hash: 0x%" PRIx64 ").\n", tag, hash);

			if (tag == RESOURCE_APPLICATION_INFO)
			{
				if (!should_filter_application_hash || hash == application_hash)
				{
					size_t compressed_size = 0;
					if (!input_db->read_entry(tag, hash, &compressed_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
						return EXIT_FAILURE;
					state_json.resize(compressed_size);
					if (!input_db->read_entry(tag, hash, &compressed_size, state_json.data(),
					                          PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
						return EXIT_FAILURE;
					if (!output_db->write_entry(tag, hash, state_json.data(), state_json.size(),
					                            PAYLOAD_WRITE_RAW_FOSSILIZE_DB_BIT))
						return EXIT_FAILURE;
					per_tag_written[tag]++;
				}
			}
		}

		if (tag == RESOURCE_GRAPHICS_PIPELINE)
			prune_replayer.access_graphics_pipelines();
		else if (tag == RESOURCE_RAYTRACING_PIPELINE)
			prune_replayer.access_raytracing_pipelines();
	}

	if (invert_module_pruning)
	{
		// In this mode we're only interesting in emitting the shader modules we did not emit for whatever reason.
		// A handy debug option in some scenarios.
		prune_replayer.filtered_blob_hashes[RESOURCE_APPLICATION_BLOB_LINK].clear();
		prune_replayer.accessed_samplers.clear();
		prune_replayer.accessed_descriptor_sets.clear();
		prune_replayer.accessed_render_passes.clear();
		prune_replayer.accessed_pipeline_layouts.clear();
		prune_replayer.accessed_graphics_pipelines.clear();
		prune_replayer.accessed_compute_pipelines.clear();
		prune_replayer.accessed_raytracing_pipelines.clear();

		size_t hash_count = 0;
		if (!input_db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &hash_count, nullptr))
		{
			LOGE("Failed to get hashes.\n");
			return EXIT_FAILURE;
		}

		vector<Hash> hashes(hash_count);
		if (!input_db->get_hash_list_for_resource_tag(RESOURCE_SHADER_MODULE, &hash_count, hashes.data()))
		{
			LOGE("Failed to get shader module hashes.\n");
			return EXIT_FAILURE;
		}

		unordered_set<Hash> unreferenced_modules;
		for (auto &h : hashes)
			if (prune_replayer.accessed_shader_modules.count(h) == 0)
				unreferenced_modules.insert(h);
		prune_replayer.accessed_shader_modules = std::move(unreferenced_modules);
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.filtered_blob_hashes[RESOURCE_APPLICATION_BLOB_LINK],
	                         RESOURCE_APPLICATION_BLOB_LINK,
	                         per_tag_written))
	{
		LOGE("Failed to copy APPLICATION_BLOCK_LINK.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_samplers, RESOURCE_SAMPLER,
	                         per_tag_written))
	{
		LOGE("Failed to copy RESOURCE_SAMPLERs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_descriptor_sets, RESOURCE_DESCRIPTOR_SET_LAYOUT,
	                         per_tag_written))
	{
		LOGE("Failed to copy DESCRIPTOR_SET_LAYOUTs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_shader_modules, RESOURCE_SHADER_MODULE,
	                         per_tag_written))
	{
		LOGE("Failed to copy SHADER_MODULEs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_render_passes, RESOURCE_RENDER_PASS,
	                         per_tag_written))
	{
		LOGE("Failed to copy RENDER_PASSes.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_pipeline_layouts, RESOURCE_PIPELINE_LAYOUT,
	                         per_tag_written))
	{
		LOGE("Failed to copy PIPELINE_LAYOUTs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_graphics_pipelines, RESOURCE_GRAPHICS_PIPELINE,
	                         per_tag_written))
	{
		LOGE("Failed to copy GRAPHICS_PIPELINEs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
	                         prune_replayer.accessed_compute_pipelines, RESOURCE_COMPUTE_PIPELINE,
	                         per_tag_written))
	{
		LOGE("Failed to copy COMPUTE_PIPELINEs.\n");
		return EXIT_FAILURE;
	}

	if (!copy_accessed_types(*input_db, *output_db, state_json,
							 prune_replayer.accessed_raytracing_pipelines, RESOURCE_RAYTRACING_PIPELINE,
							 per_tag_written))
	{
		LOGE("Failed to copy RAYTRACING_PIPELINEs.\n");
		return EXIT_FAILURE;
	}

	for (auto tag : playback_order)
		LOGI("Pruned %s entries: %u -> %u entries\n", tag_names[tag], per_tag_read[tag], per_tag_written[tag]);
}
