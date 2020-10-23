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
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "layer/utils.hpp"
#include <atomic>
#include <vector>
#include <array>
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

#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif

namespace Fossilize
{
static std::atomic<int32_t> shm_index;

struct ExternalReplayer::Impl
{
	~Impl();

	pid_t pid = -1;
	int fd = -1;
	int kill_fd = -1;
	int close_fd = -1;
	SharedControlBlock *shm_block = nullptr;
	size_t shm_block_size = 0;
	int wstatus = 0;
	bool synthesized_exit_code = false;
	std::unordered_set<Hash> faulty_spirv_modules;
	std::vector<std::pair<unsigned, Hash>> faulty_graphics_pipelines;
	std::vector<std::pair<unsigned, Hash>> faulty_compute_pipelines;
	std::unordered_set<Hash> graphics_failed_validation;
	std::unordered_set<Hash> compute_failed_validation;

	bool start(const ExternalReplayer::Options &options);
	void start_replayer_process(const ExternalReplayer::Options &options);
	ExternalReplayer::PollResult poll_progress(Progress &progress);
	uintptr_t get_process_handle() const;
	int wait();
	bool is_process_complete(int *return_status);
	bool kill();

	void parse_message(const char *msg);
	bool get_faulty_spirv_modules(size_t *count, Hash *hashes) const;
	bool get_faulty_graphics_pipelines(size_t *count, unsigned *indices, Hash *hashes) const;
	bool get_faulty_compute_pipelines(size_t *count, unsigned *indices, Hash *hashes) const;
	bool get_graphics_failed_validation(size_t *count, Hash *hashes) const;
	bool get_compute_failed_validation(size_t *count, Hash *hashes) const;
	bool get_failed(const std::unordered_set<Hash> &failed, size_t *count, Hash *hashes) const;
	bool get_failed(const std::vector<std::pair<unsigned, Hash>> &failed, size_t *count,
	                unsigned *indices, Hash *hashes) const;
	void reset_pid();
};

ExternalReplayer::Impl::~Impl()
{
	if (fd >= 0)
		close(fd);
	if (kill_fd >= 0)
		close(kill_fd);
	if (close_fd >= 0)
		close(close_fd);

	if (shm_block)
		munmap(shm_block, shm_block_size);
}

uintptr_t ExternalReplayer::Impl::get_process_handle() const
{
	return uintptr_t(pid);
}

void ExternalReplayer::Impl::reset_pid()
{
	pid = -1;
	if (kill_fd >= 0)
		close(kill_fd);
	kill_fd = -1;
	if (close_fd >= 0)
		close(close_fd);
	close_fd = -1;
}

ExternalReplayer::PollResult ExternalReplayer::Impl::poll_progress(ExternalReplayer::Progress &progress)
{
	bool complete = shm_block->progress_complete.load(std::memory_order_acquire) != 0;

	if (pid < 0 && !complete)
		return ExternalReplayer::PollResult::Error;

	// Try to avoid a situation where we're endlessly polling, in case the application died too early during startup and we failed
	// to catch it ending by receiving a completed wait through is_process_complete().
	if (!complete && pid >= 0)
	{
		int ret;
		// This serves as a check to see if process is still alive.
		if ((ret = waitpid(pid, &wstatus, WNOHANG)) > 0)
		{
			// Child process can receive SIGCONT/SIGSTOP which is benign.
			if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus))
				reset_pid();
		}
		else if (ret < 0)
		{
			// The child does not exist anymore, and we were unable to reap it.
			// This can happen if the process installed a SIGCHLD handler behind our back.
			wstatus = -errno;
			synthesized_exit_code = true;
			reset_pid();
		}

