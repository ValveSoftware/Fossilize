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

#ifndef FOSSILIZE_REPLAY_WRAPPER_ENV
#define FOSSILIZE_REPLAY_WRAPPER_ENV "FOSSILIZE_REPLAY_WRAPPER"
#endif
#ifndef FOSSILIZE_REPLAY_WRAPPER_ORIGINAL_APP_ENV
#define FOSSILIZE_REPLAY_WRAPPER_ORIGINAL_APP_ENV "FOSSILIZE_REPLAY_WRAPPER_ORIGINAL_APP"
#endif

#ifndef FOSSILIZE_DISABLE_RATE_LIMITER_ENV
#define FOSSILIZE_DISABLE_RATE_LIMITER_ENV "FOSSILIZE_DISABLE_RATE_LIMITER"
#endif

//#define SIMULATE_UNSTABLE_DRIVER
//#define SIMULATE_SPURIOUS_DEADLOCK

#ifdef SIMULATE_UNSTABLE_DRIVER
#include <random>
#ifdef _WIN32
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void simulate_crash(volatile int *v)
{
	*v = 0;
}

#ifdef _WIN32
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static int simulate_divide_by_zero(volatile int a, volatile int b)
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
static void report_module_uuid(const char (&path)[2 * VK_UUID_SIZE + 1]);
static void timeout_handler();
static void begin_heartbeat();
static void heartbeat();
#else
#define report_module_uuid(x) ((void)(x))
#define timeout_handler() ((void)0)
#define begin_heartbeat() ((void)0)
#define heartbeat() ((void)0)
#endif

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
		const VkRayTracingPipelineCreateInfoKHR *raytracing_create_info;
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

struct PipelineFeedback
{
	enum { MAX_STAGES = 8 };
	VkPipelineCreationFeedbackEXT feedbacks[MAX_STAGES] = {};
	VkPipelineCreationFeedbackEXT primary_feedback = {};
	VkPipelineCreationFeedbackCreateInfoEXT feedback = { VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT };
	VkShaderStageFlagBits stages[MAX_STAGES] = {};

	inline PipelineFeedback()
	{
		feedback.pPipelineStageCreationFeedbacks = feedbacks;
		feedback.pPipelineCreationFeedback = &primary_feedback;
	}

	inline uint64_t get_per_stage_duration(VkShaderStageFlags active_stages) const
	{
		uint64_t duration = 0;
		for (uint32_t i = 0; i < feedback.pipelineStageCreationFeedbackCount; i++)
		{
			if ((stages[i] & active_stages) != 0 &&
				(feedbacks[i].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0)
			{
				duration += feedbacks[i].duration;
			}
		}

		return duration;
	}

	inline void setup_pnext(const PipelineWorkItem &work_item)
	{
		switch (work_item.tag)
		{
		case RESOURCE_GRAPHICS_PIPELINE:
		{
			auto *info = const_cast<VkGraphicsPipelineCreateInfo *>(work_item.create_info.graphics_create_info);

			feedback.pipelineStageCreationFeedbackCount = info->stageCount;
			if (feedback.pipelineStageCreationFeedbackCount > MAX_STAGES)
				feedback.pipelineStageCreationFeedbackCount = MAX_STAGES;

			feedback.pNext = info->pNext;
			info->pNext = &feedback;

			for (uint32_t i = 0; i < feedback.pipelineStageCreationFeedbackCount; i++)
				stages[i] = info->pStages[i].stage;
			break;
		}

		case RESOURCE_COMPUTE_PIPELINE:
		{
			feedback.pipelineStageCreationFeedbackCount = 1;
			auto *info = const_cast<VkComputePipelineCreateInfo *>(work_item.create_info.compute_create_info);

			feedback.pNext = info->pNext;
			info->pNext = &feedback;

			stages[0] = VK_SHADER_STAGE_COMPUTE_BIT;
			break;
		}

		case RESOURCE_RAYTRACING_PIPELINE:
		{
			// Is there anything meaningful we can do here with per-stage feedback?
			auto *info = const_cast<VkRayTracingPipelineCreateInfoKHR *>(work_item.create_info.raytracing_create_info);
			feedback.pipelineStageCreationFeedbackCount = 0;
			feedback.pNext = info->pNext;
			info->pNext = &feedback;
			break;
		}

		default:
			break;
		}
	}
};

struct ThreadedReplayer : StateCreatorInterface
{
	struct Options
	{
		bool spirv_validate = false;
		bool pipeline_stats = false;
#ifndef _WIN32
		bool disable_signal_handler = false;
		bool disable_rate_limiter = false;
#endif
		string on_disk_pipeline_cache_path;
		string on_disk_validation_cache_path;
		string on_disk_validation_whitelist_path;
		string on_disk_validation_blacklist_path;
		string on_disk_module_identifier_path;
		string pipeline_stats_path;
		string replayer_cache_path;
		vector<unsigned> implicit_whitelist_database_indices;

		// VALVE: Add multi-threaded pipeline creation
		unsigned num_threads = thread::hardware_concurrency();

		// VALVE: --loop option for testing performance
		unsigned loop_count = 1;

		unsigned shader_cache_size_mb = 256;

		// Hash for replaying a single pipeline
		Hash pipeline_hash = 0;

		// Carve out a range of which pipelines to replay.
		// Used for multi-process replays where each process gets its own slice to churn through.
		unsigned start_graphics_index = 0;
		unsigned end_graphics_index = ~0u;
		unsigned start_compute_index = 0;
		unsigned end_compute_index = ~0u;
		unsigned start_raytracing_index = 0;
		unsigned end_raytracing_index = ~0u;

		SharedControlBlock *control_block = nullptr;

		void (*on_thread_callback)(void *userdata) = nullptr;
		void *on_thread_callback_userdata = nullptr;
		void (*on_validation_error_callback)(ThreadedReplayer *) = nullptr;

		unsigned timeout_seconds = 0;
	};

	struct DeferredGraphicsInfo
	{
		VkGraphicsPipelineCreateInfo *info = nullptr;
		Hash hash = 0;
		VkPipeline *pipeline = nullptr;
		unsigned index = 0;

		static ResourceTag get_tag() { return RESOURCE_GRAPHICS_PIPELINE; }
	};

	struct DeferredComputeInfo
	{
		VkComputePipelineCreateInfo *info = nullptr;
		Hash hash = 0;
		VkPipeline *pipeline = nullptr;
		unsigned index = 0;

		static ResourceTag get_tag() { return RESOURCE_COMPUTE_PIPELINE; }
	};

	struct DeferredRayTracingInfo
	{
		VkRayTracingPipelineCreateInfoKHR *info = nullptr;
		Hash hash = 0;
		VkPipeline *pipeline = nullptr;
		unsigned index = 0;

		static ResourceTag get_tag() { return RESOURCE_RAYTRACING_PIPELINE; }
	};

	struct PerThreadData
	{
		StateReplayer *per_thread_replayers = nullptr;
		unsigned current_parse_index = ~0u;
		unsigned current_graphics_index = ~0u;
		unsigned current_compute_index = ~0u;
		unsigned current_raytracing_index = ~0u;
		unsigned memory_context_index = 0;

		Hash current_graphics_pipeline = 0;
		Hash current_compute_pipeline = 0;
		Hash current_raytracing_pipeline = 0;
		Hash failed_module_hashes[16] = {};
		unsigned num_failed_module_hashes = 0;

		bool force_outside_range = false;
		bool triggered_validation_error = false;

		ResourceTag expected_tag = RESOURCE_COUNT;
		Hash expected_hash = 0;
		bool acknowledge_parsing_work = false;
	};

