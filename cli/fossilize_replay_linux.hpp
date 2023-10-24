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
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include "fossilize_external_replayer.hpp"
#include "path.hpp"
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
static unsigned running_processes;
static unsigned target_running_processes;

static int target_running_processes_static = -1;
static bool target_running_processes_io_stall = true;
static bool target_running_processes_dirty_pages = true;

static ThreadedReplayer::Options base_replayer_options;
static vector<const char *> databases;
static sigset_t old_mask;
static int signal_fd;
static int timer_fd;
static int epoll_fd;
static VulkanDevice::Options device_options;
static bool quiet_slave;
static bool control_fd_is_sentinel;
static int control_fd = -1;

static SharedControlBlock *control_block;
static int metadata_fd = -1;
static int heartbeats = 1;
}

static void remove_epoll_entry(int fd)
{
	if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_DEL, fd, nullptr) < 0)
		LOGE("epoll_ctl() DEL failed.\n");
}

static void close_and_remove_epoll_entry(int &fd)
{
	if (fd >= 0)
	{
		remove_epoll_entry(fd);
		close(fd);
	}
	fd = -1;
}

struct ProcessProgress
{
	unsigned start_graphics_index = 0u;
	unsigned start_compute_index = 0u;
	unsigned start_raytracing_index = 0u;
	unsigned end_graphics_index = ~0u;
	unsigned end_compute_index = ~0u;
	unsigned end_raytracing_index = ~0u;
	int heartbeats = -1;
	pid_t pid = -1;
	int crash_fd = -1;
	int timer_fd = -1;
	int watchdog_timer_fd = -1;

	int compute_progress = -1;
	int graphics_progress = -1;
	int raytracing_progress = -1;

	bool process_once();
	bool process_shutdown(int wstatus);
	bool start_child_process(vector<ProcessProgress> &siblings);
	void parse_raw(const char *str);
	void parse(const char *cmd);

	void begin_heartbeat();
	void heartbeat();

	uint32_t index = 0;
	bool stopped = false;
	bool expect_kill = false;

	char module_uuid_path[2 * VK_UUID_SIZE + 1] = {};
	std::string parse_buffer;
};

void ProcessProgress::parse_raw(const char *str)
{
	parse_buffer += str;
	size_t n;

	while ((n = parse_buffer.find_first_of('\n')) != std::string::npos)
	{
		auto cmd = parse_buffer.substr(0, n);
		parse(cmd.c_str());
		parse_buffer = parse_buffer.substr(n + 1);
	}
}

