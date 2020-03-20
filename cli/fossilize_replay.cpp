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

#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/writer.h"

#include "volk.h"
#include "device.hpp"
#include "fossilize.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "file.hpp"
#include "path.hpp"
#include "fossilize_db.hpp"
#include "fossilize_external_replayer.hpp"
#include "fossilize_external_replayer_control_block.hpp"
#include "fossilize_errors.hpp"
#include "util/object_cache.hpp"

#include <string>
#include <unordered_set>
#include <stdlib.h>
#include <string.h>
#include <chrono>	// VALVE
#include <queue>	// VALVE
#include <thread>	// VALVE
#include <mutex>	// VALVE
#include <condition_variable> // VALVE
#include <atomic>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <utility>
#include <map>
#include <assert.h>

#ifdef FOSSILIZE_REPLAYER_SPIRV_VAL
#include "spirv-tools/libspirv.hpp"
#endif

using namespace Fossilize;
using namespace std;

//#define SIMULATE_UNSTABLE_DRIVER
//#define SIMULATE_SPURIOUS_DEADLOCK

#ifdef SIMULATE_UNSTABLE_DRIVER
#include <random>
#ifdef _WIN32
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void simulate_crash(int *v)
{
	*v = 0;
}

#ifdef _WIN32
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static int simulate_divide_by_zero(int a, int b)
{
	return a / b;
}

static int simulate_stack_overflow()
{
	volatile char buffer[16 * 1024 * 1024];
	for (auto &b : buffer)
		b++;
	return buffer[6124];
}

void spurious_crash()
{
	std::uniform_int_distribution<int> dist(0, 15);
	auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	std::mt19937 rnd(ns);

	// Simulate fatal things like segfaults and aborts.
	int r = dist(rnd);

	if (r < 1)
	{
		LOGE("Simulating a crash ...\n");
		simulate_crash(nullptr);
		LOGE("Should not reach here ...\n");
	}

	if (r < 2)
	{
		LOGE("Simulating an abort ...\n");
		abort();
		LOGE("Should not reach here ...\n");
	}

	if (r < 3)
	{
		LOGE("Simulating divide by zero ...\n");
		r = simulate_divide_by_zero(1, 0);
		LOGE("Should not reach here ... Boop: %d\n", r);
	}

	if (r < 4)
	{
		LOGE("Creating a stack overflow ...\n");
		r = simulate_stack_overflow();
		LOGE("Should not reach here ... Boop: %d\n", r);
	}
}

void spurious_deadlock()
{
#ifdef SIMULATE_SPURIOUS_DEADLOCK
	std::uniform_int_distribution<int> dist(0, 15);
	auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	std::mt19937 rnd(ns);

	if (dist(rnd) < 4)
	{
		LOGE("Simulating a deadlock ...\n");
		std::this_thread::sleep_for(std::chrono::seconds(100));
	}
#endif
}
#endif

// Unstable, but deterministic.
template <typename BidirectionalItr, typename UnaryPredicate>
static BidirectionalItr unstable_remove_if(BidirectionalItr first, BidirectionalItr last, UnaryPredicate &&p)
{
	while (first != last)
	{
		if (p(*first))
		{
			--last;
			std::swap(*first, *last);
		}
		else
			++first;
	}

	return first;
}

static unique_ptr<DatabaseInterface> create_database(const vector<const char *> &databases)
{
	unique_ptr<DatabaseInterface> resolver;
	if (databases.size() == 1)
	{
		resolver.reset(create_database(databases.front(), DatabaseMode::ReadOnly));
	}
	else
	{
		resolver.reset(create_concurrent_database(nullptr, DatabaseMode::ReadOnly,
		                                          databases.data(), databases.size()));
	}
	return resolver;
}

namespace Global
{
static thread_local unsigned worker_thread_index;
}

enum MemoryConstants
{
	NUM_MEMORY_CONTEXTS = 4,
	SHADER_MODULE_MEMORY_CONTEXT = NUM_MEMORY_CONTEXTS - 1,
	PARENT_PIPELINE_MEMORY_CONTEXT = NUM_MEMORY_CONTEXTS - 2,
	NUM_PIPELINE_MEMORY_CONTEXTS = NUM_MEMORY_CONTEXTS - 2
};

struct EnqueuedWork
{
	unsigned order_index;
	std::function<void()> func;
};

static void on_validation_error(void *userdata);
#ifndef NO_ROBUST_REPLAYER
static void timeout_handler();
#endif

