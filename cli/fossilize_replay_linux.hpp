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

#pragma once

#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <errno.h>

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
                             const string &db_path);

namespace Global
{
static unordered_set<Hash> faulty_spirv_modules;
static unsigned active_processes;
static ThreadedReplayer::Options base_replayer_options;
static string db_path;
static sigset_t old_mask;
static int signal_fd;
static int epoll_fd;
static VulkanDevice::Options device_options;
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

	void process_once();
	bool process_shutdown();
	bool start_child_process();
	void parse(const char *cmd);

	uint32_t index = 0;
};

void ProcessProgress::parse(const char *cmd)
{
	if (strncmp(cmd, "CRASH", 5) == 0)
	{
		// We crashed ... Set up a timeout in case the process hangs while trying to recover.
		if (timer_fd < 0)
			timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);

		if (timer_fd >= 0)
		{
			struct itimerspec spec = {};
			spec.it_value.tv_sec = 1;
			timerfd_settime(timer_fd, 0, &spec, nullptr);

			struct epoll_event event = {};
			event.data.u32 = 0x80000000u | index;
			event.events = EPOLLIN;
			epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, timer_fd, &event);
		}
		else
			LOGE("Failed to creater timerfd. Cannot support timeout for process.\n");
	}
	else if (strncmp(cmd, "GRAPHICS", 8) == 0)
		graphics_progress = int(strtol(cmd + 8, nullptr, 0));
	else if (strncmp(cmd, "COMPUTE", 7) == 0)
		compute_progress = int(strtol(cmd + 7, nullptr, 0));
	else if (strncmp(cmd, "MODULE", 6) == 0)
	{
		auto hash = strtoull(cmd + 6, nullptr, 16);
		Global::faulty_spirv_modules.insert(hash);
	}
}

void ProcessProgress::process_once()
{
	if (!crash_file)
		return;

	char buffer[64];
	if (fgets(buffer, sizeof(buffer), crash_file))
	{
		LOGI("Got message from child: %s\n", buffer);
		parse(buffer);
	}
}

bool ProcessProgress::process_shutdown()
{
	// Flush out all messages we got.
	if (!crash_file)
		return false;

	char buffer[64];
	while (fgets(buffer, sizeof(buffer), crash_file))
	{
		LOGI("Got message from child: %s\n", buffer);
		parse(buffer);
	}

	// Close file handles associated with the process.
	fclose(crash_file);
	crash_file = nullptr;

	if (timer_fd >= 0)
	{
		close(timer_fd);
		timer_fd = -1;
	}

	// Reap child process.
	int wstatus = 0;
	Global::active_processes--;
	pid_t wait_pid = pid;
	pid = -1;
	if (waitpid(wait_pid, &wstatus, 0) < 0)
	{
		LOGE("Failed to wait for process index %u (PID: %d).\n",
		     index, wait_pid);
		return false;
	}

	// If the child did not exit in a normal manner, we failed to catch any crashing signal.
	// Do not try any further.
	if (!WIFEXITED(wstatus))
	{
		LOGE("Process index %u (PID: %d) failed to terminate in a clean fashion. We cannot continue replaying.\n",
		     index, wait_pid);
		return false;
	}

	// If application exited in normal manner, we are done.
	if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
		return false;

	if (WIFEXITED(wstatus) && WTERMSIG(wstatus) == SIGKILL)
	{
		// We had to kill the process early. Log this for debugging.
		LOGE("Process index %u (PID: %d) failed and it had to be killed in timeout with SIGKILL.\n",
		     index, wait_pid);
	}

	// We might have crashed, but we never saw any progress marker.
	// We do not know what to do from here, so we just terminate.
	if (graphics_progress < 0 || compute_progress < 0)
	{
		LOGE("Child process %d terminated before we could receive progress. Cannot continue.\n",
		     wait_pid);
		return false;
	}

	start_graphics_index = uint32_t(graphics_progress);
	start_compute_index = uint32_t(compute_progress);
	if (start_graphics_index >= end_graphics_index && start_compute_index >= end_compute_index)
		return false;
	else
	{
		LOGE("Process index %u (PID: %d) crashed, but will retry.\n", index, wait_pid);
		return true;
	}
}