	ThreadedReplayer(const VulkanDevice::Options &device_opts_, const Options &opts_)
		: opts(opts_),
		  num_worker_threads(opts.num_threads), loop_count(opts.loop_count),
		  device_opts(device_opts_)
	{
		// Cannot use initializers for atomics.
		graphics_pipeline_ns.store(0);
		compute_pipeline_ns.store(0);
		raytracing_pipeline_ns.store(0);
		shader_module_ns.store(0);
		graphics_pipeline_count.store(0);
		compute_pipeline_count.store(0);
		raytracing_pipeline_count.store(0);
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

	bool init_implicit_whitelist(DatabaseInterface &iface)
	{
		std::vector<Hash> hashes;
		size_t size = 0;

		const auto resolve = [&](ResourceTag tag) -> bool {
			if (!iface.get_hash_list_for_resource_tag(tag, &size, nullptr))
				return false;
			hashes.resize(size);
			if (!iface.get_hash_list_for_resource_tag(tag, &size, hashes.data()))
				return false;
			for (auto h : hashes)
				implicit_whitelist[tag].insert(h);
			return true;
		};

		return resolve(RESOURCE_SHADER_MODULE) &&
		       resolve(RESOURCE_GRAPHICS_PIPELINE) &&
		       resolve(RESOURCE_RAYTRACING_PIPELINE) &&
		       resolve(RESOURCE_COMPUTE_PIPELINE);
	}

	bool init_implicit_whitelist()
	{
		if (!global_database)
			return false;

		for (unsigned index : opts.implicit_whitelist_database_indices)
		{
			DatabaseInterface *db = nullptr;
			if (global_database->has_sub_databases())
				db = global_database->get_sub_database(index + 1); // We use extra_path for concurrent data bases so index 0 is unused.
			else if (index == 0)
				db = global_database;

			if (db)
			{
				if (!init_implicit_whitelist(*db))
					return false;
			}
			else
				LOGW("Could not open sub database %u, skipping it for purposes of whitelisting.\n", index);
		}

		return true;
	}

	bool init_replayer_cache()
	{
		if (!device)
			return false;

		auto &props = device->get_gpu_properties();
		char uuid[2 * VK_UUID_SIZE + 1];
		uuid[2 * VK_UUID_SIZE] = '\0';

		const auto to_hex = [](uint8_t v) -> char {
			if (v < 10)
				return char('0' + v);
			else
				return char('a' + (v - 10));
		};

		for (unsigned i = 0; i < VK_UUID_SIZE; i++)
		{
			uuid[2 * i + 0] = to_hex(props.pipelineCacheUUID[i] & 0xf);
			uuid[2 * i + 1] = to_hex((props.pipelineCacheUUID[i] >> 4) & 0xf);
		}

		replayer_cache_db.reset(create_concurrent_database((opts.replayer_cache_path + "." + uuid).c_str(), DatabaseMode::Append, nullptr, 0));
		if (!replayer_cache_db || !replayer_cache_db->prepare())
			return false;
		return true;
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
			d.current_raytracing_index = opts.start_raytracing_index;
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
		{
			sync_worker_memory_context(i);
			if (memory_context_pipeline_cache[i])
				vkDestroyPipelineCache(device->get_device(), memory_context_pipeline_cache[i], nullptr);
			memory_context_pipeline_cache[i] = VK_NULL_HANDLE;
		}
	}

	void reset_memory_context_pipeline_cache(unsigned index)
	{
		if (memory_context_pipeline_cache[index])
			vkDestroyPipelineCache(device->get_device(), memory_context_pipeline_cache[index], nullptr);
		memory_context_pipeline_cache[index] = VK_NULL_HANDLE;

		if (!disk_pipeline_cache)
		{
			// If we don't have an on-disk pipeline cache, try to limit memory by creating our own pipeline cache,
			// which is regularly freed and recreated to keep memory usage under control.
			// If we don't do this, drivers generally maintain their internal pipeline cache which grows unbound over time.
			VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
			vkCreatePipelineCache(device->get_device(), &info, nullptr, &memory_context_pipeline_cache[index]);
		}
	}

	void sync_worker_memory_context(unsigned index)
	{
		assert(index < NUM_MEMORY_CONTEXTS);
		unique_lock<mutex> lock(pipeline_work_queue_mutex);

		heartbeat();
		auto last_heartbeat = std::chrono::steady_clock::now();

		if (queued_count[index] == completed_count[index])
		{
			reset_memory_context_pipeline_cache(index);
			return;
		}

		unsigned current_completed = completed_count[index];
		unsigned num_second_timeouts = 0;

		do
		{
			bool signalled = work_done_condition[index].wait_for(
					lock, std::chrono::seconds(1),
					[&]() -> bool { return current_completed != completed_count[index]; });

			// Fire off a heartbeat every 500ms. No need to overwhelm the parent process.
			auto new_time = std::chrono::steady_clock::now();
			auto delta = new_time - last_heartbeat;
			if (std::chrono::duration_cast<std::chrono::milliseconds>(delta).count() > 500)
			{
				heartbeat();
				last_heartbeat = new_time;
			}

			if (!signalled && completed_count[index] == current_completed)
				num_second_timeouts++;
			else
				num_second_timeouts = 0;

			if (opts.timeout_seconds != 0 && num_second_timeouts >= opts.timeout_seconds)
			{
				timeout_handler();
				LOGE("Timed out replaying pipelines!\n");
				exit(2);
			}

			current_completed = completed_count[index];
		} while (queued_count[index] != completed_count[index]);

		heartbeat();

		reset_memory_context_pipeline_cache(index);
	}

	bool run_parse_work_item(StateReplayer &replayer, vector<uint8_t> &buffer, const PipelineWorkItem &work_item)
	{
		size_t json_size = 0;
		if (!global_database->read_entry(work_item.tag, work_item.hash, &json_size, nullptr, PAYLOAD_READ_CONCURRENT_BIT))
		{
			LOGW("Entry (%u: %016" PRIx64 ") does not exist, this might be benign depending on where the archive comes from.\n",
			     unsigned(work_item.tag), work_item.hash);
			if (work_item.tag == RESOURCE_SHADER_MODULE && opts.control_block)
				opts.control_block->parsed_module_failures.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		buffer.resize(json_size);

		if (!global_database->read_entry(work_item.tag, work_item.hash, &json_size, buffer.data(), PAYLOAD_READ_CONCURRENT_BIT))
		{
			LOGW("Entry (%u: %016" PRIx64 ") does not exist, this might be benign depending on where the archive comes from.\n",
			     unsigned(work_item.tag), work_item.hash);
			return false;
		}

		auto &per_thread = get_per_thread_data();
		per_thread.current_parse_index = work_item.index;
		per_thread.force_outside_range = work_item.force_outside_range;
		per_thread.memory_context_index = work_item.memory_context_index;

		// If the archive is somehow corrupt, we really do not want to parse entries which are of a different type
		// or otherwise are not what we expect.
		per_thread.expected_tag = work_item.tag;
		per_thread.expected_hash = work_item.hash;

		// It's also possible that nothing happened while parsing. That should count as a fail as well.
		per_thread.acknowledge_parsing_work = false;

		if (!replayer.parse(*this, global_database, buffer.data(), buffer.size()) ||
		    !per_thread.acknowledge_parsing_work)
		{
			LOGW("Did not replay blob (tag: %d, hash: 0x%016" PRIx64 "). See previous logs for context.\n",
			     work_item.tag, work_item.hash);

			if (opts.control_block)
			{
				if (work_item.tag == RESOURCE_GRAPHICS_PIPELINE)
				{
					opts.control_block->parsed_graphics_failures.fetch_add(1, std::memory_order_relaxed);
					opts.control_block->skipped_graphics.fetch_add(1, std::memory_order_relaxed);
				}
				else if (work_item.tag == RESOURCE_COMPUTE_PIPELINE)
				{
					opts.control_block->parsed_compute_failures.fetch_add(1, std::memory_order_relaxed);
					opts.control_block->skipped_compute.fetch_add(1, std::memory_order_relaxed);
				}
				else if (work_item.tag == RESOURCE_RAYTRACING_PIPELINE)
				{
					opts.control_block->parsed_raytracing_failures.fetch_add(1, std::memory_order_relaxed);
					opts.control_block->skipped_raytracing.fetch_add(1, std::memory_order_relaxed);
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

	void get_pipeline_stats(ResourceTag tag, Hash hash, VkPipeline pipeline,
	                        const PipelineFeedback &feedback, uint64_t time_ns)
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
			doc.AddMember("pso_wall_duration_ns", time_ns, alloc);

			uint64_t feedback_duration =
					(feedback.primary_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0 ?
					feedback.primary_feedback.duration : 0;
			doc.AddMember("pso_duration_ns", feedback_duration, alloc);

			rapidjson::Value execs(rapidjson::kArrayType);
			vector<VkPipelineExecutablePropertiesKHR> pipe_executables(pe_count);
			for (auto &exec : pipe_executables)
				exec.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
			if (vkGetPipelineExecutablePropertiesKHR(device->get_device(), &pipeline_info, &pe_count, pipe_executables.data()) != VK_SUCCESS)
				return;

			for (uint32_t exec = 0; exec < pe_count; exec++)
			{
				rapidjson::Value pe(rapidjson::kObjectType);
				pe.AddMember("executable_name", rapidjson::StringRef(pipe_executables[exec].name), alloc);
				pe.AddMember("subgroup_size", pipe_executables[exec].subgroupSize, alloc);
				uint64_t stage_time_ns = feedback.get_per_stage_duration(pipe_executables[exec].stages);
				pe.AddMember("stage_duration_ns", stage_time_ns, alloc);

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
					for (auto &stat : stats)
						stat.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;

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
							if (strstr(st.name, " hash") != nullptr)
							{
								// If the format has "hash" in its name, let's assume it's a hex value.
								// Ideally the Vulkan extension would have XINT64_KHR, but it is what it is.
								char hex_str[16 + 2 + 1];
								snprintf(hex_str, sizeof(hex_str), "0x%016" PRIx64, st.value.u64);
								stat.AddMember("value", std::string(hex_str), alloc);
							}
							else
							{
								stat.AddMember("value", std::to_string(st.value.u64), alloc);
							}
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

	void mark_replayed_resource(ResourceTag tag, Hash hash)
	{
		if (replayer_cache_db)
		{
			lock_guard<mutex> holder{replayer_cache_mutex};
			replayer_cache_db->write_entry(tag, hash, nullptr, 0, 0);
		}
	}

	bool has_resource_in_whitelist(ResourceTag tag, Hash hash)
	{
		if (validation_whitelist_db)
		{
			if (implicit_whitelist[tag].count(hash))
				return true;
			lock_guard<mutex> holder{validation_db_mutex};
			return validation_whitelist_db->has_entry(tag, hash);
		}
		else
			return false;
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

	void mark_currently_active_modules(const VkPipelineShaderStageCreateInfo *stages, uint32_t count)
	{
		if (robustness)
		{
			auto &per_thread = get_per_thread_data();
			per_thread.num_failed_module_hashes = count;
			for (unsigned i = 0; i < count; i++)
			{
				VkShaderModule module = stages[i].module;
				per_thread.failed_module_hashes[i] = shader_module_to_hash[module];
			}
		}
	}

	bool run_creation_work_item_setup_graphics(const PipelineWorkItem &work_item)
	{
		auto &per_thread = get_per_thread_data();
		per_thread.current_graphics_index = work_item.index + 1;
		per_thread.current_graphics_pipeline = work_item.hash;

		if (!work_item.create_info.graphics_create_info)
		{
			LOGE("Invalid graphics create info.\n");
			return false;
		}

		if ((work_item.create_info.graphics_create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
		    work_item.create_info.graphics_create_info->basePipelineHandle == VK_NULL_HANDLE)
		{
			// This pipeline failed for some reason, don't try to compile this one either.
			LOGE("Invalid derivative pipeline!\n");
			return false;
		}

		if (!device->get_feature_filter().graphics_pipeline_is_supported(work_item.create_info.graphics_create_info))
		{
			LOGW("Graphics pipeline %016" PRIx64 " is not supported by current device, skipping.\n", work_item.hash);
			return false;
		}

		mark_currently_active_modules(work_item.create_info.graphics_create_info->pStages,
		                              work_item.create_info.graphics_create_info->stageCount);

		return true;
	}

	bool run_creation_work_item_setup_compute(const PipelineWorkItem &work_item)
	{
		auto &per_thread = get_per_thread_data();
		per_thread.current_compute_index = work_item.index + 1;
		per_thread.current_compute_pipeline = work_item.hash;

		if (!work_item.create_info.compute_create_info)
		{
			LOGE("Invalid compute create info.\n");
			return false;
		}

		if ((work_item.create_info.compute_create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
		    work_item.create_info.compute_create_info->basePipelineHandle == VK_NULL_HANDLE)
		{
			// This pipeline failed for some reason, don't try to compile this one either.
			LOGE("Invalid derivative pipeline!\n");
			return false;
		}

		if (!device->get_feature_filter().compute_pipeline_is_supported(work_item.create_info.compute_create_info))
		{
			LOGW("Compute pipeline %016" PRIx64 " is not supported by current device, skipping.\n", work_item.hash);
			return false;
		}

		mark_currently_active_modules(&work_item.create_info.compute_create_info->stage, 1);
		return true;
	}

	bool run_creation_work_item_setup_raytracing(const PipelineWorkItem &work_item)
	{
		auto &per_thread = get_per_thread_data();
		per_thread.current_raytracing_index = work_item.index + 1;
		per_thread.current_raytracing_pipeline = work_item.hash;

		if (!work_item.create_info.raytracing_create_info)
		{
			LOGE("Invalid raytracing create info.\n");
			return false;
		}

		if ((work_item.create_info.raytracing_create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
		    work_item.create_info.raytracing_create_info->basePipelineHandle == VK_NULL_HANDLE)
		{
			// This pipeline failed for some reason, don't try to compile this one either.
			LOGE("Invalid derivative pipeline!\n");
			return false;
		}

		auto *pLibraryInfo = work_item.create_info.raytracing_create_info->pLibraryInfo;
		if (pLibraryInfo)
		{
			for (uint32_t i = 0; i < pLibraryInfo->libraryCount; i++)
			{
				if (pLibraryInfo->pLibraries[i] == VK_NULL_HANDLE)
				{
					LOGE("Invalid library!\n");
					return false;
				}
			}
		}

		if (!device->get_feature_filter().raytracing_pipeline_is_supported(work_item.create_info.raytracing_create_info))
		{
			LOGW("Raytracing pipeline %016" PRIx64 " is not supported by current device, skipping.\n", work_item.hash);
			return false;
		}

		// Not sure if there is anything meaningful we can do here since we expect tons of unrelated modules ...
		mark_currently_active_modules(nullptr, 0);
		return true;
	}

	bool run_creation_work_item_setup(const PipelineWorkItem &work_item)
	{
		auto &per_thread = get_per_thread_data();
		per_thread.current_graphics_pipeline = 0;
		per_thread.current_compute_pipeline = 0;
		per_thread.current_raytracing_pipeline = 0;
		per_thread.triggered_validation_error = false;
		bool ret = true;

		// Don't bother replaying blacklisted objects.
		if (resource_is_blacklisted(work_item.tag, work_item.hash))
		{
			LOGW("Resource is blacklisted, ignoring.\n");
			ret = false;
		}

		if (ret)
		{
			switch (work_item.tag)
			{
			case RESOURCE_GRAPHICS_PIPELINE:
				ret = run_creation_work_item_setup_graphics(work_item);
				break;

			case RESOURCE_COMPUTE_PIPELINE:
				ret = run_creation_work_item_setup_compute(work_item);
				break;

			case RESOURCE_RAYTRACING_PIPELINE:
				ret = run_creation_work_item_setup_raytracing(work_item);
				break;

			default:
				ret = false;
				break;
			}
		}

		if (!ret)
		{
			if (opts.control_block)
			{
				switch (work_item.tag)
				{
				case RESOURCE_GRAPHICS_PIPELINE:
					opts.control_block->skipped_graphics.fetch_add(1, std::memory_order_relaxed);
					break;
				case RESOURCE_COMPUTE_PIPELINE:
					opts.control_block->skipped_compute.fetch_add(1, std::memory_order_relaxed);
					break;
				case RESOURCE_RAYTRACING_PIPELINE:
					opts.control_block->skipped_raytracing.fetch_add(1, std::memory_order_relaxed);
					break;
				default:
					break;
				}
			}
			*work_item.output.pipeline = VK_NULL_HANDLE;
		}

		return ret;
	}

	VkPipelineCache get_current_pipeline_cache(const PipelineWorkItem &work_item)
	{
		VkPipelineCache cache = disk_pipeline_cache;
		if (!cache)
			cache = memory_context_pipeline_cache[work_item.memory_context_index];
		return cache;
	}

	void reset_work_item(const PipelineWorkItem &work_item) const
	{
		// Avoid leak.
		if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(device->get_device(), *work_item.hash_map_entry.pipeline, nullptr);
		*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;
	}

	bool work_item_is_dependency(const PipelineWorkItem &work_item) const
	{
		switch (work_item.tag)
		{
		case RESOURCE_GRAPHICS_PIPELINE:
			return (work_item.create_info.graphics_create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;
		case RESOURCE_RAYTRACING_PIPELINE:
			return (work_item.create_info.raytracing_create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;
		default:
			return false;
		}
	}

	void complete_work_item(const PipelineWorkItem &work_item) const
	{
		if (work_item_is_dependency(work_item))
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
	}

	void check_pipeline_cache_feedback(const PipelineFeedback &feedback)
	{
		if (disk_pipeline_cache && (feedback.primary_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0)
		{
			bool cache_hit = (feedback.primary_feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT) != 0;

			// Check per-stage feedback.
			if (!cache_hit && feedback.feedback.pipelineStageCreationFeedbackCount)
			{
				cache_hit = true;
				for (uint32_t j = 0; j < feedback.feedback.pipelineStageCreationFeedbackCount; j++)
				{
					bool valid = (feedback.feedbacks[j].flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT_EXT) != 0;
					bool hit = (feedback.feedbacks[j].flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT_EXT) != 0;
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

	void run_creation_work_item_graphics_iteration(const PipelineWorkItem &work_item, VkPipelineCache cache, bool primary)
	{
		reset_work_item(work_item);

		auto start_time = chrono::steady_clock::now();

#ifdef SIMULATE_UNSTABLE_DRIVER
		spurious_crash();
#endif

		PipelineFeedback feedback;
		if ((disk_pipeline_cache || opts.pipeline_stats) && device->pipeline_feedback_enabled())
			feedback.setup_pnext(work_item);

		if (vkCreateGraphicsPipelines(device->get_device(), cache, 1, work_item.create_info.graphics_create_info,
									  nullptr, work_item.output.pipeline) == VK_SUCCESS)
		{
			auto end_time = chrono::steady_clock::now();
			auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

			graphics_pipeline_ns.fetch_add(duration_ns, std::memory_order_relaxed);
			graphics_pipeline_count.fetch_add(1, std::memory_order_relaxed);

			if (primary)
			{
				if (opts.pipeline_stats)
					get_pipeline_stats(work_item.tag, work_item.hash, *work_item.output.pipeline, feedback, duration_ns);
				if (opts.control_block)
					opts.control_block->successful_graphics.fetch_add(1, std::memory_order_relaxed);
				check_pipeline_cache_feedback(feedback);
			}

			complete_work_item(work_item);
		}
		else
		{
			LOGE("Failed to create graphics pipeline for hash 0x%016" PRIx64 ".\n", work_item.hash);
		}
	}

	void run_creation_work_item_graphics(const PipelineWorkItem &work_item)
	{
		auto cache = get_current_pipeline_cache(work_item);
		for (unsigned i = 0; i < loop_count; i++)
			run_creation_work_item_graphics_iteration(work_item, cache, i == 0);
	}

	void run_creation_work_item_compute_iteration(const PipelineWorkItem &work_item, VkPipelineCache cache, bool primary)
	{
		reset_work_item(work_item);

		auto start_time = chrono::steady_clock::now();

#ifdef SIMULATE_UNSTABLE_DRIVER
		spurious_crash();
#endif
		PipelineFeedback feedback;
		if ((disk_pipeline_cache || opts.pipeline_stats) && device->pipeline_feedback_enabled())
			feedback.setup_pnext(work_item);

		if (vkCreateComputePipelines(device->get_device(), cache, 1,
									 work_item.create_info.compute_create_info,
									 nullptr, work_item.output.pipeline) == VK_SUCCESS)
		{
			auto end_time = chrono::steady_clock::now();
			auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

			compute_pipeline_ns.fetch_add(duration_ns, std::memory_order_relaxed);
			compute_pipeline_count.fetch_add(1, std::memory_order_relaxed);

			if (primary)
			{
				if (opts.pipeline_stats)
					get_pipeline_stats(work_item.tag, work_item.hash, *work_item.output.pipeline, feedback, duration_ns);
				if (opts.control_block)
					opts.control_block->successful_compute.fetch_add(1, std::memory_order_relaxed);
				check_pipeline_cache_feedback(feedback);
			}

			complete_work_item(work_item);
		}
		else
		{
			LOGE("Failed to create compute pipeline for hash 0x%016" PRIx64 ".\n", work_item.hash);
		}
	}

	void run_creation_work_item_compute(const PipelineWorkItem &work_item)
	{
		auto cache = get_current_pipeline_cache(work_item);
		for (unsigned i = 0; i < loop_count; i++)
			run_creation_work_item_compute_iteration(work_item, cache, i == 0);
	}

	void run_creation_work_item_raytracing_iteration(const PipelineWorkItem &work_item, VkPipelineCache cache, bool primary)
	{
		reset_work_item(work_item);

		auto start_time = chrono::steady_clock::now();

#ifdef SIMULATE_UNSTABLE_DRIVER
		spurious_crash();
#endif
		PipelineFeedback feedback;
		if ((disk_pipeline_cache || opts.pipeline_stats) && device->pipeline_feedback_enabled())
			feedback.setup_pnext(work_item);

		if (vkCreateRayTracingPipelinesKHR(device->get_device(), VK_NULL_HANDLE, cache, 1,
		                                   work_item.create_info.raytracing_create_info,
		                                   nullptr, work_item.output.pipeline) == VK_SUCCESS)
		{
			auto end_time = chrono::steady_clock::now();
			auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

			raytracing_pipeline_ns.fetch_add(duration_ns, std::memory_order_relaxed);
			raytracing_pipeline_count.fetch_add(1, std::memory_order_relaxed);

			if (primary)
			{
				if (opts.pipeline_stats)
					get_pipeline_stats(work_item.tag, work_item.hash, *work_item.output.pipeline, feedback, duration_ns);
				if (opts.control_block)
					opts.control_block->successful_raytracing.fetch_add(1, std::memory_order_relaxed);
				check_pipeline_cache_feedback(feedback);
			}

			complete_work_item(work_item);
		}
		else
		{
			LOGE("Failed to create raytracing pipeline for hash 0x%016" PRIx64 ".\n", work_item.hash);
		}
	}

	void run_creation_work_item_raytracing(const PipelineWorkItem &work_item)
	{
		auto cache = get_current_pipeline_cache(work_item);
		for (unsigned i = 0; i < loop_count; i++)
			run_creation_work_item_raytracing_iteration(work_item, cache, i == 0);
	}

	void run_creation_work_item(const PipelineWorkItem &work_item)
	{
		if (!run_creation_work_item_setup(work_item))
			return;

		bool valid_type = true;

		switch (work_item.tag)
		{
		case RESOURCE_GRAPHICS_PIPELINE:
		{
			run_creation_work_item_graphics(work_item);
			break;
		}

		case RESOURCE_COMPUTE_PIPELINE:
		{
			run_creation_work_item_compute(work_item);
			break;
		}

		case RESOURCE_RAYTRACING_PIPELINE:
		{
			run_creation_work_item_raytracing(work_item);
			break;
		}

		default:
			valid_type = false;
			break;
		}

		auto &per_thread = get_per_thread_data();

		if (valid_type)
		{
			if (!per_thread.triggered_validation_error)
				whitelist_resource(work_item.tag, work_item.hash);
			mark_replayed_resource(work_item.tag, work_item.hash);
		}

		per_thread.current_compute_pipeline = 0;
		per_thread.current_graphics_pipeline = 0;
		per_thread.current_raytracing_pipeline = 0;
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
		if (device && disk_pipeline_cache)
		{
			if (!opts.on_disk_pipeline_cache_path.empty())
			{
				size_t pipeline_cache_size = 0;
				if (vkGetPipelineCacheData(device->get_device(), disk_pipeline_cache, &pipeline_cache_size, nullptr) == VK_SUCCESS)
				{
					vector<uint8_t> pipeline_buffer(pipeline_cache_size);
					if (vkGetPipelineCacheData(device->get_device(), disk_pipeline_cache, &pipeline_cache_size, pipeline_buffer.data()) == VK_SUCCESS)
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
			vkDestroyPipelineCache(device->get_device(), disk_pipeline_cache, nullptr);
			disk_pipeline_cache = VK_NULL_HANDLE;
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

		free_pipelines();

		shader_modules.delete_cache([this](Hash, VkShaderModule module) {
			if (module != VK_NULL_HANDLE)
				vkDestroyShaderModule(device->get_device(), module, nullptr);
		});
	}

	void free_pipelines()
	{
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
		for (auto &pipeline : raytracing_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);

		// Keep track of how many entries we would have had for accurate reporting.
		compute_pipelines_cleared += compute_pipelines.size();
		graphics_pipelines_cleared += graphics_pipelines.size();
		raytracing_pipelines_cleared += raytracing_pipelines.size();

		compute_pipelines.clear();
		graphics_pipelines.clear();
		raytracing_pipelines.clear();
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
		// TODO: Could use this to create multiple VkDevices for replay as necessary if app changes.
		if (get_per_thread_data().expected_tag != RESOURCE_APPLICATION_INFO)
			return;

		if (!device_was_init)
		{
			// From this point on, we expect forward progress in finite time.
			// If the Vulkan driver is unstable and deadlocks, we'll be able to detect it and kill the process.
			begin_heartbeat();

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

			if (!opts.on_disk_module_identifier_path.empty() && device->module_identifiers_enabled())
			{
				auto &props = device->get_module_identifier_properties();
				char uuid_string[2 * VK_UUID_SIZE + 1];
				for (unsigned i = 0; i < VK_UUID_SIZE; i++)
					sprintf(uuid_string + 2 * i, "%02x", props.shaderModuleIdentifierAlgorithmUUID[i]);

				report_module_uuid(uuid_string);

				auto path = opts.on_disk_module_identifier_path;
				path += '.';
				path += uuid_string;
				path += ".foz";
				module_identifier_db.reset(create_stream_archive_database(path.c_str(), DatabaseMode::Append));
				if (!module_identifier_db->prepare())
				{
					LOGW("Failed to prepare module identifier database. Disabling identifiers.\n");
					module_identifier_db.reset();
				}
			}

			if (opts.pipeline_stats)
			{
				auto foz_path = opts.pipeline_stats_path + ".__tmp.foz";
				pipeline_stats_db.reset(create_stream_archive_database(foz_path.c_str(), DatabaseMode::OverWrite));
				if (!pipeline_stats_db->prepare())
				{
					LOGW("Failed to prepare stats database. Disabling pipeline stats.\n");
					pipeline_stats_db.reset();
					opts.pipeline_stats = false;
				}
			}

			if (!opts.on_disk_pipeline_cache_path.empty())
			{
				VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
				vector<uint8_t> on_disk_cache;

				// Try to load on-disk cache.
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

				if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &disk_pipeline_cache) != VK_SUCCESS)
				{
					LOGW("Failed to create pipeline cache, trying to create a blank one.\n");
					info.initialDataSize = 0;
					info.pInitialData = nullptr;
					if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &disk_pipeline_cache) != VK_SUCCESS)
					{
						LOGE("Failed to create pipeline cache.\n");
						disk_pipeline_cache = VK_NULL_HANDLE;
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
						LOGW("Failed to create validation cache, trying to create a blank one.\n");
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

			if (!opts.replayer_cache_path.empty())
				if (!init_replayer_cache())
					LOGW("Failed to initialize replayer cache. Ignoring!\n");

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
		auto &per_thread = get_per_thread_data();
		if (per_thread.expected_tag != RESOURCE_SAMPLER &&
		    per_thread.expected_tag != RESOURCE_DESCRIPTOR_SET_LAYOUT &&
		    per_thread.expected_tag != RESOURCE_PIPELINE_LAYOUT)
		{
			return false;
		}

		if (!device->get_feature_filter().sampler_is_supported(create_info))
		{
			LOGW("Sampler %016" PRIx64 " is not supported. Skipping.\n", index);
			return false;
		}

		if (device->create_sampler_with_ycbcr_remap(create_info, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %016" PRIx64 " Failed!\n", index);
			return false;
		}

		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		auto &per_thread = get_per_thread_data();
		if (per_thread.expected_tag != RESOURCE_DESCRIPTOR_SET_LAYOUT &&
		    per_thread.expected_tag != RESOURCE_PIPELINE_LAYOUT)
		{
			return false;
		}

		if (!device->get_feature_filter().descriptor_set_layout_is_supported(create_info))
		{
			LOGW("Descriptor set layout %016" PRIx64 " is not supported. Skipping.\n", index);
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
		if (get_per_thread_data().expected_tag != RESOURCE_PIPELINE_LAYOUT)
			return false;

		if (!device->get_feature_filter().pipeline_layout_is_supported(create_info))
		{
			LOGW("Pipeline layout %016" PRIx64 " is not supported. Skipping.\n", index);
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
		if (get_per_thread_data().expected_tag != RESOURCE_RENDER_PASS)
			return false;

		if (!device->get_feature_filter().render_pass_is_supported(create_info))
		{
			LOGW("Render pass %016" PRIx64 " is not supported. Skipping.\n", index);
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

	bool enqueue_create_render_pass2(Hash index, const VkRenderPassCreateInfo2 *create_info, VkRenderPass *render_pass) override
	{
		if (get_per_thread_data().expected_tag != RESOURCE_RENDER_PASS)
			return false;

		if (!device->get_feature_filter().render_pass2_is_supported(create_info))
		{
			LOGW("Render pass (version 2) %016" PRIx64 " is not supported. Skipping.\n", index);
			return false;
		}

		// Playback in-order.
		if (vkCreateRenderPass2KHR(device->get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
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

		auto &per_thread = get_per_thread_data();
		if (per_thread.expected_hash != hash || per_thread.expected_tag != RESOURCE_SHADER_MODULE)
		{
			LOGE("Unexpected resource type or hash in blob, ignoring.\n");
			return false;
		}

		per_thread.acknowledge_parsing_work = true;

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
		if (opts.spirv_validate && !has_resource_in_whitelist(RESOURCE_SHADER_MODULE, hash))
		{
			auto start_time = chrono::steady_clock::now();
			spv_target_env env;
			if (device->get_api_version() >= VK_VERSION_1_3)
				env = SPV_ENV_VULKAN_1_3;
			else if (device->get_api_version() >= VK_VERSION_1_2)
				env = SPV_ENV_VULKAN_1_2;
			else if (device->get_api_version() >= VK_VERSION_1_1)
				env = SPV_ENV_VULKAN_1_1;
			else
				env = SPV_ENV_VULKAN_1_0;

			spvtools::SpirvTools context(env);
			spvtools::ValidatorOptions validation_opts;
			validation_opts.SetScalarBlockLayout(device->get_feature_filter().supports_scalar_block_layout());
			context.SetMessageConsumer([](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
				LOGE("spirv-val: %s\n", message);
			});

			bool ret = context.Validate(create_info->pCode, create_info->codeSize / 4, validation_opts);

			auto end_time = chrono::steady_clock::now();
			auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
			shader_module_ns.fetch_add(duration_ns, std::memory_order_relaxed);

			if (!ret)
			{
				LOGW("Failed to validate SPIR-V module: %0" PRIX64 ", skipping!\n", hash);
				*module = VK_NULL_HANDLE;
				lock_guard<mutex> lock(internal_enqueue_mutex);
				//LOGI("Inserting shader module %016llx.\n", static_cast<unsigned long long>(hash));
				shader_modules.insert_object(hash, VK_NULL_HANDLE, 1);
				shader_module_count.fetch_add(1, std::memory_order_relaxed);

				if (opts.control_block)
					opts.control_block->module_validation_failures.fetch_add(1, std::memory_order_relaxed);

				blacklist_resource(RESOURCE_SHADER_MODULE, hash);
				return true;
			}
		}
#endif

		if (!device->get_feature_filter().shader_module_is_supported(create_info))
		{
			LOGW("Shader module %0" PRIx64 " is not supported on this device.\n", hash);
			*module = VK_NULL_HANDLE;

			lock_guard<mutex> lock(internal_enqueue_mutex);
			//LOGI("Inserting shader module %016llx.\n", static_cast<unsigned long long>(hash));
			shader_modules.insert_object(hash, VK_NULL_HANDLE, 1);
			shader_module_count.fetch_add(1, std::memory_order_relaxed);

			if (opts.control_block)
				opts.control_block->module_validation_failures.fetch_add(1, std::memory_order_relaxed);

			return true;
		}

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

				if (i == 0 && module_identifier_db)
				{
					VkShaderModuleIdentifierEXT ident = { VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT };
					vkGetShaderModuleIdentifierEXT(device->get_device(), *module, &ident);
					std::lock_guard<std::mutex> holder{module_identifier_db_mutex};
					if (!module_identifier_db->has_entry(RESOURCE_SHADER_MODULE, hash))
					{
						module_identifier_db->write_entry(RESOURCE_SHADER_MODULE, hash,
						                                  ident.identifier, ident.identifierSize,
						                                  PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT);
					}
				}

				if (opts.control_block && i == 0)
					opts.control_block->successful_modules.fetch_add(1, std::memory_order_relaxed);

				if (i == 0)
					device->get_feature_filter().register_shader_module_info(*module, create_info);
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
		if (!per_thread.triggered_validation_error)
			whitelist_resource(RESOURCE_SHADER_MODULE, hash);
		else
			blacklist_resource(RESOURCE_SHADER_MODULE, hash);

		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		// Ignore derived pipelines, no relevant drivers use them.
		auto *info = const_cast<VkComputePipelineCreateInfo *>(create_info);
		info->flags &= ~(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
		info->basePipelineHandle = VK_NULL_HANDLE;
		info->basePipelineIndex = -1;

		if (opts.pipeline_stats)
			info->flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;

		bool generates_library = (create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;

		auto &per_thread = get_per_thread_data();
		unsigned index = per_thread.current_parse_index;
		unsigned memory_index = per_thread.memory_context_index;
		bool force_outside_range = per_thread.force_outside_range;

		if (per_thread.expected_hash != hash || per_thread.expected_tag != RESOURCE_COMPUTE_PIPELINE)
		{
			LOGE("Unexpected resource type or hash in blob, ignoring.\n");
			return false;
		}

		per_thread.acknowledge_parsing_work = true;

		if (!force_outside_range && index < deferred_compute[memory_index].size())
		{
			deferred_compute[memory_index][index] = {
				const_cast<VkComputePipelineCreateInfo *>(create_info), hash, pipeline, index,
			};
		}
		else
		{
			lock_guard<mutex> holder(hash_lock);
			if (generates_library)
			{
				auto &p = compute_parents[hash];
				p = { const_cast<VkComputePipelineCreateInfo *>(create_info), hash, pipeline, index };
			}
		}

		*pipeline = (VkPipeline)hash;
		if (opts.control_block)
			opts.control_block->parsed_compute.fetch_add(1, std::memory_order_relaxed);

		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		// Ignore derived pipelines, no relevant drivers use them.
		auto *info = const_cast<VkGraphicsPipelineCreateInfo *>(create_info);
		info->flags &= ~(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
		info->basePipelineHandle = VK_NULL_HANDLE;
		info->basePipelineIndex = -1;

		if (opts.pipeline_stats)
			info->flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;

		bool generates_library = (create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;

		auto &per_thread = get_per_thread_data();
		unsigned index = per_thread.current_parse_index;
		unsigned memory_index = per_thread.memory_context_index;
		bool force_outside_range = per_thread.force_outside_range;

		if (per_thread.expected_hash != hash || per_thread.expected_tag != RESOURCE_GRAPHICS_PIPELINE)
		{
			LOGE("Unexpected resource type or hash in blob, ignoring.\n");
			return false;
		}

		per_thread.acknowledge_parsing_work = true;

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
			if (generates_library)
			{
				auto &p = graphics_parents[hash];
				p = { const_cast<VkGraphicsPipelineCreateInfo *>(create_info), hash, pipeline, index };
			}
		}

		*pipeline = (VkPipeline)hash;
		if (opts.control_block)
			opts.control_block->parsed_graphics.fetch_add(1, std::memory_order_relaxed);

		return true;
	}

	bool enqueue_create_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info, VkPipeline *pipeline) override
	{
		// Ignore derived pipelines, no relevant drivers use them.
		auto *info = const_cast<VkRayTracingPipelineCreateInfoKHR *>(create_info);
		info->flags &= ~(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
		info->basePipelineHandle = VK_NULL_HANDLE;
		info->basePipelineIndex = -1;

		bool generates_library = (create_info->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0;

		if (opts.pipeline_stats)
			info->flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;

		auto &per_thread = get_per_thread_data();
		unsigned index = per_thread.current_parse_index;
		unsigned memory_index = per_thread.memory_context_index;
		bool force_outside_range = per_thread.force_outside_range;

		if (per_thread.expected_hash != hash || per_thread.expected_tag != RESOURCE_RAYTRACING_PIPELINE)
		{
			LOGE("Unexpected resource type or hash in blob, ignoring.\n");
			return false;
		}

		per_thread.acknowledge_parsing_work = true;

		if (!force_outside_range)
		{
			assert(index < deferred_raytracing[memory_index].size());
			deferred_raytracing[memory_index][index] = {
				const_cast<VkRayTracingPipelineCreateInfoKHR *>(create_info), hash, pipeline, index,
			};
		}
		else
		{
			lock_guard<mutex> holder(hash_lock);
			if (generates_library)
			{
				auto &p = raytracing_parents[hash];
				p = { const_cast<VkRayTracingPipelineCreateInfoKHR *>(create_info), hash, pipeline, index };
			}
		}

		*pipeline = (VkPipeline)hash;
		if (opts.control_block)
			opts.control_block->parsed_raytracing.fetch_add(1, std::memory_order_relaxed);

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

		bool valid_handles = true;
		auto *library = find_pnext<VkPipelineLibraryCreateInfoKHR>(
				VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, create_info->pNext);
		if (library)
		{
			for (uint32_t i = 0; i < library->libraryCount; i++)
				if (library->pLibraries[i] == VK_NULL_HANDLE)
					valid_handles = false;
		}

		if (create_info->stage.module != VK_NULL_HANDLE && valid_handles)
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

		auto *library = find_pnext<VkPipelineLibraryCreateInfoKHR>(
				VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, create_info->pNext);

		if (library)
		{
			for (uint32_t i = 0; i < library->libraryCount; i++)
				if (library->pLibraries[i] == VK_NULL_HANDLE)
					valid_handles = false;
		}

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

	bool enqueue_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info, VkPipeline *pipeline,
	                      unsigned index, unsigned memory_context_index)
	{
		bool valid_handles = true;
		for (uint32_t i = 0; i < create_info->stageCount; i++)
			if (create_info->pStages[i].module == VK_NULL_HANDLE)
				valid_handles = false;

		if (create_info->pLibraryInfo)
		{
			for (uint32_t i = 0; i < create_info->pLibraryInfo->libraryCount; i++)
				if (create_info->pLibraryInfo->pLibraries[i] == VK_NULL_HANDLE)
					valid_handles = false;
		}

		PipelineWorkItem work_item = {};
		work_item.hash = hash;
		work_item.tag = RESOURCE_RAYTRACING_PIPELINE;
		work_item.output.pipeline = pipeline;
		work_item.index = index;
		work_item.memory_context_index = memory_context_index;

		if (valid_handles)
		{
			work_item.create_info.raytracing_create_info = create_info;
			lock_guard<mutex> lock(internal_enqueue_mutex);
			// Pointer to value in std::unordered_map remains fixed per spec (node-based).
			work_item.hash_map_entry.pipeline = &raytracing_pipelines[hash];
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

	bool enqueue_shader_modules(const VkRayTracingPipelineCreateInfoKHR *info)
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
			const_cast<VkPipelineShaderStageCreateInfo *>(info->pStages)[i].module = result.first;
		}
	}

	void resolve_shader_modules(VkRayTracingPipelineCreateInfoKHR *info)
	{
		for (uint32_t i = 0; i < info->stageCount; i++)
		{
			auto result = shader_modules.find_object((Hash) info->pStages[i].module);
			const_cast<VkPipelineShaderStageCreateInfo *>(info->pStages)[i].module = result.first;
		}
	}

	void resolve_shader_modules(VkComputePipelineCreateInfo *info)
	{
		auto result = shader_modules.find_object((Hash) info->stage.module);
		const_cast<VkComputePipelineCreateInfo*>(info)->stage.module = result.first;
	}

	template <typename DerivedInfo>
	static const VkPipelineLibraryCreateInfoKHR *work_item_get_library_info(const DerivedInfo &info)
	{
		if (!info.info)
			return nullptr;
		return find_pnext<VkPipelineLibraryCreateInfoKHR>(
			VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, info.info->pNext);
	}

	static const VkPipelineLibraryCreateInfoKHR *work_item_get_library_info(const DeferredRayTracingInfo &info)
	{
		return info.info ? info.info->pLibraryInfo : nullptr;
	}

	template <typename DerivedInfo>
	static bool work_item_is_derived(const DerivedInfo &info)
	{
		if (!info.info)
			return true;
		auto *library = work_item_get_library_info(info);
		return library && library->libraryCount != 0;
	}

	bool pipeline_library_info_is_satisfied(const VkPipelineLibraryCreateInfoKHR &library,
	                                        const unordered_map<Hash, VkPipeline> &pipelines) const
	{
		for (uint32_t i = 0; i < library.libraryCount; i++)
		{
			Hash hash = (Hash)library.pLibraries[i];
			if (!pipelines.count(hash))
				return false;
		}
		return true;
	}

	void resolve_pipeline_library_info(const VkPipelineLibraryCreateInfoKHR &library,
	                                   const unordered_map<Hash, VkPipeline> &pipelines) const
	{
		auto *libs = const_cast<VkPipeline *>(library.pLibraries);
		for (uint32_t i = 0; i < library.libraryCount; i++)
		{
			auto base_itr = pipelines.find((Hash) libs[i]);
			libs[i] = base_itr != end(pipelines) ? base_itr->second : VK_NULL_HANDLE;
		}
	}

	template <typename DerivedInfo>
	bool derived_work_item_is_satisfied(const DerivedInfo &info,
	                                    const unordered_map<Hash, VkPipeline> &pipelines) const
	{
		if (!info.info)
			return false;
		auto *library = work_item_get_library_info(info);
		return !library || pipeline_library_info_is_satisfied(*library, pipelines);
	}

	template <typename DerivedInfo>
	void resolve_pipelines(DerivedInfo &info, const unordered_map<Hash, VkPipeline> &pipelines) const
	{
		auto *library = work_item_get_library_info(info);
		if (library)
			resolve_pipeline_library_info(*library, pipelines);
	}

	template <typename DerivedInfo>
	void sort_deferred_derived_pipelines(vector<DerivedInfo> &derived, vector<DerivedInfo> &deferred)
	{
		sort(begin(deferred), end(deferred), [&](const DerivedInfo &a, const DerivedInfo &b) -> bool {
			bool a_derived = work_item_is_derived(a);
			bool b_derived = work_item_is_derived(b);
			if (a_derived == b_derived)
				return a.index < b.index;
			else
				return b_derived;
		});

		unsigned end_index_non_derived = 0;
		unsigned index = 0;
		for (auto &def : deferred)
		{
			index++;
			if (!work_item_is_derived(def))
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
	void enqueue_parent_pipeline(const DerivedInfo &info, Hash h,
	                             const unordered_map<Hash, VkPipeline> &pipelines,
	                             unordered_map<Hash, DerivedInfo> &parents,
	                             unordered_set<Hash> &parsed_parents)
	{
		if (!h)
			return;

		// pipelines will have an entry if we called enqueue_pipeline() already for this hash,
		// it's not necessarily complete yet!
		bool is_outside_range = pipelines.count(h) == 0;

		if (is_outside_range && parsed_parents.count(h) == 0)
		{
			{
				lock_guard<mutex> holder(hash_lock);
				if (parents.count(h) != 0)
					return;
			}

			PipelineWorkItem work_item;
			work_item.index = info.index;
			work_item.hash = h;
			work_item.parse_only = true;
			work_item.force_outside_range = true;
			work_item.memory_context_index = PARENT_PIPELINE_MEMORY_CONTEXT;
			work_item.tag = DerivedInfo::get_tag();
			enqueue_work_item(work_item);

			parsed_parents.insert(h);
		}
	}

	template <typename DerivedInfo>
	void enqueue_parent_pipelines_pipeline_library(const DerivedInfo &info,
	                                               const VkPipelineLibraryCreateInfoKHR &library_info,
	                                               const unordered_map<Hash, VkPipeline> &pipelines,
	                                               unordered_map<Hash, DerivedInfo> &parents,
	                                               unordered_set<Hash> &parsed_parents)
	{
		for (uint32_t i = 0; i < library_info.libraryCount; i++)
		{
			Hash h = (Hash)library_info.pLibraries[i];
			enqueue_parent_pipeline(info, h, pipelines, parents, parsed_parents);
		}
	}

	template <typename DerivedInfo>
	void enqueue_parent_pipelines(const DerivedInfo &info,
	                              const unordered_map<Hash, VkPipeline> &pipelines,
	                              unordered_map<Hash, DerivedInfo> &parents,
	                              unordered_set<Hash> &parsed_parents)
	{
		auto *library = work_item_get_library_info(info);
		if (library)
			enqueue_parent_pipelines_pipeline_library(info, *library, pipelines, parents, parsed_parents);
	}

	template <typename DerivedInfo>
	void compute_parents_depth(const unordered_map<Hash, DerivedInfo> &parents,
	                           unordered_map<Hash, uint32_t> &parents_depth,
	                           const DerivedInfo &info, uint32_t depth)
	{
		parents_depth[info.hash] = max(parents_depth.at(info.hash), depth);

		auto library = work_item_get_library_info(info);
		if (!library)
			return;

		for (uint32_t i = 0; i < library->libraryCount; i++)
		{
			Hash parent = (Hash)library->pLibraries[i];
			if (parents.count(parent) != 0 && parents_depth.at(parent) <= depth)
				compute_parents_depth(parents, parents_depth, parents.at(parent), depth + 1);
		}
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
			MAINTAIN_LRU_CACHE = 1,
			ENQUEUE_SHADER_MODULES_PRIMARY_OFFSET = 2,
			RESOLVE_SHADER_MODULE_AND_ENQUEUE_PIPELINES_PRIMARY_OFFSET = 3,
			ENQUEUE_OUT_OF_RANGE_PARENT_PIPELINES = 4,
			ENQUEUE_SHADER_MODULE_SECONDARY_OFFSET = 5,
			ENQUEUE_DERIVED_PIPELINES_OFFSET = 6,
			PASS_COUNT = 7
		};

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

		auto parsed_parents = make_shared<unordered_set<Hash>>();

		for (unsigned hash_offset = 0; hash_offset < hashes.size(); hash_offset += NUM_PIPELINES_PER_CONTEXT, iteration++)
		{
			unsigned left_to_submit = hashes.size() - hash_offset;
			unsigned to_submit = left_to_submit < NUM_PIPELINES_PER_CONTEXT ? left_to_submit : NUM_PIPELINES_PER_CONTEXT;

			// State which is used between pipeline stages.
			auto derived = make_shared<vector<DerivedInfo>>();

			// Submit pipelines to be parsed.
			work.push_back({ get_order_index(PARSE_ENQUEUE_OFFSET),
			                 [this, &hashes, deferred, memory_index, to_submit, hash_offset]() {
				                 // Drain old allocators.
				                 sync_worker_memory_context(memory_index);
				                 // Reset per memory-context allocators.
				                 for (auto &data : per_thread_data)
				                 {
					                 if (data.per_thread_replayers)
					                 {
						                 data.per_thread_replayers[memory_index].get_allocator().reset();
						                 // We might have to replay the same pipeline multiple times,
						                 // forget handle references.
						                 data.per_thread_replayers[memory_index].forget_pipeline_handle_references();
					                 }
				                 }

				                 // Important that memory is cleared here since we yoinked away the memory
				                 // when resetting allocators.
				                 deferred[memory_index].clear();

				                 deferred[memory_index].resize(to_submit);

				                 for (unsigned index = hash_offset; index < hash_offset + to_submit; index++)
				                 {
					                 auto tag = DerivedInfo::get_tag();
					                 if (cached_blobs[tag].count(hashes[index]) != 0)
					                 {
						                 // Do not do anything with this pipeline.
						                 // Need to check here which is not optimal, since we need to maintain a stable pipeline index
						                 // for the robust replayer mechanism.
						                 // TODO: Consider pre-parsing various databases and emit a SHM block for child replayer processes.
						                 if (opts.control_block)
						                 {
							                 if (tag == RESOURCE_GRAPHICS_PIPELINE)
							                 {
								                 opts.control_block->total_graphics.fetch_add(1, std::memory_order_relaxed);
								                 opts.control_block->cached_graphics.fetch_add(1, std::memory_order_relaxed);
							                 }
							                 else if (tag == RESOURCE_COMPUTE_PIPELINE)
							                 {
								                 opts.control_block->total_compute.fetch_add(1, std::memory_order_relaxed);
								                 opts.control_block->cached_compute.fetch_add(1, std::memory_order_relaxed);
							                 }
							                 else if (tag == RESOURCE_RAYTRACING_PIPELINE)
							                 {
								                 opts.control_block->total_raytracing.fetch_add(1, std::memory_order_relaxed);
								                 opts.control_block->cached_raytracing.fetch_add(1, std::memory_order_relaxed);
							                 }
						                 }
					                 }
					                 else
					                 {
						                 // We are going to free all existing parent pipelines, so we cannot reuse
						                 // any pipeline that was already compiled.
						                 PipelineWorkItem work_item;
						                 work_item.hash = hashes[index];
						                 work_item.tag = DerivedInfo::get_tag();
						                 work_item.parse_only = true;
						                 work_item.memory_context_index = memory_index;
						                 work_item.index = index - hash_offset;

						                 if (opts.control_block)
						                 {
							                 if (tag == RESOURCE_GRAPHICS_PIPELINE)
								                 opts.control_block->total_graphics.fetch_add(1, std::memory_order_relaxed);
							                 else if (tag == RESOURCE_COMPUTE_PIPELINE)
								                 opts.control_block->total_compute.fetch_add(1, std::memory_order_relaxed);
							                 else if (tag == RESOURCE_RAYTRACING_PIPELINE)
								                 opts.control_block->total_raytracing.fetch_add(1, std::memory_order_relaxed);
						                 }

						                 enqueue_work_item(work_item);
					                 }
				                 }
			                 }});

			if (memory_index == 0)
			{
				work.push_back({ get_order_index(MAINTAIN_LRU_CACHE),
				                 [this, &parents]() {
					                 // Now all worker threads are drained for any work which needs shader modules,
					                 // so we can maintain the shader module LRU cache while we're parsing new pipelines in parallel.
					                 shader_modules.prune_cache([this](Hash hash, VkShaderModule module) {
						                 assert(enqueued_shader_modules.count((VkShaderModule) hash) != 0);
						                 //LOGI("Removing shader module %016llx.\n", static_cast<unsigned long long>(hash));
						                 enqueued_shader_modules.erase((VkShaderModule) hash);
						                 if (module != VK_NULL_HANDLE)
						                 {
							                 device->get_feature_filter().unregister_shader_module_info(module);
							                 vkDestroyShaderModule(device->get_device(), module, nullptr);
						                 }

						                 shader_module_evicted_count.fetch_add(1, std::memory_order_relaxed);
					                 });

					                 // Need to forget that we have seen an object before so we can replay the same object multiple times.
					                 for (auto &per_thread : per_thread_data)
						                 if (per_thread.per_thread_replayers)
							                 per_thread.per_thread_replayers[SHADER_MODULE_MEMORY_CONTEXT].forget_handle_references();

									 // We also know that pipelines are not being compiled,
									 // so we can free pipelines.
									 // When using graphics pipeline libraries, most pipelines
									 // will be "parent" pipelines, which leads to excessive memory bloat
									 // if we keep them around indefinitely.
					                 free_pipelines();

					                 parents.clear();

					                 // We might have to replay the same pipeline multiple times,
					                 // forget handle references.
					                 for (auto &per_thread : per_thread_data)
						                 if (per_thread.per_thread_replayers)
							                 per_thread.per_thread_replayers[PARENT_PIPELINE_MEMORY_CONTEXT].forget_pipeline_handle_references();
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
				                 // Remap VkShaderModule references from hashes to real handles and enqueue
				                 // all non-derived pipelines for work.
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
			                 [this, &pipelines, &parents, derived, parsed_parents]() {
				                 // Figure out which of the parent pipelines we need.
				                 for (auto &d : *derived)
					                 enqueue_parent_pipelines(d, pipelines, parents, *parsed_parents);
			                 }});

			if (memory_index == 0)
			{
				// This is a join-like operation. We need to wait for all parent pipelines and all shader modules to have completed.
				work.push_back({get_order_index(ENQUEUE_SHADER_MODULE_SECONDARY_OFFSET),
				                [this, &pipelines, &parents, parsed_parents, hash_offset, start_index]()
				                {
					                unordered_set<Hash> dependencies;
					                while (true)
					                {
						                // Wait until all parent pipelines have been parsed.
						                sync_worker_memory_context(PARENT_PIPELINE_MEMORY_CONTEXT);

						                dependencies.clear();
						                std::swap(*parsed_parents, dependencies);

						                for (auto dependency : dependencies)
						                {
							                // Handle nested libraries.
							                enqueue_parent_pipelines(parents.at(dependency), pipelines, parents, *parsed_parents);
						                }

						                if (parsed_parents->empty())
							                break;
					                }

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
						                {
							                opts.control_block->total_graphics.fetch_add(parents.size(),
							                                                             std::memory_order_relaxed);
						                }
						                else if (tag == RESOURCE_COMPUTE_PIPELINE)
						                {
							                opts.control_block->total_compute.fetch_add(parents.size(),
							                                                            std::memory_order_relaxed);
						                }
						                else if (tag == RESOURCE_RAYTRACING_PIPELINE)
						                {
							                opts.control_block->total_raytracing.fetch_add(parents.size(),
							                                                               std::memory_order_relaxed);
						                }
					                }

					                sync_worker_memory_context(SHADER_MODULE_MEMORY_CONTEXT);

					                unordered_map<Hash, uint32_t> parents_depth;

					                dependencies.clear();
					                for (const auto &parent : parents)
					                {
						                parents_depth[parent.first] = 0;

						                auto library = work_item_get_library_info(parent.second);
						                if (!library)
							                continue;

						                for (uint32_t i = 0; i < library->libraryCount; i++)
							                dependencies.insert((Hash)library->pLibraries[i]);
					                }

					                // Compute the depth of every pipeline library using a DFS starting at the top level libraries.
					                vector<DerivedInfo> ordered_parents; 
					                for (const auto &parent : parents)
					                {
						                ordered_parents.push_back(parent.second);

						                if (dependencies.count(parent.first) == 0)
							                compute_parents_depth(parents, parents_depth, parent.second, 0);
					                }

					                sort(begin(ordered_parents), end(ordered_parents), [&](const DerivedInfo &a, const DerivedInfo &b) -> bool {
						                return parents_depth.at(a.hash) > parents_depth.at(b.hash);
					                });

					                // We might be pulling in a parent pipeline from another memory context next iteration,
					                // so we need to wait for all normal memory contexts.
					                for (unsigned i = 0; i < NUM_PIPELINE_MEMORY_CONTEXTS; i++)
						                sync_worker_memory_context(i);

					                uint32_t prev_depth = UINT32_MAX;
					                for (auto &parent : ordered_parents)
					                {
						                if (!parent.info)
							                continue;

						                if (!derived_work_item_is_satisfied(parent, pipelines))
							                continue;

						                uint32_t depth = parents_depth.at(parent.hash);
						                if (prev_depth != depth)
						                {
							                prev_depth = depth;
							                sync_worker_memory_context(PARENT_PIPELINE_MEMORY_CONTEXT);
						                }

						                resolve_shader_modules(parent.info);
						                resolve_pipelines(parent, pipelines);
						                enqueue_pipeline(parent.hash, parent.info,
						                                 parent.pipeline,
						                                 parent.index + hash_offset + start_index,
						                                 PARENT_PIPELINE_MEMORY_CONTEXT);
					                }
				                }});
			}

			work.push_back({ get_order_index(ENQUEUE_DERIVED_PIPELINES_OFFSET),
			                 [this, &pipelines, derived, &parents, memory_index, hash_offset, start_index]() {
				                 // Go over all pipelines. If there are no further dependencies to resolve, we can go ahead and queue them up.
				                 // If an entry exists in pipelines, we have queued up that hash earlier, but it might not be done compiling yet.
				                 auto itr = unstable_remove_if(begin(*derived), end(*derived), [&](const DerivedInfo &info) -> bool {
					                 return derived_work_item_is_satisfied(info, pipelines) || parents.count(info.hash) != 0;
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
					                 // itr always removes parent pipelines because they were already compiled
					                 // in ENQUEUE_SHADER_MODULE_SECONDARY_OFFSET. This way they do not count
					                 // as skipped, but it also means that we need to skip them here to avoid
					                 // compiling them twice.
					                 if (i->info && parents.count(i->hash) == 0)
					                 {
						                 resolve_shader_modules(i->info);
						                 resolve_pipelines(*i, pipelines);
						                 enqueue_pipeline(i->hash, i->info, i->pipeline,
						                                  i->index + hash_offset + start_index, memory_index);
					                 }
				                 }

				                 // It might be possible that we couldn't resolve some dependencies, log this.
				                 if (itr != begin(*derived))
				                 {
					                 auto skipped_count = unsigned(itr - begin(*derived));
					                 LOGW("%u pipelines were not compiled because parent pipelines do not exist.\n", skipped_count);

					                 if (opts.control_block)
					                 {
						                 auto tag = DerivedInfo::get_tag();
						                 if (tag == RESOURCE_GRAPHICS_PIPELINE)
							                 opts.control_block->skipped_graphics.fetch_add(skipped_count, std::memory_order_relaxed);
						                 else if (tag == RESOURCE_COMPUTE_PIPELINE)
							                 opts.control_block->skipped_compute.fetch_add(skipped_count, std::memory_order_relaxed);
						                 else if (tag == RESOURCE_RAYTRACING_PIPELINE)
							                 opts.control_block->skipped_raytracing.fetch_add(skipped_count, std::memory_order_relaxed);
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
	}

	Options opts;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;

	ObjectCache<VkShaderModule> shader_modules;

	std::unordered_map<Hash, VkRenderPass> render_passes;

	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	std::unordered_map<Hash, VkPipeline> raytracing_pipelines;
	size_t compute_pipelines_cleared = 0;
	size_t graphics_pipelines_cleared = 0;
	size_t raytracing_pipelines_cleared = 0;

	std::unordered_set<Hash> masked_shader_modules;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_hash;
	std::unordered_set<VkShaderModule> enqueued_shader_modules;
	VkPipelineCache disk_pipeline_cache = VK_NULL_HANDLE;
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
	std::unordered_set<Hash> implicit_whitelist[RESOURCE_COUNT];

	std::mutex module_identifier_db_mutex;
	std::unique_ptr<DatabaseInterface> module_identifier_db;

	std::mutex replayer_cache_mutex;
	std::unique_ptr<DatabaseInterface> replayer_cache_db;
	std::unordered_set<Hash> cached_blobs[RESOURCE_COUNT];

	std::mutex hash_lock;
	std::unordered_map<Hash, DeferredGraphicsInfo> graphics_parents;
	std::unordered_map<Hash, DeferredComputeInfo> compute_parents;
	std::unordered_map<Hash, DeferredRayTracingInfo> raytracing_parents;
	std::vector<DeferredGraphicsInfo> deferred_graphics[NUM_MEMORY_CONTEXTS];
	std::vector<DeferredComputeInfo> deferred_compute[NUM_MEMORY_CONTEXTS];
	std::vector<DeferredRayTracingInfo> deferred_raytracing[NUM_MEMORY_CONTEXTS];
	VkPipelineCache memory_context_pipeline_cache[NUM_MEMORY_CONTEXTS] = {};

	// Feed statistics from the worker threads.
	std::atomic<std::uint64_t> graphics_pipeline_ns;
	std::atomic<std::uint64_t> compute_pipeline_ns;
	std::atomic<std::uint64_t> raytracing_pipeline_ns;
	std::atomic<std::uint64_t> shader_module_ns;
	std::atomic<std::uint64_t> total_idle_ns;
	std::atomic<std::uint64_t> thread_total_ns;
	std::atomic<std::uint32_t> graphics_pipeline_count;
	std::atomic<std::uint32_t> compute_pipeline_count;
	std::atomic<std::uint32_t> raytracing_pipeline_count;
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
	if (per_thread.current_raytracing_pipeline)
		replayer->blacklist_resource(RESOURCE_RAYTRACING_PIPELINE, per_thread.current_raytracing_pipeline);

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
	"\t[--progress]\n" \
	"\t[--quiet-slave]\n" \
	"\t[--shm-name <name>]\n\t[--shm-mutex-name <name>]\n" \
	"\t[--metadata-name <name>]\n"
#else
#define EXTRA_OPTIONS \
	"\t[--slave-process]\n" \
	"\t[--master-process]\n" \
	"\t[--progress]\n" \
	"\t[--quiet-slave]\n" \
	"\t[--shmem-fd <fd>]\n" \
	"\t[--control-fd <fd>]\n" \
	"\t[--disable-signal-handler]\n" \
	"\t[--disable-rate-limiter]\n"
#endif
#else
#define EXTRA_OPTIONS ""
#endif
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--enable-pipeline-stats <path>]\n"
	     "\t[--spirv-val]\n"
	     "\t[--num-threads <count>]\n"
	     "\t[--loop <count>]\n"
	     "\t[--on-disk-pipeline-cache <path>]\n"
	     "\t[--on-disk-validation-cache <path>]\n"
	     "\t[--on-disk-validation-whitelist <path>]\n"
	     "\t[--on-disk-validation-blacklist <path>]\n"
	     "\t[--on-disk-replay-whitelist <path>]\n"
	     "\t[--on-disk-replay-whitelist-mask <module/pipeline/hex>]\n"
	     "\t[--on-disk-module-identifier <path>]\n"
	     "\t[--pipeline-hash <hash>]\n"
	     "\t[--graphics-pipeline-range <start> <end>]\n"
	     "\t[--compute-pipeline-range <start> <end>]\n"
	     "\t[--raytracing-pipeline-range <start> <end>]\n"
	     "\t[--shader-cache-size <value (MiB)>]\n"
	     "\t[--ignore-derived-pipelines] (Obsolete, always assumed to be set, kept for compatibility)\n"
	     "\t[--log-memory]\n"
	     "\t[--null-device]\n"
	     "\t[--timeout-seconds]\n"
	     "\t[--implicit-whitelist <index>]\n"
	     "\t[--replayer-cache <path>]\n"
	     EXTRA_OPTIONS
	     "\t<Database>\n");
}

#ifndef NO_ROBUST_REPLAYER
static void log_progress(const ExternalReplayer::Progress &progress)
{
	unsigned current_actions, total_actions;
	ExternalReplayer::compute_condensed_progress(progress, current_actions, total_actions);

	LOGI("=================\n");
	LOGI(" Progress report:\n");
	LOGI("   Overall %u / %u\n", current_actions, total_actions);
	LOGI("   Parsed graphics %u / %u, failed %u, cached %u\n",
	     progress.graphics.parsed, progress.graphics.total, progress.graphics.parsed_fail, progress.graphics.cached);
	LOGI("   Parsed compute %u / %u, failed %u, cached %u\n",
	     progress.compute.parsed, progress.compute.total, progress.compute.parsed_fail, progress.compute.cached);
	LOGI("   Decompress modules %u / %u, skipped %u, failed validation %u, missing %u\n",
	     progress.completed_modules, progress.total_modules, progress.banned_modules, progress.module_validation_failures, progress.missing_modules);
	LOGI("   Compile graphics %u / %u, skipped %u, cached %u\n",
	     progress.graphics.completed, progress.graphics.total, progress.graphics.skipped, progress.graphics.cached);
	LOGI("   Compile compute %u / %u, skipped %u, cached %u\n",
	     progress.compute.completed, progress.compute.total, progress.compute.skipped, progress.compute.cached);
	LOGI("   Compile raytracing %u / %u, skipped %u, cached %u\n",
	     progress.raytracing.completed, progress.raytracing.total, progress.raytracing.skipped, progress.raytracing.cached);
	LOGI("   Clean crashes %u\n", progress.clean_crashes);
	LOGI("   Dirty crashes %u\n", progress.dirty_crashes);
	LOGI("=================\n");
}

static void log_memory_usage(const std::vector<ExternalReplayer::ProcessStats> &usage,
                             const ExternalReplayer::GlobalResourceUsage *global_stats)
{
	LOGI("=================\n");
	LOGI(" Memory usage:\n");
	unsigned index = 0;
	for (auto &use : usage)
	{
		LOGI("   #%u: %5u MiB resident %5u MiB shared (%u MiB shared metadata) [activity %d].\n", index++,
		     use.resident_mib, use.shared_mib, use.shared_metadata_mib, use.heartbeats);
	}

	if (global_stats)
	{
		if (global_stats->dirty_pages_mib >= 0)
			LOGI(" Dirty filesystem writes: %u MiB.\n", global_stats->dirty_pages_mib);
		else
			LOGI(" Dirty filesystem writes: N/A.\n");

		if (global_stats->io_stall_percentage >= 0)
			LOGI(" IO stall: %u%%.\n", global_stats->io_stall_percentage);
		else
			LOGI(" IO stall: N/A.\n");

		LOGI(" Num running child processes: %u.\n", global_stats->num_running_processes);
	}
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

using ValidationFunc = bool (ExternalReplayer::*)(size_t *, Hash *) const;
using FaultFunc = bool (ExternalReplayer::*)(size_t *, unsigned *, Hash *) const;
static void log_faulty_pipelines(ExternalReplayer &replayer,
                                 ValidationFunc validation_query,
                                 FaultFunc fault_query,
                                 const char *tag)
{
	size_t count;
	if (!(replayer.*validation_query)(&count, nullptr))
		return;
	vector<Hash> hashes(count);
	if (!(replayer.*validation_query)(&count, hashes.data()))
		return;

	sort(begin(hashes), end(hashes));

	for (auto &h : hashes)
		LOGI("%s pipeline failed validation: %016" PRIx64 "\n", tag, h);

	vector<unsigned> indices;
	if (!(replayer.*fault_query)(&count, nullptr, nullptr))
		return;
	indices.resize(count);
	hashes.resize(count);
	if (!(replayer.*fault_query)(&count, indices.data(), hashes.data()))
		return;

	for (unsigned i = 0; i < count; i++)
	{
		LOGI("%s pipeline crashed or hung: %016" PRIx64 ". Repro with: --%s-pipeline-range %u %u\n",
		     tag, hashes[i], tag, indices[i], indices[i] + 1);
	}
}

static void log_faulty_pipelines(ExternalReplayer &replayer)
{
	log_faulty_pipelines(replayer, &ExternalReplayer::get_graphics_failed_validation,
	                     &ExternalReplayer::get_faulty_graphics_pipelines, "graphics");
	log_faulty_pipelines(replayer, &ExternalReplayer::get_compute_failed_validation,
	                     &ExternalReplayer::get_faulty_compute_pipelines, "compute");
	log_faulty_pipelines(replayer, &ExternalReplayer::get_raytracing_failed_validation,
	                     &ExternalReplayer::get_faulty_raytracing_pipelines, "raytracing");
}

static int run_progress_process(const VulkanDevice::Options &device_opts,
                                const ThreadedReplayer::Options &replayer_opts,
                                const vector<const char *> &databases,
                                const char *whitelist, uint32_t whitelist_mask,
                                bool log_memory)
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
	opts.on_disk_module_identifier = replayer_opts.on_disk_module_identifier_path.empty() ?
	                                 nullptr : replayer_opts.on_disk_module_identifier_path.c_str();
	opts.pipeline_stats_path = replayer_opts.pipeline_stats_path.empty() ?
	                           nullptr : replayer_opts.pipeline_stats_path.c_str();
	opts.num_threads = replayer_opts.num_threads;
	opts.quiet = true;
	opts.databases = databases.data();
	opts.num_databases = databases.size();
	opts.external_replayer_path = nullptr;
	opts.inherit_process_group = true;
	opts.spirv_validate = replayer_opts.spirv_validate;
	opts.device_index = device_opts.device_index;
	opts.enable_validation = device_opts.enable_validation;
#ifndef _WIN32
	opts.disable_signal_handler = replayer_opts.disable_signal_handler;
	opts.disable_rate_limiter = replayer_opts.disable_rate_limiter;
#endif
	opts.ignore_derived_pipelines = true;
	opts.null_device = device_opts.null_device;
	opts.start_graphics_index = replayer_opts.start_graphics_index;
	opts.end_graphics_index = replayer_opts.end_graphics_index;
	opts.start_compute_index = replayer_opts.start_compute_index;
	opts.end_compute_index = replayer_opts.end_compute_index;
	opts.start_raytracing_index = replayer_opts.start_raytracing_index;
	opts.end_raytracing_index = replayer_opts.end_raytracing_index;
	opts.use_pipeline_range =
			(replayer_opts.start_graphics_index != 0) ||
			(replayer_opts.end_graphics_index != ~0u) ||
			(replayer_opts.start_compute_index != 0) ||
			(replayer_opts.end_compute_index != ~0u) ||
			(replayer_opts.start_raytracing_index != 0) ||
			(replayer_opts.end_raytracing_index != ~0u);
	opts.timeout_seconds = replayer_opts.timeout_seconds;
	opts.implicit_whitelist_indices = replayer_opts.implicit_whitelist_database_indices.data();
	opts.num_implicit_whitelist_indices = replayer_opts.implicit_whitelist_database_indices.size();
	opts.replayer_cache_path = replayer_opts.replayer_cache_path.empty() ?
			nullptr : replayer_opts.replayer_cache_path.c_str();
	opts.on_disk_replay_whitelist = whitelist;
	opts.on_disk_replay_whitelist_mask = whitelist_mask;

	ExternalReplayer replayer;
	if (!replayer.start(opts))
	{
		LOGE("Failed to start external replayer.\n");
		return EXIT_FAILURE;
	}

	for (;;)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		ExternalReplayer::Progress progress = {};

		if (replayer.is_process_complete(nullptr))
		{
			auto result = replayer.poll_progress(progress);
			if (result != ExternalReplayer::PollResult::ResultNotReady)
				log_progress(progress);
			log_faulty_modules(replayer);
			log_faulty_pipelines(replayer);
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
		{
			log_progress(progress);

			if (log_memory)
			{
				uint32_t num_processes;

				if (replayer.poll_memory_usage(&num_processes, nullptr))
				{
					std::vector<ExternalReplayer::ProcessStats> usage(num_processes);
					if (replayer.poll_memory_usage(&num_processes, usage.data()))
					{
						usage.resize(num_processes);
						ExternalReplayer::GlobalResourceUsage global_stats = {};
						bool got_global_stats = replayer.poll_global_resource_usage(global_stats);
						log_memory_usage(usage, got_global_stats ? &global_stats : nullptr);
					}
				}
			}

			if (result == ExternalReplayer::PollResult::Complete)
			{
				log_faulty_modules(replayer);
				log_faulty_pipelines(replayer);
				return replayer.wait();
			}
			break;
		}
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
		RESOURCE_RAYTRACING_PIPELINE,
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

	header.emplace_back("Database");
	header.emplace_back("Pipeline type");
	header.emplace_back("Pipeline hash");
	header.emplace_back("PSO wall duration (ns)");
	header.emplace_back("PSO duration (ns)");
	header.emplace_back("Stage duration (ns)");
	header.emplace_back("Executable name");
	header.emplace_back("Subgroup size");

	for (auto itr = doc.Begin(); itr != doc.End(); itr++)
	{
		std::map<size_t, std::string> row;

		auto &st = *itr;

		if (!st.HasMember("db_path") ||
		    !st.HasMember("pipeline_type") ||
		    !st.HasMember("pipeline") ||
		    !st.HasMember("executables") ||
		    !st.HasMember("pso_wall_duration_ns") ||
		    !st.HasMember("pso_duration_ns"))
		{
			LOGE("db_path, pipeline_type, pso_wall_duration_ns, pso_duration_ns, pipeline and executable members expected, but not found. Stale stats FOZ file?\n");
			return;
		}

		row[0] = st["db_path"].GetString();
		row[1] = st["pipeline_type"].GetString();
		row[2] = st["pipeline"].GetString();
		row[3] = std::to_string(st["pso_wall_duration_ns"].GetUint64());
		row[4] = std::to_string(st["pso_duration_ns"].GetUint64());
		auto &execs = st["executables"];

		for (auto e_itr = execs.Begin(); e_itr != execs.End(); e_itr++)
		{
			auto &exec = *e_itr;

			if (!exec.HasMember("executable_name") ||
			    !exec.HasMember("subgroup_size") ||
			    !exec.HasMember("stats") ||
			    !exec.HasMember("stage_duration_ns"))
			{
				LOGE("Expected executable_name, subgroup_size, stage_duration_ns and stats members, but not found. Stale stats file?\n");
				return;
			}

			row[5] = std::to_string(exec["stage_duration_ns"].GetUint64());
			row[6] = exec["executable_name"].GetString();
			row[7] = std::to_string(exec["subgroup_size"].GetUint());

			for (auto st_itr = exec["stats"].Begin(); st_itr != exec["stats"].End(); st_itr++)
			{
				auto &stat = *st_itr;

				if (!stat.HasMember("value") || !stat.HasMember("name"))
				{
					LOGE("Expected value and name members, but not found. Stale stats file?\n");
					return;
				}

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

static void populate_blob_hash_set(std::unordered_set<Hash> &hashes, ResourceTag tag, DatabaseInterface &iface)
{
	std::vector<Hash> remove_hashes;
	size_t count;
	if (!iface.get_hash_list_for_resource_tag(tag, &count, nullptr))
		return;
	if (count == 0)
		return;
	remove_hashes.resize(count);
	if (!iface.get_hash_list_for_resource_tag(tag, &count, remove_hashes.data()))
		return;

	hashes.reserve(count);
	for (auto h : remove_hashes)
		hashes.insert(h);
}

static int run_normal_process(ThreadedReplayer &replayer, const vector<const char *> &databases,
                              const char *whitelist, uint32_t whitelist_mask,
                              intptr_t metadata_handle)
{
	auto start_time = chrono::steady_clock::now();
	auto start_create_archive = chrono::steady_clock::now();
	auto resolver = create_database(databases);

	if (whitelist)
	{
		resolver->set_whitelist_tag_mask(whitelist_mask);
		if (!resolver->load_whitelist_database(whitelist))
		{
			LOGE("Failed to load whitelist database: %s.\n", whitelist);
			return EXIT_FAILURE;
		}

		if (resolver->has_sub_databases())
			for (unsigned index : replayer.opts.implicit_whitelist_database_indices)
				resolver->promote_sub_database_to_whitelist(index);
	}

	if (DatabaseInterface::metadata_handle_is_valid(metadata_handle))
	{
		if (!resolver->import_metadata_from_os_handle(metadata_handle))
		{
			LOGE("Failed to import metadata.\n");
			return EXIT_FAILURE;
		}
	}

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

	if (!replayer.init_implicit_whitelist())
	{
		LOGE("Failed to initialize implicit whitelist.\n");
		return EXIT_FAILURE;
	}

	vector<Hash> resource_hashes;
	vector<uint8_t> state_json;

	static const ResourceTag initial_playback_order[] = {
		RESOURCE_APPLICATION_INFO, // This will create the device, etc.
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Trivial, run in main thread. Dependent immutable samplers are pulled in on-demand.
		RESOURCE_PIPELINE_LAYOUT, // Trivial, run in main thread
		RESOURCE_RENDER_PASS, // Trivial, run in main thread
	};

	static const ResourceTag threaded_playback_order[] = {
		RESOURCE_GRAPHICS_PIPELINE,
		RESOURCE_COMPUTE_PIPELINE,
		RESOURCE_RAYTRACING_PIPELINE,
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
		"Info Links",
		"Raytracing Pipeline",
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

		auto &per_thread_data = replayer.get_per_thread_data();
		per_thread_data.expected_tag = tag;

		for (auto &hash : resource_hashes)
		{
			size_t state_json_size = 0;
			if (resolver->read_entry(tag, hash, &state_json_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
			{
				tag_total_size_compressed += state_json_size;
			}			

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
				LOGW("Did not replay blob (tag: %s, hash: %016" PRIx64 "). See previous logs for context.\n", tag_names[tag], hash);
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

	heartbeat();

	// Now we've laid the initial ground work, kick off worker threads.
	replayer.start_worker_threads();

	vector<Hash> graphics_hashes;
	vector<Hash> compute_hashes;
	vector<Hash> raytracing_hashes;
	unsigned graphics_start_index = 0;
	unsigned compute_start_index = 0;
	unsigned raytracing_start_index = 0;

	if (replayer.opts.pipeline_hash != 0)
	{
		for (auto &tag : threaded_playback_order)
		{
			size_t state_json_size = 0;
			if (resolver->read_entry(tag, replayer.opts.pipeline_hash, &state_json_size, nullptr, 0))
			{
				if (tag == RESOURCE_GRAPHICS_PIPELINE)
					graphics_hashes.push_back(replayer.opts.pipeline_hash);
				else if (tag == RESOURCE_COMPUTE_PIPELINE)
					compute_hashes.push_back(replayer.opts.pipeline_hash);
				else if (tag == RESOURCE_RAYTRACING_PIPELINE)
					raytracing_hashes.push_back(replayer.opts.pipeline_hash);
			}
		}

		if (graphics_hashes.empty() && compute_hashes.empty() && raytracing_hashes.empty())
		{
			LOGE("Specified pipeline hash %016" PRIx64 " not found in database.\n",
			     replayer.opts.pipeline_hash);
			return EXIT_FAILURE;
		}
	}
	else
	{
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
			else if (tag == RESOURCE_RAYTRACING_PIPELINE)
			{
				hashes = &raytracing_hashes;

				end_index = min(end_index, replayer.opts.end_raytracing_index);
				start_index = max(start_index, replayer.opts.start_raytracing_index);
				start_index = min(end_index, start_index);
				raytracing_start_index = start_index;
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

			if (replayer.replayer_cache_db)
				populate_blob_hash_set(replayer.cached_blobs[tag], tag, *replayer.replayer_cache_db);

			for (auto &hash : *hashes)
			{
				size_t state_json_size = 0;
				if (resolver->read_entry(tag, hash, &state_json_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
				{
					tag_total_size_compressed += state_json_size;
				}

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
	}

	// Done parsing static objects.
	state_replayer.get_allocator().reset();

	vector<EnqueuedWork> graphics_workload;
	vector<EnqueuedWork> compute_workload;
	vector<EnqueuedWork> raytracing_workload;
	replayer.enqueue_deferred_pipelines(replayer.deferred_graphics, replayer.graphics_pipelines, replayer.graphics_parents,
	                                    graphics_workload,
	                                    graphics_hashes, graphics_start_index);
	replayer.enqueue_deferred_pipelines(replayer.deferred_compute, replayer.compute_pipelines, replayer.compute_parents,
	                                    compute_workload, compute_hashes,
	                                    compute_start_index);
	replayer.enqueue_deferred_pipelines(replayer.deferred_raytracing, replayer.raytracing_pipelines, replayer.raytracing_parents,
	                                    raytracing_workload, raytracing_hashes,
	                                    raytracing_start_index);

	sort(begin(graphics_workload), end(graphics_workload), [](const EnqueuedWork &a, const EnqueuedWork &b) {
		return a.order_index < b.order_index;
	});
	sort(begin(compute_workload), end(compute_workload), [](const EnqueuedWork &a, const EnqueuedWork &b) {
		return a.order_index < b.order_index;
	});
	sort(begin(raytracing_workload), end(raytracing_workload), [](const EnqueuedWork &a, const EnqueuedWork &b) {
		return a.order_index < b.order_index;
	});

	const auto run_work = [&replayer](const std::vector<EnqueuedWork> &workload) {
		for (auto &work : workload)
		{
			work.func();
			heartbeat();
		}

		// Need to synchronize worker threads between pipeline types to avoid a race condition
		// where memory iteration 1 for a GPL link is running, and then compute starts running
		// with only memory iteration 0 (< 1024 pipelines).
		// Then we get the pattern of:
		// - Link GPL pipeline iteration 1
		// - Sync memory context 0
		// - LRU maintenance memory context 0
		// - Free pipelines (race condition!)
		// To avoid shenanigans, synchronize everything between types.
		replayer.sync_worker_threads();
	};

	run_work(graphics_workload);
	run_work(compute_workload);
	run_work(raytracing_workload);

	replayer.tear_down_threads();

	LOGI("Total binary size for %s: %" PRIu64 " (%" PRIu64 " compressed)\n", tag_names[RESOURCE_SHADER_MODULE],
	     uint64_t(replayer.shader_module_total_size.load()),
	     uint64_t(replayer.shader_module_total_compressed_size.load()));

	replayer.compute_pipelines_cleared += replayer.compute_pipelines.size();
	replayer.graphics_pipelines_cleared += replayer.graphics_pipelines.size();
	replayer.raytracing_pipelines_cleared += replayer.raytracing_pipelines.size();

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.get_current_object_count() +
		replayer.render_passes.size() +
		replayer.compute_pipelines_cleared +
		replayer.graphics_pipelines_cleared +
		replayer.raytracing_pipelines_cleared;

	long elapsed_ms_prepare = chrono::duration_cast<chrono::milliseconds>(end_prepare - start_prepare).count();
	long elapsed_ms_read_archive = chrono::duration_cast<chrono::milliseconds>(end_create_archive - start_create_archive).count();
	long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Opening archive took %ld ms:\n", elapsed_ms_read_archive);
	LOGI("Parsing archive took %ld ms:\n", elapsed_ms_prepare);

	if (!replayer.opts.on_disk_pipeline_cache_path.empty() && replayer.device->pipeline_feedback_enabled())
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

	LOGI("Playing back %u raytracing pipelines took %.3f s (accumulated time)\n",
	     replayer.raytracing_pipeline_count.load(),
	     replayer.raytracing_pipeline_ns.load() * 1e-9);

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
	LOGI("  compute pipelines:     %7lu\n", (unsigned long)replayer.compute_pipelines_cleared);
	LOGI("  graphics pipelines:    %7lu\n", (unsigned long)replayer.graphics_pipelines_cleared);
	LOGI("  raytracing pipelines:  %7lu\n", (unsigned long)replayer.raytracing_pipelines_cleared);

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
	const char *whitelist_path = nullptr;
	uint32_t whitelist_mask = ExternalReplayer::WHITELIST_MASK_ALL_BIT;

	VulkanDevice::Options opts;
	ThreadedReplayer::Options replayer_opts;

#ifndef NO_ROBUST_REPLAYER
	bool master_process = false;
	bool slave_process = false;
	bool quiet_slave = false;
	bool progress = false;

#ifdef _WIN32
	const char *shm_name = nullptr;
	const char *shm_mutex_name = nullptr;
	const char *metadata_name = nullptr;
#else
	int shmem_fd = -1;
	int control_fd = -1;
#endif
#endif

	bool log_memory = false;

	// If a wrapper is specified, pass execution entirely to the wrapper.
	const char *wrapper_path = getenv(FOSSILIZE_REPLAY_WRAPPER_ENV);
	if (wrapper_path && *wrapper_path)
	{
		dispatch_to_replay_wrapper(wrapper_path, argv);
		// If execution fails, just continue on normally.
	}

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { databases.push_back(arg); };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--spirv-val", [&](CLIParser &) { replayer_opts.spirv_validate = true; });
	cbs.add("--on-disk-pipeline-cache", [&](CLIParser &parser) { replayer_opts.on_disk_pipeline_cache_path = parser.next_string(); });
	cbs.add("--on-disk-validation-cache", [&](CLIParser &parser) {
		replayer_opts.on_disk_validation_cache_path = parser.next_string();
		opts.enable_validation = true;
	});
	cbs.add("--on-disk-validation-blacklist", [&](CLIParser &parser) {
		replayer_opts.on_disk_validation_blacklist_path = parser.next_string();
	});
	cbs.add("--on-disk-validation-whitelist", [&](CLIParser &parser) {
		replayer_opts.on_disk_validation_whitelist_path = parser.next_string();
	});
	cbs.add("--on-disk-replay-whitelist", [&](CLIParser &parser) {
		whitelist_path = parser.next_string();
	});
	cbs.add("--on-disk-replay-whitelist-mask", [&](CLIParser &parser) {
		const char *tag = parser.next_string();
		if (strcmp(tag, "module") == 0)
			whitelist_mask = 1u << RESOURCE_SHADER_MODULE;
		else if (strcmp(tag, "pipeline") == 0)
			whitelist_mask = (1u << RESOURCE_GRAPHICS_PIPELINE) | (1u << RESOURCE_COMPUTE_PIPELINE);
		else
		{
			whitelist_mask = strtoull(tag, nullptr, 16);
			if (whitelist_mask == 0)
			{
				LOGE("Invalid --on-disk-replay-whitelist-mask: %s\n", tag);
				print_help();
				exit(EXIT_FAILURE);
			}
		}
	});
	cbs.add("--num-threads", [&](CLIParser &parser) { replayer_opts.num_threads = parser.next_uint(); });
	cbs.add("--loop", [&](CLIParser &parser) { replayer_opts.loop_count = parser.next_uint(); });
	cbs.add("--pipeline-hash", [&](CLIParser &parser) {
		const char *hash_str = parser.next_string();
		char *end;
		replayer_opts.pipeline_hash = strtoull(hash_str, &end, 16);
		if (*end != '\0')
		{
			LOGE("Not a valid pipeline hash: \"%s\"", hash_str);
			exit(EXIT_FAILURE);
		}
	});
	cbs.add("--graphics-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_graphics_index = parser.next_uint();
		replayer_opts.end_graphics_index = parser.next_uint();
	});
	cbs.add("--compute-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_compute_index = parser.next_uint();
		replayer_opts.end_compute_index = parser.next_uint();
	});
	cbs.add("--raytracing-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_raytracing_index = parser.next_uint();
		replayer_opts.end_raytracing_index = parser.next_uint();
	});
	cbs.add("--enable-pipeline-stats", [&](CLIParser &parser) { replayer_opts.pipeline_stats_path = parser.next_string(); });
	cbs.add("--on-disk-module-identifier", [&](CLIParser &parser) { replayer_opts.on_disk_module_identifier_path = parser.next_string(); });

#ifndef NO_ROBUST_REPLAYER
	cbs.add("--quiet-slave", [&](CLIParser &) { quiet_slave = true; });
	cbs.add("--master-process", [&](CLIParser &) { master_process = true; });
	cbs.add("--slave-process", [&](CLIParser &) { slave_process = true; });
	cbs.add("--progress", [&](CLIParser &) { progress = true; });

#ifdef _WIN32
	cbs.add("--shm-name", [&](CLIParser &parser) { shm_name = parser.next_string(); });
	cbs.add("--shm-mutex-name", [&](CLIParser &parser) { shm_mutex_name = parser.next_string(); });
	cbs.add("--metadata-name", [&](CLIParser &parser) { metadata_name = parser.next_string(); });
#else
	cbs.add("--shmem-fd", [&](CLIParser &parser) { shmem_fd = parser.next_uint(); });
	cbs.add("--control-fd", [&](CLIParser &parser) { control_fd = parser.next_uint(); });
#endif
#endif

	cbs.add("--shader-cache-size", [&](CLIParser &parser) { replayer_opts.shader_cache_size_mb = parser.next_uint(); });
	cbs.add("--ignore-derived-pipelines", [&](CLIParser &) { /* Obsolete option, keep around for compatibility. */ });
	cbs.add("--log-memory", [&](CLIParser &) { log_memory = true; });
	cbs.add("--null-device", [&](CLIParser &) { opts.null_device = true; });
	cbs.add("--timeout-seconds", [&](CLIParser &parser) { replayer_opts.timeout_seconds = parser.next_uint(); });
	cbs.add("--implicit-whitelist", [&](CLIParser &parser) {
		replayer_opts.implicit_whitelist_database_indices.push_back(parser.next_uint());
	});
	cbs.add("--replayer-cache", [&](CLIParser &parser) {
		replayer_opts.replayer_cache_path = parser.next_string();
	});
#ifndef _WIN32
	cbs.add("--disable-signal-handler", [&](CLIParser &) { replayer_opts.disable_signal_handler = true; });
	cbs.add("--disable-rate-limiter", [&](CLIParser &) { replayer_opts.disable_rate_limiter = true; });
#endif

	cbs.error_handler = [] { print_help(); };

	CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

#ifndef _WIN32
	// Can be handy to sideband this information in some scenarios.
	const char *rate_limit = getenv(FOSSILIZE_DISABLE_RATE_LIMITER_ENV);
	if (rate_limit && *rate_limit)
		replayer_opts.disable_rate_limiter = true;
#endif

	if (databases.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	if (replayer_opts.pipeline_hash != 0)
	{
		if (replayer_opts.start_graphics_index != 0u ||
		    replayer_opts.end_graphics_index != ~0u ||
		    replayer_opts.start_compute_index != 0u ||
		    replayer_opts.end_compute_index != ~0u)
		{
			LOGE("--pipeline-hash cannot be used together with pipeline ranges.\n");
			print_help();
			return EXIT_FAILURE;
		}

		// We don't need threading for a single pipeline
		replayer_opts.num_threads = 1;
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
#endif

	if (!replayer_opts.pipeline_stats_path.empty())
		replayer_opts.pipeline_stats = true;

#ifndef FOSSILIZE_REPLAYER_SPIRV_VAL
	if (replayer_opts.spirv_validate)
	{
		LOGE("--spirv-val is used, but SPIRV-Tools support was not enabled in fossilize-replay.\n");
		return EXIT_FAILURE;
	}
#endif

	int ret;
#ifndef NO_ROBUST_REPLAYER
	if (progress)
	{
		ret = run_progress_process(opts, replayer_opts, databases, whitelist_path, whitelist_mask, log_memory);
	}
	else if (master_process)
	{
#ifdef _WIN32
		ret = run_master_process(opts, replayer_opts,
		                         databases, whitelist_path, whitelist_mask,
		                         quiet_slave, shm_name, shm_mutex_name);
#else
		ret = run_master_process(opts, replayer_opts,
		                         databases, whitelist_path, whitelist_mask,
		                         quiet_slave, shmem_fd, control_fd);
#endif
	}
	else if (slave_process)
	{
#ifdef _WIN32
		ret = run_slave_process(opts, replayer_opts, databases, shm_name, shm_mutex_name, metadata_name);
#else
		ret = run_slave_process(opts, replayer_opts, databases);
#endif
	}
	else
#endif
	{
		ThreadedReplayer replayer(opts, replayer_opts);
#ifndef NO_ROBUST_REPLAYER
#ifndef _WIN32
		if (!replayer_opts.disable_signal_handler)
#endif
		{
			install_trivial_crash_handlers(replayer);
		}
#endif
		ret = run_normal_process(replayer, databases,
		                         whitelist_path, whitelist_mask,
		                         DatabaseInterface::invalid_metadata_handle());
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