void ProcessProgress::parse(const char *cmd)
{
	if (strncmp(cmd, "CRASH", 5) == 0)
	{
		// We crashed ... Set up a timeout in case the process hangs while trying to recover.
		close_and_remove_epoll_entry(timer_fd);
		timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

		if (timer_fd >= 0)
		{
			struct itimerspec spec = {};
			spec.it_value.tv_sec = 3;
			if (timerfd_settime(timer_fd, 0, &spec, nullptr) < 0)
				LOGE("Failed to set time with timerfd_settime.\n");

			struct epoll_event event = {};
			event.data.u32 = 0x80000000u | index;
			event.events = EPOLLIN;
			if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, timer_fd, &event))
				LOGE("Failed adding timer_fd to epoll_ctl().\n");
		}
		else
			LOGE("Failed to create timerfd. Cannot support timeout for process.\n");
	}
	else if (strncmp(cmd, "GRAPHICS_VERR", 13) == 0 ||
	         strncmp(cmd, "RAYTRACE_VERR", 13) == 0 ||
	         strncmp(cmd, "COMPUTE_VERR", 12) == 0)
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
	else if (strncmp(cmd, "RAYTRACE", 8) == 0)
	{
		char *end = nullptr;
		raytracing_progress = int(strtol(cmd + 8, &end, 0));
		if (end)
		{
			Hash raytracing_pipeline = strtoull(end, nullptr, 16);
			// raytracing_progress tells us where to start on next iteration, but -1 was actually the pipeline index that crashed.
			if (Global::control_block && raytracing_progress > 0 && raytracing_pipeline != 0)
			{
				char buffer[ControlBlockMessageSize];
				sprintf(buffer, "RAYTRACE %d %" PRIx64 "\n", raytracing_progress - 1, raytracing_pipeline);
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
	else if (strncmp(cmd, "MODULE_UUID", 11) == 0)
	{
		memcpy(module_uuid_path, cmd + 12, VK_UUID_SIZE * 2);
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
	else if (strncmp(cmd, "BEGIN_HEARTBEAT", 15) == 0)
	{
		begin_heartbeat();
	}
	else if (strncmp(cmd, "HEARTBEAT", 9) == 0)
	{
		heartbeat();
	}
	else
		LOGE("Got unexpected message from child: %s\n", cmd);
}

bool ProcessProgress::process_once()
{
	if (crash_fd < 0)
		return false;

	// Important to use raw FD IO here since we are not guaranteed
	// to be able to read a newline here.
	// This can happen if the child process started writing to the file,
	// but received a SIGSTOP in the middle of writing for whatever reason.
	char buffer[65];
	ssize_t ret = ::read(crash_fd, buffer, sizeof(buffer) - 1);
	if (ret <= 0)
		return false;

	buffer[ret] = '\0';
	parse_raw(buffer);
	return true;
}

void ProcessProgress::begin_heartbeat()
{
	close_and_remove_epoll_entry(watchdog_timer_fd);
	watchdog_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

	if (watchdog_timer_fd >= 0)
	{
		struct itimerspec spec = {};
		spec.it_value.tv_sec = 10;
		if (timerfd_settime(watchdog_timer_fd, 0, &spec, nullptr) < 0)
			LOGE("Failed to set time with timerfd_settime.\n");

		struct epoll_event event = {};
		event.data.u32 = 0x40000000u | index;
		event.events = EPOLLIN;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, watchdog_timer_fd, &event))
			LOGE("Failed adding timer_fd to epoll_ctl().\n");
	}
	else
		LOGE("Failed to create timerfd. Cannot support timeout for process.\n");
}

void ProcessProgress::heartbeat()
{
	if (watchdog_timer_fd >= 0)
	{
		heartbeats++;
		// Rearm timer
		struct itimerspec spec = {};
		spec.it_value.tv_sec = 10;
		if (timerfd_settime(watchdog_timer_fd, 0, &spec, nullptr) < 0)
			LOGE("Failed to set time with timerfd_settime.\n");
	}
}

bool ProcessProgress::process_shutdown(int wstatus)
{
	// Flush out all messages we got.
	while (process_once());
	parse_buffer.clear();
	heartbeats = -1;

	close_and_remove_epoll_entry(crash_fd);
	// Close the timerfd.
	close_and_remove_epoll_entry(timer_fd);
	close_and_remove_epoll_entry(watchdog_timer_fd);

	// Reap child process.
	Global::active_processes--;
	auto wait_pid = pid;
	pid = -1;

	// If application exited in normal manner, we are done.
	if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0)
		return false;

	if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL && expect_kill)
	{
		LOGI("Parent process is shutting down. Expected SIGKILL.\n");
		return false;
	}

	if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL)
	{
		// We had to kill the process early. Log this for debugging.
		LOGE("Process index %u (PID: %d) failed and it had to be killed in timeout with SIGKILL.\n",
		     index, wait_pid);
	}

	// If the child did not exit in a normal manner, we failed to catch any crashing signal.
	// Do not try any further.
	if (!WIFEXITED(wstatus) && WIFSIGNALED(wstatus) &&
	    (WTERMSIG(wstatus) != SIGKILL && WTERMSIG(wstatus) != SIGSEGV))
	{
		LOGE("Process index %u (PID: %d) failed to terminate in a clean fashion (signal %d). We cannot continue replaying.\n",
		     index, wait_pid, WTERMSIG(wstatus));

		if (Global::control_block)
			Global::control_block->dirty_process_deaths.fetch_add(1, std::memory_order_relaxed);
		return false;
	}

	// We might have crashed, but we never saw any progress marker.
	// We do not know what to do from here, so we just terminate.
	if (graphics_progress < 0 || compute_progress < 0 || raytracing_progress < 0)
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
	start_raytracing_index = uint32_t(raytracing_progress);
	if (start_graphics_index >= end_graphics_index &&
	    start_compute_index >= end_compute_index &&
	    start_raytracing_index >= end_raytracing_index)
	{
		LOGE("Process index %u (PID: %d) crashed, but there is nothing more to replay.\n", index, wait_pid);
		return false;
	}
	else
	{
		LOGE("Process index %u (PID: %d) crashed, but will retry.\n", index, wait_pid);
		LOGE("  New graphics range (%u, %u)\n", start_graphics_index, end_graphics_index);
		LOGE("  New compute range (%u, %u)\n", start_compute_index, end_compute_index);
		LOGE("  New raytracing range (%u, %u)\n", start_raytracing_index, end_raytracing_index);
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

static bool poll_process_memory_usage(pid_t pid, ExternalReplayer::ProcessStats &stats)
{
	long long values[3];
	char path[1024];
	snprintf(path, sizeof(path), "/proc/%d/statm", pid);
	FILE *file = fopen(path, "r");
	if (!file)
		return false;

	if (fscanf(file, "%lld %lld %lld",
	           &values[0], &values[1], &values[2]) != 3)
	{
		fclose(file);
		return false;
	}
	fclose(file);

	int64_t resident = (values[1] * getpagesize()) / (1024 * 1024);
	int64_t shared = (values[2] * getpagesize()) / (1024 * 1024);
	if (resident > UINT32_MAX)
		resident = UINT32_MAX;
	if (shared > UINT32_MAX)
		shared = UINT32_MAX;

	stats.resident_mib = resident;
	stats.shared_mib = shared;
	return true;
}

static void poll_self_child_memory_usage(const std::vector<ProcessProgress> &processes)
{
	uint32_t num_processes = processes.size() + 1;
	if (num_processes > MaxProcessStats)
		num_processes = MaxProcessStats;

	ExternalReplayer::ProcessStats stats = {};
	if (!poll_process_memory_usage(getpid(), stats))
		stats = {};

	if (Global::control_block)
	{
		Global::control_block->process_reserved_memory_mib[0].store(stats.resident_mib, std::memory_order_relaxed);
		Global::control_block->process_shared_memory_mib[0].store(stats.shared_mib, std::memory_order_relaxed);
		Global::control_block->process_heartbeats[0].store(Global::heartbeats, std::memory_order_relaxed);
	}
	else
	{
		LOGI("Master process: %5u MiB resident, %5u MiB shared.\n", stats.resident_mib, stats.shared_mib);
	}

	for (uint32_t i = 1; i < num_processes; i++)
	{
		if (processes[i - 1].pid < 0)
			stats = {};
		else if (!poll_process_memory_usage(processes[i - 1].pid, stats))
			stats = {};

		if (Global::control_block)
		{
			Global::control_block->process_reserved_memory_mib[i].store(stats.resident_mib, std::memory_order_relaxed);
			Global::control_block->process_shared_memory_mib[i].store(stats.shared_mib, std::memory_order_relaxed);
			Global::control_block->process_heartbeats[i].store(
					processes[i - 1].stopped ? 0 : processes[i - 1].heartbeats, std::memory_order_relaxed);
		}
		else
		{
			LOGI("Child process #%u: %15u MiB resident, %15u MiB shared.\n",
			     i - 1, stats.resident_mib, stats.shared_mib);
		}
	}

	if (Global::control_block)
	{
		Global::control_block->num_running_processes.store(Global::running_processes, std::memory_order_relaxed);
		Global::control_block->num_processes_memory_stats.store(num_processes, std::memory_order_release);
	}
}

bool ProcessProgress::start_child_process(vector<ProcessProgress> &siblings)
{
	graphics_progress = -1;
	compute_progress = -1;
	raytracing_progress = -1;
	stopped = false;

	if (start_graphics_index >= end_graphics_index &&
	    start_compute_index >= end_compute_index &&
	    start_raytracing_index >= end_raytracing_index)
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
		crash_fd = crash_fds[0];
		pid = new_pid;
		heartbeats = 1;

		send_faulty_modules_and_close(input_fds[1]);
		close(crash_fds[1]);
		close(input_fds[0]);
		Global::active_processes++;

		epoll_event event = {};
		event.data.u32 = index;
		event.events = EPOLLIN | EPOLLRDHUP;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, crash_fd, &event) < 0)
		{
			LOGE("Failed to add file to epoll.\n");
			return false;
		}

		return true;
	}
	else if (new_pid == 0)
	{
		// Close various FDs we won't use.
		close(Global::signal_fd);
		close(Global::epoll_fd);
		close(Global::timer_fd);
		close(crash_fds[0]);
		close(input_fds[1]);
		if (Global::control_fd >= 0)
			close(Global::control_fd);

		// We're the child process.
		// Unblock the signal mask.
		if (pthread_sigmask(SIG_SETMASK, &Global::old_mask, nullptr) != 0)
			return EXIT_FAILURE;

		// Make sure we don't hold unrelated epoll sensitive FDs open.
		for (auto &sibling : siblings)
		{
			if (&sibling == this)
				continue;

			if (sibling.crash_fd >= 0)
			{
				::close(sibling.crash_fd);
				sibling.crash_fd = -1;
			}

			if (sibling.timer_fd >= 0)
			{
				close(sibling.timer_fd);
				sibling.timer_fd = -1;
			}

			if (sibling.watchdog_timer_fd >= 0)
			{
				close(sibling.watchdog_timer_fd);
				sibling.watchdog_timer_fd = -1;
			}
		}

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
		copy_opts.start_raytracing_index = start_raytracing_index;
		copy_opts.end_raytracing_index = end_raytracing_index;
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

		if (!copy_opts.on_disk_module_identifier_path.empty() && index != 0)
		{
			copy_opts.on_disk_module_identifier_path += ".";
			copy_opts.on_disk_module_identifier_path += std::to_string(index);
		}

		exit(run_slave_process(Global::device_options, copy_opts, Global::databases));
	}
	else
		return false;
}

