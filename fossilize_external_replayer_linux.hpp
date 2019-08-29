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
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "layer/utils.hpp"
#include <atomic>
#include <vector>
#include <unordered_set>
#include <string>
#include <signal.h>
#include <limits.h>
#include "path.hpp"
#include "fossilize_external_replayer_control_block.hpp"

#ifdef __linux__
#include "platform/futex_wrapper_linux.hpp"
#else
#include "platform/gcc_clang_spinlock.hpp"
#endif

namespace Fossilize
{
static std::atomic<int32_t> shm_index;

struct ExternalReplayer::Impl
{
	~Impl();

	pid_t pid = -1;
	int fd = -1;
	SharedControlBlock *shm_block = nullptr;
	size_t shm_block_size = 0;
	int wstatus = 0;
	std::unordered_set<Hash> faulty_spirv_modules;
	std::unordered_set<Hash> graphics_failed_validation;
	std::unordered_set<Hash> compute_failed_validation;

	bool start(const ExternalReplayer::Options &options);
	ExternalReplayer::PollResult poll_progress(Progress &progress);
	uintptr_t get_process_handle() const;
	int wait();
	bool is_process_complete(int *return_status);
	bool kill();

	void parse_message(const char *msg);
	bool get_faulty_spirv_modules(size_t *count, Hash *hashes) const;
	bool get_graphics_failed_validation(size_t *count, Hash *hashes) const;
	bool get_compute_failed_validation(size_t *count, Hash *hashes) const;
	bool get_failed(const std::unordered_set<Hash> &failed, size_t *count, Hash *hashes) const;
};

ExternalReplayer::Impl::~Impl()
{
	if (fd >= 0)
		close(fd);

	if (shm_block)
		munmap(shm_block, shm_block_size);
}

uintptr_t ExternalReplayer::Impl::get_process_handle() const
{
	return uintptr_t(pid);
}

ExternalReplayer::PollResult ExternalReplayer::Impl::poll_progress(ExternalReplayer::Progress &progress)
{
	bool complete = shm_block->progress_complete.load(std::memory_order_acquire) != 0;

	if (pid < 0 && !complete)
		return ExternalReplayer::PollResult::Error;

	if (shm_block->progress_started.load(std::memory_order_acquire) == 0)
		return ExternalReplayer::PollResult::ResultNotReady;

	progress.compute.total = shm_block->total_compute.load(std::memory_order_relaxed);
	progress.compute.parsed = shm_block->parsed_compute.load(std::memory_order_relaxed);
	progress.compute.skipped = shm_block->skipped_compute.load(std::memory_order_relaxed);
	progress.compute.completed = shm_block->successful_compute.load(std::memory_order_relaxed);
	progress.graphics.total = shm_block->total_graphics.load(std::memory_order_relaxed);
	progress.graphics.parsed = shm_block->parsed_graphics.load(std::memory_order_relaxed);
	progress.graphics.skipped = shm_block->skipped_graphics.load(std::memory_order_relaxed);
	progress.graphics.completed = shm_block->successful_graphics.load(std::memory_order_relaxed);
	progress.completed_modules = shm_block->successful_modules.load(std::memory_order_relaxed);
	progress.total_modules = shm_block->total_modules.load(std::memory_order_relaxed);
	progress.banned_modules = shm_block->banned_modules.load(std::memory_order_relaxed);
	progress.module_validation_failures = shm_block->module_validation_failures.load(std::memory_order_relaxed);
	progress.clean_crashes = shm_block->clean_process_deaths.load(std::memory_order_relaxed);
	progress.dirty_crashes = shm_block->dirty_process_deaths.load(std::memory_order_relaxed);

	futex_wrapper_lock(&shm_block->futex_lock);
	size_t read_avail = shared_control_block_read_avail(shm_block);
	for (size_t i = ControlBlockMessageSize; i <= read_avail; i += ControlBlockMessageSize)
	{
		char buf[ControlBlockMessageSize] = {};
		shared_control_block_read(shm_block, buf, sizeof(buf));
		parse_message(buf);
	}
	futex_wrapper_unlock(&shm_block->futex_lock);
	return complete ? ExternalReplayer::PollResult::Complete : ExternalReplayer::PollResult::Running;
}

static int wstatus_to_return(int wstatus)
{
	if (WIFEXITED(wstatus))
		return WEXITSTATUS(wstatus);
	else if (WIFSIGNALED(wstatus))
		return -WTERMSIG(wstatus);
	else
		return 0;
}

void ExternalReplayer::Impl::parse_message(const char *msg)
{
	if (strncmp(msg, "MODULE", 6) == 0)
	{
		auto hash = strtoull(msg + 6, nullptr, 16);
		faulty_spirv_modules.insert(hash);
	}
	else if (strncmp(msg, "GRAPHICS_VERR", 13) == 0)
	{
		auto hash = strtoull(msg + 13, nullptr, 16);
		graphics_failed_validation.insert(hash);
	}
	else if (strncmp(msg, "COMPUTE_VERR", 12) == 0)
	{
		auto hash = strtoull(msg + 12, nullptr, 16);
		compute_failed_validation.insert(hash);
	}
}

bool ExternalReplayer::Impl::is_process_complete(int *return_status)
{
	if (pid == -1)
	{
		if (return_status)
			*return_status = wstatus_to_return(wstatus);
		return true;
	}

	if (waitpid(pid, &wstatus, WNOHANG) <= 0)
		return false;

	// Pump the fifo through.
	ExternalReplayer::Progress progress = {};
	poll_progress(progress);

	pid = -1;

	if (return_status)
		*return_status = wstatus_to_return(wstatus);
	return true;
}

int ExternalReplayer::Impl::wait()
{
	if (pid == -1)
		return wstatus_to_return(wstatus);

	// Pump the fifo through.
	ExternalReplayer::Progress progress = {};
	poll_progress(progress);

	if (waitpid(pid, &wstatus, 0) < 0)
		return -1;

	// Pump the fifo through.
	poll_progress(progress);
	pid = -1;
	return wstatus_to_return(wstatus);
}

bool ExternalReplayer::Impl::kill()
{
	if (pid < 0)
		return false;
	return killpg(pid, SIGTERM) == 0;
}

bool ExternalReplayer::Impl::get_failed(const std::unordered_set<Hash> &failed, size_t *count, Hash *hashes) const
{
	if (hashes)
	{
		if (*count != failed.size())
			return false;

		for (auto &mod : failed)
			*hashes++ = mod;
		return true;
	}
	else
	{
		*count = failed.size();
		return true;
	}
}

bool ExternalReplayer::Impl::get_faulty_spirv_modules(size_t *count, Hash *hashes) const
{
	return get_failed(faulty_spirv_modules, count, hashes);
}

bool ExternalReplayer::Impl::get_graphics_failed_validation(size_t *count, Hash *hashes) const
{
	return get_failed(graphics_failed_validation, count, hashes);
}

bool ExternalReplayer::Impl::get_compute_failed_validation(size_t *count, Hash *hashes) const
{
	return get_failed(compute_failed_validation, count, hashes);
}

bool ExternalReplayer::Impl::start(const ExternalReplayer::Options &options)
{
	char shm_name[256];
	sprintf(shm_name, "/fossilize-external-%d-%d", getpid(), shm_index.fetch_add(1, std::memory_order_relaxed));
	fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0)
	{
		LOGE("Failed to create shared memory.\n");
		return false;
	}

