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

#pragma once

#include "fossilize_types.hpp"
#include <stddef.h>

namespace Fossilize
{
class ExternalReplayer
{
public:
	struct Environment
	{
		const char *key;
		const char *value;
	};

	enum WhiteListMaskBits
	{
		WHITELIST_MASK_MODULE_BIT = 1u << RESOURCE_SHADER_MODULE,
		WHITELIST_MASK_GRAPHICS_PIPELINE_BIT = 1u << RESOURCE_GRAPHICS_PIPELINE,
		WHITELIST_MASK_COMPUTE_PIPELINE_BIT = 1u << RESOURCE_COMPUTE_PIPELINE,
		WHITELIST_MASK_PIPELINE_BIT = WHITELIST_MASK_COMPUTE_PIPELINE_BIT | WHITELIST_MASK_GRAPHICS_PIPELINE_BIT,
		WHITELIST_MASK_ALL_BIT = WHITELIST_MASK_MODULE_BIT | WHITELIST_MASK_PIPELINE_BIT,
		WHITELIST_MASK_INT_MAX = 0x7fffffff
	};
	using WhiteListMaskFlags = unsigned;

	struct Options
	{
		// Path to the fossilize-replay executable.
		// May be null, in which case the calling process must be fossilize-replay itself.
		const char *external_replayer_path;

		// If num_external_replayer_arguments is not zero,
		// external_replayer_path is ignored.
		// external_replayer_arguments[0] is the path to execute
		// and additional arguments can be used to invoke wrapper scripts with more
		// complex sets of arguments.
		const char * const *external_replayer_arguments;
		unsigned num_external_replayer_arguments;

		// Paths to one or more Fossilize database to be replayed.
		// Multiple paths may be used here in which case the replayer will treat
		// the database as a union of all the databases in "databases".
		const char * const *databases;
		unsigned num_databases;

		// Represents indices into databases array.
		// All blobs in a selected database are marked as being implicitly whitelisted,
		// and extra validation steps are avoided.
		// Is only meaningful if used along on_disk_validation_whitelist.
		const unsigned *implicit_whitelist_indices;
		unsigned num_implicit_whitelist_indices;

		// Path to an on-disk pipeline cache. Maps to --on-disk-pipeline-cache.
		const char *on_disk_pipeline_cache;

		// Path to an on-disk validation cache. Maps to --on-disk-validation-cache.
		const char *on_disk_validation_cache;

		// Path to on-disk validation white/blacklists.
		const char *on_disk_validation_whitelist;
		const char *on_disk_validation_blacklist;

		// Path to on-disk module identifier cache.
		// The actual path is appended with ".$moduleIdentifierAlgorithmUUID.foz".
		const char *on_disk_module_identifier;

		// Path to store pipeline stats in.
		const char *pipeline_stats_path;

		// Path to a replayer cache.
		// path.$pipelineCacheUUID.*.foz will be written and any pipelines in
		// path.$pipelineCacheUUID.foz will be skipped.
		const char *replayer_cache_path;

		// Extra environment variables which will be added to the child process tree.
		// Will not modify environment of caller.
		const Environment *environment_variables;
		unsigned num_environment_variables;

		// Maps to --num-threads. If 0, no argument for --num-threads is passed.
		unsigned num_threads;

		// Maps to --device-index.
		unsigned device_index;

		// Carve out a range of which pipelines to replay if use_pipeline_range is set.
		// Used for multi-process replays where each process gets its own slice to churn through.
		unsigned start_graphics_index;
		unsigned end_graphics_index;
		unsigned start_compute_index;
		unsigned end_compute_index;
		unsigned start_raytracing_index;
		unsigned end_raytracing_index;
		bool use_pipeline_range;

		// Redirect stdout and stderr to /dev/null or NUL.
		bool quiet;

		// (Linux only) Inherits the process group used by caller. Lets all child processes for replayer
		// belong to caller. Useful for CLI tools which use this interface.
		// If this is used, ExternalReplayer::kill() won't work since it relies on process groups to work.
		// (Windows only) If true, a JobObject is created to make sure that if the calling process is killed, so are the Fossilize replayer processes.
		bool inherit_process_group;