struct StallState
{
	int32_t dirty_pages_mib = 0;
	int64_t io_stalled_us = 0;
	int64_t timestamp_ns = 0;
};

static bool get_dirty_page_info(StallState &state)
{
	FILE *file = fopen("/proc/meminfo", "r");
	if (!file)
	{
		if (Global::control_block)
			Global::control_block->dirty_pages_mib.store(-1, std::memory_order_relaxed);
		return false;
	}

	bool got_dirty = false;

	char buffer[1024];
	while (fgets(buffer, sizeof(buffer), file))
	{
		if (strncmp(buffer, "Dirty:", 6) == 0)
		{
			errno = 0;
			auto dirty_pages_kb = strtoll(buffer + 6, nullptr, 10);
			if (errno == 0)
			{
				state.dirty_pages_mib = dirty_pages_kb / 1024;
				if (Global::control_block)
					Global::control_block->dirty_pages_mib.store(state.dirty_pages_mib, std::memory_order_relaxed);
				got_dirty = true;
			}
		}
	}

	if (!got_dirty && Global::control_block)
		Global::control_block->dirty_pages_mib.store(-1, std::memory_order_relaxed);

	fclose(file);
	return got_dirty;
}

static bool get_stall_info(const char *path, int64_t &total_us)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return false;

	bool ret = false;
	char buffer[1024];
	if (fgets(buffer, sizeof(buffer), file))
	{
		if (strncmp(buffer, "some ", 5) == 0)
		{
			const char *total = strstr(buffer, "total=");
			if (total)
			{
				errno = 0;
				total_us = strtoull(total + 6, nullptr, 10);
				ret = errno == 0;
			}
		}
	}

	fclose(file);
	return ret;
}

