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

#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include "fossilize_external_replayer.hpp"
#include "platform/futex_wrapper_linux.hpp"
#include <inttypes.h>

static bool write_all(int fd, const char *str)
{
	// write is async-signal safe, but not stdio.
	size_t len = strlen(str);
	while (len)
	{
		ssize_t wrote = write(fd, str, len);
		if (wrote <= 0)
			return false;

		str += wrote;
		len -= wrote;
	}

	return true;
}

static int run_slave_process(const VulkanDevice::Options &opts,
                             const ThreadedReplayer::Options &replayer_opts,
                             const vector<const char *> &databases);

namespace Global
{
static unordered_set<Hash> faulty_spirv_modules;
static unsigned active_processes;
static ThreadedReplayer::Options base_replayer_options;
static vector<const char *> databases;
static sigset_t old_mask;
static int signal_fd;
static int epoll_fd;
static VulkanDevice::Options device_options;
static bool quiet_slave;

static SharedControlBlock *control_block;
}

struct ProcessProgress
{
	unsigned start_graphics_index = 0u;
	unsigned start_compute_index = 0u;
	unsigned end_graphics_index = ~0u;
	unsigned end_compute_index = ~0u;
	pid_t pid = -1;
	FILE *crash_file = nullptr;
	int timer_fd = -1;

	int compute_progress = -1;
	int graphics_progress = -1;

	bool process_once();
	bool process_shutdown(int wstatus);
	bool start_child_process();
	void parse(const char *cmd);

	uint32_t index = 0;
};

void ProcessProgress::parse(const char *cmd)
{
	if (strncmp(cmd, "CRASH", 5) == 0)
	{
		// We crashed ... Set up a timeout in case the process hangs while trying to recover.
		if (timer_fd >= 0)
			close(timer_fd);
		timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);

		if (timer_fd >= 0)
		{
			struct itimerspec spec = {};
			spec.it_value.tv_sec = 1;
			if (timerfd_settime(timer_fd, 0, &spec, nullptr) < 0)
				LOGE("Failed to set time with timerfd_settime.\n");

			struct epoll_event event = {};
			event.data.u32 = 0x80000000u | index;
			event.events = EPOLLIN;
			if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, timer_fd, &event))
				LOGE("Failed adding timer_fd to epoll_ctl().\n");
		}
		else
			LOGE("Failed to creater timerfd. Cannot support timeout for process.\n");
	}
	else if (strncmp(cmd, "GRAPHICS_VERR", 13) == 0 || strncmp(cmd, "COMPUTE_VERR", 12) == 0)
	{
		if (Global::control_block)
		{
			// Just forward the message.
			char buffer[ControlBlockMessageSize] = {};
			strcpy(buffer, cmd);

			futex_wrapper_lock(&Global::control_block->futex_lock);
			shared_control_block_write(Global::control_block, buffer, sizeof(buffer));
			futex_wrapper_unlock(&Global::control_block->futex_lock);
		}
	}
	else if (strncmp(cmd, "GRAPHICS", 8) == 0)
	{
		char *end = nullptr;
		graphics_progress = int(strtol(cmd + 8, &end, 0));
		if (end)
		{
			Hash graphics_pipeline = strtoull(end, nullptr, 16);
			// graphics_progress tells us where to start on next iteration, but -1 was actually the pipeline index that crashed.
			if (Global::control_block && graphics_progress > 0 && graphics_pipeline != 0)
			{
				char buffer[ControlBlockMessageSize];
				sprintf(buffer, "GRAPHICS %d %" PRIx64 "\n", graphics_progress - 1, graphics_pipeline);
				futex_wrapper_lock(&Global::control_block->futex_lock);
				shared_control_block_write(Global::control_block, buffer, sizeof(buffer));
				futex_wrapper_unlock(&Global::control_block->futex_lock);
			}
		}
	}
	else if (strncmp(cmd, "COMPUTE", 7) == 0)
	{
		char *end = nullptr;
		compute_progress = int(strtol(cmd + 7, &end, 0));
		if (end)
		{
			Hash compute_pipeline = strtoull(end, nullptr, 16);
			// compute_progress tells us where to start on next iteration, but -1 was actually the pipeline index that crashed.
			if (Global::control_block && compute_progress > 0 && compute_pipeline)
			{
				char buffer[ControlBlockMessageSize];
				sprintf(buffer, "COMPUTE %d %" PRIx64 "\n", compute_progress - 1, compute_pipeline);
				futex_wrapper_lock(&Global::control_block->futex_lock);
				shared_control_block_write(Global::control_block, buffer, sizeof(buffer));
				futex_wrapper_unlock(&Global::control_block->futex_lock);
			}
		}
	}
	else if (strncmp(cmd, "MODULE", 6) == 0)
	{
		auto hash = strtoull(cmd + 6, nullptr, 16);
		Global::faulty_spirv_modules.insert(hash);

		if (Global::control_block)
		{
			Global::control_block->banned_modules.fetch_add(1, std::memory_order_relaxed);
			char buffer[ControlBlockMessageSize] = {};
			strcpy(buffer, cmd);

			futex_wrapper_lock(&Global::control_block->futex_lock);
			shared_control_block_write(Global::control_block, buffer, sizeof(buffer));
			futex_wrapper_unlock(&Global::control_block->futex_lock);
		}
	}
	else
		LOGE("Got unexpected message from child: %s\n", cmd);
}

