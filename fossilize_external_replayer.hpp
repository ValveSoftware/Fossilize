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

namespace Fossilize
{
class ExternalReplayer
{
public:
	struct Options
	{
		const char *external_replayer_path;
		const char *database;
		const char *on_disk_pipeline_cache;
		unsigned num_threads;
		bool pipeline_cache;
		bool quiet;
	};

	ExternalReplayer();
	~ExternalReplayer();
	void operator=(const ExternalReplayer &) = delete;
	ExternalReplayer(const ExternalReplayer &) = delete;

	bool start(const Options &options);

	// On Unix, this can be cast to a pid_t, on Windows, a HANDLE.
	// On Unix, the caller is responsible for reaping the child PID when it dies, unless
	// the blocking wait() is used, which translates to waitpid, and will therefore reap the child
	// process itself.
	uintptr_t get_process_handle() const;

	// Blocking waits for the process to complete and closes the process handle.
	// Returns true if child process was waited for correctly and exited cleanly, i.e. exit(0), otherwise false.
	bool wait();

	// Queries if the process is dead, but does *not* reap the child process, even if it is dead.
	// This is more useful if the calling process has a central system in place to reap child
	// processes.
	bool is_process_complete();

	// Requests that process (and its children) are killed.
	bool kill();

	enum class PollResult : unsigned
	{
		Running,
		Complete,
		ResultNotReady,
		Error
	};

	struct TypeProgress
	{
		uint32_t completed;
		uint32_t skipped;
		uint32_t total;
	};

	struct Progress
	{
		TypeProgress compute;
		TypeProgress graphics;

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