static bool poll_stall_information(StallState &state)
{
	if (!get_dirty_page_info(state))
		state.dirty_pages_mib = -1;
	if (!get_stall_info("/proc/pressure/io", state.io_stalled_us))
		state.io_stalled_us = -1;

	timespec ts = {};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return false;
	state.timestamp_ns = ts.tv_sec * 1000000000ll + ts.tv_nsec;
	return true;
}

static void manage_thrashing_behavior(std::vector<ProcessProgress> &child_processes,
                                      StallState &old_state, const StallState &new_state)
{
	// It is quite easy to overload the system with IO if we're not careful, so check for this.

	float delta_us = 1e-3f * float(new_state.timestamp_ns - old_state.timestamp_ns);
	float io_stall_us = float(new_state.io_stalled_us - old_state.io_stalled_us);

	int32_t delta_dirty_pages_mib = 0;
	if (new_state.dirty_pages_mib >= 0 && old_state.dirty_pages_mib >= 0)
		delta_dirty_pages_mib = new_state.dirty_pages_mib - old_state.dirty_pages_mib;

	float io_stall_ratio = -1.0f;
	if (new_state.io_stalled_us >= 0 && old_state.io_stalled_us >= 0)
		io_stall_ratio = io_stall_us / delta_us;
	io_stall_ratio = std::min(1.0f, io_stall_ratio);

	unsigned target_running_processes = Global::running_processes;

	// Some heuristics here.
	bool go_down = false;
	bool go_up = false;

	if (Global::target_running_processes_dirty_pages)
	{
		if (delta_dirty_pages_mib > 50)
		{
			// If we (or someone else) created more than 50 MiB of dirty data in this tick,
			// we should lower number of processes.
			go_down = true;
		}
		else if (delta_dirty_pages_mib < -10)
		{
			// Dirty pages is going down, can increase CPU load now.
			go_up = true;
		}
	}

	// IO stalls usually means someone is hammering IO.
	// We don't want to contribute to that.
	if (Global::target_running_processes_io_stall && io_stall_ratio >= 0.0f)
	{
		if (io_stall_ratio > 0.3f)
			go_down = true;
		else if (io_stall_ratio < 0.1f)
			go_up = true;
	}

	// Ensure forward progress. Make at least one process active
	// if we have had a period of complete sleep.
	if (target_running_processes == 0)
		go_up = true;

	if (go_down && target_running_processes)
		target_running_processes--;
	else if (go_up)
		target_running_processes++;

	if (Global::control_block)
	{
		Global::control_block->io_stall_percentage.store(
				io_stall_ratio < 0.0f ? -1 : int32_t(io_stall_ratio * 100.0f),
				std::memory_order_relaxed);
	}

	old_state = new_state;

	target_running_processes = std::min<unsigned>(target_running_processes, child_processes.size());
	Global::target_running_processes = target_running_processes;
}

static void update_target_running_processes(std::vector<ProcessProgress> &child_processes)
{
	if (Global::target_running_processes_static >= 0)
	{
		Global::target_running_processes = Global::target_running_processes_static;
		Global::target_running_processes = std::min<unsigned>(Global::target_running_processes, child_processes.size());
	}

	Global::running_processes = 0;
	for (auto &process : child_processes)
		if (process.pid > 0 && !process.stopped)
			Global::running_processes++;

	if (Global::running_processes > Global::target_running_processes)
	{
		// Put processes to sleep.
		unsigned to_stop = Global::running_processes - Global::target_running_processes;
		size_t num_processes = child_processes.size();
		for (size_t i = 0; i < num_processes && to_stop; i++)
		{
			if (child_processes[i].pid >= 0 && !child_processes[i].stopped && !child_processes[i].expect_kill)
			{
				if (::kill(child_processes[i].pid, SIGSTOP) == 0)
				{
					to_stop--;
					child_processes[i].stopped = true;
					Global::running_processes--;
				}
			}
		}
	}
	else if (Global::running_processes < Global::target_running_processes)
	{
		// Wake up sleeping processes.
		unsigned to_wake_up = Global::target_running_processes - Global::running_processes;
		size_t num_processes = child_processes.size();
		for (size_t i = 0; i < num_processes && to_wake_up; i++)
		{
			if (child_processes[i].pid > 0 && child_processes[i].stopped && !child_processes[i].expect_kill)
			{
				// Re-arm any timers before waking up to avoid potential scenario where
				// we hit watchdog timer right after waking up child process.
				child_processes[i].heartbeat();

				if (::kill(child_processes[i].pid, SIGCONT) == 0)
				{
					to_wake_up--;
					child_processes[i].stopped = false;
					Global::running_processes++;
				}
			}
		}
	}
}