bool ProcessProgress::process_once()
{
	if (!crash_file)
		return false;

	char buffer[64];
	if (fgets(buffer, sizeof(buffer), crash_file))
	{
		parse(buffer);
		return true;
	}
	else
		return false;
}

bool ProcessProgress::process_shutdown(int wstatus)
{
	// Flush out all messages we got.
	while (process_once());
	if (crash_file)
		fclose(crash_file);
	crash_file = nullptr;

	// Close the timerfd.
	if (timer_fd >= 0)
	{
		close(timer_fd);
		timer_fd = -1;
	}

	// Reap child process.
	Global::active_processes--;
	auto wait_pid = pid;
	pid = -1;

	// If application exited in normal manner, we are done.
	if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
		return false;

	if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL)
	{
		// We had to kill the process early. Log this for debugging.
		LOGE("Process index %u (PID: %d) failed and it had to be killed in timeout with SIGKILL.\n",
		     index, wait_pid);
	}

	// If the child did not exit in a normal manner, we failed to catch any crashing signal.
	// Do not try any further.
	if (!WIFEXITED(wstatus) && WIFSIGNALED(wstatus) && WTERMSIG(wstatus) != SIGKILL)
	{
		LOGE("Process index %u (PID: %d) failed to terminate in a clean fashion. We cannot continue replaying.\n",
		     index, wait_pid);

		if (Global::control_block)
			Global::control_block->dirty_process_deaths.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	// We might have crashed, but we never saw any progress marker.
	// We do not know what to do from here, so we just terminate.
	if (graphics_progress < 0 || compute_progress < 0)
	{
		LOGE("Child process %d terminated before we could receive progress. Cannot continue.\n",
		     wait_pid);
		if (Global::control_block)
			Global::control_block->dirty_process_deaths.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	if (Global::control_block)
		Global::control_block->clean_process_deaths.fetch_add(1, std::memory_order_relaxed);

	start_graphics_index = uint32_t(graphics_progress);
	start_compute_index = uint32_t(compute_progress);
	if (start_graphics_index >= end_graphics_index && start_compute_index >= end_compute_index)
	{
		LOGE("Process index %u (PID: %d) crashed, but there is nothing more to replay.\n", index, wait_pid);
		return false;
	}
	else
	{
		LOGE("Process index %u (PID: %d) crashed, but will retry.\n", index, wait_pid);
		LOGE("  New graphics range (%u, %u)\n", start_graphics_index, end_graphics_index);
		LOGE("  New compute range (%u, %u)\n", start_compute_index, end_compute_index);
		return true;
	}
}

static void send_faulty_modules_and_close(int fd)
{
	for (auto &m : Global::faulty_spirv_modules)
	{
		char buffer[18];
		sprintf(buffer, "%" PRIx64 "\n", m);
		write_all(fd, buffer);
	}

	close(fd);
}

bool ProcessProgress::start_child_process()
{
	graphics_progress = -1;
	compute_progress = -1;

	if (start_graphics_index >= end_graphics_index &&
	    start_compute_index >= end_compute_index)
	{
		// Nothing to do.
		return true;
	}

	int crash_fds[2];
	int input_fds[2];
	if (pipe(crash_fds) < 0)
		return false;
	if (pipe(input_fds) < 0)
		return false;

	pid_t new_pid = fork(); // Fork off a child.
	if (new_pid > 0)
	{
		// We're the parent, keep track of the process in a thread to avoid a lot of complex multiplexing code.
		crash_file = fdopen(crash_fds[0], "r");
		if (!crash_file)
			return false;
		pid = new_pid;

		send_faulty_modules_and_close(input_fds[1]);
		close(crash_fds[1]);
		close(input_fds[0]);
		Global::active_processes++;

		epoll_event event = {};
		event.data.u32 = index;
		event.events = EPOLLIN | EPOLLRDHUP;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, fileno(crash_file), &event) < 0)
		{
			LOGE("Failed to add file to epoll.\n");
			return false;
		}

		return true;
	}
	else if (new_pid == 0)
	{
		// We're the child process.
		// Unblock the signal mask.
		if (pthread_sigmask(SIG_SETMASK, &Global::old_mask, nullptr) < 0)
			return EXIT_FAILURE;

		// Close various FDs we won't use.
		close(Global::signal_fd);
		close(Global::epoll_fd);
		close(crash_fds[0]);
		close(input_fds[1]);

		// Override stdin/stdout.
		if (dup2(crash_fds[1], STDOUT_FILENO) < 0)
			return EXIT_FAILURE;
		if (dup2(input_fds[0], STDIN_FILENO) < 0)
			return EXIT_FAILURE;

		close(crash_fds[1]);
		close(input_fds[0]);

		// Redirect stderr to /dev/null if the child process is supposed to be quiet.
		if (Global::quiet_slave)
		{
			int fd_dev_null = open("/dev/null", O_WRONLY);
			if (fd_dev_null >= 0)
			{
				dup2(fd_dev_null, STDERR_FILENO);
				close(fd_dev_null);
			}
		}

		// Run the slave process.
		auto copy_opts = Global::base_replayer_options;
		copy_opts.start_graphics_index = start_graphics_index;
		copy_opts.end_graphics_index = end_graphics_index;
		copy_opts.start_compute_index = start_compute_index;
		copy_opts.end_compute_index = end_compute_index;
		copy_opts.control_block = Global::control_block;
		if (!copy_opts.on_disk_pipeline_cache_path.empty() && index != 0)
		{
			copy_opts.on_disk_pipeline_cache_path += ".";
			copy_opts.on_disk_pipeline_cache_path += std::to_string(index);
		}

		if (!copy_opts.on_disk_validation_cache_path.empty() && index != 0)
		{
			copy_opts.on_disk_validation_cache_path += ".";
			copy_opts.on_disk_validation_cache_path += std::to_string(index);
		}

		if (!copy_opts.pipeline_stats_path.empty() && index != 0)
		{
			copy_opts.pipeline_stats_path += ".";
			copy_opts.pipeline_stats_path += std::to_string(index);
		}

		exit(run_slave_process(Global::device_options, copy_opts, Global::databases));
	}
	else
		return false;
}

