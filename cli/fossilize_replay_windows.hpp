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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>

static bool write_all(HANDLE file, const char *str)
{
	size_t len = strlen(str);
	while (len)
	{
		DWORD wrote;
		if (!WriteFile(file, str, len, &wrote, nullptr))
			return false;
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
static VulkanDevice::Options device_options;
static bool quiet_slave;
}

struct ProcessProgress
{
	unsigned start_graphics_index = 0u;
	unsigned start_compute_index = 0u;
	unsigned end_graphics_index = ~0u;
	unsigned end_compute_index = ~0u;
	HANDLE process = INVALID_HANDLE_VALUE;
	FILE *crash_file = nullptr;
	HANDLE crash_file_handle = INVALID_HANDLE_VALUE;

	int compute_progress = -1;
	int graphics_progress = -1;

	bool process_once();
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

bool ProcessProgress::process_shutdown()
{
	// Flush out all messages we got.
	while (process_once());
	if (crash_file)
		fclose(crash_file);
	crash_file = nullptr;
	crash_file_handle = INVALID_HANDLE_VALUE;

	// Reap child process.
	Global::active_processes--;
	if (process != INVALID_HANDLE_VALUE)
	{
		if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0)
			return false;

		DWORD code = 0;
		if (!GetExitCodeProcess(process, &code))
			LOGE("Failed to get exit code of process.\n");
		CloseHandle(process);
		process = INVALID_HANDLE_VALUE;
	}

#if 0
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
		return false;
	}
#endif

	// We might have crashed, but we never saw any progress marker.
	// We do not know what to do from here, so we just terminate.
	if (graphics_progress < 0 || compute_progress < 0)
	{
		LOGE("Child process terminated before we could receive progress. Cannot continue.\n");
		return false;
	}

	start_graphics_index = uint32_t(graphics_progress);
	start_compute_index = uint32_t(compute_progress);
	if (start_graphics_index >= end_graphics_index && start_compute_index >= end_compute_index)
		return false;
	else
	{
		LOGE("Process index %u crashed, but will retry.\n", index);
		LOGE("  New graphics range (%u, %u)\n", start_graphics_index, end_graphics_index);
		LOGE("  New compute range (%u, %u)\n", start_compute_index, end_compute_index);
		return true;
	}
}

static void send_faulty_modules_and_close(HANDLE file)
{
	for (auto &m : Global::faulty_spirv_modules)
	{
		char buffer[18];
		sprintf(buffer, "%llx\n", static_cast<unsigned long long>(m));
		write_all(file, buffer);
	}

	CloseHandle(file);
}

bool ProcessProgress::start_child_process()
{
	graphics_progress = -1;
	compute_progress = -1;

	char filename[_MAX_PATH];
	if (FAILED(GetModuleFileNameA(nullptr, filename, sizeof(filename))))
		return false;

	std::string cmdline;
	cmdline += "\"";
	cmdline += filename;
	cmdline += "\" ";
	cmdline += "\"";
	cmdline += Global::db_path;
	cmdline += "\" ";
	cmdline += "--slave-process ";
	cmdline += "--num-threads 1 ";
	cmdline += "--graphics-pipeline-range ";
	cmdline += to_string(start_graphics_index);
	cmdline += " ";
	cmdline += to_string(end_graphics_index);
	cmdline += " --compute-pipeline-range ";
	cmdline += to_string(start_compute_index);
	cmdline += " ";
	cmdline += to_string(end_compute_index);

	if (Global::base_replayer_options.pipeline_cache)
		cmdline += " --pipeline-cache";

	SECURITY_ATTRIBUTES attrs = {};
	attrs.bInheritHandle = TRUE;
	attrs.nLength = sizeof(SECURITY_ATTRIBUTES);
	attrs.lpSecurityDescriptor = nullptr;

	HANDLE slave_stdout_read = INVALID_HANDLE_VALUE;
	HANDLE slave_stdout_write = INVALID_HANDLE_VALUE;
	HANDLE master_stdout_read = INVALID_HANDLE_VALUE;
	HANDLE master_stdout_write = INVALID_HANDLE_VALUE;

	if (!CreatePipe(&slave_stdout_read, &master_stdout_write, &attrs, 0))
	{
		LOGE("Failed to create pipe.\n");
		return false;
	}

	if (!CreatePipe(&master_stdout_read, &slave_stdout_write, &attrs, 0))
	{
		LOGE("Failed to create pipe.\n");
		return false;
	}

	if (!SetHandleInformation(master_stdout_read, HANDLE_FLAG_INHERIT, 0))
	{
		LOGE("Failed to set handle information.\n");
		return false;
	}

	if (!SetHandleInformation(master_stdout_write, HANDLE_FLAG_INHERIT, 0))
	{
		LOGE("Failed to set handle information.\n");
		return false;
	}

	STARTUPINFO si = {};
	si.cb = sizeof(STARTUPINFO);
	si.hStdOutput = slave_stdout_write;
	si.hStdInput = slave_stdout_read;
	si.dwFlags |= STARTF_USESTDHANDLES;
	PROCESS_INFORMATION pi = {};
	HANDLE nul = INVALID_HANDLE_VALUE;

	if (Global::quiet_slave)
	{
		nul = CreateFile("NUL", GENERIC_WRITE, 0, nullptr, 0, 0, nullptr);
		if (nul == INVALID_HANDLE_VALUE)
		{
			LOGE("Failed to open NUL file for writing.\n");
			return false;
		}

		si.hStdError = nul;
	}
	else
	{
		if (!SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		{
			LOGE("Failed to enable inheritance for stderror handle.\n");
			return false;
		}
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	}

	char *duped_string = _strdup(cmdline.c_str());

	if (!CreateProcessA(nullptr, duped_string, &attrs, &attrs, TRUE, 0, nullptr, nullptr, &si, &pi))
	{
		LOGE("Failed to create child process.\n");
		free(duped_string);
		return false;
	}
	free(duped_string);

	CloseHandle(slave_stdout_read);
	CloseHandle(slave_stdout_write);

	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);

	CloseHandle(pi.hThread);
	process = pi.hProcess;
	send_faulty_modules_and_close(master_stdout_write);

	crash_file_handle = master_stdout_read;
	int fd = _open_osfhandle(intptr_t(master_stdout_read), _O_RDONLY | _O_TEXT);
	if (fd < 0)
	{
		LOGE("Failed to open PIPE handle as FD.\n");
		return false;
	}

	crash_file = _fdopen(fd, "r");
	if (!crash_file)
	{
		LOGE("Failed to open pipe as FILE.\n");
		return false;
	}

	Global::active_processes++;
	return true;
}