static void handle_control_command(const char *command)
{
	if (strncmp(command, "RUNNING_TARGET", 14) == 0)
	{
		errno = 0;
		Global::target_running_processes_static = strtol(command + 14, nullptr, 10);
		if (errno)
			Global::target_running_processes_static = -1;
	}
	else if (strcmp(command, "IO_STALL_AUTO_ADJUST ON") == 0)
		Global::target_running_processes_io_stall = true;
	else if (strcmp(command, "IO_STALL_AUTO_ADJUST OFF") == 0)
		Global::target_running_processes_io_stall = false;
	else if (strcmp(command, "DIRTY_PAGE_BLOAT_AUTO_ADJUST ON") == 0)
		Global::target_running_processes_dirty_pages = true;
	else if (strcmp(command, "DIRTY_PAGE_BLOAT_AUTO_ADJUST OFF") == 0)
		Global::target_running_processes_dirty_pages = false;
	else if (strcmp(command, "DETACH") == 0)
		Global::control_fd_is_sentinel = false;
	else
		LOGE("Unrecognized control command: %s.\n", command);
}

static void shutdown_processes(std::vector<ProcessProgress> &child_processes)
{
	for (auto &child_process : child_processes)
	{
		if (child_process.pid >= 0)
		{
			child_process.stopped = false;
			child_process.expect_kill = true;
			kill(child_process.pid, SIGKILL);
		}
	}
}

static void handle_control_commands(std::vector<ProcessProgress> &child_processes)
{
	char buffer[4096];
	ssize_t ret;

	while ((ret = ::recv(Global::control_fd, buffer, sizeof(buffer) - 1, 0)) > 0)
	{
		buffer[ret] = '\0';
		handle_control_command(buffer);
	}

	bool close_fd = false;
	if (ret == 0)
	{
		if (Global::control_fd_is_sentinel)
		{
			LOGI("Parent process closed control FD with no notification, terminating early ...\n");
			shutdown_processes(child_processes);
		}
		close_fd = true;
	}
	else if (errno != EAGAIN)
		close_fd = true;

	if (close_fd)
		close_and_remove_epoll_entry(Global::control_fd);
}

static bool poll_children_process_states(vector<ProcessProgress> &child_processes)
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

		// Child process can receive SIGCONT/SIGSTOP which is benign.
		// This should normally only happen when the process is being debugged.
		if (!WIFEXITED(wstatus) && !WIFSIGNALED(wstatus))
			continue;

		if (itr != end(child_processes))
		{
			if (itr->process_shutdown(wstatus) && !itr->start_child_process(child_processes))
			{
				LOGE("Failed to start child process.\n");
				return false;
			}
			update_target_running_processes(child_processes);
		}
		else
			LOGE("Got SIGCHLD from unknown process PID %d.\n", pid);
	}

	return true;
}