static int run_master_process(const VulkanDevice::Options &opts,
                              const ThreadedReplayer::Options &replayer_opts,
                              const vector<const char *> &databases,
                              bool quiet_slave, int shmem_fd)
{
	Global::quiet_slave = quiet_slave;
	Global::device_options = opts;
	Global::base_replayer_options = replayer_opts;
	Global::databases = databases;
	unsigned processes = replayer_opts.num_threads;

	// Split shader cache overhead across all processes.
	Global::base_replayer_options.shader_cache_size_mb /= max(Global::base_replayer_options.num_threads, 1u);
	Global::base_replayer_options.num_threads = 1;

	// Try to map the shared control block.
	if (shmem_fd >= 0)
	{
		LOGI("Attempting to map shmem block.\n");
		struct stat s = {};
		if (fstat(shmem_fd, &s) >= 0)
		{
			void *mapped = mmap(nullptr, s.st_size, PROT_READ | PROT_WRITE,
			                    MAP_SHARED, shmem_fd, 0);

			if (mapped != MAP_FAILED)
			{
				const auto is_pot = [](size_t size) { return (size & (size - 1)) == 0; };
				// Detect some obvious shenanigans.
				Global::control_block = static_cast<SharedControlBlock *>(mapped);
				if (Global::control_block->version_cookie != ControlBlockMagic ||
				    Global::control_block->ring_buffer_offset < sizeof(SharedControlBlock) ||
				    Global::control_block->ring_buffer_size == 0 ||
				    !is_pot(Global::control_block->ring_buffer_size) ||
				    Global::control_block->ring_buffer_offset + Global::control_block->ring_buffer_size > size_t(s.st_size))
				{
					LOGE("Control block is corrupt.\n");
					munmap(mapped, s.st_size);
					Global::control_block = nullptr;
				}
			}
		}

		close(shmem_fd);
	}

	size_t num_graphics_pipelines;
	size_t num_compute_pipelines;
	{
		auto db = create_database(databases);
		if (!db->prepare())
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}

		if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &num_graphics_pipelines, nullptr))
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}

		if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &num_compute_pipelines, nullptr))
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}
	}

	if (Global::control_block)
		Global::control_block->progress_started.store(1, std::memory_order_release);

	Global::active_processes = 0;

	// We will wait for child processes explicitly with signalfd.
	// Block delivery of signals in the normal way.
	// For this to work, there cannot be any other threads in the process
	// which may capture SIGCHLD anyways.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	if (pthread_sigmask(SIG_BLOCK, &mask, &Global::old_mask) < 0)
	{
		LOGE("Failed to block signal mask.\n");
		return EXIT_FAILURE;
	}

	// signalfd allows us to poll for signals rather than rely on
	// painful async signal handling.
	// epoll_pwait might work, but I'd rather not debug it.
	Global::signal_fd = signalfd(-1, &mask, 0);
	if (Global::signal_fd < 0)
	{
		LOGE("Failed to create signalfd. Too old Linux kernel?\n");
		return EXIT_FAILURE;
	}

	vector<ProcessProgress> child_processes(processes);

	// Create an epoll instance and add the signal fd to it.
	// The signalfd will signal when SIGCHLD is pending.
	Global::epoll_fd = epoll_create(2 * int(processes) + 1);
	if (Global::epoll_fd < 0)
	{
		LOGE("Failed to create epollfd. Too old Linux kernel?\n");
		return EXIT_FAILURE;
	}

	{
		epoll_event event = {};
		event.events = EPOLLIN;
		event.data.u32 = UINT32_MAX;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, Global::signal_fd, &event) < 0)
		{
			LOGE("Failed to add signalfd to epoll.\n");
			return EXIT_FAILURE;
		}
	}

	unsigned requested_graphic_pipelines = replayer_opts.end_graphics_index - replayer_opts.start_graphics_index;
	unsigned graphics_pipeline_offset = 0;
	unsigned requested_compute_pipelines = replayer_opts.end_compute_index - replayer_opts.start_compute_index;
	unsigned compute_pipeline_offset = 0;

	if (requested_graphic_pipelines < num_graphics_pipelines)
	{
		num_graphics_pipelines = requested_graphic_pipelines;
		graphics_pipeline_offset = replayer_opts.start_graphics_index;
	}

	if (requested_compute_pipelines < num_compute_pipelines)
	{
		num_compute_pipelines = requested_compute_pipelines;
		compute_pipeline_offset = replayer_opts.start_compute_index;
	}

	// fork() and pipe() strategy.
	for (unsigned i = 0; i < processes; i++)
	{
		auto &progress = child_processes[i];
		progress.start_graphics_index = graphics_pipeline_offset + (i * unsigned(num_graphics_pipelines)) / processes;
		progress.end_graphics_index = graphics_pipeline_offset + ((i + 1) * unsigned(num_graphics_pipelines)) / processes;
		progress.start_compute_index = compute_pipeline_offset + (i * unsigned(num_compute_pipelines)) / processes;
		progress.end_compute_index = compute_pipeline_offset + ((i + 1) * unsigned(num_compute_pipelines)) / processes;
		progress.index = i;
		if (!progress.start_child_process())
		{
			LOGE("Failed to start child process.\n");
			return EXIT_FAILURE;
		}
	}

	while (Global::active_processes != 0)
	{
		epoll_event events[64];
		int ret = epoll_wait(Global::epoll_fd, events, 64, -1);
		if (ret < 0)
		{
			LOGE("epoll_wait() failed.\n");
			return EXIT_FAILURE;
		}

		// Check for three cases in the epoll.
		// - Child process wrote something to stdout, we need to parse it.
		// - SIGCHLD happened, we need to reap child processes.
		// - TimerFD fired, we reached a timeout and we should SIGKILL the child process
		//   responsible.
		for (int i = 0; i < ret; i++)
		{
			auto &e = events[i];
			if (e.events & (EPOLLIN | EPOLLRDHUP))
			{
				if (e.data.u32 != UINT32_MAX)
				{
					auto &proc = child_processes[e.data.u32 & 0x7fffffffu];

					if (e.data.u32 & 0x80000000u)
					{
						// Timeout triggered. kill the process and reap it.
						// SIGCHLD handler should rearm the process as necessary.
						if (proc.timer_fd >= 0)
						{
							kill(proc.pid, SIGKILL);
							close(proc.timer_fd);
							proc.timer_fd = -1;
						}
					}
					else if (proc.crash_file)
					{
						if (!proc.process_once())
						{
							fclose(proc.crash_file);
							proc.crash_file = nullptr;
						}
					}
				}
				else
				{
					// Read from signalfd to clear the pending flag.
					signalfd_siginfo info = {};
					if (read(Global::signal_fd, &info, sizeof(info)) <= 0)
					{
						LOGE("Reading from signalfd failed.\n");
						return EXIT_FAILURE;
					}

					if (info.ssi_signo == SIGCHLD)
					{
						// We'll only receive one SIGCHLD signal, even if multiple processes
						// completed at the same time.
						// Use the typical waitpid loop to reap every process.
						pid_t pid = 0;
						int wstatus = 0;

						while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0)
						{
							auto itr = find_if(begin(child_processes), end(child_processes),
							                   [&](const ProcessProgress &progress)
							                   {
								                   return progress.pid == pid;
							                   });

							if (itr != end(child_processes))
							{
								if (itr->process_shutdown(wstatus) && !itr->start_child_process())
								{
									LOGE("Failed to start child process.\n");
									return EXIT_FAILURE;
								}
							}
							else
								LOGE("Got SIGCHLD from unknown process PID %d.\n", pid);
						}
					}
				}
			}
			else if (e.events & EPOLLERR)
			{
				if (e.data.u32 < 0x80000000u)
				{
					auto &proc = child_processes[e.data.u32 & 0x7fffffffu];
					if (proc.crash_file)
					{
						fclose(proc.crash_file);
						proc.crash_file = nullptr;
					}
				}
			}
		}
	}

	if (Global::control_block)
		Global::control_block->progress_complete.store(1, std::memory_order_release);

	return EXIT_SUCCESS;
}