struct ThreadedReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;
		bool spirv_validate = false;
		bool ignore_derived_pipelines = false;
		bool pipeline_stats = false;
		string on_disk_pipeline_cache_path;
		string on_disk_validation_cache_path;
		string on_disk_validation_whitelist_path;
		string on_disk_validation_blacklist_path;
		string pipeline_stats_path;

		// VALVE: Add multi-threaded pipeline creation
		unsigned num_threads = thread::hardware_concurrency();

		// VALVE: --loop option for testing performance
		unsigned loop_count = 1;

		unsigned shader_cache_size_mb = 256;

		// Carve out a range of which pipelines to replay.
		// Used for multi-process replays where each process gets its own slice to churn through.
		unsigned start_graphics_index = 0;
		unsigned end_graphics_index = ~0u;
		unsigned start_compute_index = 0;
		unsigned end_compute_index = ~0u;

		SharedControlBlock *control_block = nullptr;

		void (*on_thread_callback)(void *userdata) = nullptr;
		void *on_thread_callback_userdata = nullptr;
		void (*on_validation_error_callback)(ThreadedReplayer *) = nullptr;

		unsigned timeout_seconds = 0;
	};

	struct DeferredGraphicsInfo
	{
		VkGraphicsPipelineCreateInfo *info;
		Hash hash;
		VkPipeline *pipeline;
		unsigned index;

		static ResourceTag get_tag() { return RESOURCE_GRAPHICS_PIPELINE; }
	};

	struct DeferredComputeInfo
	{
		VkComputePipelineCreateInfo *info;
		Hash hash;
		VkPipeline *pipeline;
		unsigned index;

		static ResourceTag get_tag() { return RESOURCE_COMPUTE_PIPELINE; }
	};

	struct PipelineWorkItem
	{
		Hash hash = 0;
		ResourceTag tag = RESOURCE_COUNT;
		unsigned index = 0;
		unsigned memory_context_index = 0;
		bool parse_only = false;
		bool force_outside_range = false;

		union
		{
			const VkGraphicsPipelineCreateInfo *graphics_create_info;
			const VkComputePipelineCreateInfo *compute_create_info;
			const VkShaderModuleCreateInfo *shader_module_create_info;
		} create_info = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} output = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} hash_map_entry = {};
	};

	struct PerThreadData
	{
		StateReplayer *per_thread_replayers = nullptr;
		unsigned current_parse_index = ~0u;
		unsigned current_graphics_index = ~0u;
		unsigned current_compute_index = ~0u;
		unsigned memory_context_index = 0;

		Hash current_graphics_pipeline = 0;
		Hash current_compute_pipeline = 0;
		Hash failed_module_hashes[16] = {};
		unsigned num_failed_module_hashes = 0;

		bool force_outside_range = false;
		bool triggered_validation_error = false;
	};

	ThreadedReplayer(const VulkanDevice::Options &device_opts_, const Options &opts_)
		: opts(opts_),
		  num_worker_threads(opts.num_threads), loop_count(opts.loop_count),
		  device_opts(device_opts_)
	{
		// Cannot use initializers for atomics.
		graphics_pipeline_ns.store(0);
		compute_pipeline_ns.store(0);
		shader_module_ns.store(0);
		graphics_pipeline_count.store(0);
		compute_pipeline_count.store(0);
		shader_module_count.store(0);
		shader_module_evicted_count.store(0);
		thread_total_ns.store(0);
		total_idle_ns.store(0);
		total_peak_memory.store(0);
		pipeline_cache_hits.store(0);
		pipeline_cache_misses.store(0);

		shader_module_total_compressed_size.store(0);
		shader_module_total_size.store(0);
		per_thread_data.resize(num_worker_threads + 1);

		// Could potentially overflow on 32-bit.
#if ((SIZE_MAX / (1024 * 1024)) < UINT_MAX)
		size_t target_size;
		if (opts.shader_cache_size_mb <= (SIZE_MAX / (1024 * 1024)))
			target_size = size_t(opts.shader_cache_size_mb) * 1024 * 1024;
		else
			target_size = SIZE_MAX;
#else
		size_t target_size = size_t(opts.shader_cache_size_mb) * 1024 * 1024;
#endif

		shader_modules.set_target_size(target_size);
		init_whitelist_db();
		init_blacklist_db();
	}

	PerThreadData &get_per_thread_data()
	{
		return per_thread_data[Global::worker_thread_index];
	}

	void init_whitelist_db()
	{
		if (!opts.on_disk_validation_whitelist_path.empty())
		{
			validation_whitelist_db.reset(create_concurrent_database(opts.on_disk_validation_whitelist_path.c_str(), DatabaseMode::Append, nullptr, 0));
			if (!validation_whitelist_db->prepare())
			{
				LOGE("Could not open validation whitelist DB. Ignoring.\n");
				validation_whitelist_db.reset();
			}
		}
	}

	void init_blacklist_db()
	{
		if (!opts.on_disk_validation_blacklist_path.empty())
		{
			validation_blacklist_db.reset(create_concurrent_database(opts.on_disk_validation_blacklist_path.c_str(), DatabaseMode::Append, nullptr, 0));
			if (!validation_blacklist_db->prepare())
			{
				LOGE("Could not open validation blacklist DB. Ignoring.\n");
				validation_blacklist_db.reset();
			}
		}
	}

	void start_worker_threads()
	{
		thread_initialized_count = 0;

		// Make sure main thread sees degenerate current_*_index. Any crash in main thread is fatal.
		for (unsigned i = 0; i < num_worker_threads; i++)
		{
			auto &d = per_thread_data[i + 1];
			d.current_graphics_index = opts.start_graphics_index;
			d.current_compute_index = opts.start_compute_index;
		}

		// Create a thread pool with the # of specified worker threads (defaults to thread::hardware_concurrency()).
		for (unsigned i = 0; i < num_worker_threads; i++)
			thread_pool.push_back(std::thread(&ThreadedReplayer::worker_thread, this, i + 1));

		// Make sure all threads have started so we can poke around the per thread allocators from
		// the main thread when the memory contexts in each thread have been drained.
		{
			unique_lock<mutex> holder(pipeline_work_queue_mutex);
			work_done_condition[0].wait(holder, [&]() -> bool {
				return thread_initialized_count == num_worker_threads;
			});
		}
	}

	void sync_worker_threads()
	{
		for (unsigned i = 0; i < NUM_MEMORY_CONTEXTS; i++)
			sync_worker_memory_context(i);
	}

	void sync_worker_memory_context(unsigned index)
	{
		assert(index < NUM_MEMORY_CONTEXTS);
		unique_lock<mutex> lock(pipeline_work_queue_mutex);

		if (queued_count[index] == completed_count[index])
			return;

		if (opts.timeout_seconds != 0)
		{
			bool signalled;
			unsigned current_completed = completed_count[index];
			do
			{
				signalled = work_done_condition[index].wait_for(lock, std::chrono::seconds(opts.timeout_seconds),
				                                                [&]() -> bool
				                                                {
					                                                return current_completed != completed_count[index];
				                                                });
				if (!signalled && completed_count[index] == current_completed)
				{
#ifndef NO_ROBUST_REPLAYER
					timeout_handler();
#else
					LOGE("Timed out replaying pipelines!\n");
					exit(2);
#endif
				}

				current_completed = completed_count[index];
			} while (queued_count[index] != completed_count[index]);
		}
		else
		{
			work_done_condition[index].wait(lock, [&]() -> bool
			{
				return queued_count[index] == completed_count[index];
			});
		}
	}

	bool run_parse_work_item(StateReplayer &replayer, vector<uint8_t> &buffer, const PipelineWorkItem &work_item)
	{
		size_t json_size = 0;
		if (!global_database->read_entry(work_item.tag, work_item.hash, &json_size, nullptr, PAYLOAD_READ_CONCURRENT_BIT))
		{
			LOGE("Failed to read entry (%u: %016" PRIx64 ")\n", unsigned(work_item.tag), work_item.hash);
			return false;
		}

		buffer.resize(json_size);

		if (!global_database->read_entry(work_item.tag, work_item.hash, &json_size, buffer.data(), PAYLOAD_READ_CONCURRENT_BIT))
		{
			LOGE("Failed to read entry (%u: %016" PRIx64 ")\n", unsigned(work_item.tag), work_item.hash);
			return false;
		}

		auto &per_thread = get_per_thread_data();
		per_thread.current_parse_index = work_item.index;
		per_thread.force_outside_range = work_item.force_outside_range;
		per_thread.memory_context_index = work_item.memory_context_index;

		if (!replayer.parse(*this, global_database, buffer.data(), buffer.size()))
		{
			LOGE("Failed to parse blob (tag: %d, hash: 0x%016" PRIx64 ").\n", work_item.tag, work_item.hash);

			// If we failed to parse, we need to at least clear out the state to something sensible.
			unsigned index = per_thread.current_parse_index;
			unsigned memory_index = per_thread.memory_context_index;
			bool force_outside_range = per_thread.force_outside_range;
			if (!force_outside_range)
			{
				if (work_item.tag == RESOURCE_GRAPHICS_PIPELINE)
				{
					assert(index < deferred_graphics[memory_index].size());
					deferred_graphics[memory_index][index] = {};
				}
				else if (work_item.tag == RESOURCE_COMPUTE_PIPELINE)
				{
					assert(index < deferred_compute[memory_index].size());
					deferred_compute[memory_index][index] = {};
				}
			}
		}

		if (work_item.tag == RESOURCE_SHADER_MODULE)
		{
			// No reason to retain memory in this allocator anymore.
			replayer.get_allocator().reset();

			// Feed shader module statistics.
			shader_module_total_size.fetch_add(json_size, std::memory_order_relaxed);
			if (global_database->read_entry(work_item.tag, work_item.hash, &json_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
				shader_module_total_compressed_size.fetch_add(json_size, std::memory_order_relaxed);
		}

		return true;
	}

	void get_pipeline_stats(ResourceTag tag, Hash hash, VkPipeline pipeline)
	{
		VkPipelineInfoKHR pipeline_info = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR };
		pipeline_info.pipeline = pipeline;

		uint32_t pe_count = 0;
		if (vkGetPipelineExecutablePropertiesKHR(device->get_device(), &pipeline_info, &pe_count, nullptr) != VK_SUCCESS)
			return;

		if (pe_count > 0)
		{
			rapidjson::Document doc;
			doc.SetObject();
			auto &alloc = doc.GetAllocator();

			std::string db_path = global_database->get_db_path_for_hash(tag, hash);

			char hash_str[17];
			snprintf(hash_str, sizeof(hash_str), "%016" PRIx64, hash);

			doc.AddMember("db_path", db_path, alloc);
			doc.AddMember("pipeline", std::string(hash_str), alloc);
			doc.AddMember("pipeline_type", std::string(tag == RESOURCE_GRAPHICS_PIPELINE ? "GRAPHICS" : "COMPUTE"), alloc);

			rapidjson::Value execs(rapidjson::kArrayType);
			vector<VkPipelineExecutablePropertiesKHR> pipe_executables(pe_count);
			if (vkGetPipelineExecutablePropertiesKHR(device->get_device(), &pipeline_info, &pe_count, pipe_executables.data()) != VK_SUCCESS)
				return;

			for (uint32_t exec = 0; exec < pe_count; exec++)
			{
				rapidjson::Value pe(rapidjson::kObjectType);
				pe.AddMember("executable_name", rapidjson::StringRef(pipe_executables[exec].name), alloc);

				rapidjson::Value pe_stats(rapidjson::kArrayType);

				uint32_t stat_count = 0;
				VkPipelineExecutableInfoKHR exec_info = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
				exec_info.pipeline = pipeline;
				exec_info.executableIndex = exec;

				if (vkGetPipelineExecutableStatisticsKHR(device->get_device(), &exec_info, &stat_count, nullptr) != VK_SUCCESS)
					continue;

				if (stat_count > 0)
				{
					vector<VkPipelineExecutableStatisticKHR> stats(stat_count);
					if (vkGetPipelineExecutableStatisticsKHR(device->get_device(), &exec_info, &stat_count, stats.data()) != VK_SUCCESS)
						continue;

					for (auto &st : stats)
					{
						rapidjson::Value stat(rapidjson::kObjectType);

						stat.AddMember("name", std::string(st.name), alloc);
						switch (st.format)
						{
						case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
							stat.AddMember("value", std::string(st.value.b32 == VK_TRUE ? "true" : "false"), alloc);
							break;
						case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
							stat.AddMember("value", std::to_string(st.value.i64), alloc);
							break;
						case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
							stat.AddMember("value", std::to_string(st.value.u64), alloc);
							break;
						case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
							stat.AddMember("value", std::to_string(st.value.f64), alloc);
							break;
						default:
							LOGE("Unhandled format: %d", st.format);
							break;
						}
						pe_stats.PushBack(stat, alloc);
					}
				}
				pe.AddMember("stats", pe_stats, alloc);
				execs.PushBack(pe, alloc);
			}
			doc.AddMember("executables", execs, alloc);

			rapidjson::StringBuffer buffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
			doc.Accept(writer);

			if (pipeline_stats_db)
			{
				// Need lock here because multiple threads could be in flight.
				lock_guard<mutex> lock(pipeline_stats_queue_mutex);

				if (pipeline_stats_db->write_entry(tag, hash, buffer.GetString(), buffer.GetLength(), 0))
					pipeline_stats_db->flush();
				else
					LOGE("Failed to write pipeline stats entry to database.\n");
			}
		}
	}

	void blacklist_resource(ResourceTag tag, Hash hash)
	{
		if (validation_blacklist_db)
		{
			lock_guard<mutex> holder{validation_db_mutex};
			validation_blacklist_db->write_entry(tag, hash, nullptr, 0, 0);
		}
	}

	void whitelist_resource(ResourceTag tag, Hash hash)
	{
		if (validation_whitelist_db)
		{
			lock_guard<mutex> holder{validation_db_mutex};
			validation_whitelist_db->write_entry(tag, hash, nullptr, 0, 0);
		}
	}

	bool resource_is_blacklisted(ResourceTag tag, Hash hash)
	{
		if (validation_blacklist_db)
		{
			lock_guard<mutex> holder{validation_db_mutex};
			return validation_blacklist_db->has_entry(tag, hash);
		}
		else
			return false;
	}

	void run_creation_work_item(const PipelineWorkItem &work_item)
	{
		switch (work_item.tag)
		{
		case RESOURCE_GRAPHICS_PIPELINE:
		{
			auto &per_thread = get_per_thread_data();
			per_thread.current_graphics_index = work_item.index + 1;
			per_thread.current_graphics_pipeline = work_item.hash;
			per_thread.current_compute_pipeline = 0;
			per_thread.triggered_validation_error = false;

			// Make sure to iterate the index so main thread and worker threads
			// have a coherent idea of replayer state.
			if (!work_item.create_info.graphics_create_info)
			{
				if (opts.control_block)
					opts.control_block->skipped_graphics.fetch_add(1, std::memory_order_relaxed);
				break;
			}

			if (robustness)
			{
				per_thread.num_failed_module_hashes = work_item.create_info.graphics_create_info->stageCount;
				for (unsigned i = 0; i < work_item.create_info.graphics_create_info->stageCount; i++)
				{
					VkShaderModule module = work_item.create_info.graphics_create_info->pStages[i].module;
					per_thread.failed_module_hashes[i] = shader_module_to_hash[module];
				}
			}

			if ((work_item.create_info.graphics_create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0)
			{
				// This pipeline failed for some reason, don't try to compile this one either.
				if (work_item.create_info.graphics_create_info->basePipelineHandle == VK_NULL_HANDLE)
				{
					*work_item.output.pipeline = VK_NULL_HANDLE;
					LOGE("Invalid derivative pipeline!\n");
					break;
				}
			}

			// Don't bother replaying blacklisted objects.
			if (resource_is_blacklisted(work_item.tag, work_item.hash))
			{
				*work_item.output.pipeline = VK_NULL_HANDLE;
				LOGE("Resource is blacklisted, ignoring.\n");
				if (opts.control_block)
					opts.control_block->skipped_graphics.fetch_add(1, std::memory_order_relaxed);
				break;
			}

			if (!device->get_feature_filter().graphics_pipeline_is_supported(work_item.create_info.graphics_create_info))
			{
				*work_item.output.pipeline = VK_NULL_HANDLE;
				LOGE("Graphics pipeline %016" PRIx64 " is not supported by current device, skipping.\n", work_item.hash);
				if (opts.control_block)
					opts.control_block->skipped_graphics.fetch_add(1, std::memory_order_relaxed);
				break;
			}

			for (unsigned i = 0; i < loop_count; i++)
			{
				// Avoid leak.
				if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(device->get_device(), *work_item.hash_map_entry.pipeline, nullptr);
				*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

				auto start_time = chrono::steady_clock::now();

#ifdef SIMULATE_UNSTABLE_DRIVER
				spurious_crash();
#endif

				VkPipelineCreationFeedbackEXT feedbacks[16] = {};
				VkPipelineCreationFeedbackEXT primary_feedback = {};
				VkPipelineCreationFeedbackCreateInfoEXT feedback = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT };
				feedback.pipelineStageCreationFeedbackCount = work_item.create_info.graphics_create_info->stageCount;
				feedback.pPipelineStageCreationFeedbacks = feedbacks;
				feedback.pPipelineCreationFeedback = &primary_feedback;

				if (opts.pipeline_cache && device->pipeline_feedback_enabled())
					const_cast<VkGraphicsPipelineCreateInfo *>(work_item.create_info.graphics_create_info)->pNext = &feedback;

				if (vkCreateGraphicsPipelines(device->get_device(), pipeline_cache, 1, work_item.create_info.graphics_create_info,
				                              nullptr, work_item.output.pipeline) == VK_SUCCESS)
				{
					auto end_time = chrono::steady_clock::now();
					auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

					graphics_pipeline_ns.fetch_add(duration_ns, std::memory_order_relaxed);
					graphics_pipeline_count.fetch_add(1, std::memory_order_relaxed);

					if (opts.pipeline_stats && i == 0)
						get_pipeline_stats(work_item.tag, work_item.hash, *work_item.output.pipeline);

					if (!opts.ignore_derived_pipelines && (work_item.create_info.graphics_create_info->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) != 0)
					{
						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;
					}
					else
					{
						// Destroy the pipeline right away to save memory if we don't need it for purposes of creating derived pipelines later.
						*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;
						vkDestroyPipeline(device->get_device(), *work_item.output.pipeline, nullptr);
						*work_item.output.pipeline = VK_NULL_HANDLE;
					}

					if (opts.control_block && i == 0)
						opts.control_block->successful_graphics.fetch_add(1, std::memory_order_relaxed);

					if (opts.pipeline_cache && i == 0 && (primary_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0)
					{
						bool cache_hit = (primary_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT) != 0;

						// Check per-stage feedback.
						if (!cache_hit)
						{
							cache_hit = true;
							for (uint32_t j = 0; j < feedback.pipelineStageCreationFeedbackCount; j++)
							{
								bool valid = (feedbacks[j].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0;
								bool hit = (feedbacks[j].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT) != 0;
								if (!valid || !hit)
									cache_hit = false;
							}
						}

						if (cache_hit)
							pipeline_cache_hits.fetch_add(1, std::memory_order_relaxed);
						else
							pipeline_cache_misses.fetch_add(1, std::memory_order_relaxed);
					}
				}
				else
				{
					LOGE("Failed to create graphics pipeline for hash 0x%016" PRIx64 ".\n", work_item.hash);
				}
			}

			if (device_opts.enable_validation && !per_thread.triggered_validation_error)
				whitelist_resource(work_item.tag, work_item.hash);

			per_thread.current_graphics_pipeline = 0;
			per_thread.current_compute_pipeline = 0;
			break;
		}

		case RESOURCE_COMPUTE_PIPELINE:
		{
			auto &per_thread = get_per_thread_data();
			per_thread.current_compute_index = work_item.index + 1;
			per_thread.current_compute_pipeline = work_item.hash;
			per_thread.current_graphics_pipeline = 0;
			per_thread.triggered_validation_error = false;

			// Make sure to iterate the index so main thread and worker threads
			// have a coherent idea of replayer state.
			if (!work_item.create_info.compute_create_info)
			{
				if (opts.control_block)
					opts.control_block->skipped_compute.fetch_add(1, std::memory_order_relaxed);
				break;
			}

			if (robustness)
			{
				per_thread.num_failed_module_hashes = 1;
				VkShaderModule module = work_item.create_info.compute_create_info->stage.module;
				per_thread.failed_module_hashes[0] = shader_module_to_hash[module];
			}

			if ((work_item.create_info.compute_create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0)
			{
				// This pipeline failed for some reason, don't try to compile this one either.
				if (work_item.create_info.compute_create_info->basePipelineHandle == VK_NULL_HANDLE)
				{
					*work_item.output.pipeline = VK_NULL_HANDLE;
					break;
				}
			}

			// Don't bother replaying blacklisted objects.
			if (resource_is_blacklisted(work_item.tag, work_item.hash))
			{
				*work_item.output.pipeline = VK_NULL_HANDLE;
				LOGE("Resource is blacklisted, ignoring.\n");
				if (opts.control_block)
					opts.control_block->skipped_compute.fetch_add(1, std::memory_order_relaxed);
				break;
			}

			if (!device->get_feature_filter().compute_pipeline_is_supported(work_item.create_info.compute_create_info))
			{
				*work_item.output.pipeline = VK_NULL_HANDLE;
				LOGE("Compute pipeline %016" PRIx64 " is not supported by current device, skipping.\n", work_item.hash);
				if (opts.control_block)
					opts.control_block->skipped_compute.fetch_add(1, std::memory_order_relaxed);
				break;
			}

			for (unsigned i = 0; i < loop_count; i++)
			{
				// Avoid leak.
				if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
					vkDestroyPipeline(device->get_device(), *work_item.hash_map_entry.pipeline, nullptr);
				*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

				auto start_time = chrono::steady_clock::now();

#ifdef SIMULATE_UNSTABLE_DRIVER
				spurious_crash();
#endif
				VkPipelineCreationFeedbackEXT feedbacks = {};
				VkPipelineCreationFeedbackEXT primary_feedback = {};
				VkPipelineCreationFeedbackCreateInfoEXT feedback = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT };
				feedback.pipelineStageCreationFeedbackCount = 1;
				feedback.pPipelineStageCreationFeedbacks = &feedbacks;
				feedback.pPipelineCreationFeedback = &primary_feedback;

				if (opts.pipeline_cache && device->pipeline_feedback_enabled())
					const_cast<VkComputePipelineCreateInfo *>(work_item.create_info.compute_create_info)->pNext = &feedback;

				if (vkCreateComputePipelines(device->get_device(), pipeline_cache, 1,
				                             work_item.create_info.compute_create_info,
				                             nullptr, work_item.output.pipeline) == VK_SUCCESS)
				{
					auto end_time = chrono::steady_clock::now();
					auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

					compute_pipeline_ns.fetch_add(duration_ns, std::memory_order_relaxed);
					compute_pipeline_count.fetch_add(1, std::memory_order_relaxed);

					if (opts.pipeline_stats && i == 0)
						get_pipeline_stats(work_item.tag, work_item.hash, *work_item.output.pipeline);

					if (!opts.ignore_derived_pipelines && (work_item.create_info.compute_create_info->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) != 0)
					{
						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;
					}
					else
					{
						// Destroy the pipeline right away to save memory if we don't need it for purposes of creating derived pipelines later.
						*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;
						vkDestroyPipeline(device->get_device(), *work_item.output.pipeline, nullptr);
						*work_item.output.pipeline = VK_NULL_HANDLE;
					}

					if (opts.control_block && i == 0)
						opts.control_block->successful_compute.fetch_add(1, std::memory_order_relaxed);

					if (opts.pipeline_cache && i == 0 && (primary_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0)
					{
						bool cache_hit = (primary_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT) != 0;

						if (!cache_hit)
						{
							bool valid = (feedbacks.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0;
							bool hit = (feedbacks.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT) != 0;
							cache_hit = valid && hit;
						}

						if (cache_hit)
							pipeline_cache_hits.fetch_add(1, std::memory_order_relaxed);
						else
							pipeline_cache_misses.fetch_add(1, std::memory_order_relaxed);
					}
				}
				else
				{
					LOGE("Failed to create compute pipeline for hash 0x%016" PRIx64 ".\n", work_item.hash);
				}
			}

			if (device_opts.enable_validation && !per_thread.triggered_validation_error)
				whitelist_resource(work_item.tag, work_item.hash);

			per_thread.current_compute_pipeline = 0;
			per_thread.current_graphics_pipeline = 0;
			break;
		}

		default:
			break;
		}
	}

	void worker_thread(unsigned thread_index)
	{
		Global::worker_thread_index = thread_index;

		if (opts.on_thread_callback)
			opts.on_thread_callback(opts.on_thread_callback_userdata);

		uint64_t idle_ns = 0;
		auto thread_start_time = chrono::steady_clock::now();

		// Pipelines and shader modules are decompressed and parsed in the worker threads.
		// Inherit references to the trivial modules.
		StateReplayer per_thread_replayer[NUM_MEMORY_CONTEXTS];
		for (auto &r : per_thread_replayer)
		{
			r.set_resolve_derivative_pipeline_handles(false);
			r.set_resolve_shader_module_handles(false);
			r.copy_handle_references(*global_replayer);
		}

		get_per_thread_data().per_thread_replayers = per_thread_replayer;
		// Let main thread know that the per thread replayers have been initialized correctly.
		{
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			thread_initialized_count++;
			work_done_condition[0].notify_one();
		}

		vector<uint8_t> json_buffer;

		for (;;)
		{
			PipelineWorkItem work_item;
			auto idle_start_time = chrono::steady_clock::now();
			{
				unique_lock<mutex> lock(pipeline_work_queue_mutex);
				work_available_condition.wait(lock, [&]() -> bool {
					return shutting_down || !pipeline_work_queue.empty();
				});

				if (shutting_down)
					break;

				work_item = pipeline_work_queue.front();
				pipeline_work_queue.pop();
			}

			auto idle_end_time = chrono::steady_clock::now();
			auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(idle_end_time - idle_start_time).count();
			idle_ns += duration_ns;

			if (work_item.parse_only)
				run_parse_work_item(per_thread_replayer[work_item.memory_context_index], json_buffer, work_item);
			else
				run_creation_work_item(work_item);

			idle_start_time = chrono::steady_clock::now();
			{
				unsigned context_index = work_item.memory_context_index;
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				completed_count[context_index]++;

				// Makes sense to signal main thread now.
				// If we have a timeout, we need to keep the dispatcher thread aware of the progress,
				// so wake it up after each work item is complete.
				if (opts.timeout_seconds != 0 || (completed_count[context_index] == queued_count[context_index]))
					work_done_condition[context_index].notify_one();
			}

			idle_end_time = chrono::steady_clock::now();
			duration_ns = chrono::duration_cast<chrono::nanoseconds>(idle_end_time - idle_start_time).count();
			idle_ns += duration_ns;
		}

		total_idle_ns.fetch_add(idle_ns, std::memory_order_relaxed);
		auto thread_end_time = chrono::steady_clock::now();
		thread_total_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(thread_end_time - thread_start_time).count(),
		                          std::memory_order_relaxed);

		size_t peak_memory = 0;
		for (auto &r : per_thread_replayer)
			peak_memory += r.get_allocator().get_peak_memory_consumption();

		total_peak_memory.fetch_add(peak_memory, std::memory_order_relaxed);
	}

	void flush_pipeline_cache()
	{
		if (device && pipeline_cache)
		{
			if (!opts.on_disk_pipeline_cache_path.empty())
			{
				size_t pipeline_cache_size = 0;
				if (vkGetPipelineCacheData(device->get_device(), pipeline_cache, &pipeline_cache_size, nullptr) == VK_SUCCESS)
				{
					vector<uint8_t> pipeline_buffer(pipeline_cache_size);
					if (vkGetPipelineCacheData(device->get_device(), pipeline_cache, &pipeline_cache_size, pipeline_buffer.data()) == VK_SUCCESS)
					{
						// This isn't safe to do in a signal handler, but it's unlikely to be a problem in practice.
						FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "wb");
						if (!file || fwrite(pipeline_buffer.data(), 1, pipeline_buffer.size(), file) != pipeline_buffer.size())
							LOGE("Failed to write pipeline cache data to disk.\n");
						if (file)
							fclose(file);
					}
				}
			}
			vkDestroyPipelineCache(device->get_device(), pipeline_cache, nullptr);
			pipeline_cache = VK_NULL_HANDLE;
		}
	}

	void flush_validation_cache()
	{
		if (device && validation_cache)
		{
			if (!opts.on_disk_validation_cache_path.empty())
			{
				size_t validation_cache_size = 0;
				if (vkGetValidationCacheDataEXT(device->get_device(), validation_cache, &validation_cache_size, nullptr) == VK_SUCCESS)
				{
					vector<uint8_t> validation_buffer(validation_cache_size);
					if (vkGetValidationCacheDataEXT(device->get_device(), validation_cache, &validation_cache_size, validation_buffer.data()) == VK_SUCCESS)
					{
						// This isn't safe to do in a signal handler, but it's unlikely to be a problem in practice.
						FILE *file = fopen(opts.on_disk_validation_cache_path.c_str(), "wb");
						if (!file || fwrite(validation_buffer.data(), 1, validation_buffer.size(), file) != validation_buffer.size())
							LOGE("Failed to write pipeline cache data to disk.\n");
						if (file)
							fclose(file);
					}
				}
			}
			vkDestroyValidationCacheEXT(device->get_device(), validation_cache, nullptr);
			validation_cache = VK_NULL_HANDLE;
		}
	}

	void tear_down_threads()
	{
		// Signal that it's time for threads to die.
		{
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			shutting_down = true;
			work_available_condition.notify_all();
		}

		for (auto &thread : thread_pool)
			if (thread.joinable())
				thread.join();
		thread_pool.clear();
	}

	~ThreadedReplayer()
	{
		tear_down_threads();
		flush_pipeline_cache();
		flush_validation_cache();

		for (auto &sampler : samplers)
			if (sampler.second)
				vkDestroySampler(device->get_device(), sampler.second, nullptr);
		for (auto &layout : layouts)
			if (layout.second)
				vkDestroyDescriptorSetLayout(device->get_device(), layout.second, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout.second)
				vkDestroyPipelineLayout(device->get_device(), pipeline_layout.second, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass.second)
				vkDestroyRenderPass(device->get_device(), render_pass.second, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);

		shader_modules.delete_cache([this](Hash, VkShaderModule module) {
			if (module != VK_NULL_HANDLE)
				vkDestroyShaderModule(device->get_device(), module, nullptr);
		});
	}

	bool validate_validation_cache_header(const vector<uint8_t> &blob) const
	{
		if (blob.size() < 8 + VK_UUID_SIZE)
		{
			LOGE("Validation cache header is too small.\n");
			return false;
		}

		const auto read_le = [&](unsigned offset) -> uint32_t {
			return uint32_t(blob[offset + 0]) |
			       (uint32_t(blob[offset + 1]) << 8) |
			       (uint32_t(blob[offset + 2]) << 16) |
			       (uint32_t(blob[offset + 3]) << 24);
		};

		if (read_le(4) != VK_VALIDATION_CACHE_HEADER_VERSION_ONE_EXT)
			return false;

		// Doesn't seem to be a way to get the UUID, but layer should reject mismatches.
		return true;
	}

	bool validate_pipeline_cache_header(const vector<uint8_t> &blob) const
	{
		if (blob.size() < 16 + VK_UUID_SIZE)
		{
			LOGI("Pipeline cache header is too small.\n");
			return false;
		}

		const auto read_le = [&](unsigned offset) -> uint32_t {
			return uint32_t(blob[offset + 0]) |
				(uint32_t(blob[offset + 1]) << 8) |
				(uint32_t(blob[offset + 2]) << 16) |
				(uint32_t(blob[offset + 3]) << 24);
		};

		auto length = read_le(0);
		if (length != 16 + VK_UUID_SIZE)
		{
			LOGI("Length of pipeline cache header is not as expected.\n");
			return false;
		}

		auto version = read_le(4);
		if (version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
		{
			LOGI("Version of pipeline cache header is not 1.\n");
			return false;
		}

		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(device->get_gpu(), &props);
		if (props.vendorID != read_le(8))
		{
			LOGI("Mismatch of vendorID and cache vendorID.\n");
			return false;
		}

		if (props.deviceID != read_le(12))
		{
			LOGI("Mismatch of deviceID and cache deviceID.\n");
			return false;
		}

		if (memcmp(props.pipelineCacheUUID, blob.data() + 16, VK_UUID_SIZE) != 0)
		{
			LOGI("Mismatch between pipelineCacheUUID.\n");
			return false;
		}

		return true;
	}

	void set_application_info(Hash, const VkApplicationInfo *app, const VkPhysicalDeviceFeatures2 *features) override
	{
		if (device_was_init)
			sync_threads();

		if (1 || !device_was_init)
		{
			// Now we can init the device with correct app info.
			device_was_init = true;
			device.reset(new VulkanDevice);
			device_opts.application_info = app;
			device_opts.features = features;
			device_opts.want_pipeline_stats = opts.pipeline_stats;
			auto start_device = chrono::steady_clock::now();
			if (!device->init_device(device_opts))
			{
				LOGE("Failed to create Vulkan device, bailing ...\n");
				exit(EXIT_FAILURE);
			}

			device->set_validation_error_callback(on_validation_error, this);

			if (opts.pipeline_stats && !device->has_pipeline_stats())
			{
				LOGI("Requested pipeline stats, but device does not support them. Disabling.\n");
				opts.pipeline_stats = false;
			}

			if (opts.pipeline_stats)
			{
				auto foz_path = opts.pipeline_stats_path + ".__tmp.foz";
				pipeline_stats_db.reset(create_stream_archive_database(foz_path.c_str(), DatabaseMode::Append));
				if (!pipeline_stats_db->prepare())
				{
					LOGE("Failed to prepare stats database. Disabling pipeline stats.\n");
					pipeline_stats_db.reset();
					opts.pipeline_stats = false;
				}
			}

			if (opts.pipeline_cache)
			{
				VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
				vector<uint8_t> on_disk_cache;

				// Try to load on-disk cache.
				if (!opts.on_disk_pipeline_cache_path.empty())
				{
					FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "rb");
					if (file)
					{
						fseek(file, 0, SEEK_END);
						size_t len = ftell(file);
						rewind(file);

						if (len != 0)
						{
							on_disk_cache.resize(len);
							if (fread(on_disk_cache.data(), 1, len, file) == len)
							{
								if (validate_pipeline_cache_header(on_disk_cache))
								{
									info.pInitialData = on_disk_cache.data();
									info.initialDataSize = on_disk_cache.size();
								}
								else
									LOGI("Failed to validate pipeline cache. Creating a blank one.\n");
							}
						}
					}
				}

				if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
				{
					LOGE("Failed to create pipeline cache, trying to create a blank one.\n");
					info.initialDataSize = 0;
					info.pInitialData = nullptr;
					if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
					{
						LOGE("Failed to create pipeline cache.\n");
						pipeline_cache = VK_NULL_HANDLE;
					}
				}
			}

			if (!opts.on_disk_validation_cache_path.empty())
			{
				if (device->has_validation_cache())
				{
					VkValidationCacheCreateInfoEXT info = {VK_STRUCTURE_TYPE_VALIDATION_CACHE_CREATE_INFO_EXT };
					vector<uint8_t> on_disk_cache;

					FILE *file = fopen(opts.on_disk_validation_cache_path.c_str(), "rb");
					if (file)
					{
						fseek(file, 0, SEEK_END);
						size_t len = ftell(file);
						rewind(file);

						if (len != 0)
						{
							on_disk_cache.resize(len);
							if (fread(on_disk_cache.data(), 1, len, file) == len)
							{
								if (validate_validation_cache_header(on_disk_cache))
								{
									info.pInitialData = on_disk_cache.data();
									info.initialDataSize = on_disk_cache.size();
								}
								else
									LOGI("Failed to validate validation cache. Creating a blank one.\n");
							}
						}
					}

					if (vkCreateValidationCacheEXT(device->get_device(), &info, nullptr, &validation_cache) != VK_SUCCESS)
					{
						LOGE("Failed to create validation cache, trying to create a blank one.\n");
						info.initialDataSize = 0;
						info.pInitialData = nullptr;
						if (vkCreateValidationCacheEXT(device->get_device(), &info, nullptr, &validation_cache) != VK_SUCCESS)
						{
							LOGE("Failed to create validation cache.\n");
							validation_cache = VK_NULL_HANDLE;
						}
					}
				}
				else
				{
					LOGE("Requested validation cache, but validation layers do not support this extension.\n");
				}
			}

			auto end_device = chrono::steady_clock::now();
			long time_ms = chrono::duration_cast<chrono::milliseconds>(end_device - start_device).count();
			LOGI("Creating Vulkan device took: %ld ms\n", time_ms);

			if (app)
			{
				LOGI("Replaying for application:\n");
				LOGI("  apiVersion: %u.%u.%u\n",
				     VK_VERSION_MAJOR(app->apiVersion),
				     VK_VERSION_MINOR(app->apiVersion),
				     VK_VERSION_PATCH(app->apiVersion));
				LOGI("  engineVersion: %u\n", app->engineVersion);
				LOGI("  applicationVersion: %u\n", app->applicationVersion);
				if (app->pEngineName)
					LOGI("  engineName: %s\n", app->pEngineName);
				if (app->pApplicationName)
					LOGI("  applicationName: %s\n", app->pApplicationName);
			}
		}
	}

	bool enqueue_create_sampler(Hash index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		if (!device->get_feature_filter().sampler_is_supported(create_info))
		{
			LOGE("Sampler %016" PRIx64 " is not supported. Skipping.\n", index);
			return false;
		}

		// Playback in-order.
		if (vkCreateSampler(device->get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %016" PRIx64 " Failed!\n", index);
			return false;
		}
		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		if (!device->get_feature_filter().descriptor_set_layout_is_supported(create_info))
		{
			LOGE("Descriptor set layout %016" PRIx64 " is not supported. Skipping.\n", index);
			return false;
		}

		// Playback in-order.
		if (vkCreateDescriptorSetLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating descriptor set layout %016" PRIx64 " Failed!\n", index);
			return false;
		}
		layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		if (!device->get_feature_filter().pipeline_layout_is_supported(create_info))
		{
			LOGE("Pipeline layout %016" PRIx64 " is not supported. Skipping.\n", index);
			return false;
		}

		// Playback in-order.
		if (vkCreatePipelineLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating pipeline layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		pipeline_layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_render_pass(Hash index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		if (!device->get_feature_filter().render_pass_is_supported(create_info))
		{
			LOGE("Render pass %016" PRIx64 " is not supported. Skipping.\n", index);
			return false;
		}

		// Playback in-order.
		if (vkCreateRenderPass(device->get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
		{
			LOGE("Creating render pass %0" PRIX64 " Failed!\n", index);
			return false;
		}
		render_passes[index] = *render_pass;
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		*module = VK_NULL_HANDLE;
		if (masked_shader_modules.count(hash) || resource_is_blacklisted(RESOURCE_SHADER_MODULE, hash))
		{
			lock_guard<mutex> lock(internal_enqueue_mutex);
			//LOGI("Inserting shader module %016llx.\n", static_cast<unsigned long long>(hash));
			shader_modules.insert_object(hash, *module, 1);
			if (opts.control_block)
				opts.control_block->banned_modules.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

#ifdef FOSSILIZE_REPLAYER_SPIRV_VAL
		if (opts.spirv_validate)
		{
			auto start_time = chrono::steady_clock::now();
			spvtools::SpirvTools context(device->get_api_version() >= VK_VERSION_1_1 ? SPV_ENV_VULKAN_1_1 : SPV_ENV_VULKAN_1_0);
			context.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
				LOGE("spirv-val: %s\n", message);
			});

			bool ret = context.Validate(create_info->pCode, create_info->codeSize / 4);

			auto end_time = chrono::steady_clock::now();
			auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
			shader_module_ns.fetch_add(duration_ns, std::memory_order_relaxed);

			if (!ret)
			{
				LOGE("Failed to validate SPIR-V module: %0" PRIX64 "\n", hash);
				*module = VK_NULL_HANDLE;
				lock_guard<mutex> lock(internal_enqueue_mutex);
				//LOGI("Inserting shader module %016llx.\n", static_cast<unsigned long long>(hash));
				shader_modules.insert_object(hash, VK_NULL_HANDLE, 1);
				shader_module_count.fetch_add(1, std::memory_order_relaxed);

				if (opts.control_block)
					opts.control_block->module_validation_failures.fetch_add(1, std::memory_order_relaxed);

				return true;
			}
		}
#endif

		if (!device->get_feature_filter().shader_module_is_supported(create_info))
		{
			LOGE("Shader module %0" PRIx64 " is not supported on this device.\n", hash);
			*module = VK_NULL_HANDLE;

			lock_guard<mutex> lock(internal_enqueue_mutex);
			//LOGI("Inserting shader module %016llx.\n", static_cast<unsigned long long>(hash));
			shader_modules.insert_object(hash, VK_NULL_HANDLE, 1);
			shader_module_count.fetch_add(1, std::memory_order_relaxed);

			if (opts.control_block)
				opts.control_block->module_validation_failures.fetch_add(1, std::memory_order_relaxed);

			return true;
		}

		auto &per_thread = get_per_thread_data();
		per_thread.triggered_validation_error = false;

		for (unsigned i = 0; i < loop_count; i++)
		{
			// Avoid leak.
			if (*module != VK_NULL_HANDLE)
				vkDestroyShaderModule(device->get_device(), *module, nullptr);
			*module = VK_NULL_HANDLE;

			VkShaderModuleValidationCacheCreateInfoEXT validation_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT };
			if (validation_cache)
			{
				validation_info.validationCache = validation_cache;
				const_cast<VkShaderModuleCreateInfo *>(create_info)->pNext = &validation_info;
			}

			auto start_time = chrono::steady_clock::now();
			if (vkCreateShaderModule(device->get_device(), create_info, nullptr, module) == VK_SUCCESS)
			{
				auto end_time = chrono::steady_clock::now();
				auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
				shader_module_ns.fetch_add(duration_ns, std::memory_order_relaxed);
				shader_module_count.fetch_add(1, std::memory_order_relaxed);

				if (robustness)
				{
					lock_guard<mutex> lock(internal_enqueue_mutex);
					shader_module_to_hash[*module] = hash;
				}

				if (opts.control_block && i == 0)
					opts.control_block->successful_modules.fetch_add(1, std::memory_order_relaxed);
			}
			else
			{
				LOGE("Failed to create shader module for hash 0x%016" PRIx64 ".\n", hash);
			}
		}

		{
			lock_guard<mutex> lock(internal_enqueue_mutex);
			//LOGI("Inserting shader module %016llx.\n", static_cast<unsigned long long>(hash));
			shader_modules.insert_object(hash, *module, create_info->codeSize);
		}

		// vkCreateShaderModule doesn't generally crash anything, so just deal with blacklisting here
		// rather than in an error callback.
		if (device_opts.enable_validation)
		{
			if (!per_thread.triggered_validation_error)
				whitelist_resource(RESOURCE_SHADER_MODULE, hash);
			else
				blacklist_resource(RESOURCE_SHADER_MODULE, hash);
		}

		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		bool derived = (create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
		bool parent = (create_info->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) != 0;
		if (derived && create_info->basePipelineHandle == VK_NULL_HANDLE)
			LOGE("Creating a derived pipeline with NULL handle.\n");

		// It has never been observed that an application uses multiple layers of derived pipelines.
		// Rather than trying to replay with arbitrary layers of derived-ness - which may or may not have any impact on caching -
		// We force these pipelines to be non-derived.
		// That way we avoid handing the complicated case.
		// Would need to be fixed if it's determined that:
		// - An application uses a deep inheritance hierarchy
		// - Some drivers care about this information w.r.t. caching.
		if (parent && derived)
		{
			LOGE("A parent pipeline is also a derived pipeline. Avoid potential deep dependency chains in replay by forcing the pipeline to be non-derived.\n");
			auto *info = const_cast<VkComputePipelineCreateInfo *>(create_info);
			info->flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
			info->basePipelineHandle = VK_NULL_HANDLE;
			info->basePipelineIndex = 0;
		}
		else if (opts.ignore_derived_pipelines)
		{
			auto *info = const_cast<VkComputePipelineCreateInfo *>(create_info);
			info->flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
			info->basePipelineHandle = VK_NULL_HANDLE;
			info->basePipelineIndex = 0;
		}

		if (opts.pipeline_stats)
		{
			auto *info = const_cast<VkComputePipelineCreateInfo *>(create_info);
			info->flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
		}

		auto &per_thread = get_per_thread_data();
		unsigned index = per_thread.current_parse_index;
		unsigned memory_index = per_thread.memory_context_index;
		bool force_outside_range = per_thread.force_outside_range;

		if (!force_outside_range && index < deferred_compute[memory_index].size())
		{
			assert(index < deferred_compute[memory_index].size());
			deferred_compute[memory_index][index] = {
				const_cast<VkComputePipelineCreateInfo *>(create_info), hash, pipeline, index,
			};
		}
		else
		{
			lock_guard<mutex> holder(hash_lock);
			compute_parents[hash] = {
				const_cast<VkComputePipelineCreateInfo *>(create_info), hash, pipeline, index,
			};
		}

		*pipeline = (VkPipeline)hash;
		if (opts.control_block)
			opts.control_block->parsed_compute.fetch_add(1, std::memory_order_relaxed);

		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		bool derived = (create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
		bool parent = (create_info->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) != 0;
		if (derived && create_info->basePipelineHandle == VK_NULL_HANDLE)
			LOGE("Creating a derived pipeline with NULL handle.\n");

		// It has never been observed that an application uses multiple layers of derived pipelines.
		// Rather than trying to replay with arbitrary layers of derived-ness - which may or may not have any impact on caching -
		// We force these pipelines to be non-derived.
		// That way we avoid handing the complicated case.
		// Would need to be fixed if it's determined that:
		// - An application uses a deep inheritance hierarchy
		// - Some drivers care about this information w.r.t. caching.
		if (parent && derived)
		{
			LOGE("A parent pipeline is also a derived pipeline. Avoid potential deep dependency chains in replay by forcing the pipeline to be non-derived.\n");
			auto *info = const_cast<VkGraphicsPipelineCreateInfo *>(create_info);
			info->flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
			info->basePipelineHandle = VK_NULL_HANDLE;
			info->basePipelineIndex = 0;
		}
		else if (opts.ignore_derived_pipelines)
		{
			auto *info = const_cast<VkGraphicsPipelineCreateInfo *>(create_info);
			info->flags &= ~VK_PIPELINE_CREATE_DERIVATIVE_BIT;
			info->basePipelineHandle = VK_NULL_HANDLE;
			info->basePipelineIndex = 0;
		}

		if (opts.pipeline_stats)
		{
			auto *info = const_cast<VkGraphicsPipelineCreateInfo *>(create_info);
			info->flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
		}

		auto &per_thread = get_per_thread_data();
		unsigned index = per_thread.current_parse_index;
		unsigned memory_index = per_thread.memory_context_index;
		bool force_outside_range = per_thread.force_outside_range;

		if (!force_outside_range)
		{
			assert(index < deferred_graphics[memory_index].size());
			deferred_graphics[memory_index][index] = {
				const_cast<VkGraphicsPipelineCreateInfo *>(create_info), hash, pipeline, index,
			};
		}
		else
		{
			lock_guard<mutex> holder(hash_lock);
			graphics_parents[hash] = {
				const_cast<VkGraphicsPipelineCreateInfo *>(create_info), hash, pipeline, index,
			};
		}

		*pipeline = (VkPipeline)hash;
		if (opts.control_block)
			opts.control_block->parsed_graphics.fetch_add(1, std::memory_order_relaxed);

		return true;
	}

	bool enqueue_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline,
	                      unsigned index, unsigned memory_context_index)
	{
		PipelineWorkItem work_item = {};
		work_item.hash = hash;
		work_item.tag = RESOURCE_COMPUTE_PIPELINE;
		work_item.output.pipeline = pipeline;
		work_item.index = index;
		work_item.memory_context_index = memory_context_index;

		if (create_info->stage.module != VK_NULL_HANDLE)
		{
			work_item.create_info.compute_create_info = create_info;
			// Pointer to value in std::unordered_map remains fixed per spec (node-based).
			lock_guard<mutex> lock(internal_enqueue_mutex);
			work_item.hash_map_entry.pipeline = &compute_pipelines[hash];
		}
		//else
		//	LOGE("Skipping replay of graphics pipeline index %u.\n", graphics_pipeline_index);

		enqueue_work_item(work_item);

		return true;
	}

	bool enqueue_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline,
	                      unsigned index, unsigned memory_context_index)
	{
		bool valid_handles = true;
		for (uint32_t i = 0; i < create_info->stageCount; i++)
			if (create_info->pStages[i].module == VK_NULL_HANDLE)
				valid_handles = false;

		PipelineWorkItem work_item = {};
		work_item.hash = hash;
		work_item.tag = RESOURCE_GRAPHICS_PIPELINE;
		work_item.output.pipeline = pipeline;
		work_item.index = index;
		work_item.memory_context_index = memory_context_index;

		if (valid_handles)
		{
			work_item.create_info.graphics_create_info = create_info;
			lock_guard<mutex> lock(internal_enqueue_mutex);
			// Pointer to value in std::unordered_map remains fixed per spec (node-based).
			work_item.hash_map_entry.pipeline = &graphics_pipelines[hash];
		}
		//else
		//	LOGE("Skipping replay of graphics pipeline index %u.\n", graphics_pipeline_index);

		enqueue_work_item(work_item);

		return true;
	}

	bool enqueue_shader_module(VkShaderModule shader_module_hash)
	{
		if (enqueued_shader_modules.count(shader_module_hash) == 0)
		{
			if (opts.control_block)
				opts.control_block->total_modules.fetch_add(1, std::memory_order_relaxed);

			PipelineWorkItem work_item;
			work_item.tag = RESOURCE_SHADER_MODULE;
			work_item.hash = (Hash) shader_module_hash;
			work_item.parse_only = true;
			work_item.memory_context_index = SHADER_MODULE_MEMORY_CONTEXT;
			enqueue_work_item(work_item);
			enqueued_shader_modules.insert(shader_module_hash);
			//LOGI("Queueing up shader module: %016llx.\n", static_cast<unsigned long long>((Hash) shader_module_hash));
			return true;
		}
		else
		{
			//LOGI("Not queueing up shader module: %016llx.\n", static_cast<unsigned long long>((Hash) shader_module_hash));
			return false;
		}
	}

	bool enqueue_shader_modules(const VkGraphicsPipelineCreateInfo *info)
	{
		bool ret = false;
		for (uint32_t i = 0; i < info->stageCount; i++)
			if (enqueue_shader_module(info->pStages[i].module))
				ret = true;
		return ret;
	}

	bool enqueue_shader_modules(const VkComputePipelineCreateInfo *info)
	{
		bool ret = enqueue_shader_module(info->stage.module);
		return ret;
	}

	void resolve_shader_modules(VkGraphicsPipelineCreateInfo *info)
	{
		for (uint32_t i = 0; i < info->stageCount; i++)
		{
			auto result = shader_modules.find_object((Hash) info->pStages[i].module);
			if (!result.second)
			{
				LOGE("Could not find shader module %016" PRIx64 " in cache.\n", (Hash) info->pStages[i].module);
			}
			const_cast<VkPipelineShaderStageCreateInfo *>(info->pStages)[i].module = result.first;
		}
	}

	void resolve_shader_modules(VkComputePipelineCreateInfo *info)
	{
		auto result = shader_modules.find_object((Hash) info->stage.module);
		if (!result.second)
		{
			LOGE("Could not find shader module %016" PRIx64 " in cache.\n", (Hash) info->stage.module);
		}
		const_cast<VkComputePipelineCreateInfo*>(info)->stage.module = result.first;
	}

	template <typename DerivedInfo>
	void sort_deferred_derived_pipelines(vector<DerivedInfo> &derived, vector<DerivedInfo> &deferred)
	{
		sort(begin(deferred), end(deferred), [&](const DerivedInfo &a, const DerivedInfo &b) -> bool {
			bool a_derived = !a.info || (a.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
			bool b_derived = !b.info || (b.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
			if (a_derived == b_derived)
				return a.index < b.index;
			else
				return b_derived;
		});

		unsigned end_index_non_derived = deferred.size();
		unsigned index = 0;
		for (auto &def : deferred)
		{
			index++;
			if (def.info && (def.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) == 0)
				end_index_non_derived = index;
		}

		derived.clear();
		assert(deferred.size() >= end_index_non_derived);
		derived.reserve(deferred.size() - end_index_non_derived);
		copy(begin(deferred) + end_index_non_derived, end(deferred), back_inserter(derived));
		deferred.erase(begin(deferred) + end_index_non_derived, end(deferred));

		auto itr = remove_if(begin(derived), end(derived), [](const DerivedInfo &a) { return a.info == nullptr; });
		derived.erase(itr, end(derived));
	}

	template <typename DerivedInfo>
	void enqueue_deferred_pipelines(vector<DerivedInfo> *deferred, const unordered_map<Hash, VkPipeline> &pipelines,
	                                unordered_map<Hash, DerivedInfo> &parents,
	                                vector<EnqueuedWork> &work, const vector<Hash> &hashes, unsigned start_index)
	{
		static const unsigned NUM_PIPELINES_PER_CONTEXT = 1024;

		// Make sure that if we sort by work_index, we get an interleaved execution pattern which
		// will naturally pipeline.
		enum PassOrder
		{
			PARSE_ENQUEUE_OFFSET = 0,
			MAINTAIN_SHADER_MODULE_LRU_CACHE = 1,
			ENQUEUE_SHADER_MODULES_PRIMARY_OFFSET = 2,
			RESOLVE_SHADER_MODULE_AND_ENQUEUE_PIPELINES_PRIMARY_OFFSET = 3,
			ENQUEUE_OUT_OF_RANGE_PARENT_PIPELINES = 4,
			ENQUEUE_SHADER_MODULE_SECONDARY_OFFSET = 5,
			ENQUEUE_DERIVED_PIPELINES_OFFSET = 6,
			PASS_COUNT = 7
		};

		auto outside_range_hashes = make_shared<unordered_set<Hash>>();

		unsigned memory_index = 0;
		unsigned iteration = 0;

		// This makes sure that we interleave execution of N iterations. Essentially, we get an execution order:
		// - Pass 0, chunk 0 (context 0)
		// - Pass 0, chunk 1 (context 1)
		// - Pass 1, chunk 0 (context 0)
		// - Pass 1, chunk 1 (context 1)
		// - ...
		// - Pass 6, chunk 0 (reclaim memory for context 0)
		// - Pass 6, chunk 1 (reclaim memory for context 1)
		// - Pass 7, sync point
		// - Pass 0, chunk 2 (context 0)
		// - Pass 1, chunk 3 (context 1)

		const auto get_order_index = [&](unsigned pass) -> unsigned {
			return (iteration / NUM_PIPELINE_MEMORY_CONTEXTS) * PASS_COUNT * NUM_PIPELINE_MEMORY_CONTEXTS +
			       pass * NUM_PIPELINE_MEMORY_CONTEXTS +
			       memory_index;
		};

		for (unsigned hash_offset = 0; hash_offset < hashes.size(); hash_offset += NUM_PIPELINES_PER_CONTEXT, iteration++)
		{
			unsigned left_to_submit = hashes.size() - hash_offset;
			unsigned to_submit = left_to_submit < NUM_PIPELINES_PER_CONTEXT ? left_to_submit : NUM_PIPELINES_PER_CONTEXT;

			// State which is used between pipeline stages.
			auto derived = make_shared<vector<DerivedInfo>>();

			// Submit pipelines to be parsed.
			work.push_back({ get_order_index(PARSE_ENQUEUE_OFFSET),
			                 [this, &hashes, &pipelines, deferred, memory_index, to_submit, hash_offset]() {
				                 // Drain old allocators.
				                 sync_worker_memory_context(memory_index);
				                 // Reset per memory-context allocators.
				                 for (auto &data : per_thread_data)
					                 if (data.per_thread_replayers)
						                 data.per_thread_replayers[memory_index].get_allocator().reset();

				                 deferred[memory_index].resize(to_submit);
				                 for (unsigned index = hash_offset; index < hash_offset + to_submit; index++)
				                 {
					                 if (pipelines.count(hashes[index]) == 0)
					                 {
						                 ThreadedReplayer::PipelineWorkItem work_item;
						                 work_item.hash = hashes[index];
						                 work_item.tag = DerivedInfo::get_tag();
						                 work_item.parse_only = true;
						                 work_item.memory_context_index = memory_index;
						                 work_item.index = index - hash_offset;

						                 auto tag = DerivedInfo::get_tag();
						                 if (opts.control_block)
						                 {
							                 if (tag == RESOURCE_GRAPHICS_PIPELINE)
								                 opts.control_block->total_graphics.fetch_add(1, std::memory_order_relaxed);
							                 else if (tag == RESOURCE_COMPUTE_PIPELINE)
								                 opts.control_block->total_compute.fetch_add(1, std::memory_order_relaxed);
						                 }

						                 enqueue_work_item(work_item);
					                 }
					                 else
					                 {
						                 // This pipeline has already been processed before in order to resolve parent pipelines.
						                 // Don't do anything with it this iteration since it has already been compiled.
						                 deferred[memory_index][index - hash_offset] = {};
					                 }
				                 }
			                 }});

			if (memory_index == 0)
			{
				work.push_back({ get_order_index(MAINTAIN_SHADER_MODULE_LRU_CACHE),
				                 [this]() {
					                 // Now all worker threads are drained for any work which needs shader modules,
					                 // so we can maintain the shader module LRU cache while we're parsing new pipelines in parallel.
					                 shader_modules.prune_cache([this](Hash hash, VkShaderModule module) {
						                 assert(enqueued_shader_modules.count((VkShaderModule) hash) != 0);
						                 //LOGI("Removing shader module %016llx.\n", static_cast<unsigned long long>(hash));
						                 enqueued_shader_modules.erase((VkShaderModule) hash);
						                 if (module != VK_NULL_HANDLE)
							                 vkDestroyShaderModule(device->get_device(), module, nullptr);

						                 shader_module_evicted_count.fetch_add(1, std::memory_order_relaxed);
					                 });

					                 // Need to forget that we have seen an object before so we can replay the same object multiple times.
					                 for (auto &per_thread : per_thread_data)
						                 if (per_thread.per_thread_replayers)
							                 per_thread.per_thread_replayers[SHADER_MODULE_MEMORY_CONTEXT].forget_handle_references();
				                 }});
			}

			work.push_back({ get_order_index(ENQUEUE_SHADER_MODULES_PRIMARY_OFFSET),
			                 [this, derived, deferred, memory_index]() {
				                 // Make sure all parsing of pipelines is complete for this memory context.
				                 sync_worker_memory_context(memory_index);

				                 // Enqueue creation of all shader modules which are referenced by the pipelines.
				                 for (auto &item : deferred[memory_index])
					                 if (item.info)
						                 enqueue_shader_modules(item.info);

				                 // There are two primary kinds of pipelines, derived and non-derived. We split compilation in two here.
				                 // Non-derived pipelines have no dependencies on other pipelines, so we can go ahead,
				                 // and we force that parent pipeline candidates cannot also be derived, which simplifies a lot of things.
				                 // While decompressing modules, we can sort the pipeline infos here in parallel.
				                 sort_deferred_derived_pipelines(*derived, deferred[memory_index]);
			                 }});

			work.push_back({ get_order_index(RESOLVE_SHADER_MODULE_AND_ENQUEUE_PIPELINES_PRIMARY_OFFSET),
			                 [this, derived, deferred, memory_index, hash_offset, start_index]() {
				                 // Make sure all VkShaderModules have been queued and completed.
				                 // We reserve a special memory context for all shader modules since other memory indices
				                 // might queue up shader module work which we need in our memory index.
				                 // Remap VkShaderModule references from hashes to real handles and enqueue all non-derived pipelines for work.
				                 sync_worker_memory_context(SHADER_MODULE_MEMORY_CONTEXT);

				                 for (auto &item : deferred[memory_index])
				                 {
					                 if (item.info)
					                 {
						                 resolve_shader_modules(item.info);
						                 enqueue_pipeline(item.hash, item.info, item.pipeline,
						                                  item.index + hash_offset + start_index, memory_index);
					                 }
				                 }
			                 }});

			work.push_back({ get_order_index(ENQUEUE_OUT_OF_RANGE_PARENT_PIPELINES),
			                 [this, &pipelines, derived, outside_range_hashes]() {
				                 // Figure out which of the parent pipelines we need.
				                 for (auto &d : *derived)
				                 {
					                 if (!d.info)
						                 continue;
					                 Hash h = (Hash)d.info->basePipelineHandle;

					                 // pipelines will have an entry if we called enqueue_pipeline() already for this hash,
					                 // it's not necessarily complete yet!
					                 bool is_inside_range = pipelines.count(h) != 0;

					                 if (!is_inside_range)
					                 {
						                 // We have not seen this parent pipeline before.
						                 // Make sure this auxillary entry has been parsed and is not seen before.
						                 if (!outside_range_hashes->count(h))
						                 {
							                 PipelineWorkItem work_item;
							                 work_item.index = d.index;
							                 work_item.hash = h;
							                 work_item.parse_only = true;
							                 work_item.force_outside_range = true;
							                 work_item.memory_context_index = PARENT_PIPELINE_MEMORY_CONTEXT;
							                 work_item.tag = DerivedInfo::get_tag();
							                 outside_range_hashes->insert(h);
							                 enqueue_work_item(work_item);
						                 }
					                 }
				                 }
			                 }});

			if (memory_index == 0)
			{
				// This is a join-like operation. We need to wait for all parent pipelines and all shader modules to have completed.
				work.push_back({get_order_index(ENQUEUE_SHADER_MODULE_SECONDARY_OFFSET),
				                [this, &parents, hash_offset, start_index]()
				                {
					                // Wait until all parent pipelines have been parsed.
					                sync_worker_memory_context(PARENT_PIPELINE_MEMORY_CONTEXT);

					                // Queue up all shader modules if somehow the shader modules used by parent pipelines differ from children ...
					                for (auto &parent : parents)
					                {
						                // If we enqueued something here, we have to wait for shader handles to resolve.
						                if (parent.second.info)
							                enqueue_shader_modules(parent.second.info);
					                }

					                if (opts.control_block)
					                {
						                auto tag = DerivedInfo::get_tag();
						                if (tag == RESOURCE_GRAPHICS_PIPELINE)
							                opts.control_block->total_graphics.fetch_add(parents.size(),
							                                                             std::memory_order_relaxed);
						                else if (tag == RESOURCE_COMPUTE_PIPELINE)
							                opts.control_block->total_compute.fetch_add(parents.size(),
							                                                            std::memory_order_relaxed);
					                }

					                sync_worker_memory_context(SHADER_MODULE_MEMORY_CONTEXT);
					                // The shader module memory context recycles itself.

					                for (auto &parent : parents)
					                {
						                if (parent.second.info)
						                {
							                resolve_shader_modules(parent.second.info);
							                assert((parent.second.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) ==
							                       0);
							                enqueue_pipeline(parent.second.hash, parent.second.info,
							                                 parent.second.pipeline,
							                                 parent.second.index + hash_offset + start_index,
							                                 PARENT_PIPELINE_MEMORY_CONTEXT);
						                }
					                }

					                parents.clear();

					                // We might be pulling in a parent pipeline from another memory context next iteration,
					                // so we need to wait for all normal memory contexts.
					                for (unsigned i = 0; i < NUM_PIPELINE_MEMORY_CONTEXTS; i++)
						                sync_worker_memory_context(i);
				                }});
			}

			work.push_back({ get_order_index(ENQUEUE_DERIVED_PIPELINES_OFFSET),
			                 [this, &pipelines, derived, memory_index, hash_offset, start_index]() {
				                 // Go over all pipelines. If there are no further dependencies to resolve, we can go ahead and queue them up.
				                 // If an entry exists in pipelines, we have queued up that hash earlier, but it might not be done compiling yet.
				                 auto itr = unstable_remove_if(begin(*derived), end(*derived), [&](const DerivedInfo &info) -> bool {
					                 if (info.info)
					                 {
						                 Hash hash = (Hash)info.info->basePipelineHandle;
						                 return pipelines.count(hash) != 0;
					                 }
					                 else
						                 return false;
				                 });

				                 // Wait for parent pipelines to complete.
				                 // Only needed for first memory index.
				                 if (memory_index == 0)
				                 {
					                 sync_worker_memory_context(PARENT_PIPELINE_MEMORY_CONTEXT);

					                 // Now we have replayed the pipelines, so we can reclaim parent pipeline memory for this iteration.
					                 for (auto &per_thread : per_thread_data)
						                 if (per_thread.per_thread_replayers)
							                 per_thread.per_thread_replayers[PARENT_PIPELINE_MEMORY_CONTEXT].get_allocator().reset();
				                 }

				                 // Now we can enqueue pipeline compilation with correct pipeline handles.
				                 for (auto i = itr; i != end(*derived); ++i)
				                 {
					                 if (i->info)
					                 {
						                 resolve_shader_modules(i->info);
						                 auto base_itr = pipelines.find((Hash) i->info->basePipelineHandle);
						                 i->info->basePipelineHandle =
								                 base_itr != end(pipelines) ? base_itr->second : VK_NULL_HANDLE;
						                 enqueue_pipeline(i->hash, i->info, i->pipeline,
						                                  i->index + hash_offset + start_index, memory_index);
					                 }
				                 }

				                 // It might be possible that we couldn't resolve some dependencies, log this.
				                 if (itr != begin(*derived))
				                 {
					                 auto skipped_count = unsigned(itr - begin(*derived));
					                 LOGE("%u pipelines were not compiled because parent pipelines do not exist.\n", skipped_count);

					                 if (opts.control_block)
					                 {
						                 auto tag = DerivedInfo::get_tag();
						                 if (tag == RESOURCE_GRAPHICS_PIPELINE)
							                 opts.control_block->skipped_graphics.fetch_add(skipped_count, std::memory_order_relaxed);
						                 else if (tag == RESOURCE_COMPUTE_PIPELINE)
							                 opts.control_block->skipped_compute.fetch_add(skipped_count, std::memory_order_relaxed);
					                 }
				                 }
			                 }});

			memory_index = (memory_index + 1) % NUM_PIPELINE_MEMORY_CONTEXTS;
		}
	}

	void sync_threads() override
	{
		sync_worker_threads();
	}

	// Support ignoring shader module which are known to cause crashes.
	void mask_shader_module(Hash hash)
	{
		masked_shader_modules.insert(hash);
	}

	const vector<thread> &get_threads() const
	{
		return thread_pool;
	}

	void emergency_teardown()
	{
#ifdef SIMULATE_UNSTABLE_DRIVER
		spurious_deadlock();
#endif
		flush_pipeline_cache();
		flush_validation_cache();
		if (validation_whitelist_db)
			validation_whitelist_db->flush();
		if (validation_blacklist_db)
			validation_blacklist_db->flush();
		device.reset();
	}

	Options opts;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;

	ObjectCache<VkShaderModule> shader_modules;

	std::unordered_map<Hash, VkRenderPass> render_passes;
	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	std::unordered_set<Hash> masked_shader_modules;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_hash;
	std::unordered_set<VkShaderModule> enqueued_shader_modules;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
	VkValidationCacheEXT validation_cache = VK_NULL_HANDLE;

	// VALVE: multi-threaded work queue for replayer

	void enqueue_work_item(const PipelineWorkItem &item)
	{
		lock_guard<mutex> lock(pipeline_work_queue_mutex);
		pipeline_work_queue.push(item);
		work_available_condition.notify_one();
		queued_count[item.memory_context_index]++;
	}

	unsigned num_worker_threads = 0;
	unsigned loop_count = 0;

	unsigned queued_count[NUM_MEMORY_CONTEXTS] = {};
	unsigned completed_count[NUM_MEMORY_CONTEXTS] = {};
	unsigned thread_initialized_count = 0;
	std::condition_variable work_available_condition;
	std::condition_variable work_done_condition[NUM_MEMORY_CONTEXTS];

	std::vector<std::thread> thread_pool;
	std::vector<PerThreadData> per_thread_data;
	std::mutex pipeline_work_queue_mutex;
	std::mutex internal_enqueue_mutex;
	std::queue<PipelineWorkItem> pipeline_work_queue;

	std::mutex pipeline_stats_queue_mutex;
	std::unique_ptr<DatabaseInterface> pipeline_stats_db;

	std::mutex validation_db_mutex;
	std::unique_ptr<DatabaseInterface> validation_whitelist_db;
	std::unique_ptr<DatabaseInterface> validation_blacklist_db;

	std::mutex hash_lock;
	std::unordered_map<Hash, DeferredGraphicsInfo> graphics_parents;
	std::unordered_map<Hash, DeferredComputeInfo> compute_parents;
	std::vector<DeferredGraphicsInfo> deferred_graphics[NUM_MEMORY_CONTEXTS];
	std::vector<DeferredComputeInfo> deferred_compute[NUM_MEMORY_CONTEXTS];

	// Feed statistics from the worker threads.
	std::atomic<std::uint64_t> graphics_pipeline_ns;
	std::atomic<std::uint64_t> compute_pipeline_ns;
	std::atomic<std::uint64_t> shader_module_ns;
	std::atomic<std::uint64_t> total_idle_ns;
	std::atomic<std::uint64_t> thread_total_ns;
	std::atomic<std::uint32_t> graphics_pipeline_count;
	std::atomic<std::uint32_t> compute_pipeline_count;
	std::atomic<std::uint32_t> shader_module_count;
	std::atomic<std::uint32_t> shader_module_evicted_count;
	std::atomic<std::uint32_t> pipeline_cache_hits;
	std::atomic<std::uint32_t> pipeline_cache_misses;

	std::atomic<std::uint64_t> shader_module_total_size;
	std::atomic<std::uint64_t> shader_module_total_compressed_size;

	std::atomic<size_t> total_peak_memory;

	bool shutting_down = false;

	unique_ptr<VulkanDevice> device;
	bool device_was_init = false;
	VulkanDevice::Options device_opts;

	// Crash recovery.
	bool robustness = false;

	const StateReplayer *global_replayer = nullptr;
	DatabaseInterface *global_database = nullptr;
};

static void on_validation_error(void *userdata)
{
	auto *replayer = static_cast<ThreadedReplayer *>(userdata);

	auto &per_thread = replayer->get_per_thread_data();
	per_thread.triggered_validation_error = true;

	if (per_thread.current_graphics_pipeline)
		replayer->blacklist_resource(RESOURCE_GRAPHICS_PIPELINE, per_thread.current_graphics_pipeline);
	if (per_thread.current_compute_pipeline)
		replayer->blacklist_resource(RESOURCE_COMPUTE_PIPELINE, per_thread.current_compute_pipeline);

	if (replayer->opts.on_validation_error_callback)
		replayer->opts.on_validation_error_callback(replayer);
}

static void print_help()
{
#ifndef NO_ROBUST_REPLAYER
#ifdef _WIN32
#define EXTRA_OPTIONS \
	"\t[--slave-process]\n" \
	"\t[--master-process]\n" \
	"\t[--timeout <seconds>]\n" \
	"\t[--progress]\n" \
	"\t[--quiet-slave]\n" \
	"\t[--shm-name <name>]\n\t[--shm-mutex-name <name>]\n"
#else
#define EXTRA_OPTIONS \
	"\t[--slave-process]\n" \
	"\t[--master-process]\n" \
	"\t[--timeout <seconds>]\n" \
	"\t[--progress]\n" \
	"\t[--quiet-slave]\n" \
	"\t[--shm-fd <fd>]\n"
#endif
#else
#define EXTRA_OPTIONS ""
#endif
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--enable-pipeline-stats <path>]\n"
	     "\t[--pipeline-cache]\n"
	     "\t[--spirv-val]\n"
	     "\t[--num-threads <count>]\n"
	     "\t[--loop <count>]\n"
	     "\t[--on-disk-pipeline-cache <path>]\n"
	     "\t[--on-disk-validation-cache <path>]\n"
	     "\t[--on-disk-validation-whitelist <path>]\n"
	     "\t[--on-disk-validation-blacklist <path>]\n"
	     "\t[--graphics-pipeline-range <start> <end>]\n"
	     "\t[--compute-pipeline-range <start> <end>]\n"
	     "\t[--shader-cache-size <value (MiB)>]\n"
	     "\t[--ignore-derived-pipelines]\n"
	     "\t[--log-memory]\n"
	     "\t[--null-device]\n"
	     "\t[--timeout-seconds]\n"
	     EXTRA_OPTIONS
	     "\t<Database>\n");
}

#ifndef NO_ROBUST_REPLAYER
static void log_progress(const ExternalReplayer::Progress &progress)
{
	LOGI("=================\n");
	LOGI(" Progress report:\n");
	LOGI("   Parsed graphics %u / %u\n", progress.graphics.parsed, progress.graphics.total);
	LOGI("   Parsed compute %u / %u\n", progress.compute.parsed, progress.compute.total);
	LOGI("   Decompress modules %u / %u, skipped %u, failed validation %u\n",
	     progress.completed_modules, progress.total_modules, progress.banned_modules, progress.module_validation_failures);
	LOGI("   Compile graphics %u / %u, skipped %u\n", progress.graphics.completed, progress.graphics.total, progress.graphics.skipped);
	LOGI("   Compile compute %u / %u, skipped %u\n", progress.compute.completed, progress.compute.total, progress.compute.skipped);
	LOGI("   Clean crashes %u\n", progress.clean_crashes);
	LOGI("   Dirty crashes %u\n", progress.dirty_crashes);
	LOGI("=================\n");
}

static void log_faulty_modules(ExternalReplayer &replayer)
{
	size_t count;
	if (!replayer.get_faulty_spirv_modules(&count, nullptr))
		return;
	vector<Hash> hashes(count);
	if (!replayer.get_faulty_spirv_modules(&count, hashes.data()))
		return;

	sort(begin(hashes), end(hashes));

	for (auto &h : hashes)
		LOGI("Detected faulty SPIR-V module: %016" PRIx64 "\n", h);
}

static void log_faulty_graphics(ExternalReplayer &replayer)
{
	size_t count;
	if (!replayer.get_graphics_failed_validation(&count, nullptr))
		return;
	vector<Hash> hashes(count);
	if (!replayer.get_graphics_failed_validation(&count, hashes.data()))
		return;

	sort(begin(hashes), end(hashes));

	for (auto &h : hashes)
	{
		LOGI("Graphics pipeline failed validation: %016" PRIx64 "\n", h);

		// Ad-hoc hack to test automatic pruning ideas ...
		//printf("--skip-graphics %016llx ", static_cast<unsigned long long>(h));
	}

	vector<unsigned> indices;
	if (!replayer.get_faulty_graphics_pipelines(&count, nullptr, nullptr))
		return;
	indices.resize(count);
	hashes.resize(count);
	if (!replayer.get_faulty_graphics_pipelines(&count, indices.data(), hashes.data()))
		return;

	for (unsigned i = 0; i < count; i++)
	{
		LOGI("Graphics pipeline crashed or hung: %016" PRIx64 ". Repro with: --graphics-pipeline-range %u %u\n",
		     hashes[i], indices[i], indices[i] + 1);
	}
}

static void log_faulty_compute(ExternalReplayer &replayer)
{
	size_t count;
	if (!replayer.get_compute_failed_validation(&count, nullptr))
		return;
	vector<Hash> hashes(count);
	if (!replayer.get_compute_failed_validation(&count, hashes.data()))
		return;

	sort(begin(hashes), end(hashes));

	for (auto &h : hashes)
	{
		LOGI("Compute pipeline failed validation: %016" PRIx64 "\n", h);

		// Ad-hoc hack to test automatic pruning ideas ...
		//printf("--skip-compute %016llx ", static_cast<unsigned long long>(h));
	}

	vector<unsigned> indices;
	if (!replayer.get_faulty_compute_pipelines(&count, nullptr, nullptr))
		return;
	indices.resize(count);
	hashes.resize(count);
	if (!replayer.get_faulty_compute_pipelines(&count, indices.data(), hashes.data()))
		return;

	for (unsigned i = 0; i < count; i++)
	{
		LOGI("Compute pipeline crashed or hung: %016" PRIx64 ". Repro with: --compute-pipeline-range %u %u\n",
		     hashes[i], indices[i], indices[i] + 1);
	}
}

static int run_progress_process(const VulkanDevice::Options &device_opts,
                                const ThreadedReplayer::Options &replayer_opts,
                                const vector<const char *> &databases, int timeout)
{
	ExternalReplayer::Options opts = {};
	opts.on_disk_pipeline_cache = replayer_opts.on_disk_pipeline_cache_path.empty() ?
	                              nullptr : replayer_opts.on_disk_pipeline_cache_path.c_str();
	opts.on_disk_validation_cache = replayer_opts.on_disk_validation_cache_path.empty() ?
	                                nullptr : replayer_opts.on_disk_validation_cache_path.c_str();
	opts.on_disk_validation_whitelist = replayer_opts.on_disk_validation_whitelist_path.empty() ?
	                                    nullptr : replayer_opts.on_disk_validation_whitelist_path.c_str();
	opts.on_disk_validation_blacklist = replayer_opts.on_disk_validation_blacklist_path.empty() ?
	                                    nullptr : replayer_opts.on_disk_validation_blacklist_path.c_str();
	opts.pipeline_stats_path = replayer_opts.pipeline_stats_path.empty() ?
	                           nullptr : replayer_opts.pipeline_stats_path.c_str();
	opts.pipeline_cache = replayer_opts.pipeline_cache;
	opts.num_threads = replayer_opts.num_threads;
	opts.quiet = true;
	opts.databases = databases.data();
	opts.num_databases = databases.size();
	opts.external_replayer_path = nullptr;
	opts.inherit_process_group = true;
	opts.spirv_validate = replayer_opts.spirv_validate;
	opts.device_index = device_opts.device_index;
	opts.enable_validation = device_opts.enable_validation;
	opts.ignore_derived_pipelines = replayer_opts.ignore_derived_pipelines;
	opts.null_device = device_opts.null_device;
	opts.start_graphics_index = replayer_opts.start_graphics_index;
	opts.end_graphics_index = replayer_opts.end_graphics_index;
	opts.start_compute_index = replayer_opts.start_compute_index;
	opts.end_compute_index = replayer_opts.end_compute_index;
	opts.use_pipeline_range =
			(replayer_opts.start_graphics_index != 0) ||
			(replayer_opts.end_graphics_index != ~0u) ||
			(replayer_opts.start_compute_index != 0) ||
			(replayer_opts.end_compute_index != ~0u);
	opts.timeout_seconds = replayer_opts.timeout_seconds;

	ExternalReplayer replayer;
	if (!replayer.start(opts))
	{
		LOGE("Failed to start external replayer.\n");
		return EXIT_FAILURE;
	}

	bool has_killed = false;
	auto start_time = std::chrono::steady_clock::now();

	for (;;)
	{
		if (!has_killed && timeout > 0)
		{
			auto current_time = std::chrono::steady_clock::now();
			auto delta = current_time - start_time;
			if (std::chrono::duration_cast<std::chrono::seconds>(delta).count() >= timeout)
			{
				LOGE("Killing process due to timeout.\n");
				replayer.kill();
				has_killed = true;
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		ExternalReplayer::Progress progress = {};

		if (replayer.is_process_complete(nullptr))
		{
			auto result = replayer.poll_progress(progress);
			if (result != ExternalReplayer::PollResult::ResultNotReady)
				log_progress(progress);
			log_faulty_modules(replayer);
			log_faulty_graphics(replayer);
			log_faulty_compute(replayer);
			return replayer.wait();
		}

		auto result = replayer.poll_progress(progress);

		switch (result)
		{
		case ExternalReplayer::PollResult::Error:
			return EXIT_FAILURE;

		case ExternalReplayer::PollResult::ResultNotReady:
			break;

		case ExternalReplayer::PollResult::Complete:
		case ExternalReplayer::PollResult::Running:
			log_progress(progress);
			if (result == ExternalReplayer::PollResult::Complete)
			{
				log_faulty_modules(replayer);
				log_faulty_graphics(replayer);
				log_faulty_compute(replayer);
				return replayer.wait();
			}
			break;
		}
	}
}

static void log_process_memory();
#endif

static bool parse_json_stats(const std::string &foz_path, rapidjson::Document &doc)
{
	auto db = std::unique_ptr<DatabaseInterface>(create_stream_archive_database(foz_path.c_str(), DatabaseMode::ReadOnly));
	if (!db)
		return false;
	if (!db->prepare())
		return false;

	static const ResourceTag stat_tags[] = {
		RESOURCE_GRAPHICS_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
	};

	doc.SetArray();

	std::vector<char> json_buffer;

	for (auto &tag : stat_tags)
	{
		size_t num_hashes = 0;
		if (!db->get_hash_list_for_resource_tag(tag, &num_hashes, nullptr))
			return false;
		std::vector<Hash> hashes(num_hashes);
		if (!db->get_hash_list_for_resource_tag(tag, &num_hashes, hashes.data()))
			return false;

		for (auto &hash : hashes)
		{
			rapidjson::Document tmp_doc;
			size_t json_size = 0;
			if (!db->read_entry(tag, hash, &json_size, nullptr, 0))
				continue;
			json_buffer.resize(json_size);
			if (!db->read_entry(tag, hash, &json_size, json_buffer.data(), 0))
				continue;
			json_buffer.push_back('\0');

			tmp_doc.Parse(rapidjson::StringRef(json_buffer.data()));
			if (tmp_doc.HasParseError())
				continue;

			rapidjson::Value v;
			v.CopyFrom(tmp_doc, doc.GetAllocator());
			doc.PushBack(v, doc.GetAllocator());
		}
	}

	return true;
}

static void stats_to_csv(const std::string &stats_path, rapidjson::Document &doc)
{
	std::vector<std::string> header;
	std::unordered_map<std::string, size_t> columns;
	std::vector<std::map<size_t, std::string>> rows;

	header.push_back("Database");
	header.push_back("Pipeline type");
	header.push_back("Pipeline hash");
	header.push_back("Executable name");

	for (auto itr = doc.Begin(); itr != doc.End(); itr++)
	{
		std::map<size_t, std::string> row;

		auto &st = *itr;

		row[0] = st["db_path"].GetString();
		row[1] = st["pipeline_type"].GetString();
		row[2] = st["pipeline"].GetString();

		auto &execs = st["executables"];
		for (auto e_itr = execs.Begin(); e_itr != execs.End(); e_itr++)
		{
			auto &exec = *e_itr;
			row[3] = exec["executable_name"].GetString();
			for (auto st_itr = exec["stats"].Begin(); st_itr != exec["stats"].End(); st_itr++)
			{
				auto &stat = *st_itr;
				std::string key = stat["name"].GetString();
				size_t insert_at;

				auto col = columns.find(key);
				if (col == columns.end())
				{
					insert_at = header.size();
					columns[key] = insert_at;
					header.push_back(key);
				}
				else
				{
					insert_at = col->second;
				}

				row[insert_at] = stat["value"].GetString();
			}

			rows.push_back(row);
		}
	}

	FILE *fp = fopen(stats_path.c_str(), "w");
	if (!fp)
		return;

	size_t colnumber = 0;
	for (auto &h : header)
		fprintf(fp, "%s%s", h.c_str(), ++colnumber < header.size() ? "," : "\n");

	for (auto &r : rows)
	{
		colnumber = 0;
		for (size_t i = 0; i < header.size(); i++)
		{
			auto col = r.find(i);
			fprintf(fp, "%s%s", col == r.end() ? "" : col->second.c_str(), ++colnumber < header.size() ? "," : "\n");
		}
	}

	fclose(fp);
}

#ifndef NO_ROBUST_REPLAYER
static void dump_stats(const std::string &stats_path, const std::vector<std::string> &foz_paths)
{
	rapidjson::Document doc;
	doc.SetArray();
	auto &alloc = doc.GetAllocator();

	for (auto &sp : foz_paths)
	{
		rapidjson::Document tmp_doc;
		if (!parse_json_stats(sp, tmp_doc))
			continue;

		doc.Reserve(doc.Size() + tmp_doc.Size(), alloc);
		for (auto itr = tmp_doc.Begin(); itr != tmp_doc.End(); itr++)
		{
			rapidjson::Value v;
			v.CopyFrom(*itr, alloc);
			doc.PushBack(v, alloc);
		}
		remove(sp.c_str());
	}

	stats_to_csv(stats_path, doc);
}
#endif

static void dump_stats(const std::string &stats_path)
{
	rapidjson::Document doc;
	auto foz_path = stats_path + ".__tmp.foz";

	if (!parse_json_stats(foz_path, doc))
		return;
	stats_to_csv(stats_path, doc);
	remove(foz_path.c_str());
}

#ifndef NO_ROBUST_REPLAYER
static void install_trivial_crash_handlers(ThreadedReplayer &replayer);
#endif

static int run_normal_process(ThreadedReplayer &replayer, const vector<const char *> &databases)
{
	auto start_time = chrono::steady_clock::now();
	auto start_create_archive = chrono::steady_clock::now();
	auto resolver = create_database(databases);

	auto end_create_archive = chrono::steady_clock::now();

	auto start_prepare = chrono::steady_clock::now();
	if (!resolver->prepare())
	{
		LOGE("Failed to prepare database.\n");
		return EXIT_FAILURE;
	}
	auto end_prepare = chrono::steady_clock::now();

	StateReplayer state_replayer;
	state_replayer.set_resolve_derivative_pipeline_handles(false);
	state_replayer.set_resolve_shader_module_handles(false);
	replayer.global_replayer = &state_replayer;
	replayer.global_database = resolver.get();

	vector<Hash> resource_hashes;
	vector<uint8_t> state_json;

	static const ResourceTag initial_playback_order[] = {
		RESOURCE_APPLICATION_INFO, // This will create the device, etc.
		RESOURCE_SAMPLER, // Trivial, run in main thread.
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Trivial, run in main thread
		RESOURCE_PIPELINE_LAYOUT, // Trivial, run in main thread
		RESOURCE_RENDER_PASS, // Trivial, run in main thread
	};

	static const ResourceTag threaded_playback_order[] = {
		RESOURCE_GRAPHICS_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
	};

	static const char *tag_names[] = {
		"AppInfo",
		"Sampler",
		"Descriptor Set Layout",
		"Pipeline Layout",
		"Shader Module",
		"Render Pass",
		"Graphics Pipeline",
		"Compute Pipeline",
	};

	for (auto &tag : initial_playback_order)
	{
		auto main_thread_start = std::chrono::steady_clock::now();
		size_t tag_total_size = 0;
		size_t tag_total_size_compressed = 0;
		size_t resource_hash_count = 0;

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, nullptr))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		resource_hashes.resize(resource_hash_count);

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, resource_hashes.data()))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto &hash : resource_hashes)
		{
			size_t state_json_size = 0;
			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}
			tag_total_size_compressed += state_json_size;

			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			state_json.resize(state_json_size);
			tag_total_size += state_json_size;

			if (!resolver->read_entry(tag, hash, &state_json_size, state_json.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			if (!state_replayer.parse(replayer, resolver.get(), state_json.data(), state_json.size()))
				LOGE("Failed to replay blob (tag: %s, hash: %016" PRIx64 ").\n", tag_names[tag], hash);
		}

		if (tag == RESOURCE_APPLICATION_INFO)
		{
			// Just in case there was no application info in the database, we provide a dummy info,
			// this makes sure the VkDevice is created.
			replayer.set_application_info(0, nullptr, nullptr);
		}

		LOGI("Total binary size for %s: %" PRIu64 " (%" PRIu64 " compressed)\n", tag_names[tag],
		     uint64_t(tag_total_size),
		     uint64_t(tag_total_size_compressed));

		auto main_thread_end = std::chrono::steady_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(main_thread_end - main_thread_start).count();
		LOGI("Total time decoding %s in main thread: %.3f s\n", tag_names[tag], duration * 1e-9);
	}

	// Now we've laid the initial ground work, kick off worker threads.
	replayer.start_worker_threads();

	vector<Hash> graphics_hashes;
	vector<Hash> compute_hashes;
	unsigned graphics_start_index = 0;
	unsigned compute_start_index = 0;

	for (auto &tag : threaded_playback_order)
	{
		size_t tag_total_size = 0;
		size_t tag_total_size_compressed = 0;
		size_t resource_hash_count = 0;

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, nullptr))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		unsigned start_index = 0;
		unsigned end_index = resource_hash_count;

		vector<Hash> *hashes = nullptr;

		if (tag == RESOURCE_GRAPHICS_PIPELINE)
		{
			hashes = &graphics_hashes;

			end_index = min(end_index, replayer.opts.end_graphics_index);
			start_index = max(start_index, replayer.opts.start_graphics_index);
			start_index = min(end_index, start_index);
			graphics_start_index = start_index;

		}
		else if (tag == RESOURCE_COMPUTE_PIPELINE)
		{
			hashes = &compute_hashes;

			end_index = min(end_index, replayer.opts.end_compute_index);
			start_index = max(start_index, replayer.opts.start_compute_index);
			start_index = min(end_index, start_index);
			compute_start_index = start_index;
		}

		assert(hashes);
		hashes->resize(resource_hash_count);

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, hashes->data()))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		move(begin(*hashes) + start_index, begin(*hashes) + end_index, begin(*hashes));
		hashes->erase(begin(*hashes) + (end_index - start_index), end(*hashes));

		for (auto &hash : *hashes)
		{
			size_t state_json_size = 0;
			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}
			tag_total_size_compressed += state_json_size;

			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}
			tag_total_size += state_json_size;
		}

		LOGI("Total binary size for %s: %" PRIu64 " (%" PRIu64 " compressed)\n", tag_names[tag],
		     uint64_t(tag_total_size),
		     uint64_t(tag_total_size_compressed));
	}

	// Done parsing static objects.
	state_replayer.get_allocator().reset();

	vector<EnqueuedWork> graphics_workload;
	vector<EnqueuedWork> compute_workload;
	replayer.enqueue_deferred_pipelines(replayer.deferred_graphics, replayer.graphics_pipelines, replayer.graphics_parents,
	                                    graphics_workload,
	                                    graphics_hashes, graphics_start_index);
	replayer.enqueue_deferred_pipelines(replayer.deferred_compute, replayer.compute_pipelines, replayer.compute_parents,
	                                    compute_workload, compute_hashes,
	                                    compute_start_index);

	sort(begin(graphics_workload), end(graphics_workload), [](const EnqueuedWork &a, const EnqueuedWork &b) {
		return a.order_index < b.order_index;
	});
	sort(begin(compute_workload), end(compute_workload), [](const EnqueuedWork &a, const EnqueuedWork &b) {
		return a.order_index < b.order_index;
	});

	for (auto &work : graphics_workload)
		work.func();
	for (auto &work : compute_workload)
		work.func();

	// VALVE: drain all outstanding pipeline compiles
	replayer.sync_worker_threads();
	replayer.tear_down_threads();

	LOGI("Total binary size for %s: %" PRIu64 " (%" PRIu64 " compressed)\n", tag_names[RESOURCE_SHADER_MODULE],
	     uint64_t(replayer.shader_module_total_size.load()),
	     uint64_t(replayer.shader_module_total_compressed_size.load()));

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.get_current_object_count() +
		replayer.render_passes.size() +
		replayer.compute_pipelines.size() +
		replayer.graphics_pipelines.size();

	long elapsed_ms_prepare = chrono::duration_cast<chrono::milliseconds>(end_prepare - start_prepare).count();
	long elapsed_ms_read_archive = chrono::duration_cast<chrono::milliseconds>(end_create_archive - start_create_archive).count();
	long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Opening archive took %ld ms:\n", elapsed_ms_read_archive);
	LOGI("Parsing archive took %ld ms:\n", elapsed_ms_prepare);

	if (replayer.opts.pipeline_cache && replayer.device->pipeline_feedback_enabled())
	{
		LOGI("Pipeline cache hits reported: %u\n", replayer.pipeline_cache_hits.load());
		LOGI("Pipeline cache misses reported: %u\n", replayer.pipeline_cache_misses.load());
	}

	LOGI("Playing back %u shader modules took %.3f s (accumulated time)\n",
	     replayer.shader_module_count.load(),
	     replayer.shader_module_ns.load() * 1e-9);

	LOGI("Shader cache evicted %u shader modules in total\n",
	     replayer.shader_module_evicted_count.load());

	LOGI("Playing back %u graphics pipelines took %.3f s (accumulated time)\n",
	     replayer.graphics_pipeline_count.load(),
	     replayer.graphics_pipeline_ns.load() * 1e-9);

	LOGI("Playing back %u compute pipelines took %.3f s (accumulated time)\n",
	     replayer.compute_pipeline_count.load(),
	     replayer.compute_pipeline_ns.load() * 1e-9);

	LOGI("Threads were idling in total for %.3f s (accumulated time)\n",
	     replayer.total_idle_ns.load() * 1e-9);

	LOGI("Threads were active in total for %.3f s (accumulated time)\n",
	     replayer.thread_total_ns.load() * 1e-9);

	LOGI("Total peak memory consumption by parser: %.3f MB.\n",
	     (replayer.total_peak_memory.load() + state_replayer.get_allocator().get_peak_memory_consumption()) * 1e-6);

	LOGI("Replayed %lu objects in %ld ms:\n", total_size, elapsed_ms);
	LOGI("  samplers:              %7lu\n", (unsigned long)replayer.samplers.size());
	LOGI("  descriptor set layouts:%7lu\n", (unsigned long)replayer.layouts.size());
	LOGI("  pipeline layouts:      %7lu\n", (unsigned long)replayer.pipeline_layouts.size());
	LOGI("  render passes:         %7lu\n", (unsigned long)replayer.render_passes.size());
	LOGI("  compute pipelines:     %7lu\n", (unsigned long)replayer.compute_pipelines.size());
	LOGI("  graphics pipelines:    %7lu\n", (unsigned long)replayer.graphics_pipelines.size());

	return EXIT_SUCCESS;
}