static int run_master_process(const VulkanDevice::Options &opts,
                              const ThreadedReplayer::Options &replayer_opts,
                              const vector<const char *> &databases,
                              const char *whitelist, uint32_t whitelist_mask,
                              bool quiet_slave, int shmem_fd, int control_fd)
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
	size_t num_raytracing_pipelines;

	unsigned requested_graphic_pipelines = replayer_opts.end_graphics_index - replayer_opts.start_graphics_index;
	unsigned graphics_pipeline_offset = 0;
	unsigned requested_compute_pipelines = replayer_opts.end_compute_index - replayer_opts.start_compute_index;
	unsigned compute_pipeline_offset = 0;
	unsigned requested_raytracing_pipelines = replayer_opts.end_raytracing_index - replayer_opts.start_raytracing_index;
	unsigned raytracing_pipeline_offset = 0;

	{
		auto db = create_database(databases);

		if (whitelist)
		{
			db->set_whitelist_tag_mask(whitelist_mask);
			if (!db->load_whitelist_database(whitelist))
			{
				LOGE("Failed to load whitelist database: %s.\n", whitelist);
				exit(EXIT_FAILURE);
			}
		}

		if (!db->prepare())
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}

		// Export metadata so that we don't have to re-parse the archive over and over.
		char export_name[DatabaseInterface::OSHandleNameSize];
		DatabaseInterface::get_unique_os_export_name(export_name, sizeof(export_name));
		Global::metadata_fd = db->export_metadata_to_os_handle(export_name);
		if (Global::metadata_fd < 0)
		{
			LOGE("Failed to export metadata to FD handle.\n");
			return EXIT_FAILURE;
		}

		if (Global::control_block)
		{
			struct stat s;
			if (fstat(Global::metadata_fd, &s) == 0)
				Global::control_block->metadata_shared_size_mib.store(uint32_t(s.st_size / (1024 * 1024)), std::memory_order_relaxed);
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

		if (!db->get_hash_list_for_resource_tag(RESOURCE_RAYTRACING_PIPELINE, &num_raytracing_pipelines, nullptr))
		{
			for (auto &path : databases)
				LOGE("Failed to parse database %s.\n", path);
			return EXIT_FAILURE;
		}

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

		if (requested_raytracing_pipelines < num_raytracing_pipelines)
		{
			num_raytracing_pipelines = requested_raytracing_pipelines;
			raytracing_pipeline_offset = replayer_opts.start_raytracing_index;
		}

		if (Global::control_block)
		{
			Global::control_block->static_total_count_graphics = num_graphics_pipelines;
			Global::control_block->static_total_count_compute = num_compute_pipelines;
			Global::control_block->static_total_count_raytracing = num_raytracing_pipelines;
		}
	}

	if (Global::control_block)
		Global::control_block->progress_started.store(1, std::memory_order_release);

	Global::active_processes = 0;
	StallState stall_state;
	bool use_stall_state = !replayer_opts.disable_rate_limiter && poll_stall_information(stall_state);

	// We might have inherited awkward signal state from parent process, which will interfere
	// with the signalfd loop. Reset the dispositions to their default state.
	signal(SIGCHLD, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);

	// We will wait for child processes explicitly with signalfd.
	// Block delivery of signals in the normal way.
	// For this to work, there cannot be any other threads in the process
	// which may capture SIGCHLD anyways.
	// Also block SIGINT/SIGTERM,
	// we'll need to cleanly take down the process group when this triggers.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	if (pthread_sigmask(SIG_BLOCK, &mask, &Global::old_mask) != 0)
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
	Global::target_running_processes = processes;
	Global::running_processes = processes;

	// Create an epoll instance and add the signal fd to it.
	// The signalfd will signal when SIGCHLD is pending.
	Global::epoll_fd = epoll_create(2 * int(processes) + 3);
	if (Global::epoll_fd < 0)
	{
		LOGE("Failed to create epollfd. Too old Linux kernel?\n");
		return EXIT_FAILURE;
	}

	static constexpr uint32_t POLL_VALUE_SIGNAL_FD = UINT32_MAX;
	static constexpr uint32_t POLL_VALUE_CONTROL_FD = UINT32_MAX - 1u;
	static constexpr uint32_t POLL_VALUE_TIMER_FD = UINT32_MAX - 2u;
	static constexpr uint32_t POLL_VALUE_MAX_CHILD = UINT32_MAX - 3u;

	{
		epoll_event event = {};
		event.events = EPOLLIN;
		event.data.u32 = POLL_VALUE_SIGNAL_FD;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, Global::signal_fd, &event) < 0)
		{
			LOGE("Failed to add signalfd to epoll.\n");
			return EXIT_FAILURE;
		}
	}

	Global::timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (Global::timer_fd < 0)
	{
		LOGE("Failed to create timerfd. Too old Linux kernel?\n");
		return EXIT_FAILURE;
	}

	{
		epoll_event event = {};
		event.events = EPOLLIN;
		event.data.u32 = POLL_VALUE_TIMER_FD;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, Global::timer_fd, &event) < 0)
		{
			LOGE("Failed to add timerfd to epoll.\n");
			return EXIT_FAILURE;
		}

		// Poll child memory usage every second.
		struct itimerspec spec = {};
		spec.it_interval.tv_sec = 1;
		spec.it_value.tv_sec = 1;
		if (timerfd_settime(Global::timer_fd, 0, &spec, nullptr) < 0)
		{
			LOGE("Failed to set time with timerfd_settime.\n");
			return EXIT_FAILURE;
		}
	}

	// We can receive commands which can be used to dynamically adjust for various parameters.
	if (control_fd >= 0)
	{
		Global::control_fd_is_sentinel = true;
		Global::control_fd = control_fd;
		LOGI("Using control FD as sentinel.\n");

		epoll_event event = {};
		event.events = EPOLLIN;
		event.data.u32 = POLL_VALUE_CONTROL_FD;
		if (epoll_ctl(Global::epoll_fd, EPOLL_CTL_ADD, control_fd, &event) < 0)
		{
			LOGE("Failed to add control_fd to epoll.\n");
			return EXIT_FAILURE;
		}

		if (fcntl(control_fd, F_SETFL, fcntl(control_fd, F_GETFL) | O_NONBLOCK) < 0)
		{
			LOGE("Failed to set O_NONBLOCK.\n");
			return EXIT_FAILURE;
		}
	}
	else
		LOGI("Not using control_fd.\n");

	// fork() and pipe() strategy.
	for (unsigned i = 0; i < processes; i++)
	{
		auto &progress = child_processes[i];
		progress.start_graphics_index = graphics_pipeline_offset + (i * unsigned(num_graphics_pipelines)) / processes;
		progress.end_graphics_index = graphics_pipeline_offset + ((i + 1) * unsigned(num_graphics_pipelines)) / processes;
		progress.start_compute_index = compute_pipeline_offset + (i * unsigned(num_compute_pipelines)) / processes;
		progress.end_compute_index = compute_pipeline_offset + ((i + 1) * unsigned(num_compute_pipelines)) / processes;
		progress.start_raytracing_index = raytracing_pipeline_offset + (i * unsigned(num_raytracing_pipelines)) / processes;
		progress.end_raytracing_index = raytracing_pipeline_offset + ((i + 1) * unsigned(num_raytracing_pipelines)) / processes;
		progress.index = i;
		if (!progress.start_child_process(child_processes))
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
			if (errno == EINTR)
				continue;
			LOGE("epoll_wait() failed (errno = %d).\n", errno);
			return EXIT_FAILURE;
		}

		Global::heartbeats += ret;

		// Check for three cases in the epoll.
		// - Child process wrote something to stdout, we need to parse it.
		// - SIGCHLD happened, we need to reap child processes.
		// - TimerFD fired, we reached a timeout and we should SIGKILL the child process
		//   responsible.
		for (int i = 0; i < ret; i++)
		{
			auto &e = events[i];
			if (e.events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP))
			{
				if (e.data.u32 <= POLL_VALUE_MAX_CHILD)
				{
					auto &proc = child_processes[e.data.u32 & 0x3fffffffu];

					if (e.data.u32 & 0x80000000u)
					{
						// Timeout triggered. kill the process and reap it.
						// SIGCHLD handler should rearm the process as necessary.
						if (proc.timer_fd >= 0)
						{
							LOGE("Timeout triggered for child process #%u.\n", e.data.u32 & 0x3fffffffu);
							kill(proc.pid, SIGKILL);
							close_and_remove_epoll_entry(proc.timer_fd);
						}
					}
					else if (e.data.u32 & 0x40000000u)
					{
						// Timeout triggered. kill the process and reap it.
						// SIGCHLD handler should rearm the process as necessary.
						if (proc.watchdog_timer_fd >= 0)
						{
							uint64_t dummy;
							(void)::read(proc.watchdog_timer_fd, &dummy, sizeof(dummy));

							// Hitting watchdog timer while asleep is okay.
							if (!proc.stopped)
							{
								LOGE("Watchdog timeout triggered for child process #%u.\n", e.data.u32 & 0x3fffffffu);
								kill(proc.pid, SIGKILL);
								close_and_remove_epoll_entry(proc.watchdog_timer_fd);
							}
						}
					}
					else if (proc.crash_fd >= 0)
					{
						if (!proc.process_once())
							close_and_remove_epoll_entry(proc.crash_fd);
					}
				}
				else if (e.data.u32 == POLL_VALUE_SIGNAL_FD)
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
						if (!poll_children_process_states(child_processes))
							return EXIT_FAILURE;
					}
					else if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM)
					{
						// This process specifically received a request to be terminated.
						// If we have children in a SIGSTOP state, they will not die unless
						// someone explicitly SIGKILLs it, so make sure we take down our children
						// before we go.
						LOGI("Received signal %s, cleaning up ...\n", info.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM");
						shutdown_processes(child_processes);
					}
				}
				else if (e.data.u32 == POLL_VALUE_CONTROL_FD)
				{
					handle_control_commands(child_processes);
					// After a control command treat it as having received a timer event
					// so we react quickly to requested changes.
					update_target_running_processes(child_processes);
					poll_self_child_memory_usage(child_processes);
				}
				else if (e.data.u32 == POLL_VALUE_TIMER_FD)
				{
					uint64_t v;
					if (read(Global::timer_fd, &v, sizeof(v)) >= 0)
					{
						// SIGCHLD should take care of it, but we have observed weird situations
						// with lingering zombies, so to be robust against lost signals, poll this as well.
						if (!poll_children_process_states(child_processes))
							return EXIT_FAILURE;

						StallState new_stall_state;
						if (use_stall_state && poll_stall_information(new_stall_state))
							manage_thrashing_behavior(child_processes, stall_state, new_stall_state);
						update_target_running_processes(child_processes);
						poll_self_child_memory_usage(child_processes);
					}
				}
			}
			else if (e.events & EPOLLERR)
			{
				if (e.data.u32 < 0x80000000u)
				{
					auto &proc = child_processes[e.data.u32 & 0x7fffffffu];
					close_and_remove_epoll_entry(proc.crash_fd);
				}
			}
		}
	}

	if (Global::control_block)
		Global::control_block->progress_complete.store(1, std::memory_order_release);

	if (!replayer_opts.on_disk_module_identifier_path.empty())
	{
		if (strlen(child_processes[0].module_uuid_path) != 0)
		{
			std::vector<std::string> paths;
			for (size_t idx = 0; idx < replayer_opts.num_threads; idx++)
			{
				if (strlen(child_processes[idx].module_uuid_path) == 0)
				{
					LOGW("No module UUID path was found for thread %zu, skipping identifiers.\n", idx);
					continue;
				}
				auto path = replayer_opts.on_disk_module_identifier_path;
				if (idx != 0)
				{
					path += ".";
					path += std::to_string(idx);
				}
				path += ".";
				path += child_processes[idx].module_uuid_path;
				path += ".foz";
				paths.push_back(path);
			}

			if (paths.size() > 1)
			{
				std::vector<const char *> input_paths;
				input_paths.reserve(paths.size() - 1);
				for (auto itr = paths.begin() + 1; itr != paths.end(); ++itr)
					input_paths.push_back(itr->c_str());

				// It's possible that no shader module was written, so don't treat that as an error.
				if (!merge_concurrent_databases(paths.front().c_str(), input_paths.data(), input_paths.size(), true))
					LOGE("Failed to merge identifier databases.\n");
				for (auto itr = paths.begin() + 1; itr != paths.end(); ++itr)
					remove(itr->c_str());
			}
		}
		else
		{
			LOGW("No module UUID path was found, skipping identifiers.\n");
		}
	}

	return EXIT_SUCCESS;
}

