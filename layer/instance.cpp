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

#include "instance.hpp"
#include "utils.hpp"
#include <mutex>
#include <unordered_map>
#include <memory>
#include "fossilize_application_filter.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <signal.h>
#include "fossilize_inttypes.h"
#include "fossilize_errors.hpp"

namespace Fossilize
{
void Instance::init(VkInstance instance_, const VkApplicationInfo *pApp, VkLayerInstanceDispatchTable *pTable_, PFN_vkGetInstanceProcAddr gpa_)
{
	instance = instance_;
	pTable = pTable_;
	gpa = gpa_;

	// pNext in appInfo is not supported.
	if (pApp && !pApp->pNext)
	{
		pAppInfo = alloc.allocate<VkApplicationInfo>();
		*pAppInfo = *pApp;

		if (pApp->pApplicationName)
		{
			size_t len = strlen(pApp->pApplicationName) + 1;
			char *pAppName = alloc.allocate_n<char>(len);
			memcpy(pAppName, pApp->pApplicationName, len);
			pAppInfo->pApplicationName = pAppName;
		}

		if (pApp->pEngineName)
		{
			size_t len = strlen(pApp->pEngineName) + 1;
			char *pEngineName = alloc.allocate_n<char>(len);
			memcpy(pEngineName, pApp->pEngineName, len);
			pAppInfo->pEngineName = pEngineName;
		}
	}
}

// Make this global to the process so we can share pipeline recording across VkInstances as well in-case an application is using external memory sharing techniques, (VR?).
// We only access this data structure on device creation, so performance is not a real concern.
static std::mutex recorderLock;

struct Recorder
{
	std::unique_ptr<ApplicationInfoFilter> filter;
	std::unique_ptr<DatabaseInterface> interface;
	std::unique_ptr<StateRecorder> recorder;
};
static std::unordered_map<Hash, Recorder> globalRecorders;

#ifdef ANDROID
static std::string getSystemProperty(const char *key)
{
	// Environment variables are not easy to set on Android.
	// Make use of the system properties instead.
	char value[256];
	char command[256];
	snprintf(command, sizeof(command), "getprop %s", key);

	// __system_get_property is apparently removed in recent NDK, so just use popen.
	size_t len = 0;
	FILE *file = popen(command, "rb");
	if (file)
	{
		len = fread(value, 1, sizeof(value) - 1, file);
		// Last character is a newline, so remove that.
		if (len > 1)
			value[len - 1] = '\0';
		else
			len = 0;
		fclose(file);
	}

	return len ? value : "";
}
#endif

#ifndef FOSSILIZE_DUMP_PATH_ENV
#define FOSSILIZE_DUMP_PATH_ENV "FOSSILIZE_DUMP_PATH"
#endif

#ifndef FOSSILIZE_DUMP_PATH_READ_ONLY_ENV
#define FOSSILIZE_DUMP_PATH_READ_ONLY_ENV "FOSSILIZE_DUMP_PATH_READ_ONLY"
#endif

#ifndef FOSSILIZE_APPLICATION_INFO_FILTER_PATH_ENV
#define FOSSILIZE_APPLICATION_INFO_FILTER_PATH_ENV "FOSSILIZE_APPLICATION_INFO_FILTER_PATH"
#endif

#ifdef FOSSILIZE_LAYER_CAPTURE_SIGSEGV
static thread_local const VkComputePipelineCreateInfo *tls_compute_create_info = nullptr;
static thread_local const VkGraphicsPipelineCreateInfo *tls_graphics_create_info = nullptr;
static thread_local StateRecorder *tls_recorder = nullptr;

static bool emergencyRecord()
{
	bool ret = false;
	if (tls_recorder)
	{
		if (tls_graphics_create_info)
			ret = tls_recorder->record_graphics_pipeline(VK_NULL_HANDLE, *tls_graphics_create_info, nullptr, 0);
		if (tls_compute_create_info)
			ret = tls_recorder->record_compute_pipeline(VK_NULL_HANDLE, *tls_compute_create_info, nullptr, 0);

		// Flush out the recording thread.
		tls_recorder->tear_down_recording_thread();
	}

	return ret;
}

#ifdef _WIN32
static LONG WINAPI crashHandler(_EXCEPTION_POINTERS *)
{
	LOGE_LEVEL("Caught segmentation fault! Emergency serialization of state to disk ...\n");
	emergencyRecord();
	LOGE_LEVEL("Done with emergency serialization, hopefully this worked :D\n");

	MessageBoxA(nullptr, "Pipeline creation triggered an access violation, the offending state was serialized. The application will now terminate.",
	            "Pipeline creation access violation", 0);

	// Clean exit instead of reporting the segfault.
	// Use exit code 2 to mark a segfaulted child.
	ExitProcess(2);
	return EXCEPTION_EXECUTE_HANDLER;
}

static void installSegfaultHandler()
{
	// Setup a last resort SEH handler. This overrides any global messagebox saying "application crashed",
	// which is what we want.
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
	SetUnhandledExceptionFilter(crashHandler);
}
#else
static void segfaultHandler(int sig)
{
	LOGE_LEVEL("Caught segmentation fault! Emergency serialization of state to disk ...\n");
	emergencyRecord();
	LOGE_LEVEL("Done with emergency serialization, hopefully this worked :D\n");

	// Now we can die properly.
	raise(sig);
}

static void installSegfaultHandler()
{
	struct sigaction sa = {};
	sa.sa_flags = SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = segfaultHandler;

	if (sigaction(SIGSEGV, &sa, nullptr) < 0)
		LOGE_LEVEL("Failed to install SIGSEGV handler!\n");
	if (sigaction(SIGFPE, &sa, nullptr) < 0)
		LOGE_LEVEL("Failed to install SIGFPE handler!\n");
	if (sigaction(SIGABRT, &sa, nullptr) < 0)
		LOGE_LEVEL("Failed to install SIGABRT handler!\n");
}
#endif

void Instance::braceForGraphicsPipelineCrash(StateRecorder *recorder,
                                             const VkGraphicsPipelineCreateInfo *info)
{
	tls_recorder = recorder;
	tls_graphics_create_info = info;
	tls_compute_create_info = nullptr;
}

void Instance::braceForComputePipelineCrash(StateRecorder *recorder,
                                            const VkComputePipelineCreateInfo *info)
{
	tls_recorder = recorder;
	tls_compute_create_info = info;
	tls_graphics_create_info = nullptr;
}

void Instance::completedPipelineCompilation()
{
	tls_recorder = nullptr;
	tls_graphics_create_info = nullptr;
	tls_compute_create_info = nullptr;
}
#endif

Instance::Instance()
{
#ifdef FOSSILIZE_LAYER_CAPTURE_SIGSEGV
#ifdef ANDROID
	auto sigsegv = getSystemProperty("debug.fossilize.dump_sigsegv");
	if (!sigsegv.empty() && strtoul(sigsegv.c_str(), nullptr, 0) != 0)
	{
		installSegfaultHandler();
		enableCrashHandler = true;
	}
#else
	const char *sigsegv = getenv("FOSSILIZE_DUMP_SIGSEGV");
	if (sigsegv && strtoul(sigsegv, nullptr, 0) != 0)
	{
		installSegfaultHandler();
		enableCrashHandler = true;
	}
#endif
#endif
}

StateRecorder *Instance::getStateRecorderForDevice(const VkApplicationInfo *appInfo, const VkPhysicalDeviceFeatures2 *features)
{
	auto appInfoFeatureHash = Hashing::compute_application_feature_hash(appInfo, features);
	auto hash = Hashing::compute_combined_application_feature_hash(appInfoFeatureHash);

	std::lock_guard<std::mutex> lock(recorderLock);
	auto itr = globalRecorders.find(hash);
	if (itr != end(globalRecorders))
		return itr->second.recorder.get();

	auto &entry = globalRecorders[hash];

	std::string serializationPath;
	const char *extraPaths = nullptr;
#ifdef ANDROID
	serializationPath = "/sdcard/fossilize";
	auto logPath = getSystemProperty("debug.fossilize.dump_path");
	if (!logPath.empty())
	{
		serializationPath = logPath;
		LOGI("Overriding serialization path: \"%s\".\n", logPath.c_str());
	}
	const char *filterPath = nullptr;
#else
	serializationPath = "fossilize";
	const char *path = getenv(FOSSILIZE_DUMP_PATH_ENV);
	if (path)
	{
		serializationPath = path;
		LOGI("Overriding serialization path: \"%s\".\n", path);
	}
	extraPaths = getenv(FOSSILIZE_DUMP_PATH_READ_ONLY_ENV);
	const char *filterPath = getenv(FOSSILIZE_APPLICATION_INFO_FILTER_PATH_ENV);
#endif

	if (filterPath)
	{
		entry.filter.reset(new ApplicationInfoFilter);
		entry.filter->parse_async(filterPath);
	}

	char hashString[17];
	sprintf(hashString, "%016" PRIx64, hash);
	if (!serializationPath.empty())
		serializationPath += ".";
	serializationPath += hashString;
	entry.interface.reset(create_concurrent_database_with_encoded_extra_paths(serializationPath.c_str(),
	                                                                          DatabaseMode::Append,
	                                                                          extraPaths));

	auto *recorder = new StateRecorder;
	entry.recorder.reset(recorder);
	recorder->set_database_enable_compression(true);
	recorder->set_database_enable_checksum(true);
	recorder->set_application_info_filter(entry.filter.get());
	if (appInfo)
		if (!recorder->record_application_info(*appInfo))
			LOGE_LEVEL("Failed to record application info.\n");
	if (features)
		if (!recorder->record_physical_device_features(*features))
			LOGE_LEVEL("Failed to record physical device features.\n");
	recorder->init_recording_thread(entry.interface.get());

	return recorder;
}

}