static void send_faulty_modules_and_close(int fd)
{
	for (auto &m : Global::faulty_spirv_modules)
	{
		char buffer[18];
		sprintf(buffer, "%llx\n", static_cast<unsigned long long>(m));
		write_all(fd, buffer);
	}

	close(fd);
}

bool ProcessProgress::start_child_process()
{
	graphics_progress = -1;
	compute_progress = -1;

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
		return true;
	}
	else if (new_pid == 0)
	{
		// We're the child process.
		if (pthread_sigmask(SIG_SETMASK, &Global::old_mask, nullptr) < 0)
			return EXIT_FAILURE;
		close(Global::signal_fd);
		close(crash_fds[0]);
		close(input_fds[1]);

		if (dup2(crash_fds[1], STDOUT_FILENO) < 0)
			return EXIT_FAILURE;
		if (dup2(input_fds[0], STDIN_FILENO) < 0)
			return EXIT_FAILURE;

		auto copy_opts = Global::base_replayer_options;
		copy_opts.start_graphics_index = start_graphics_index;
		copy_opts.end_graphics_index = end_graphics_index;
		copy_opts.start_compute_index = start_compute_index;
		copy_opts.end_compute_index = end_compute_index;
		exit(run_slave_process(Global::device_options, copy_opts, Global::db_path));
	}
	else
		return false;
}

static int run_master_process(const VulkanDevice::Options &opts,
                              const ThreadedReplayer::Options &replayer_opts,
                              const string &db_path)
{
	Global::device_options = opts;
	Global::base_replayer_options = replayer_opts;
	Global::db_path = db_path;
	unsigned processes = replayer_opts.num_threads;
	Global::base_replayer_options.num_threads = 1;

	size_t num_graphics_pipelines;
	size_t num_compute_pipelines;
	{
		auto db = unique_ptr<DatabaseInterface>(create_database(db_path.c_str(), DatabaseMode::ReadOnly));
		if (!db->prepare())
		{
			LOGE("Failed to parse database %s.\n", db_path.c_str());
			return EXIT_FAILURE;
		}

		if (!db->get_hash_list_for_resource_tag(RESOURCE_GRAPHICS_PIPELINE, &num_graphics_pipelines, nullptr))
		{
			LOGE("Failed to parse database %s.\n", db_path.c_str());
			return EXIT_FAILURE;
		}

		if (!db->get_hash_list_for_resource_tag(RESOURCE_COMPUTE_PIPELINE, &num_compute_pipelines, nullptr))
		{
			LOGE("Failed to parse database %s.\n", db_path.c_str());
			return EXIT_FAILURE;
		}
	}

	Global::active_processes = 0;

	// We will wait for child processes explicitly with signalfd.
	// Block delivery of signals in the normal way.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	if (pthread_sigmask(SIG_BLOCK, &mask, &Global::old_mask) < 0)
	{
		LOGE("Failed to block signal mask.\n");
		return EXIT_FAILURE;
	}

	Global::signal_fd = signalfd(-1, &mask, 0);
	if (Global::signal_fd < 0)
	{
		LOGE("Failed to create signalfd. Too old Linux kernel?\n");
		return EXIT_FAILURE;
	}

	vector<ProcessProgress> child_processes(processes);

	// fork() and pipe() strategy.
	for (unsigned i = 0; i < processes; i++)
	{
		auto &progress = child_processes[i];
		progress.start_graphics_index = (i * unsigned(num_graphics_pipelines)) / processes;
		progress.end_graphics_index = ((i + 1) * unsigned(num_graphics_pipelines)) / processes;
		progress.start_compute_index = (i * unsigned(num_compute_pipelines)) / processes;
		progress.end_compute_index = ((i + 1) * unsigned(num_compute_pipelines)) / processes;
		progress.index = i;
		progress.start_child_process();
	}

	Global::epoll_fd = epoll_create(int(processes) + 1);
	if (Global::epoll_fd < 0)
	{
		LOGE("Failed to create epollfd. Too old Linux kernel?\n");
		return EXIT_FAILURE;
	}

	for (auto &child : child_processes)
	{
		epoll_event event = {};
		event.data.u32 = child.index;
		event.events = EPOLLIN;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, fileno(child.crash_file), &event) < 0)
		{
			LOGE("Failed to add file to epoll.\n");
			return EXIT_FAILURE;
		}
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

	while (Global::active_processes != 0)
	{
		epoll_event events[64];
		int ret = epoll_wait(Global::epoll_fd, events, 64, -1);
		if (ret < 0)
		{
			LOGE("epoll_wait() failed.\n");
			return EXIT_FAILURE;
		}

		for (int i = 0; i < ret; i++)
		{
			auto &e = events[i];
			if (e.events & EPOLLIN)
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
					else
						proc.process_once();
				}
				else
				{
					signalfd_siginfo info = {};
					if (read(Global::signal_fd, &info, sizeof(info)) < ssize_t(sizeof(info)))
					{
						LOGE("Reading from signalfd failed.\n");
						return EXIT_FAILURE;
					}
					pid_t pid = info.ssi_pid;

					auto itr = find_if(begin(child_processes), end(child_processes), [&](const ProcessProgress &progress) {
						return progress.pid == pid;
					});

					if (itr != end(child_processes))
					{
						if (itr->process_shutdown() && !itr->start_child_process())
						{
							LOGE("Failed to start child process.\n");
							return EXIT_FAILURE;
						}
					}
				}
			}
			else if (e.events & (EPOLLHUP | EPOLLERR))
			{
				// Child process has probably shut down.
				// This is okay, we'll see a SIGCHLD eventually and flush the crash report file.
				// For now, just remove the fd from epoll.
				if (e.data.u32 < 0x80000000u)
				{
					auto &proc = child_processes[e.data.u32 & 0x7fffffffu];
					epoll_ctl(Global::epoll_fd, EPOLL_CTL_DEL, fileno(proc.crash_file), nullptr);
				}
			}
		}
	}

	return EXIT_SUCCESS;
}