		// If ret is 0, that means the process is still alive and nothing happened to it yet.
	}

	if (shm_block->progress_started.load(std::memory_order_acquire) == 0)
		return ExternalReplayer::PollResult::ResultNotReady;

	progress.compute.total = shm_block->total_compute.load(std::memory_order_relaxed);
	progress.compute.parsed = shm_block->parsed_compute.load(std::memory_order_relaxed);
	progress.compute.parsed_fail = shm_block->parsed_compute_failures.load(std::memory_order_relaxed);
	progress.compute.skipped = shm_block->skipped_compute.load(std::memory_order_relaxed);
	progress.compute.cached = shm_block->cached_compute.load(std::memory_order_relaxed);
	progress.compute.completed = shm_block->successful_compute.load(std::memory_order_relaxed);
	progress.graphics.total = shm_block->total_graphics.load(std::memory_order_relaxed);
	progress.graphics.parsed = shm_block->parsed_graphics.load(std::memory_order_relaxed);
	progress.graphics.parsed_fail = shm_block->parsed_graphics_failures.load(std::memory_order_relaxed);
	progress.graphics.skipped = shm_block->skipped_graphics.load(std::memory_order_relaxed);
	progress.graphics.cached = shm_block->cached_graphics.load(std::memory_order_relaxed);
	progress.graphics.completed = shm_block->successful_graphics.load(std::memory_order_relaxed);
	progress.completed_modules = shm_block->successful_modules.load(std::memory_order_relaxed);
	progress.missing_modules = shm_block->parsed_module_failures.load(std::memory_order_relaxed);
	progress.total_modules = shm_block->total_modules.load(std::memory_order_relaxed);
	progress.banned_modules = shm_block->banned_modules.load(std::memory_order_relaxed);
	progress.module_validation_failures = shm_block->module_validation_failures.load(std::memory_order_relaxed);
	progress.clean_crashes = shm_block->clean_process_deaths.load(std::memory_order_relaxed);
	progress.dirty_crashes = shm_block->dirty_process_deaths.load(std::memory_order_relaxed);

	progress.total_graphics_pipeline_blobs = shm_block->static_total_count_graphics.load(std::memory_order_relaxed);
	progress.total_compute_pipeline_blobs = shm_block->static_total_count_compute.load(std::memory_order_relaxed);

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

static int wstatus_to_return(int wstatus, bool synthesized_exit_code)
{
	if (synthesized_exit_code)
		return wstatus;
	else if (WIFEXITED(wstatus))
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
	else if (strncmp(msg, "GRAPHICS", 8) == 0)
	{
		char *end = nullptr;
		int index = int(strtol(msg + 8, &end, 0));
		if (index >= 0 && end)
		{
			Hash graphics_pipeline = strtoull(end, nullptr, 16);
			faulty_graphics_pipelines.push_back({ unsigned(index), graphics_pipeline });
		}
	}
	else if (strncmp(msg, "COMPUTE", 7) == 0)
	{
		char *end = nullptr;
		int index = int(strtol(msg + 7, &end, 0));
		if (index >= 0 && end)
		{
			Hash compute_pipeline = strtoull(end, nullptr, 16);
			faulty_compute_pipelines.push_back({ unsigned(index), compute_pipeline });
		}
	}
}

bool ExternalReplayer::Impl::is_process_complete(int *return_status)
{
	if (pid == -1)
	{
		if (return_status)
			*return_status = wstatus_to_return(wstatus, synthesized_exit_code);
		return true;
	}

	int ret;
	if ((ret = waitpid(pid, &wstatus, WNOHANG)) == 0)
		return false;

	// Child process can receive SIGCONT/SIGSTOP which is benign.
	if (ret > 0 && !WIFEXITED(wstatus) && !WIFSIGNALED(wstatus))
		return false;

	if (ret < 0)
	{
		// If we error out here, we will not be able to receive a functioning return code, so
		// just return -errno.
		wstatus = -errno;
		synthesized_exit_code = true;
	}

	// Pump the fifo through.
	ExternalReplayer::Progress progress = {};
	poll_progress(progress);

	reset_pid();

	if (return_status)
		*return_status = wstatus_to_return(wstatus, synthesized_exit_code);
	return true;
}

int ExternalReplayer::Impl::wait()
{
	if (pid == -1)
		return wstatus_to_return(wstatus, synthesized_exit_code);

	// Pump the fifo through.
	ExternalReplayer::Progress progress = {};
	poll_progress(progress);

	for (;;)
	{
		if (waitpid(pid, &wstatus, 0) < 0)
		{
			wstatus = -errno;
			synthesized_exit_code = true;
			break;
		}

		// Child process can receive SIGCONT/SIGSTOP which is benign.
		if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus))
			break;
	}

	// Pump the fifo through.
	poll_progress(progress);
	reset_pid();
	return wstatus_to_return(wstatus, synthesized_exit_code);
}