static ThreadedReplayer *global_replayer = nullptr;
static int crash_fd;
static stack_t alt_stack;

static void validation_error_cb(ThreadedReplayer *replayer)
{
	auto &per_thread = replayer->get_per_thread_data();
	char buffer[64];

	if (per_thread.current_graphics_pipeline)
	{
		sprintf(buffer, "GRAPHICS_VERR %" PRIx64 "\n", per_thread.current_graphics_pipeline);
		write_all(crash_fd, buffer);
	}

	if (per_thread.current_compute_pipeline)
	{
		sprintf(buffer, "COMPUTE_VERR %" PRIx64 "\n", per_thread.current_compute_pipeline);
		write_all(crash_fd, buffer);
	}
}

static void crash_handler(ThreadedReplayer &replayer, ThreadedReplayer::PerThreadData &per_thread)
{
	char buffer[64];

	// Report to parent process which VkShaderModule's might have contributed to our untimely death.
	// This allows a new process to ignore these modules.
	for (unsigned i = 0; i < per_thread.num_failed_module_hashes; i++)
	{
		sprintf(buffer, "MODULE %" PRIx64 "\n", per_thread.failed_module_hashes[i]);
		if (!write_all(crash_fd, buffer))
			_exit(2);
	}

	// Report where we stopped, so we can continue.
	sprintf(buffer, "GRAPHICS %d %" PRIx64 "\n", per_thread.current_graphics_index, per_thread.current_graphics_pipeline);
	if (!write_all(crash_fd, buffer))
		_exit(2);

	sprintf(buffer, "COMPUTE %d %" PRIx64 "\n", per_thread.current_compute_index, per_thread.current_compute_pipeline);
	if (!write_all(crash_fd, buffer))
		_exit(2);

	replayer.emergency_teardown();
}

