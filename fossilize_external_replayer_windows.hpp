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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "layer/utils.hpp"
#include <string>
#include <atomic>
#include <unordered_set>
#include <memory>
#include <vector>
#include <algorithm>
#include "fossilize_external_replayer_control_block.hpp"
#include "path.hpp"

namespace Fossilize
{
static std::atomic<int32_t> shm_index;

struct ExternalReplayer::Impl
{
	~Impl();

	HANDLE process = nullptr;
	HANDLE mapping_handle = nullptr;
	HANDLE mutex = nullptr;
	HANDLE job_handle = nullptr;
	SharedControlBlock *shm_block = nullptr;
	size_t shm_block_size = 0;
	DWORD exit_code = 0;
	std::unordered_set<Hash> faulty_spirv_modules;
	std::vector<std::pair<unsigned, Hash>> faulty_graphics_pipelines;
	std::vector<std::pair<unsigned, Hash>> faulty_compute_pipelines;
	std::vector<std::pair<unsigned, Hash>> faulty_raytracing_pipelines;
	std::unordered_set<Hash> graphics_failed_validation;
	std::unordered_set<Hash> compute_failed_validation;
	std::unordered_set<Hash> raytracing_failed_validation;

	bool start(const ExternalReplayer::Options &options);
	ExternalReplayer::PollResult poll_progress(Progress &progress);
	uintptr_t get_process_handle() const;
	int wait();
	bool is_process_complete(int *return_status);
	bool kill();