	// Reserve 4 kB for control data, and 64 kB for a cross-process SHMEM ring buffer.
	shm_block_size = 64 * 1024 + 4 * 1024;

	if (ftruncate(fd, shm_block_size) < 0)
		return false;

	shm_block = static_cast<SharedControlBlock *>(mmap(nullptr, shm_block_size,
	                                                   PROT_READ | PROT_WRITE, MAP_SHARED,
	                                                   fd, 0));
	if (shm_block == MAP_FAILED)
	{
		LOGE("Failed to mmap shared block.\n");
		return false;
	}

	// I believe zero-filled pages are guaranteed, but don't take any chances.
	// Cast to void explicitly to avoid warnings on GCC 8.
	memset(static_cast<void *>(shm_block), 0, shm_block_size);
	shm_block->version_cookie = ControlBlockMagic;

	shm_block->ring_buffer_size = 64 * 1024;
	shm_block->ring_buffer_offset = 4 * 1024;

	// We need to let our child inherit the shared FD.
	int current_flags = fcntl(fd, F_GETFD);
	if (current_flags < 0)
	{
		LOGE("Failed to get FD flags.\n");
		return false;
	}

	if (fcntl(fd, F_SETFD, current_flags & ~FD_CLOEXEC) < 0)
	{
		LOGE("Failed to set FD flags.\n");
		return false;
	}