static void crash_handler(int)
{
	// stderr is reserved for generic logging.
	// stdout/stdin is for IPC with master process.

	if (!write_all(crash_fd, "CRASH\n"))
		_exit(2);

	// This might hang indefinitely if we are exceptionally unlucky,
	// the parent will have a timeout after receiving the crash message.
	// If that fails, it can SIGKILL us.
	// We want to make sure any database writing threads in the driver gets a chance to complete its work
	// before we die.

	if (global_replayer)
	{
		auto &per_thread = global_replayer->get_per_thread_data();
		crash_handler(*global_replayer, per_thread);
	}

	// Clean exit instead of reporting the segfault.
	// _exit is async-signal safe, but not exit().
	// Use exit code 2 to mark a segfaulted child.
	_exit(2);
}

static void timeout_handler()
{
	if (!global_replayer || !global_replayer->robustness)
	{
		LOGE("Pipeline compilation timed out.\n");
		_exit(2);
	}

	// Pretend we crashed in a safe way.
	// Send a signal to the worker thread to make sure we tear down on that thread.
	pthread_kill(global_replayer->thread_pool.front().native_handle(), SIGABRT);
}

static void thread_callback(void *)
{
	// Alternate signal stacks set by sigaltstack are per-thread.
	// Need to install the alternate stack on all threads.
	if (sigaltstack(&alt_stack, nullptr) < 0)
		LOGE("Failed to install alternate stack.\n");

	// Don't block any signals in the worker threads.
	sigset_t mask;
	sigemptyset(&mask);
	if (pthread_sigmask(SIG_SETMASK, &mask, nullptr) < 0)
		LOGE("Failed to set signal mask.\n");
}