static ThreadedReplayer *global_replayer = nullptr;
static int crash_fd;

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
		char buffer[64];

		// Report to parent process which VkShaderModule's might have contributed to our untimely death.
		// This allows a new process to ignore these modules.
		for (unsigned i = 0; i < global_replayer->num_failed_module_hashes; i++)
		{
			sprintf(buffer, "MODULE %llx\n",
					static_cast<unsigned long long>(global_replayer->failed_module_hashes[i]));
			if (!write_all(crash_fd, buffer))
				_exit(2);
		}

		// Report where we stopped, so we can continue.
		sprintf(buffer, "GRAPHICS %d\n", global_replayer->thread_current_graphics_index);
		if (!write_all(crash_fd, buffer))
			_exit(2);

		sprintf(buffer, "COMPUTE %d\n", global_replayer->thread_current_compute_index);
		if (!write_all(crash_fd, buffer))
			_exit(2);

		global_replayer->emergency_teardown();
	}

	// Clean exit instead of reporting the segfault.
	// _exit is async-signal safe, but not exit().
	// Use exit code 2 to mark a segfaulted child.
	_exit(2);
}

static int run_slave_process(const VulkanDevice::Options &opts,
                             const ThreadedReplayer::Options &replayer_opts,
                             const string &db_path)
{
	ThreadedReplayer replayer(opts, replayer_opts);
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
			LOGE("Ignoring module %llx\n", hash);
			replayer.mask_shader_module(Hash(hash));
		}
	}

	// Make sure that the driver cannot mess up the master process.
	// Just stdout.
	crash_fd = dup(STDOUT_FILENO);
	close(STDOUT_FILENO);

	global_replayer = &replayer;

	// Just in case the driver crashed due to stack overflow,
	// provide an alternate stack where we can clean up "safely".
	stack_t ss;
	ss.ss_sp = malloc(1024 * 1024);
	ss.ss_size = 1024 * 1024;
	ss.ss_flags = 0;
	if (sigaltstack(&ss, nullptr) < 0)
		return EXIT_FAILURE;

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

	return run_normal_process(replayer, db_path);
}