		// Validates all SPIR-V with spirv-val before replaying.
		// Modules which fail to validate will not be used.
		bool spirv_validate;

		// Enable full validation layers.
		bool enable_validation;

		// Disable crash signal handler (for debugging and obtaining coredumps).
		bool disable_signal_handler;

		// Disable rate limiter (e.g. Linux PSI monitoring).
		// Intended for when running Fossilize dumps off-line on dedicated hardware.
		// Also disables any deliberate lowering of priorities for the process group.
		bool disable_rate_limiter;

		// Ignores derived pipelines, reduces memory consumption when replaying.
		// Only useful if the driver in question ignores use of derived pipelines when hashing pipelines internally.
		// OBSOLETE. This option is only kept for backwards compat.
		// All known drivers ignore derived pipelines and is no longer replayed as-is.
		bool ignore_derived_pipelines;

		// Creates a dummy device, useful for benchmarking time and/or memory consumption in isolation.
		bool null_device;

		// If non-zero, enables a timeout for pipeline compilation to have forward progress on drivers
		// which enter infinite loops during compilation.
		unsigned timeout_seconds;

		// If used, will only replay a blob if it exists in the whitelist.
		// The intented use of this is to use validation whitelists and then only replay
		// blobs which are known to have passed validation.
		const char *on_disk_replay_whitelist;
		// The whitelist mask controls which resource types are considered for replay.
		// If a resource tag is not set, it is assumed at all resources of that type is whitelisted.
		// This is useful if we only want to use the whitelist for e.g. shader modules.
		// Must be non-zero if on_disk_replay_whitelist is set.
		WhiteListMaskFlags on_disk_replay_whitelist_mask;
	};

	ExternalReplayer();
	~ExternalReplayer();
	void operator=(const ExternalReplayer &) = delete;
	ExternalReplayer(const ExternalReplayer &) = delete;

	// This may only be called once.
	bool start(const Options &options);

	// On Unix, this can be cast to a pid_t, on Windows, a HANDLE.
	// On Unix, the caller is responsible for reaping the child PID when it dies, unless
	// the blocking wait() is used, which translates to waitpid, and will therefore reap the child
	// process itself.
	uintptr_t get_process_handle() const;

	// If the process is not complete, waits in a blocking fashion for the process to complete and closes the process handle.
	// Returns the exit code for the process, or if a fatal signal killed the process, -SIGNAL is returned.
	// If the process was already waited for, returns the cached exit code.
	int wait();

	// Queries if the process is dead. If process is found to be dead, it also reaps the child.
	// If child was reaped in this function call, true is returned,
	// and the return status is returned in *return_status if argument is non-null.
	// If process is already reaped, *return_status returns the cached return status ala wait().
	bool is_process_complete(int *return_status);

	// Requests that process (and its children) are killed.
	// Can only be used when inherit_process_group is false.
	bool kill();

	// As the replayer is progressing, it might find SPIR-V modules which might have contributed to a crash.
	// This allows the caller to later investigate what these modules are doing.
	bool get_faulty_spirv_modules(size_t *num_hashes, Hash *hashes) const;

	// Report pipelines which actually crashed. The indices are useful for replaying an archive
	// with a given pipeline range.
	bool get_faulty_graphics_pipelines(size_t *num_pipelines, unsigned *indices, Hash *hashes) const;
	bool get_faulty_compute_pipelines(size_t *num_pipelines, unsigned *indices, Hash *hashes) const;
	bool get_faulty_raytracing_pipelines(size_t *num_pipelines, unsigned *indices, Hash *hashes) const;

	// If validation is enabled, gets a list of all pipelines which failed validation.
	bool get_graphics_failed_validation(size_t *num_hashes, Hash *hashes) const;
	bool get_compute_failed_validation(size_t *num_hashes, Hash *hashes) const;
	bool get_raytracing_failed_validation(size_t *num_hashes, Hash *hashes) const;