static ThreadedReplayer *global_replayer = nullptr;
static int crash_fd = -1;
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

	if (per_thread.current_raytracing_pipeline)
	{
		sprintf(buffer, "RAYTRACE_VERR %" PRIx64 "\n", per_thread.current_raytracing_pipeline);
		write_all(crash_fd, buffer);
	}
}

static void report_module_uuid(const char (&path)[2 * VK_UUID_SIZE + 1])
{
	if (crash_fd >= 0)
	{
		char buffer[64];
		sprintf(buffer, "MODULE_UUID %s\n", path);
		if (!write_all(crash_fd, buffer))
			_exit(2);
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

	// If we crashed outside the domain of compiling pipelines we are kind of screwed,
	// so treat it as a dirty crash, don't report anything. We have no way to ensure forward progress
	// if we try to restart.
	if (per_thread.current_graphics_pipeline ||
	    per_thread.current_compute_pipeline ||
	    per_thread.current_raytracing_pipeline)
	{
		// Report where we stopped, so we can continue.
		sprintf(buffer, "GRAPHICS %d %" PRIx64 "\n", per_thread.current_graphics_index,
		        per_thread.current_graphics_pipeline);
		if (!write_all(crash_fd, buffer))
			_exit(2);

		sprintf(buffer, "COMPUTE %d %" PRIx64 "\n", per_thread.current_compute_index,
		        per_thread.current_compute_pipeline);
		if (!write_all(crash_fd, buffer))
			_exit(2);

		sprintf(buffer, "RAYTRACE %d %" PRIx64 "\n", per_thread.current_raytracing_index,
		        per_thread.current_raytracing_pipeline);
		if (!write_all(crash_fd, buffer))
			_exit(2);
	}

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

static void report_failed_pipeline()
{
	if (global_replayer)
	{
		auto *per_thread = &global_replayer->get_per_thread_data();

		if (per_thread->current_graphics_pipeline)
		{
			unsigned index = per_thread->current_graphics_index - 1;
			LOGE("Graphics pipeline crashed or hung: %016" PRIx64 ". Rerun with: --graphics-pipeline-range %u %u.\n",
			     per_thread->current_graphics_pipeline, index, index + 1);
		}
		else if (per_thread->current_compute_pipeline)
		{
			unsigned index = per_thread->current_compute_index - 1;
			LOGE("Compute pipeline crashed or hung, hash: %016" PRIx64 ". Rerun with: --compute-pipeline-range %u %u.\n",
			     per_thread->current_compute_pipeline, index, index + 1);
		}
		else if (per_thread->current_raytracing_pipeline)
		{
			unsigned index = per_thread->current_raytracing_index - 1;
			LOGE("Raytracing pipeline crashed or hung, hash: %016" PRIx64 ". Rerun with: --raytracing-pipeline-range %u %u.\n",
			     per_thread->current_raytracing_pipeline, index, index + 1);
		}
	}
}

static void crash_handler_trivial(int)
{
	report_failed_pipeline();
	LOGE("Crashed or hung while replaying.\n");
	fflush(stderr);
	_exit(1);
}

static void install_trivial_crash_handlers(ThreadedReplayer &replayer)
{
	global_replayer = &replayer;

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	sigemptyset(&act.sa_mask);
	act.sa_handler = crash_handler_trivial;
	act.sa_flags = SA_RESETHAND;

	sigaction(SIGSEGV, &act, nullptr);
	sigaction(SIGFPE, &act, nullptr);
	sigaction(SIGILL, &act, nullptr);
	sigaction(SIGBUS, &act, nullptr);
	sigaction(SIGABRT, &act, nullptr);
}

static void timeout_handler()
{
	if (global_replayer->thread_pool.size() > 1)
	{
		LOGE("Using timeout handling with more than one worker thread, cannot know which thread is the culprit.\n");
		// Just send signal to main thread so we don't emit any false positives.
		pthread_kill(pthread_self(), SIGABRT);
	}
	else
	{
		// Pretend we crashed in a safe way.
		// Send a signal to the worker thread to make sure we tear down on that thread.
		pthread_kill(global_replayer->thread_pool.front().native_handle(), SIGABRT);
	}
}

static void begin_heartbeat()
{
	if (crash_fd >= 0)
		write_all(crash_fd, "BEGIN_HEARTBEAT\n");
}

static void heartbeat()
{
	if (crash_fd >= 0)
		write_all(crash_fd, "HEARTBEAT\n");
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

	if (!replayer_opts.disable_signal_handler)
	{
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
	}

	// Don't allow the main thread to handle abort signals.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGABRT);
	sigset_t old_mask;
	if (pthread_sigmask(SIG_BLOCK, &mask, &old_mask) < 0)
		return EXIT_FAILURE;

	int ret = run_normal_process(replayer, databases, nullptr, 0, Global::metadata_fd);
	global_replayer = nullptr;

	if (!replayer_opts.disable_signal_handler)
	{
		// Cannot reliably handle these signals if they occur during teardown of the process.
		signal(SIGSEGV, SIG_DFL);
		signal(SIGFPE, SIG_DFL);
		signal(SIGILL, SIG_DFL);
		signal(SIGBUS, SIG_DFL);
		signal(SIGABRT, SIG_DFL);
	}
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

// Attempt to dispatch to a replay wrapper which is never expected to return.
// Wrapper is responsible for clearing the environment to prevent loops.
static void dispatch_to_replay_wrapper(const char *wrapper_path, char *const argv[])
{
	string exec_path = Path::get_executable_path();
	if (!exec_path.empty())
		setenv(FOSSILIZE_REPLAY_WRAPPER_ORIGINAL_APP_ENV, exec_path.c_str(), 1);
	execvp(wrapper_path, argv);
}
