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

#include <stdint.h>
#include "fossilize.hpp"

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

		// Path to a Fossilize database to be replayed.
		const char *database;

		// Path to an on-disk pipeline cache. Maps to --on-disk-pipeline-cache.
		const char *on_disk_pipeline_cache;

		// Maps to --num-threads. If 0, no argument for --num-threads is passed.
		unsigned num_threads;

		// Maps to --pipeline-cache
		bool pipeline_cache;

		// Redirect stdout and stderr to /dev/null or NUL.
		bool quiet;
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
	bool get_faulty_spirv_modules(size_t *num_hashes, Hash *hashes);

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

		uint32_t clean_crashes;
		uint32_t dirty_crashes;
	};

	PollResult poll_progress(Progress &progress);

private:
	struct Impl;
	Impl *impl;
};
}