	// Now that we have mapped, makes sure the SHM segment gets deleted when our processes go away.
	if (shm_unlink(shm_name) < 0)
	{
		LOGE("Failed to unlink shared memory segment.\n");
		return false;
	}

	pid_t new_pid = fork();
	if (new_pid > 0)
	{
		close(fd);
		fd = -1;
		pid = new_pid;
	}
	else if (new_pid == 0)
	{
		if (!options.inherit_process_group)
		{
			// Set the process group ID so we can kill all the child processes as needed.
			if (setpgid(0, 0) < 0)
			{
				LOGE("Failed to set PGID in child.\n");
				exit(1);
			}
		}

		char fd_name[16];
		sprintf(fd_name, "%d", fd);
		char num_thread_holder[16];

		std::string self_path;
		if (!options.external_replayer_path)
			self_path = Path::get_executable_path();

		std::vector<const char *> argv;
		if (options.external_replayer_path)
			argv.push_back(options.external_replayer_path);
		else
			argv.push_back(self_path.c_str());

		for (unsigned i = 0; i < options.num_databases; i++)
			argv.push_back(options.databases[i]);

		argv.push_back("--master-process");
		if (options.quiet)
			argv.push_back("--quiet-slave");
		argv.push_back("--shmem-fd");
		argv.push_back(fd_name);

		if (options.pipeline_cache)
			argv.push_back("--pipeline-cache");
		if (options.spirv_validate)
			argv.push_back("--spirv-val");

		if (options.num_threads)
		{
			argv.push_back("--num-threads");
			sprintf(num_thread_holder, "%u", options.num_threads);
			argv.push_back(num_thread_holder);
		}

		if (options.on_disk_pipeline_cache)
		{
			argv.push_back("--on-disk-pipeline-cache");
			argv.push_back(options.on_disk_pipeline_cache);
		}

		if (options.on_disk_validation_cache)
		{
			argv.push_back("--on-disk-validation-cache");
			argv.push_back(options.on_disk_validation_cache);
		}

		if (options.enable_validation)
			argv.push_back("--enable-validation");

		if (options.ignore_derived_pipelines)
			argv.push_back("--ignore-derived-pipelines");

		if (options.null_device)
			argv.push_back("--null-device");

		argv.push_back("--device-index");
		char index_name[16];
		sprintf(index_name, "%u", options.device_index);
		argv.push_back(index_name);

		char graphics_range_start[16], graphics_range_end[16];
		char compute_range_start[16], compute_range_end[16];

		if (options.use_pipeline_range)
		{
			argv.push_back("--graphics-pipeline-range");
			sprintf(graphics_range_start, "%u", options.start_graphics_index);
			sprintf(graphics_range_end, "%u", options.end_graphics_index);
			argv.push_back(graphics_range_start);
			argv.push_back(graphics_range_end);

			argv.push_back("--compute-pipeline-range");
			sprintf(compute_range_start, "%u", options.start_compute_index);
			sprintf(compute_range_end, "%u", options.end_compute_index);
			argv.push_back(compute_range_start);
			argv.push_back(compute_range_end);
		}

		if (options.pipeline_stats_path)
		{
			argv.push_back("--enable-pipeline-stats");
			argv.push_back(options.pipeline_stats_path);
		}

		argv.push_back(nullptr);

		if (options.quiet)
		{
			int null_fd = open("/dev/null", O_WRONLY);
			if (null_fd >= 0)
			{
				dup2(null_fd, STDOUT_FILENO);
				dup2(null_fd, STDERR_FILENO);
				close(null_fd);
			}
		}

		if (execv(options.external_replayer_path ? options.external_replayer_path : self_path.c_str(),
		          const_cast<char * const*>(argv.data())) < 0)
		{
			LOGE("Failed to start external process %s with execv.\n", options.external_replayer_path);
			exit(errno);
		}
		else
		{
			LOGE("Failed to start external process %s with execv.\n", options.external_replayer_path);
			exit(1);
		}
	}
	else
	{
		LOGE("Failed to create child process.\n");
		return false;
	}

	return true;
}
}

