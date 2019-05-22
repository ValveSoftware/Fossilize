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
#include "path.hpp"
#include "fossilize_db.hpp"
#include "fossilize_external_replayer.hpp"
#include "fossilize_external_replayer_control_block.hpp"
#include "fossilize_exception.hpp"

#include <cinttypes>
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
BidirectionalItr unstable_remove_if(BidirectionalItr first, BidirectionalItr last, UnaryPredicate &&p)
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

namespace Global
{
static thread_local unsigned worker_thread_index;
}

struct ThreadedReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;
		bool spirv_validate = false;
		string on_disk_pipeline_cache_path;

		// VALVE: Add multi-threaded pipeline creation
		unsigned num_threads = thread::hardware_concurrency();

		// VALVE: --loop option for testing performance
		unsigned loop_count = 1;

		// Carve out a range of which pipelines to replay.
		// Used for multi-process replays where each process gets its own slice to churn through.
		unsigned start_graphics_index = 0;
		unsigned end_graphics_index = ~0u;
		unsigned start_compute_index = 0;
		unsigned end_compute_index = ~0u;

		SharedControlBlock *control_block = nullptr;

		void (*on_thread_callback)(void *userdata) = nullptr;
		void *on_thread_callback_userdata = nullptr;
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
		unsigned current_parse_index = ~0u;
		unsigned current_graphics_index = ~0u;
		unsigned current_compute_index = ~0u;
		bool force_outside_range = false;

		// Make sure each per-thread data lands in its own cache line to avoid false sharing.
		uint8_t _padding[64 - 3 * sizeof(unsigned) - sizeof(bool)];
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
		thread_total_ns.store(0);
		total_idle_ns.store(0);

		shader_module_total_compressed_size.store(0);
		shader_module_total_size.store(0);
	}

	PerThreadData &get_per_thread_data()
	{
		return per_thread_data[Global::worker_thread_index];
	}

	void start_worker_threads()
	{
		per_thread_data.resize(num_worker_threads + 1);
		for (auto &d : per_thread_data)
		{
			d.current_graphics_index = opts.start_graphics_index;
			d.current_compute_index = opts.start_compute_index;
		}

		// Create a thread pool with the # of specified worker threads (defaults to thread::hardware_concurrency()).
		for (unsigned i = 0; i < num_worker_threads; i++)
			thread_pool.push_back(std::thread(&ThreadedReplayer::worker_thread, this, i + 1));
	}

	void sync_worker_threads()
	{
		unique_lock<mutex> lock(pipeline_work_queue_mutex);
		work_done_condition.wait(lock, [&]() -> bool {
			return queued_count == completed_count;
		});
	}

	bool run_parse_work_item(StateReplayer &replayer, vector<uint8_t> &buffer, const PipelineWorkItem &work_item)
	{
		size_t json_size = 0;
		if (!global_database->read_entry(work_item.tag, work_item.hash, &json_size, nullptr, PAYLOAD_READ_CONCURRENT_BIT))
			return false;

		buffer.resize(json_size);

		if (!global_database->read_entry(work_item.tag, work_item.hash, &json_size, buffer.data(), PAYLOAD_READ_CONCURRENT_BIT))
			return false;

		per_thread_data[Global::worker_thread_index].current_parse_index = work_item.index;
		per_thread_data[Global::worker_thread_index].force_outside_range = work_item.force_outside_range;

		if (!work_item.force_outside_range)
		{
			if (work_item.tag == RESOURCE_GRAPHICS_PIPELINE)
			{
				lock_guard<mutex> holder(hash_lock);
				graphics_hashes_in_range.insert(work_item.hash);
			}
			else if (work_item.tag == RESOURCE_COMPUTE_PIPELINE)
			{
				lock_guard<mutex> holder(hash_lock);
				compute_hashes_in_range.insert(work_item.hash);
			}
		}

		try
		{
			replayer.parse(*this, global_database, buffer.data(), buffer.size());
			if (work_item.tag == RESOURCE_SHADER_MODULE)
			{
				// No reason to retain memory in this allocator anymore.
				replayer.get_allocator().reset();

				// Feed shader module statistics.
				shader_module_total_size.fetch_add(json_size, std::memory_order_relaxed);
				if (global_database->read_entry(work_item.tag, work_item.hash, &json_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
					shader_module_total_compressed_size.fetch_add(json_size, std::memory_order_relaxed);
			}
		}
		catch (const std::exception &e)
		{
			LOGE("StateReplayer threw exception parsing: %s\n", e.what());
			return false;
		}

		return true;
	}

	void run_creation_work_item(const PipelineWorkItem &work_item)
	{
		switch (work_item.tag)
		{
		case RESOURCE_GRAPHICS_PIPELINE:
		{
			get_per_thread_data().current_graphics_index = work_item.index + 1;

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
				num_failed_module_hashes = work_item.create_info.graphics_create_info->stageCount;
				for (unsigned i = 0; i < work_item.create_info.graphics_create_info->stageCount; i++)
				{
					VkShaderModule module = work_item.create_info.graphics_create_info->pStages[i].module;
					failed_module_hashes[i] = shader_module_to_hash[module];
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

				if (vkCreateGraphicsPipelines(device->get_device(), pipeline_cache, 1, work_item.create_info.graphics_create_info,
				                              nullptr, work_item.output.pipeline) == VK_SUCCESS)
				{
					auto end_time = chrono::steady_clock::now();
					auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

					graphics_pipeline_ns.fetch_add(duration_ns, std::memory_order_relaxed);
					graphics_pipeline_count.fetch_add(1, std::memory_order_relaxed);

					*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;

					if (opts.control_block && i == 0)
						opts.control_block->successful_graphics.fetch_add(1, std::memory_order_relaxed);
				}
				else
				{
					LOGE("Failed to create graphics pipeline for hash 0x%llx.\n",
					     static_cast<unsigned long long>(work_item.hash));
				}
			}
			break;
		}

		case RESOURCE_COMPUTE_PIPELINE:
		{
			get_per_thread_data().current_compute_index = work_item.index + 1;

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
				num_failed_module_hashes = 1;
				VkShaderModule module = work_item.create_info.compute_create_info->stage.module;
				failed_module_hashes[0] = shader_module_to_hash[module];
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

				if (vkCreateComputePipelines(device->get_device(), pipeline_cache, 1,
				                             work_item.create_info.compute_create_info,
				                             nullptr, work_item.output.pipeline) == VK_SUCCESS)
				{
					auto end_time = chrono::steady_clock::now();
					auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

					compute_pipeline_ns.fetch_add(duration_ns, std::memory_order_relaxed);
					compute_pipeline_count.fetch_add(1, std::memory_order_relaxed);

					*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;

					if (opts.control_block && i == 0)
						opts.control_block->successful_compute.fetch_add(1, std::memory_order_relaxed);
				}
				else
				{
					LOGE("Failed to create compute pipeline for hash 0x%llx.\n",
					     static_cast<unsigned long long>(work_item.hash));
				}
			}
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
		StateReplayer per_thread_replayer;
		per_thread_replayer.set_resolve_derivative_pipeline_handles(false);
		per_thread_replayer.set_resolve_shader_module_handles(false);
		per_thread_replayer.copy_handle_references(*global_replayer);

		// A separate replayer for shader modules so we can reclaim memory right after parsing.
		// After replaying a VkShaderModule, we never need to refer to any de-serialized shader module create info again.
		StateReplayer per_thread_replayer_shader_modules;

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
			{
				run_parse_work_item(work_item.tag == RESOURCE_SHADER_MODULE ? per_thread_replayer_shader_modules
				                                                            : per_thread_replayer,
				                    json_buffer, work_item);
			}
			else
				run_creation_work_item(work_item);

			idle_start_time = chrono::steady_clock::now();
			{
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				completed_count++;
				if (completed_count == queued_count) // Makes sense to signal main thread now.
					work_done_condition.notify_one();
			}

			idle_end_time = chrono::steady_clock::now();
			duration_ns = chrono::duration_cast<chrono::nanoseconds>(idle_end_time - idle_start_time).count();
			idle_ns += duration_ns;
		}

		total_idle_ns.fetch_add(idle_ns, std::memory_order_relaxed);
		auto thread_end_time = chrono::steady_clock::now();
		thread_total_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(thread_end_time - thread_start_time).count(),
		                          std::memory_order_relaxed);
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

		for (auto &sampler : samplers)
			if (sampler.second)
				vkDestroySampler(device->get_device(), sampler.second, nullptr);
		for (auto &layout : layouts)
			if (layout.second)
				vkDestroyDescriptorSetLayout(device->get_device(), layout.second, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout.second)
				vkDestroyPipelineLayout(device->get_device(), pipeline_layout.second, nullptr);
		for (auto &shader_module : shader_modules)
			if (shader_module.second)
				vkDestroyShaderModule(device->get_device(), shader_module.second, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass.second)
				vkDestroyRenderPass(device->get_device(), render_pass.second, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
	}

	bool validate_pipeline_cache_header(const vector<uint8_t> &blob)
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

		if (!device_was_init)
		{
			// Now we can init the device with correct app info.
			device_was_init = true;
			device.reset(new VulkanDevice);
			device_opts.application_info = app;
			device_opts.features = features;
			device_opts.need_disasm = false;
			auto start_device = chrono::steady_clock::now();
			if (!device->init_device(device_opts))
			{
				LOGE("Failed to create Vulkan device, bailing ...\n");
				exit(EXIT_FAILURE);
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
		// Playback in-order.
		if (vkCreateSampler(device->get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %0" PRIX64 " Failed!\n", index);
			return false;
		}
		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		// Playback in-order.
		if (vkCreateDescriptorSetLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating descriptor set layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
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
		if (masked_shader_modules.count(hash))
		{
			*module = VK_NULL_HANDLE;
			lock_guard<mutex> lock(internal_enqueue_mutex);
			shader_modules[hash] = VK_NULL_HANDLE;
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
				shader_modules[hash] = VK_NULL_HANDLE;
				shader_module_count.fetch_add(1, std::memory_order_relaxed);

				if (opts.control_block)
					opts.control_block->module_validation_failures.fetch_add(1, std::memory_order_relaxed);

				return true;
			}
		}
#endif

		VkShaderModule *hash_map_entry;
		{
			lock_guard<mutex> lock(internal_enqueue_mutex);
			hash_map_entry = &shader_modules[hash];
		}

		for (unsigned i = 0; i < loop_count; i++)
		{
			// Avoid leak.
			if (*hash_map_entry != VK_NULL_HANDLE)
				vkDestroyShaderModule(device->get_device(), *hash_map_entry, nullptr);
			*hash_map_entry = VK_NULL_HANDLE;

			auto start_time = chrono::steady_clock::now();
			if (vkCreateShaderModule(device->get_device(), create_info, nullptr, module) == VK_SUCCESS)
			{
				auto end_time = chrono::steady_clock::now();
				auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
				shader_module_ns.fetch_add(duration_ns, std::memory_order_relaxed);
				shader_module_count.fetch_add(1, std::memory_order_relaxed);
				*hash_map_entry = *module;

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
				LOGE("Failed to create shader module for hash 0x%llx.\n",
				     static_cast<unsigned long long>(hash));
			}
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

		auto &per_thread = get_per_thread_data();
		unsigned index = per_thread.current_parse_index;
		bool force_outside_range = per_thread.force_outside_range;

		if (!force_outside_range && index >= opts.start_compute_index && index < opts.end_compute_index)
		{
			deferred_compute[index - opts.start_compute_index] = {
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

		auto &per_thread = get_per_thread_data();
		unsigned index = per_thread.current_parse_index;
		bool force_outside_range = per_thread.force_outside_range;

		if (!force_outside_range && index >= opts.start_graphics_index && index < opts.end_graphics_index)
		{
			deferred_graphics[index - opts.start_graphics_index] = {
				const_cast<VkGraphicsPipelineCreateInfo *>(create_info), hash, pipeline, index,
			};
		}
		else
		{
			lock_guard<mutex> holder(hash_lock);
			graphics_parents[hash] = {
				const_cast<VkGraphicsPipelineCreateInfo *>(create_info), hash, pipeline, index,
			};
		};

		*pipeline = (VkPipeline)hash;
		if (opts.control_block)
			opts.control_block->parsed_graphics.fetch_add(1, std::memory_order_relaxed);

		return true;
	}

	template <typename DerivedInfo>
	bool resolve_derived_pipelines(vector<DerivedInfo> &derived,
	                               const unordered_set<Hash> &in_range_set,
	                               unordered_map<Hash, DerivedInfo> &parents,
	                               unordered_map<Hash, VkPipeline> &pipelines)
	{
		unordered_set<Hash> outside_range_hashes;

		// Figure out which of the parent pipelines we need.
		for (auto &d : derived)
		{
			Hash h = (Hash)d.info->basePipelineHandle;
			bool is_inside_range = in_range_set.count(h) != 0;

			if (!is_inside_range)
			{
				// Make sure this auxillary entry has been parsed.
				if (!outside_range_hashes.count(h))
				{
					PipelineWorkItem work_item;
					work_item.index = d.index;
					work_item.hash = h;
					work_item.parse_only = true;
					work_item.force_outside_range = true;
					work_item.tag = DerivedInfo::get_tag();
					outside_range_hashes.insert(h);
					enqueue_work_item(work_item);
				}
			}
		}

		sync_worker_threads();

		// Queue up all shader modules if somehow the shader modules used by parent pipelines differ from children ...
		for (auto &parent : parents)
		{
			// If we enqueued something here, we have to wait for shader handles to resolve.
			enqueue_shader_modules(parent.second.info);
		}

		sync_worker_threads();

		if (opts.control_block)
		{
			if (DerivedInfo::get_tag() == RESOURCE_GRAPHICS_PIPELINE)
				opts.control_block->total_graphics.fetch_add(parents.size(), std::memory_order_relaxed);
			else if (DerivedInfo::get_tag() == RESOURCE_COMPUTE_PIPELINE)
				opts.control_block->total_compute.fetch_add(parents.size(), std::memory_order_relaxed);
		}

		for (auto &parent : parents)
		{
			resolve_shader_modules(parent.second.info);
			assert((parent.second.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) == 0);
			enqueue_pipeline(parent.second.hash, parent.second.info, parent.second.pipeline, parent.second.index);
		}

		parents.clear();

		// Go over all pipelines. If there are no further dependencies to resolve, we can go ahead and queue them up.
		// If an entry exists in graphics_pipelines, we have queued up that hash earlier, but it might not be done compiling yet.
		auto itr = unstable_remove_if(begin(derived), end(derived), [&](const DerivedInfo &info) -> bool {
			Hash hash = (Hash)info.info->basePipelineHandle;
			return pipelines.count(hash) != 0;
		});

		if (itr == end(derived)) // We cannot progress ... This shouldn't happen.
		{
			LOGE("Nothing more to do in resolve_derived_pipelines, but there are still pipelines left to replay.\n");
			return false;
		}

		// Wait for parent pipelines to complete.
		sync_worker_threads();

		// Now we can enqueue with correct pipeline handles.
		for (auto i = itr; i != end(derived); ++i)
		{
			resolve_shader_modules(i->info);
			i->info->basePipelineHandle = pipelines[(Hash)i->info->basePipelineHandle];
			if (!enqueue_pipeline(i->hash, i->info, i->pipeline, i->index))
				return false;
		}

		// The pipelines are now in-flight, try resolving new dependencies in next iteration.
		derived.erase(itr, end(derived));
		return true;
	}

	bool resolve_derived_compute_pipelines()
	{
		return resolve_derived_pipelines(deferred_compute, graphics_hashes_in_range, compute_parents, compute_pipelines);
	}

	bool resolve_derived_graphics_pipelines()
	{
		return resolve_derived_pipelines(deferred_graphics, compute_hashes_in_range, graphics_parents, graphics_pipelines);
	}

	bool enqueue_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline,
	                      unsigned index)
	{
		PipelineWorkItem work_item = {};
		work_item.hash = hash;
		work_item.tag = RESOURCE_COMPUTE_PIPELINE;
		work_item.output.pipeline = pipeline;
		work_item.index = index;

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
	                      unsigned index)
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
			enqueue_work_item(work_item);
			enqueued_shader_modules.insert(shader_module_hash);
			return true;
		}
		else
			return false;
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
			const_cast<VkPipelineShaderStageCreateInfo *>(info->pStages)[i].module =
					shader_modules[(Hash) info->pStages[i].module];
		}
	}

	void resolve_shader_modules(VkComputePipelineCreateInfo *info)
	{
		const_cast<VkComputePipelineCreateInfo*>(info)->stage.module =
				shader_modules[(Hash) info->stage.module];
	}

	template <typename DerivedInfo>
	void sort_deferred_derived_pipelines(vector<DerivedInfo> &derived, vector<DerivedInfo> &deferred)
	{
		sort(begin(deferred), end(deferred), [&](const DerivedInfo &a, const DerivedInfo &b) -> bool {
			bool a_derived = (a.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
			bool b_derived = (b.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
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
			if ((def.info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) == 0)
				end_index_non_derived = index;
		}

		derived.clear();
		assert(deferred.size() >= end_index_non_derived);
		derived.reserve(deferred.size() - end_index_non_derived);
		copy(begin(deferred) + end_index_non_derived, end(deferred), back_inserter(derived));
		deferred.erase(begin(deferred) + end_index_non_derived, end(deferred));
	}

	void flush_deferred_pipelines()
	{
		// Make sure all parsing of pipelines is complete.
		sync_worker_threads();

		// Make sure all referenced shader modules have been parsed and created.
		for (auto &item : deferred_graphics)
			enqueue_shader_modules(item.info);
		for (auto &item : deferred_compute)
			enqueue_shader_modules(item.info);

		// While decompressing, we can sort the pipeline infos in parallel.
		// Some pipelines might be derived here, so only resolve and queue up the pipelines we can after shader modules are done.
		// We want a stable index order, so indices for non-derived will be smaller than derived ones.
		vector<DeferredGraphicsInfo> derived_graphics;
		vector<DeferredComputeInfo> derived_compute;
		sort_deferred_derived_pipelines(derived_graphics, deferred_graphics);
		sort_deferred_derived_pipelines(derived_compute, deferred_compute);

		// Make sure all VkShaderModules have been queued and completed.
		// Remap VkShaderModule references from hashes to real handles and enqueue for work.
		sync_worker_threads();
		for (auto &item : deferred_graphics)
		{
			resolve_shader_modules(item.info);
			enqueue_pipeline(item.hash, item.info, item.pipeline, item.index);
		}

		for (auto &item : deferred_compute)
		{
			resolve_shader_modules(item.info);
			enqueue_pipeline(item.hash, item.info, item.pipeline, item.index);
		}

		// The derived pipelines are resolved later.
		deferred_graphics = move(derived_graphics);
		deferred_compute = move(derived_compute);
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
		device.reset();
	}

	Options opts;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> shader_modules;
	std::unordered_map<Hash, VkRenderPass> render_passes;
	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	std::unordered_set<Hash> masked_shader_modules;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_hash;
	std::unordered_set<VkShaderModule> enqueued_shader_modules;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

	// VALVE: multi-threaded work queue for replayer

	void enqueue_work_item(const PipelineWorkItem &item)
	{
		lock_guard<mutex> lock(pipeline_work_queue_mutex);
		pipeline_work_queue.push(item);
		work_available_condition.notify_one();
		queued_count++;
	}

	unsigned num_worker_threads = 0;
	unsigned loop_count = 0;
	unsigned queued_count = 0;
	unsigned completed_count = 0;
	std::vector<std::thread> thread_pool;
	std::vector<PerThreadData> per_thread_data;
	std::mutex pipeline_work_queue_mutex;
	std::mutex internal_enqueue_mutex;
	std::queue<PipelineWorkItem> pipeline_work_queue;
	std::condition_variable work_available_condition;
	std::condition_variable work_done_condition;

	std::mutex hash_lock;
	std::unordered_map<Hash, DeferredGraphicsInfo> graphics_parents;
	std::unordered_map<Hash, DeferredComputeInfo> compute_parents;
	std::vector<DeferredGraphicsInfo> deferred_graphics;
	std::vector<DeferredComputeInfo> deferred_compute;
	std::unordered_set<Hash> graphics_hashes_in_range;
	std::unordered_set<Hash> compute_hashes_in_range;

	// Feed statistics from the worker threads.
	std::atomic<std::uint64_t> graphics_pipeline_ns;
	std::atomic<std::uint64_t> compute_pipeline_ns;
	std::atomic<std::uint64_t> shader_module_ns;
	std::atomic<std::uint64_t> total_idle_ns;
	std::atomic<std::uint64_t> thread_total_ns;
	std::atomic<std::uint32_t> graphics_pipeline_count;
	std::atomic<std::uint32_t> compute_pipeline_count;
	std::atomic<std::uint32_t> shader_module_count;

	std::atomic<std::uint64_t> shader_module_total_size;
	std::atomic<std::uint64_t> shader_module_total_compressed_size;

	bool shutting_down = false;

	unique_ptr<VulkanDevice> device;
	bool device_was_init = false;
	VulkanDevice::Options device_opts;

	// Crash recovery.
	Hash failed_module_hashes[6] = {};
	unsigned num_failed_module_hashes = 0;
	bool robustness = false;

	const StateReplayer *global_replayer = nullptr;
	DatabaseInterface *global_database = nullptr;
};

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
	     "\t[--pipeline-cache]\n"
	     "\t[--spirv-val]\n"
	     "\t[--num-threads <count>]\n"
	     "\t[--loop <count>]\n"
	     "\t[--on-disk-pipeline-cache <path>]\n"
	     "\t[--graphics-pipeline-range <start> <end>]\n"
	     "\t[--compute-pipeline-range <start> <end>]\n"
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

	for (auto &h : hashes)
		LOGI("Detected faulty SPIR-V module: %llx\n", static_cast<unsigned long long>(h));
}

static int run_progress_process(const VulkanDevice::Options &,
                                const ThreadedReplayer::Options &replayer_opts,
                                const string &db_path, int timeout)
{
	ExternalReplayer::Options opts = {};
	opts.on_disk_pipeline_cache = replayer_opts.on_disk_pipeline_cache_path.empty() ?
		nullptr : replayer_opts.on_disk_pipeline_cache_path.c_str();
	opts.pipeline_cache = replayer_opts.pipeline_cache;
	opts.num_threads = replayer_opts.num_threads;
	opts.quiet = true;
	opts.database = db_path.c_str();
	opts.external_replayer_path = nullptr;
	opts.inherit_process_group = true;
	opts.spirv_validate = replayer_opts.spirv_validate;

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

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		ExternalReplayer::Progress progress = {};
		auto result = replayer.poll_progress(progress);

		if (replayer.is_process_complete(nullptr))
		{
			if (result != ExternalReplayer::PollResult::ResultNotReady)
				log_progress(progress);
			log_faulty_modules(replayer);
			return replayer.wait();
		}

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
				return replayer.wait();
			}
			break;
		}
	}
}
#endif

static int run_normal_process(ThreadedReplayer &replayer, const string &db_path)
{
	auto start_time = chrono::steady_clock::now();
	auto start_create_archive = chrono::steady_clock::now();
	auto resolver = unique_ptr<DatabaseInterface>(create_database(db_path.c_str(), DatabaseMode::ReadOnly));
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

			try
			{
				state_replayer.parse(replayer, resolver.get(), state_json.data(), state_json.size());
			}
			catch (const exception &e)
			{
				LOGE("StateReplayer threw exception parsing (tag: %s, hash: %s): %s\n",
				     tag_names[tag], uint64_string(hash).c_str(), e.what());
			}
		}

		LOGI("Total binary size for %s: %llu (%llu compressed)\n", tag_names[tag],
		     static_cast<unsigned long long>(tag_total_size),
		     static_cast<unsigned long long>(tag_total_size_compressed));

		auto main_thread_end = std::chrono::steady_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(main_thread_end - main_thread_start).count();
		LOGI("Total time decoding %s in main thread: %.3f s\n", tag_names[tag], duration * 1e-9);
	}

	// Now we've laid the initial ground work, kick off worker threads.
	replayer.start_worker_threads();

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

		if (tag == RESOURCE_GRAPHICS_PIPELINE)
		{
			end_index = min(end_index, replayer.opts.end_graphics_index);
			start_index = max(start_index, replayer.opts.start_graphics_index);
			start_index = min(end_index, start_index);

			replayer.deferred_graphics.resize(end_index - start_index);

			if (replayer.opts.control_block)
				replayer.opts.control_block->total_graphics.fetch_add(end_index - start_index, std::memory_order_relaxed);
		}
		else if (tag == RESOURCE_COMPUTE_PIPELINE)
		{
			end_index = min(end_index, replayer.opts.end_compute_index);
			start_index = max(start_index, replayer.opts.start_compute_index);
			start_index = min(end_index, start_index);

			replayer.deferred_compute.resize(end_index - start_index);
			if (replayer.opts.control_block)
				replayer.opts.control_block->total_compute.fetch_add(end_index - start_index, std::memory_order_relaxed);
		}

		resource_hashes.resize(resource_hash_count);

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, resource_hashes.data()))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		move(begin(resource_hashes) + start_index, begin(resource_hashes) + end_index, begin(resource_hashes));
		resource_hashes.erase(begin(resource_hashes) + (end_index - start_index), end(resource_hashes));

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
			tag_total_size += state_json_size;

			// Defer parsing and decompression to the worker threads.
			ThreadedReplayer::PipelineWorkItem work_item;
			work_item.hash = hash;
			work_item.tag = tag;
			work_item.parse_only = true;
			work_item.index = start_index;
			replayer.enqueue_work_item(work_item);

			start_index++;
		}

		LOGI("Total binary size for %s: %llu (%llu compressed)\n", tag_names[tag],
		     static_cast<unsigned long long>(tag_total_size),
		     static_cast<unsigned long long>(tag_total_size_compressed));
	}

	replayer.flush_deferred_pipelines();

	// Resolve any derived pipelines.
	if (!replayer.deferred_graphics.empty())
		replayer.resolve_derived_graphics_pipelines();
	if (!replayer.deferred_compute.empty())
		replayer.resolve_derived_compute_pipelines();

	// VALVE: drain all outstanding pipeline compiles
	replayer.sync_worker_threads();
	replayer.tear_down_threads();

	LOGI("Total binary size for %s: %llu (%llu compressed)\n", tag_names[RESOURCE_SHADER_MODULE],
	     static_cast<unsigned long long>(replayer.shader_module_total_size.load()),
	     static_cast<unsigned long long>(replayer.shader_module_total_compressed_size.load()));

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.size() +
		replayer.render_passes.size() +
		replayer.compute_pipelines.size() +
		replayer.graphics_pipelines.size();

	long elapsed_ms_prepare = chrono::duration_cast<chrono::milliseconds>(end_prepare - start_prepare).count();
	long elapsed_ms_read_archive = chrono::duration_cast<chrono::milliseconds>(end_create_archive - start_create_archive).count();
	long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Opening archive took %ld ms:\n", elapsed_ms_read_archive);
	LOGI("Parsing archive took %ld ms:\n", elapsed_ms_prepare);

	LOGI("Playing back %u shader modules took %.3f s (accumulated time)\n",
	     replayer.shader_module_count.load(),
	     replayer.shader_module_ns.load() * 1e-9);

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

	LOGI("Replayed %lu objects in %ld ms:\n", total_size, elapsed_ms);
	LOGI("  samplers:              %7lu\n", (unsigned long)replayer.samplers.size());
	LOGI("  descriptor set layouts:%7lu\n", (unsigned long)replayer.layouts.size());
	LOGI("  pipeline layouts:      %7lu\n", (unsigned long)replayer.pipeline_layouts.size());
	LOGI("  shader modules:        %7lu\n", (unsigned long)replayer.shader_modules.size());
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
	string db_path;
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

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { db_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
	cbs.add("--spirv-val", [&](CLIParser &) { replayer_opts.spirv_validate = true; });
	cbs.add("--on-disk-pipeline-cache", [&](CLIParser &parser) { replayer_opts.on_disk_pipeline_cache_path = parser.next_string(); });
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

	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (db_path.empty())
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

#ifndef FOSSILIZE_REPLAYER_SPIRV_VAL
	if (replayer_opts.spirv_validate)
		LOGE("--spirv-val is used, but SPIRV-Tools support was not enabled in fossilize-replay. Will be ignored.\n");
#endif

	int ret;
#ifndef NO_ROBUST_REPLAYER
	if (progress)
	{
		ret = run_progress_process(opts, replayer_opts, db_path, timeout);
	}
	else if (master_process)
	{
#ifdef _WIN32
		ret = run_master_process(opts, replayer_opts, db_path, quiet_slave, shm_name, shm_mutex_name);
#else
		ret = run_master_process(opts, replayer_opts, db_path, quiet_slave, shmem_fd);
#endif
	}
	else if (slave_process)
	{
#ifdef _WIN32
		ret = run_slave_process(opts, replayer_opts, db_path, shm_name, shm_mutex_name);
#else
		ret = run_slave_process(opts, replayer_opts, db_path);
#endif
	}
	else
#endif
	{
		ThreadedReplayer replayer(opts, replayer_opts);
		ret = run_normal_process(replayer, db_path);
	}

	return ret;
}