bool ExternalReplayer::Impl::kill()
{
	if (pid < 0)
		return false;

	if (kill_fd >= 0)
	{
		// Before we attempt to kill, we must make sure that the new process group has been created.
		// This read will block until we close the FD in the forked process, ensuring that we
		// can immediately call killpg() against it, since that close will only happen after setpgid().
		char dummy;
		if (::read(kill_fd, &dummy, sizeof(dummy)) < 0)
		{
			close(kill_fd);
			kill_fd = -1;
			return false;
		}
		close(kill_fd);
		kill_fd = -1;
	}

	bool ret = killpg(pid, SIGTERM) == 0;
	if (!ret)
		LOGI("ExternalReplayer::Impl::kill(): Failed to kill: errno %d.\n", errno);
	return ret;
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

bool ExternalReplayer::Impl::get_failed(const std::vector<std::pair<unsigned, Hash>> &failed,
                                        size_t *count, unsigned *indices, Hash *hashes) const
{
	if (hashes)
	{
		if (*count != failed.size())
			return false;

		for (auto &mod : failed)
		{
			*indices++ = mod.first;
			*hashes++ = mod.second;
		}
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

bool ExternalReplayer::Impl::get_faulty_graphics_pipelines(size_t *count, unsigned int *indices, Hash *hashes) const
{
	return get_failed(faulty_graphics_pipelines, count, indices, hashes);
}

bool ExternalReplayer::Impl::get_faulty_compute_pipelines(size_t *count, unsigned *indices, Hash *hashes) const
{
	return get_failed(faulty_compute_pipelines, count, indices, hashes);
}

bool ExternalReplayer::Impl::get_graphics_failed_validation(size_t *count, Hash *hashes) const
{
	return get_failed(graphics_failed_validation, count, hashes);
}

bool ExternalReplayer::Impl::get_compute_failed_validation(size_t *count, Hash *hashes) const
{
	return get_failed(compute_failed_validation, count, hashes);
}

void ExternalReplayer::Impl::start_replayer_process(const ExternalReplayer::Options &options)
{
	char fd_name[16];
	sprintf(fd_name, "%d", fd);
	char num_thread_holder[16];

	std::string self_path;
	if (!options.external_replayer_path)
	{
#ifdef __linux__
		self_path = "/proc/self/exe";
#else
		self_path = Path::get_executable_path();
#endif
	}

	std::vector<const char *> argv;
	if (options.external_replayer_path)
		argv.push_back(options.external_replayer_path);
	else
		argv.push_back(self_path.c_str());

	if (options.external_replayer_extra_path)
		argv.push_back(options.external_replayer_extra_path);

	for (unsigned i = 0; i < options.num_databases; i++)
		argv.push_back(options.databases[i]);

	argv.push_back("--master-process");
	if (options.quiet)
		argv.push_back("--quiet-slave");
	argv.push_back("--shmem-fd");
	argv.push_back(fd_name);

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

	if (options.on_disk_validation_whitelist)
	{
		argv.push_back("--on-disk-validation-whitelist");
		argv.push_back(options.on_disk_validation_whitelist);
	}

	if (options.on_disk_validation_blacklist)
	{
		argv.push_back("--on-disk-validation-blacklist");
		argv.push_back(options.on_disk_validation_blacklist);
	}

	if (options.replayer_cache_path)
	{
		argv.push_back("--replayer-cache");
		argv.push_back(options.replayer_cache_path);
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

	char timeout[16];
	if (options.timeout_seconds)
	{
		argv.push_back("--timeout-seconds");
		sprintf(timeout, "%u", options.timeout_seconds);
		argv.push_back(timeout);
	}

	std::vector<std::array<char, 16>> implicit_indices;
	implicit_indices.resize(options.num_implicit_whitelist_indices);
	for (unsigned i = 0; i < options.num_implicit_whitelist_indices; i++)
	{
		argv.push_back("--implicit-whitelist");
		sprintf(implicit_indices[i].data(), "%u", options.implicit_whitelist_indices[i]);
		argv.push_back(implicit_indices[i].data());
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

	// Replayer should have idle priority.
	// nice() can return -1 in valid scenarios, need to check errno.
	errno = 0;
	nice(19);
	if (errno != 0)
		LOGE("Failed to set nice value for external replayer!\n");

#ifdef __linux__
	// Replayer crunches a lot of numbers, hint the scheduler.
	// This results in better throughput at the same or lower CPU usage (due
	// to better CPU cache utilization with bigger time slices), it doesn't
	// preempt interactive tasks (less impact on games), and it also makes a
	// better chance for the block layer to coalesce IO requests (more IO
	// may be dispatched per time slice).
	{
		struct sched_param p = {};
		if (sched_setscheduler(0, SCHED_BATCH, &p) < 0)
			LOGE("Failed to set scheduling policy for external replayer!\n");
	}
#endif

	// We're now in the child process, so it's safe to override environment here.
	for (unsigned i = 0; i < options.num_environment_variables; i++)
		setenv(options.environment_variables[i].key, options.environment_variables[i].value, 1);

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

static bool create_low_priority_autogroup()
{
	pid_t group_pid;

	// Set the process group ID so we can kill all the child processes as needed.
	// Use a new session ID so that we get a new scheduling autogroup.
	// This will also create a new process group.
	if ((group_pid = setsid()) < 0)
	{
		LOGE("Failed to set PGID in child.\n");
		return false;
	}

	// Sanity check that setsid did what we expected.
	if (group_pid != getpgrp() || getpgrp() != getpid())
	{
		LOGE("Failed to validate PGID in child.\n");
		return false;
	}

#ifdef __linux__
	bool autogroups_enabled = false;
	{
		FILE *file = fopen("/proc/sys/kernel/sched_autogroup_enabled", "rb");
		if (file)
		{
			char buffer[2] = {};
			if (fread(buffer, 1, sizeof(buffer), file) >= 1)
				autogroups_enabled = buffer[0] == '1';
			fclose(file);
		}

		// If the kernel does not enable autogroup scheduling support, don't bother.
	}

	if (autogroups_enabled)
	{
		// There is no API for setting the autogroup scheduling, so do it here.
		// Reference: https://github.com/nlburgin/reallynice
		FILE *file = fopen("/proc/self/autogroup", "w");
		if (file)
		{
			LOGI("Setting autogroup scheduling.\n");
			fputs("19", file);
			fclose(file);
		}
		else
			LOGE("/proc/self/autogroup does not exist on this system. Skipping autogrouping.\n");
	}
	else
		LOGI("Autogroup scheduling is not enabled on this kernel. Will rely entirely on nice().\n");
#endif

	return true;
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

	int fds[2];
	if (pipe(fds) < 0)
		return false;

	int close_fds[2] = { -1, -1 };
	if (!options.inherit_process_group && pipe(close_fds) < 0)
		return false;

	pid_t new_pid = fork();
	if (new_pid > 0)
	{
		close(fd);
		close(fds[1]);
		fd = -1;
		pid = new_pid;
		kill_fd = fds[0];

		if (!options.inherit_process_group)
		{
			close(close_fds[0]);
			close_fd = close_fds[1];
		}
	}
	else if (new_pid == 0)
	{
		close(fds[0]);
		if (!options.inherit_process_group)
		{
			close(close_fds[1]);

			if (!create_low_priority_autogroup())
			{
				LOGE("Failed to create session.\n");
				exit(1);
			}

			if (dup2(close_fds[0], STDIN_FILENO) < 0)
			{
				LOGE("Failed to dup FD to stdin in child process.\n");
				exit(1);
			}

			close(close_fds[0]);
		}

		// Notify parent process that it can return.
		close(fds[1]);

		start_replayer_process(options);
	}
	else
	{
		LOGE("Failed to create child process.\n");
		return false;
	}

	return true;
}
}