static int run_slave_process(const VulkanDevice::Options &opts,
                             const ThreadedReplayer::Options &replayer_opts,
                             const vector<const char *> &databases)
{
	// Just in case the driver crashed due to stack overflow,
	// provide an alternate stack where we can clean up "safely".
	alt_stack = {};
	alt_stack.ss_sp = malloc(1024 * 1024);
	alt_stack.ss_size = 1024 * 1024;
	alt_stack.ss_flags = 0;
	if (sigaltstack(&alt_stack, nullptr) < 0)
		return EXIT_FAILURE;

	auto tmp_opts = replayer_opts;
	tmp_opts.on_thread_callback = thread_callback;
	tmp_opts.on_validation_error_callback = validation_error_cb;
	ThreadedReplayer replayer(opts, tmp_opts);
	replayer.robustness = true;

	// In slave mode, we can receive a list of shader module hashes we should ignore.
	// This is to avoid trying to replay the same faulty shader modules again and again.
	char ignored_shader_module_hash[16 + 2];
	while (fgets(ignored_shader_module_hash, sizeof(ignored_shader_module_hash), stdin))
	{
		errno = 0;
		auto hash = strtoull(ignored_shader_module_hash, nullptr, 16);
		if (hash == 0)
			break;
		if (errno == 0)
		{
			//LOGE("Ignoring module %llx\n", hash);
			replayer.mask_shader_module(Hash(hash));
		}
	}

	// Make sure that the driver cannot mess up the master process by writing random data to stdout.
	crash_fd = dup(STDOUT_FILENO);
	close(STDOUT_FILENO);

	global_replayer = &replayer;

	// Install the signal handlers.
	// It's very important that this runs in a single thread,
	// so we cannot have some rogue thread overriding these handlers.
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);
	act.sa_handler = crash_handler;
	act.sa_flags = SA_RESETHAND | SA_ONSTACK;

	if (sigaction(SIGSEGV, &act, nullptr) < 0)
		return EXIT_FAILURE;
	if (sigaction(SIGFPE, &act, nullptr) < 0)
		return EXIT_FAILURE;
	if (sigaction(SIGILL, &act, nullptr) < 0)
		return EXIT_FAILURE;
	if (sigaction(SIGBUS, &act, nullptr) < 0)
		return EXIT_FAILURE;
	if (sigaction(SIGABRT, &act, nullptr) < 0)
		return EXIT_FAILURE;

	// Don't allow the main thread to handle abort signals.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGABRT);
	sigset_t old_mask;
	if (pthread_sigmask(SIG_BLOCK, &mask, &old_mask) < 0)
		return EXIT_FAILURE;

	int ret = run_normal_process(replayer, databases);
	global_replayer = nullptr;

	// Cannot reliably handle these signals if they occur during teardown of the process.
	signal(SIGSEGV, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGILL, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
	pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);

	free(alt_stack.ss_sp);

#if 0
	if (Global::control_block)
	{
		futex_wrapper_lock(&Global::control_block->futex_lock);
		char msg[ControlBlockMessageSize] = {};
		sprintf(msg, "SLAVE_FINISHED\n");
		shared_control_block_write(Global::control_block, msg, sizeof(msg));
		futex_wrapper_unlock(&Global::control_block->futex_lock);
	}
#endif

	return ret;
}

static void log_process_memory()
{
	char path[1024];
	sprintf(path, "/proc/%d/status", getpid());
	FILE *file = fopen(path, "r");
	if (!file)
	{
		LOGE("Failed to log process memory.\n");
		return;
	}

	char line_buffer[1024];
	while (fgets(line_buffer, sizeof(line_buffer), file))
		fprintf(stderr, "%s", line_buffer);

	fclose(file);
}
