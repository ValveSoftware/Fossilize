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

#include "fossilize_external_replayer.hpp"
#include "fossilize_external_replayer_control_block.hpp"

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
#include <string>
#include <pthread.h>
#include <signal.h>

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

	bool start(const ExternalReplayer::Options &options);
	ExternalReplayer::PollResult poll_progress(Progress &progress);
	uintptr_t get_process_handle() const;
	bool wait();
	bool is_process_complete();
	bool kill();
};

ExternalReplayer::Impl::~Impl()
{
	if (fd >= 0)
		close(fd);

	if (shm_block)
	{
		pthread_mutex_destroy(&shm_block->lock);
		munmap(shm_block, shm_block_size);
	}
}

uintptr_t ExternalReplayer::Impl::get_process_handle() const
{
	return uintptr_t(pid);
}

ExternalReplayer::PollResult ExternalReplayer::Impl::poll_progress(ExternalReplayer::Progress &progress)
{
	if (pid < 0)
		return ExternalReplayer::PollResult::Error;

	bool complete = shm_block->progress_complete.load(std::memory_order_acquire);

	if (!shm_block->progress_started.load(std::memory_order_acquire))
		return ExternalReplayer::PollResult::ResultNotReady;

	progress.compute.total = shm_block->total_compute.load(std::memory_order_relaxed);
	progress.compute.skipped = shm_block->skipped_compute.load(std::memory_order_relaxed);
	progress.compute.completed = shm_block->successful_compute.load(std::memory_order_relaxed);
	progress.graphics.total = shm_block->total_graphics.load(std::memory_order_relaxed);
	progress.graphics.skipped = shm_block->skipped_graphics.load(std::memory_order_relaxed);
	progress.graphics.completed = shm_block->successful_graphics.load(std::memory_order_relaxed);
	progress.total_modules = shm_block->total_modules.load(std::memory_order_relaxed);
	progress.banned_modules = shm_block->banned_modules.load(std::memory_order_relaxed);
	progress.clean_crashes = shm_block->clean_process_deaths.load(std::memory_order_relaxed);
	progress.dirty_crashes = shm_block->dirty_process_deaths.load(std::memory_order_relaxed);

	size_t read_avail = shared_control_block_read_avail(shm_block);
	for (size_t i = ControlBlockMessageSize; i <= read_avail; i += ControlBlockMessageSize)
	{
		char buf[ControlBlockMessageSize] = {};
		shared_control_block_read(shm_block, buf, sizeof(buf));
		LOGI("From FIFO: %s\n", buf);
	}
	return complete ? ExternalReplayer::PollResult::Complete : ExternalReplayer::PollResult::Running;
}

bool ExternalReplayer::Impl::is_process_complete()
{
	if (pid == -1)
		return true;
	return ::killpg(pid, 0) < 0;
}

bool ExternalReplayer::Impl::wait()
{
	if (pid == -1)
		return false;

	// Pump the fifo through.
	ExternalReplayer::Progress progress = {};
	poll_progress(progress);

	int wstatus;
	if (waitpid(pid, &wstatus, 0) < 0)
		return false;

	// Pump the fifo through.
	poll_progress(progress);

	pid = -1;
	return WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
}

bool ExternalReplayer::Impl::kill()
{
	if (pid < 0)
		return false;
	return killpg(pid, SIGTERM) == 0;
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
	memset(static_cast<void *>(shm_block), 0, shm_block_size);

	shm_block->ring_buffer_size = 64 * 1024;
	shm_block->ring_buffer_offset = 4 * 1024;

	pthread_mutexattr_t attr;
	if (pthread_mutexattr_init(&attr) < 0)
		return false;
	if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) < 0)
		return false;
	if (pthread_mutex_init(&shm_block->lock, &attr) < 0)
		return false;
	pthread_mutexattr_destroy(&attr);

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

	// vfork blocks until the child has called exec().
	pid_t new_pid = vfork();
	if (new_pid > 0)
	{
		close(fd);
		fd = -1;
		pid = new_pid;
	}
	else if (new_pid == 0)
	{
		// Set the process group ID so we can kill all the child processes as needed.
		if (setpgid(0, 0) < 0)
		{
			LOGE("Failed to set PGID in child.\n");
			exit(1);
		}

		char fd_name[16];
		sprintf(fd_name, "%d", fd);
		char num_thread_holder[16];

		std::vector<const char *> argv;
		argv.push_back(options.external_replayer_path);
		argv.push_back(options.database);
		argv.push_back("--master-process");
		argv.push_back("--quiet-slave");
		argv.push_back("--shmem-fd");
		argv.push_back(fd_name);

		if (options.pipeline_cache)
			argv.push_back("--pipeline-cache");

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

		if (execv(options.external_replayer_path, const_cast<char * const*>(argv.data())) < 0)
			exit(errno);
		else
			exit(1);
	}
	else
	{
		LOGE("Failed to create child process.\n");
		return false;
	}

	return true;
}
}

namespace Fossilize
{
ExternalReplayer::ExternalReplayer()
{
	impl = new Impl;
}

ExternalReplayer::~ExternalReplayer()
{
	delete impl;
}

bool ExternalReplayer::wait()
{
	return impl->wait();
}

bool ExternalReplayer::kill()
{
	return impl->kill();
}

uintptr_t ExternalReplayer::get_process_handle() const
{
	return impl->get_process_handle();
}

bool ExternalReplayer::start(const Options &options)
{
	return impl->start(options);
}

ExternalReplayer::PollResult ExternalReplayer::poll_progress(Progress &progress)
{
	return impl->poll_progress(progress);
}

bool ExternalReplayer::is_process_complete()
{
	return impl->is_process_complete();
}
}