	void parse_message(const char *msg);
	bool get_faulty_spirv_modules(size_t *count, Hash *hashes) const;
	bool get_faulty_graphics_pipelines(size_t *count, unsigned *indices, Hash *hashes) const;
	bool get_faulty_compute_pipelines(size_t *count, unsigned *indices, Hash *hashes) const;
	bool get_faulty_raytracing_pipelines(size_t *count, unsigned *indices, Hash *hashes) const;
	bool get_graphics_failed_validation(size_t *count, Hash *hashes) const;
	bool get_compute_failed_validation(size_t *count, Hash *hashes) const;
	bool get_raytracing_failed_validation(size_t *count, Hash *hashes) const;
	bool get_failed(const std::unordered_set<Hash> &failed, size_t *count, Hash *hashes) const;
	bool get_failed(const std::vector<std::pair<unsigned, Hash>> &failed, size_t *count,
	                unsigned *indices, Hash *hashes) const;
	bool poll_memory_usage(uint32_t *num_processes, ProcessStats *stats) const;
	bool poll_global_resource_usage(GlobalResourceUsage &usage) const;
	bool send_message(const char *msg);
};

ExternalReplayer::Impl::~Impl()
{
	if (shm_block)
		UnmapViewOfFile(shm_block);
	if (mapping_handle)
		CloseHandle(mapping_handle);
	if (mutex)
		CloseHandle(mutex);
	if (process)
		CloseHandle(process);
	if (job_handle)
		CloseHandle(job_handle);
}

bool ExternalReplayer::Impl::poll_memory_usage(uint32_t *, ProcessStats *) const
{
	return false;
}

bool ExternalReplayer::Impl::poll_global_resource_usage(GlobalResourceUsage &) const
{
	return false;
}

uintptr_t ExternalReplayer::Impl::get_process_handle() const
{
	return reinterpret_cast<uintptr_t>(process);
}

ExternalReplayer::PollResult ExternalReplayer::Impl::poll_progress(ExternalReplayer::Progress &progress)
{
	bool complete = shm_block->progress_complete.load(std::memory_order_acquire) != 0;

	if (!process && !complete)
		return ExternalReplayer::PollResult::Error;

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

	progress.raytracing.total = shm_block->total_raytracing.load(std::memory_order_relaxed);
	progress.raytracing.parsed = shm_block->parsed_raytracing.load(std::memory_order_relaxed);
	progress.raytracing.parsed_fail = shm_block->parsed_raytracing.load(std::memory_order_relaxed);
	progress.raytracing.skipped = shm_block->skipped_raytracing.load(std::memory_order_relaxed);
	progress.raytracing.cached = shm_block->cached_raytracing.load(std::memory_order_relaxed);
	progress.raytracing.completed = shm_block->successful_raytracing.load(std::memory_order_relaxed);

	progress.completed_modules = shm_block->successful_modules.load(std::memory_order_relaxed);
	progress.missing_modules = shm_block->parsed_module_failures.load(std::memory_order_relaxed);
	progress.total_modules = shm_block->total_modules.load(std::memory_order_relaxed);
	progress.banned_modules = shm_block->banned_modules.load(std::memory_order_relaxed);
	progress.module_validation_failures = shm_block->module_validation_failures.load(std::memory_order_relaxed);
	progress.clean_crashes = shm_block->clean_process_deaths.load(std::memory_order_relaxed);
	progress.dirty_crashes = shm_block->dirty_process_deaths.load(std::memory_order_relaxed);

	progress.total_graphics_pipeline_blobs = shm_block->static_total_count_graphics.load(std::memory_order_relaxed);
	progress.total_compute_pipeline_blobs = shm_block->static_total_count_compute.load(std::memory_order_relaxed);
	progress.total_raytracing_pipeline_blobs = shm_block->static_total_count_raytracing.load(std::memory_order_relaxed);

	if (WaitForSingleObject(mutex, INFINITE) == WAIT_OBJECT_0)
	{
		size_t read_avail = shared_control_block_read_avail(shm_block);
		for (size_t i = ControlBlockMessageSize; i <= read_avail; i += ControlBlockMessageSize)
		{
			char buf[ControlBlockMessageSize] = {};
			shared_control_block_read(shm_block, buf, sizeof(buf));
			parse_message(buf);
		}
		ReleaseMutex(mutex);
	}
	return complete ? ExternalReplayer::PollResult::Complete : ExternalReplayer::PollResult::Running;
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
	else if (strncmp(msg, "RAYTRACE_VERR", 13) == 0)
	{
		auto hash = strtoull(msg + 13, nullptr, 16);
		raytracing_failed_validation.insert(hash);
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
	else if (strncmp(msg, "RAYTRACE", 8) == 0)
	{
		char *end = nullptr;
		int index = int(strtol(msg + 8, &end, 0));
		if (index >= 0 && end)
		{
			Hash raytrace_pipeline = strtoull(end, nullptr, 16);
			faulty_raytracing_pipelines.push_back({ unsigned(index), raytrace_pipeline });
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
	if (!process)
	{
		if (return_status)
			*return_status = exit_code;
		return true;
	}

	if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
	{
		GetExitCodeProcess(process, &exit_code);
		CloseHandle(process);
		process = nullptr;

		// Pump the fifo through.
		ExternalReplayer::Progress progress = {};
		poll_progress(progress);

		if (return_status)
			*return_status = exit_code;
		return true;
	}
	else
		return false;
}

int ExternalReplayer::Impl::wait()
{
	if (!process)
		return exit_code;

	// Pump the fifo through.
	ExternalReplayer::Progress progress = {};
	poll_progress(progress);

	if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0)
		return -1;

	// Pump the fifo through.
	poll_progress(progress);

	GetExitCodeProcess(process, &exit_code);
	CloseHandle(process);
	process = nullptr;
	if (job_handle)
	{
		CloseHandle(job_handle);
		job_handle = nullptr;
	}
	return exit_code;
}

bool ExternalReplayer::Impl::kill()
{
	if (!process)
		return false;
	return TerminateProcess(process, 1);
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

bool ExternalReplayer::Impl::get_faulty_raytracing_pipelines(size_t *count, unsigned int *indices, Hash *hashes) const
{
	return get_failed(faulty_raytracing_pipelines, count, indices, hashes);
}

bool ExternalReplayer::Impl::get_graphics_failed_validation(size_t *count, Hash *hashes) const
{
	return get_failed(graphics_failed_validation, count, hashes);
}

bool ExternalReplayer::Impl::get_compute_failed_validation(size_t *count, Hash *hashes) const
{
	return get_failed(compute_failed_validation, count, hashes);
}

bool ExternalReplayer::Impl::get_raytracing_failed_validation(size_t *count, Hash *hashes) const
{
	return get_failed(raytracing_failed_validation, count, hashes);
}

static char *create_modified_environment(unsigned num_environment_variables, const ExternalReplayer::Environment *environment_variables)
{
	if (!num_environment_variables)
		return nullptr;

	char *environment = GetEnvironmentStringsA();
	if (!environment)
	{
		LOGE("Failed to obtain current environment for process.\n");
		return nullptr;
	}

	char *base_environment = environment;

	std::vector<std::unique_ptr<std::string>> allocated_envs;
	std::vector<const char *> env;
	while (*environment != '\0')
	{
		env.push_back(environment);
		environment += strlen(environment) + 1;
	}

	for (unsigned i = 0; i < num_environment_variables; i++)
	{
		bool override_environment = false;

		std::string key_value = environment_variables[i].key;
		key_value += "=";
		key_value += environment_variables[i].value;
		allocated_envs.emplace_back(new std::string(std::move(key_value)));
		const char *new_env = allocated_envs.back()->c_str();

		for (auto &e : env)
		{
			size_t key_len = strlen(environment_variables[i].key);
			if (strncmp(e, environment_variables[i].key, key_len) == 0 &&
			    ((e[key_len] == '=') || (e[key_len] == '\0')))
			{
				e = new_env;
				override_environment = true;
				break;
			}
		}

		if (!override_environment)
			env.push_back(new_env);
	}

	// Apparently need to sort the environment block alphabetically.
	std::sort(env.begin(), env.end(), [](const char *a, const char *b) {
		return strcmp(a, b) < 0;
	});

	size_t environment_block_size = 0;
	for (auto &e : env)
		environment_block_size += strlen(e) + 1;
	environment_block_size++;

	char *new_block = static_cast<char *>(malloc(environment_block_size));
	if (!new_block)
	{
		LOGE("Failed to allocate memory for new environment block.\n");
		return nullptr;
	}

	char *block = new_block;

	for (auto &e : env)
	{
		size_t len = strlen(e);
		memcpy(block, e, len);
		block += len;
		*block++ = '\0';
	}
	*block = '\0';

	FreeEnvironmentStringsA(base_environment);
	return new_block;
}

bool ExternalReplayer::Impl::start(const ExternalReplayer::Options &options)
{
	// Reserve 4 kB for control data, and 64 kB for a cross-process SHMEM ring buffer.
	shm_block_size = 64 * 1024 + 4 * 1024;

	char shm_name[256];
	char shm_mutex_name[256];
	sprintf(shm_name, "fossilize-external-%lu-%d", GetCurrentProcessId(), shm_index.fetch_add(1, std::memory_order_relaxed));
	sprintf(shm_mutex_name, "fossilize-external-%lu-%d", GetCurrentProcessId(), shm_index.fetch_add(1, std::memory_order_relaxed));
	mapping_handle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)shm_block_size, shm_name);

	if (!mapping_handle)
	{
		LOGE("Failed to create file mapping.\n");
		return false;
	}

	shm_block = static_cast<SharedControlBlock *>(MapViewOfFile(mapping_handle, FILE_MAP_READ | FILE_MAP_WRITE,
	                                                            0, 0, shm_block_size));

	if (!shm_block)
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

	mutex = CreateMutexA(nullptr, FALSE, shm_mutex_name);
	if (!mutex)
	{
		LOGE("Failed to create named mutex.\n");
		return false;
	}

	std::string cmdline;

	if (options.num_external_replayer_arguments)
	{
		for (unsigned i = 0; i < options.num_external_replayer_arguments; i++)
		{
			if (i != 0)
				cmdline += " ";
			cmdline += "\"";
			cmdline += options.external_replayer_arguments[i];
			cmdline += "\"";
		}
	}
	else
	{
		cmdline += "\"";
		if (options.external_replayer_path)
			cmdline += options.external_replayer_path;
		else
			cmdline += Path::get_executable_path();
		cmdline += "\"";
	}

	for (unsigned i = 0; i < options.num_databases; i++)
	{
		cmdline += " \"";
		cmdline += options.databases[i];
		cmdline += "\"";
	}

	cmdline += " --master-process";
	if (options.quiet)
		cmdline += " --quiet-slave";
	cmdline += " --shm-name ";
	cmdline += shm_name;
	cmdline += " --shm-mutex-name ";
	cmdline += shm_mutex_name;

	if (options.spirv_validate)
		cmdline += " --spirv-val";

	if (options.num_threads)
	{
		cmdline += " --num-threads ";
		cmdline += std::to_string(options.num_threads);
	}

	if (options.on_disk_pipeline_cache)
	{
		cmdline += " --on-disk-pipeline-cache ";
		cmdline += "\"";
		cmdline += options.on_disk_pipeline_cache;
		cmdline += "\"";
	}

	if (options.on_disk_validation_cache)
	{
		cmdline += " --on-disk-validation-cache ";
		cmdline += "\"";
		cmdline += options.on_disk_validation_cache;
		cmdline += "\"";
	}

	if (options.on_disk_validation_whitelist)
	{
		cmdline += " --on-disk-validation-whitelist ";
		cmdline += "\"";
		cmdline += options.on_disk_validation_whitelist;
		cmdline += "\"";
	}

	if (options.on_disk_validation_blacklist)
	{
		cmdline += " --on-disk-validation-blacklist ";
		cmdline += "\"";
		cmdline += options.on_disk_validation_blacklist;
		cmdline += "\"";
	}

	if (options.on_disk_replay_whitelist)
	{
		cmdline += " --on-disk-replay-whitelist ";
		cmdline += "\"";
		cmdline += options.on_disk_replay_whitelist;
		cmdline += "\"";

		cmdline += " --on-disk-replay-whitelist-mask ";
		char whitelist_hex[9];
		sprintf(whitelist_hex, "%x", options.on_disk_replay_whitelist_mask);
		cmdline += whitelist_hex;
	}

	if (options.on_disk_module_identifier)
	{
		cmdline += " --on-disk-module-identifier ";
		cmdline += "\"";
		cmdline += options.on_disk_module_identifier;
		cmdline += "\"";
	}

	if (options.replayer_cache_path)
	{
		cmdline += " --replayer-cache ";
		cmdline += "\"";
		cmdline += options.replayer_cache_path;
		cmdline += "\"";
	}

	cmdline += " --device-index ";
	cmdline += std::to_string(options.device_index);

	if (options.enable_validation)
		cmdline += " --enable-validation";

	if (options.null_device)
		cmdline += " --null-device";

	if (options.use_pipeline_range)
	{
		cmdline += " --graphics-pipeline-range ";
		cmdline += std::to_string(options.start_graphics_index);
		cmdline += " ";
		cmdline += std::to_string(options.end_graphics_index);

		cmdline += " --compute-pipeline-range ";
		cmdline += std::to_string(options.start_compute_index);
		cmdline += " ";
		cmdline += std::to_string(options.end_compute_index);

		cmdline += " --raytracing-pipeline-range ";
		cmdline += std::to_string(options.start_raytracing_index);
		cmdline += " ";
		cmdline += std::to_string(options.end_raytracing_index);
	}

	if (options.pipeline_stats_path)
	{
		cmdline += " --enable-pipeline-stats ";
		cmdline += "\"";
		cmdline += options.pipeline_stats_path;
		cmdline += "\"";
	}

	if (options.timeout_seconds)
	{
		cmdline += " --timeout-seconds ";
		cmdline += std::to_string(options.timeout_seconds);
	}

	for (unsigned i = 0; i < options.num_implicit_whitelist_indices; i++)
	{
		cmdline += " --implicit-whitelist ";
		cmdline += std::to_string(options.implicit_whitelist_indices[i]);
	}

	STARTUPINFO si = {};
	si.cb = sizeof(STARTUPINFO);
	si.dwFlags = STARTF_USESTDHANDLES;
	SECURITY_ATTRIBUTES attrs = {};
	attrs.bInheritHandle = TRUE;
	attrs.nLength = sizeof(attrs);

	HANDLE nul = INVALID_HANDLE_VALUE;
	if (options.quiet)
	{
		nul = CreateFileA("NUL", GENERIC_WRITE, 0, &attrs, OPEN_EXISTING, 0, nullptr);
		if (nul == INVALID_HANDLE_VALUE)
		{
			LOGE("Failed to open NUL file for writing.\n");
			return false;
		}

		si.hStdError = nul;
		si.hStdOutput = nul;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	}
	else
	{
		if (!SetHandleInformation(GetStdHandle(STD_OUTPUT_HANDLE), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		{
			LOGE("Failed to enable inheritance for stderror handle.\n");
			return false;
		}
		si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);

		if (!SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
		{
			LOGE("Failed to enable inheritance for stderror handle.\n");
			return false;
		}
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	}

	job_handle = CreateJobObjectA(nullptr, nullptr);
	if (!job_handle)
	{
		LOGE("Failed to create job handle.\n");
		// Not fatal, we just won't bother with this.
	}
	else
	{
		// Kill all child processes if the parent dies.
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
		jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
		{
			LOGE("Failed to set information for job object.\n");
			// Again, not fatal.
		}
	}

	// Modifying the environment for a child process on Windows is an ordeal since we have no intermediate fork
	// we can modify environment in.
	char *modified_environment = create_modified_environment(options.num_environment_variables, options.environment_variables);
	if (!modified_environment && options.num_environment_variables != 0)
	{
		LOGE("Failed to create modified environment.\n");
		return false;
	}

	// For whatever reason, this string must be mutable. Dupe it.
	char *duped_string = _strdup(cmdline.c_str());
	PROCESS_INFORMATION pi = {};
	// Replayer should have idle priority.
	if (!CreateProcessA(nullptr, duped_string, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | IDLE_PRIORITY_CLASS,
	                    modified_environment, nullptr,
	                    &si, &pi))
	{
		LOGE("Failed to create child process.\n");
		free(duped_string);
		free(modified_environment);
		return false;
	}

	free(modified_environment);

	if (job_handle && !AssignProcessToJobObject(job_handle, pi.hProcess))
	{
		LOGE("Failed to assign process to job handle.\n");
		// This isn't really fatal, just continue on.
	}

	// Now we can resume the main thread, after we've added the process to our job object.
	ResumeThread(pi.hThread);

	free(duped_string);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);

	CloseHandle(pi.hThread);
	process = pi.hProcess;
	return true;
}

bool ExternalReplayer::Impl::send_message(const char *)
{
	return false;
}
}