// The implementations are drastically different.
// To simplify build system, just include implementation inline here.
#ifndef NO_ROBUST_REPLAYER
#ifdef __linux__
#include "fossilize_replay_linux.hpp"
#elif defined(_WIN32)
#include "fossilize_replay_windows.hpp"
#else
#error "Unsupported platform."
#endif
#endif

int main(int argc, char *argv[])
{
	vector<const char *> databases;
	VulkanDevice::Options opts;
	ThreadedReplayer::Options replayer_opts;

#ifndef NO_ROBUST_REPLAYER
	bool master_process = false;
	bool slave_process = false;
	bool quiet_slave = false;
	bool progress = false;
	int timeout = -1;

#ifdef _WIN32
	const char *shm_name = nullptr;
	const char *shm_mutex_name = nullptr;
#else
	int shmem_fd = -1;
#endif
#endif

	bool log_memory = false;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { databases.push_back(arg); };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
	cbs.add("--spirv-val", [&](CLIParser &) { replayer_opts.spirv_validate = true; });
	cbs.add("--on-disk-pipeline-cache", [&](CLIParser &parser) { replayer_opts.on_disk_pipeline_cache_path = parser.next_string(); });
	cbs.add("--on-disk-validation-cache", [&](CLIParser &parser) {
		replayer_opts.on_disk_validation_cache_path = parser.next_string();
		opts.enable_validation = true;
	});
	cbs.add("--on-disk-validation-blacklist", [&](CLIParser &parser) {
		replayer_opts.on_disk_validation_blacklist_path = parser.next_string();
		opts.enable_validation = true;
	});
	cbs.add("--on-disk-validation-whitelist", [&](CLIParser &parser) {
		replayer_opts.on_disk_validation_whitelist_path = parser.next_string();
		opts.enable_validation = true;
	});
	cbs.add("--num-threads", [&](CLIParser &parser) { replayer_opts.num_threads = parser.next_uint(); });
	cbs.add("--loop", [&](CLIParser &parser) { replayer_opts.loop_count = parser.next_uint(); });
	cbs.add("--graphics-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_graphics_index = parser.next_uint();
		replayer_opts.end_graphics_index = parser.next_uint();
	});
	cbs.add("--compute-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_compute_index = parser.next_uint();
		replayer_opts.end_compute_index = parser.next_uint();
	});
	cbs.add("--enable-pipeline-stats", [&](CLIParser &parser) { replayer_opts.pipeline_stats_path = parser.next_string(); });

