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
	struct Options
	{
		// Path to the fossilize-replay executable.
		// May be null, in which case the calling process must be fossilize-replay itself.
		const char *external_replayer_path;

		// Paths to one or more Fossilize database to be replayed.
		// Multiple paths may be used here in which case the replayer will treat
		// the database as a union of all the databases in "databases".
		const char * const *databases;
		unsigned num_databases;

		// Path to an on-disk pipeline cache. Maps to --on-disk-pipeline-cache.
		const char *on_disk_pipeline_cache;

		// Path to an on-disk validation cache. Maps to --on-disk-validation-cache.
		const char *on_disk_validation_cache;

		// Path to store pipeline stats in.
		const char *pipeline_stats_path;

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
		bool use_pipeline_range;

		// Maps to --pipeline-cache
		bool pipeline_cache;

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

		// Ignores derived pipelines, reduces memory consumption when replaying.
		// Only useful if the driver in question ignores use of derived pipelines when hashing pipelines internally.
		bool ignore_derived_pipelines;

		// Creates a dummy device, useful for benchmarking time and/or memory consumption in isolation.
		bool null_device;
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
	bool kill();

	// As the replayer is progressing, it might find SPIR-V modules which might have contributed to a crash.
	// This allows the caller to later investigate what these modules are doing.
	bool get_faulty_spirv_modules(size_t *num_hashes, Hash *hashes) const;

	// If validation is enabled, gets a list of all pipelines which failed validation.
	bool get_graphics_failed_validation(size_t *num_hashes, Hash *hashes) const;
	bool get_compute_failed_validation(size_t *num_hashes, Hash *hashes) const;

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
		uint32_t completed;
		uint32_t skipped;
		uint32_t total;
	};

	struct Progress
	{
		TypeProgress compute;
		TypeProgress graphics;

		uint32_t completed_modules;
		uint32_t total_modules;
		uint32_t banned_modules;
		uint32_t module_validation_failures;

		uint32_t clean_crashes;
		uint32_t dirty_crashes;
	};

	PollResult poll_progress(Progress &progress);

private:
	struct Impl;
	Impl *impl;
};
}