static int run_master_process(const VulkanDevice::Options &opts,
                              const ThreadedReplayer::Options &replayer_opts,
                              const string &db_path,
                              bool quiet_slave)
{
	Global::quiet_slave = quiet_slave;
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
	vector<ProcessProgress> child_processes(processes);
	vector<HANDLE> wait_handles;

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

	wait_handles.reserve(3 * processes);

	while (Global::active_processes != 0)
	{
		wait_handles.clear();
		for (auto &process : child_processes)
		{
			if (process.process != INVALID_HANDLE_VALUE)
				wait_handles.push_back(process.process);
			if (process.crash_file_handle != INVALID_HANDLE_VALUE)
				wait_handles.push_back(process.crash_file_handle);

			DWORD ret = WaitForMultipleObjects(wait_handles.size(), wait_handles.data(), FALSE, INFINITE);
			if (ret == WAIT_FAILED)
			{
				LOGE("WaitForMultipleObjects failed.\n");
				return EXIT_FAILURE;
			}
			else if (ret == WAIT_TIMEOUT)
				continue;
			else if (ret >= WAIT_ABANDONED_0)
				continue;
			else if (ret >= WAIT_OBJECT_0 && ret < WAIT_OBJECT_0 + wait_handles.size())
			{
				HANDLE handle = wait_handles[ret - WAIT_OBJECT_0];
				auto process_itr = find_if(begin(child_processes), end(child_processes), [&](const ProcessProgress &prog) {
					return prog.process == handle;
				});

				auto file_itr = find_if(begin(child_processes), end(child_processes), [&](const ProcessProgress &prog) {
					return prog.crash_file_handle == handle;
				});

				if (process_itr != end(child_processes))
				{
					// The process finished.
					process_itr->process_shutdown();
				}
				else if (file_itr != end(child_processes))
				{
					// Read a command.
					file_itr->process_once();
				}
			}
		}

	}

	return EXIT_SUCCESS;
}

static ThreadedReplayer *global_replayer = nullptr;
static HANDLE crash_handle;
#if 0
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
#endif

static void log_and_die()
{
	DWORD dw = GetLastError();
	char *lpMsgBuf = nullptr;
	FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	               nullptr,
	               dw,
	               MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
	               reinterpret_cast<LPSTR>(&lpMsgBuf), 0, nullptr);

	LOGE("Error: %s\n", lpMsgBuf);
	ExitProcess(1);
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

	if (!DuplicateHandle(
		GetCurrentProcess(),
		GetStdHandle(STD_OUTPUT_HANDLE),
		GetCurrentProcess(),
		&crash_handle,
		DUPLICATE_SAME_ACCESS,
		FALSE, DUPLICATE_CLOSE_SOURCE))
	{
		LOGE("Failed to duplicate stdout handle.\n");
		log_and_die();
	}

	global_replayer = &replayer;
	return run_normal_process(replayer, db_path);
}