#ifndef NO_ROBUST_REPLAYER
	cbs.add("--quiet-slave", [&](CLIParser &) { quiet_slave = true; });
	cbs.add("--master-process", [&](CLIParser &) { master_process = true; });
	cbs.add("--slave-process", [&](CLIParser &) { slave_process = true; });
	cbs.add("--timeout", [&](CLIParser &parser) { timeout = parser.next_uint(); });
	cbs.add("--progress", [&](CLIParser &) { progress = true; });

#ifdef _WIN32
	cbs.add("--shm-name", [&](CLIParser &parser) { shm_name = parser.next_string(); });
	cbs.add("--shm-mutex-name", [&](CLIParser &parser) { shm_mutex_name = parser.next_string(); });
#else
	cbs.add("--shmem-fd", [&](CLIParser &parser) { shmem_fd = parser.next_uint(); });
#endif
#endif

	cbs.add("--shader-cache-size", [&](CLIParser &parser) { replayer_opts.shader_cache_size_mb = parser.next_uint(); });
	cbs.add("--ignore-derived-pipelines", [&](CLIParser &) { replayer_opts.ignore_derived_pipelines = true; });
	cbs.add("--log-memory", [&](CLIParser &) { log_memory = true; });
	cbs.add("--null-device", [&](CLIParser &) { opts.null_device = true; });
	cbs.add("--timeout-seconds", [&](CLIParser &parser) { replayer_opts.timeout_seconds = parser.next_uint(); });

	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (databases.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

#ifndef NO_ROBUST_REPLAYER
	// We cannot safely deal with multiple threads here, force one thread.
	if (slave_process)
	{
		if (replayer_opts.num_threads > 1)
			LOGE("Cannot use more than one thread per slave process. Forcing 1 thread.\n");
		replayer_opts.num_threads = 1;
	}

	if (replayer_opts.num_threads < 1)
		replayer_opts.num_threads = 1;

	if (!replayer_opts.on_disk_pipeline_cache_path.empty())
		replayer_opts.pipeline_cache = true;
#endif

	if (!replayer_opts.pipeline_stats_path.empty())
		replayer_opts.pipeline_stats = true;

#ifndef FOSSILIZE_REPLAYER_SPIRV_VAL
	if (replayer_opts.spirv_validate)
		LOGE("--spirv-val is used, but SPIRV-Tools support was not enabled in fossilize-replay. Will be ignored.\n");
#endif

	int ret;
#ifndef NO_ROBUST_REPLAYER
	if (progress)
	{
		ret = run_progress_process(opts, replayer_opts, databases, timeout);
	}
	else if (master_process)
	{
#ifdef _WIN32
		ret = run_master_process(opts, replayer_opts, databases, quiet_slave, shm_name, shm_mutex_name);
#else
		ret = run_master_process(opts, replayer_opts, databases, quiet_slave, shmem_fd);
#endif
	}
	else if (slave_process)
	{
#ifdef _WIN32
		ret = run_slave_process(opts, replayer_opts, databases, shm_name, shm_mutex_name);
#else
		ret = run_slave_process(opts, replayer_opts, databases);
#endif
	}
	else
#endif
	{
		ThreadedReplayer replayer(opts, replayer_opts);
#ifndef NO_ROBUST_REPLAYER
		install_trivial_crash_handlers(replayer);
#endif
		ret = run_normal_process(replayer, databases);
#ifndef NO_ROBUST_REPLAYER
		if (log_memory)
			log_process_memory();
#endif
	}

	if (replayer_opts.pipeline_stats
#ifndef NO_ROBUST_REPLAYER
		&& !(slave_process || progress)
#endif
		)
	{
#ifndef NO_ROBUST_REPLAYER
		if (master_process)
		{
			std::vector<std::string> paths;

			for (size_t idx = 0; idx < replayer_opts.num_threads; idx++)
			{
				auto path = replayer_opts.pipeline_stats_path;
				if (idx != 0)
				{
					path += ".";
					path += std::to_string(idx);
				}
				path += ".__tmp.foz";
				paths.push_back(path);
			}
			dump_stats(replayer_opts.pipeline_stats_path, paths);
		}
		else
#endif
			dump_stats(replayer_opts.pipeline_stats_path);
	}

	return ret;
}