	enum class PollResult : unsigned
	{
		Running,
		Complete,
		ResultNotReady,
		Error
	};

	struct TypeProgress
	{
		uint32_t parsed;
		uint32_t parsed_fail;
		uint32_t completed;
		uint32_t skipped;
		uint32_t cached;

		// This value is dynamic and will be incremented as pipelines are queued up for parsing.
		uint32_t total;
	};

	struct Progress
	{
		TypeProgress compute;
		TypeProgress graphics;
		TypeProgress raytracing;

		uint32_t completed_modules;
		uint32_t missing_modules;
		uint32_t total_modules;
		uint32_t banned_modules;
		uint32_t module_validation_failures;

		uint32_t clean_crashes;
		uint32_t dirty_crashes;

		// These values are static and represents the total amount pipelines in the archive that we expect to replay.
		uint32_t total_graphics_pipeline_blobs;
		uint32_t total_compute_pipeline_blobs;
		uint32_t total_raytracing_pipeline_blobs;
	};

	PollResult poll_progress(Progress &progress);
	static void compute_condensed_progress(const Progress &progress, unsigned &completed, unsigned &total);

	struct GlobalResourceUsage
	{
		// Number of outstanding dirty pages on the system.
		// Can be used to keep track if driver cache threads are being swarmed.
		// If negative, the query failed.
		int32_t dirty_pages_mib;

		// Stats from PSI on Linux, represents IO stall time from 0 to 100.
		// If negative, the query failed.
		int32_t io_stall_percentage;

		// Number of active child processes.
		// This can change dynamically based on stall factors.
		uint32_t num_running_processes;
	};

	struct ProcessStats
	{
		// Maps to RSS in Linux (element 1 in statm). Measured in MiB.
		uint32_t resident_mib;
		// Maps to Resident shared (element 2 in statm) in Linux. Measured in MiB.
		uint32_t shared_mib;

		// Set to how much shared metadata this process maps.
		// This can be subtracted from shared_mib to figure out
		// how much unrelated shared memory is used.
		uint32_t shared_metadata_mib;

		// resident - shared is the amount of resident memory which is unique to the process,

		// -1 means dead process, 0 means stopped process.
		int32_t heartbeats;
	};

	// num_processes must not be nullptr.
	// If stats is non-nullptr, num_processes will receive the actual number of values that were reported in stats.
	// Since number of child processes is technically volatile, the number of child processes can change
	// between a call to poll_memory_usage(&count, nullptr) and poll_memory_usage(&count, stats).
	// *num_processes is the upper bound when called with stats.
	// This returns false if platform does not yet support memory query.
	// If memory query is not available yet, 0 process stats will be returned.
	// The internal data is updated at some regular interval.
	// The first process is the primary replaying process.
	bool poll_memory_usage(uint32_t *num_processes, ProcessStats *stats) const;

	// Only supported on Linux so far.
	bool poll_global_resource_usage(GlobalResourceUsage &stats) const;

	// **EXPERIMENTAL**
	// Sends a message to replayer process. The interface is somewhat ad-hoc for now.
	// Only supported on Linux so far.
	// This can be used to control dynamic behavior related to scheduling.
	// - "RUNNING_TARGET %n" (if >= 0, locks replayer to use n active processes for replay)
	// - "RUNNING_TARGET -1" (Default: use automatic scheduling, IO pressure will fiddle with process count)
	// - "IO_STALL_AUTO_ADJUST ON" (Allows IO pressure to adjust automatic scheduler)
	// - "IO_STALL_AUTO_ADJUST OFF" (Disables IO pressure from adjusting automatic scheduler)
	// - "DIRTY_PAGE_AUTO_ADJUST ON" (Allows dirty page pressure to adjust automatic scheduler)
	// - "DIRTY_PAGE_AUTO_ADJUST OFF" (Disables dirty page pressure from adjusting automatic scheduler).
	// - "DETACH" (Detach child processes from this process).
	bool send_message(const char *msg);

private:
	struct Impl;
	Impl *impl;
};
}
