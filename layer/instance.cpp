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
#include "path.hpp"
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
// We only need one global application info filter.
// We can kick off async parse of this as early as the layer is loaded, e.g. vkCreateInstance.
// Global ApplicationInfoFilter must outlive the recorder, so declare earlier in translation unit to guarantee this.
static std::unique_ptr<ApplicationInfoFilter> globalInfoFilter;
static std::mutex globalInfoFilterLock;
static bool globalInfoFilterDone;

#ifndef FOSSILIZE_APPLICATION_INFO_FILTER_PATH_ENV
#define FOSSILIZE_APPLICATION_INFO_FILTER_PATH_ENV "FOSSILIZE_APPLICATION_INFO_FILTER_PATH"
#endif

static ApplicationInfoFilter *getApplicationInfoFilter()
{
	std::lock_guard<std::mutex> lock(globalInfoFilterLock);
	if (globalInfoFilterDone)
		return globalInfoFilter.get();

#ifdef ANDROID
	const char *filterPath = nullptr;
#else
	const char *filterPath = getenv(FOSSILIZE_APPLICATION_INFO_FILTER_PATH_ENV);
#endif

	if (filterPath)
	{
		globalInfoFilter.reset(ApplicationInfoFilter::parse(
				filterPath,
				[](const char *env, void *) -> const char * { return getenv(env); },
				nullptr));

		if (!globalInfoFilter)
			LOGE_LEVEL("Failed to parse ApplicationInfoFilter, letting recording go through.\n");
	}

	globalInfoFilterDone = true;
	return globalInfoFilter.get();
}

void Instance::init(VkInstance instance_, const VkApplicationInfo *pApp, VkLayerInstanceDispatchTable *pTable_, PFN_vkGetInstanceProcAddr gpa_)
{
	infoFilter = getApplicationInfoFilter();
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
	std::unique_ptr<DatabaseInterface> interface;
	std::unique_ptr<DatabaseInterface> module_identifier_interface;
	std::unique_ptr<DatabaseInterface> last_use_interface;
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

#ifndef FOSSILIZE_DUMP_SYNC_ENV
#define FOSSILIZE_DUMP_SYNC_ENV "FOSSILIZE_DUMP_SYNC"
#endif

#ifndef FOSSILIZE_IDENTIFIER_DUMP_PATH_ENV
#define FOSSILIZE_IDENTIFIER_DUMP_PATH_ENV "FOSSILIZE_IDENTIFIER_DUMP_PATH"
#endif

#ifndef FOSSILIZE_LAST_USE_TAG_ENV
#define FOSSILIZE_LAST_USE_TAG_ENV "FOSSILIZE_LAST_USE_TAG"
#endif

#ifndef FOSSILIZE_PRECOMPILE_QA_ENV
#define FOSSILIZE_PRECOMPILE_QA_ENV "FOSSILIZE_PRECOMPILE_QA"
#endif

#ifdef FOSSILIZE_LAYER_CAPTURE_SIGSEGV
static thread_local const VkComputePipelineCreateInfo *tls_compute_create_info = nullptr;
static thread_local const VkGraphicsPipelineCreateInfo *tls_graphics_create_info = nullptr;
static thread_local const VkRayTracingPipelineCreateInfoKHR *tls_raytracing_create_info = nullptr;
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
		if (tls_raytracing_create_info)
			ret = tls_recorder->record_raytracing_pipeline(VK_NULL_HANDLE, *tls_raytracing_create_info, nullptr, 0);

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
#endif

void Instance::braceForGraphicsPipelineCrash(StateRecorder *recorder,
                                             const VkGraphicsPipelineCreateInfo *info)
{
#ifdef FOSSILIZE_LAYER_CAPTURE_SIGSEGV
	tls_recorder = recorder;
	tls_graphics_create_info = info;
	tls_compute_create_info = nullptr;
	tls_raytracing_create_info = nullptr;
#else
	(void)recorder;
	(void)info;
#endif
}

void Instance::braceForComputePipelineCrash(StateRecorder *recorder,
                                            const VkComputePipelineCreateInfo *info)
{
#ifdef FOSSILIZE_LAYER_CAPTURE_SIGSEGV
	tls_recorder = recorder;
	tls_compute_create_info = info;
	tls_graphics_create_info = nullptr;
	tls_raytracing_create_info = nullptr;
#else
	(void)recorder;
	(void)info;
#endif
}

void Instance::braceForRayTracingPipelineCrash(StateRecorder *recorder,
                                               const VkRayTracingPipelineCreateInfoKHR *info)
{
#ifdef FOSSILIZE_LAYER_CAPTURE_SIGSEGV
	tls_recorder = recorder;
	tls_compute_create_info = nullptr;
	tls_graphics_create_info = nullptr;
	tls_raytracing_create_info = info;
#else
	(void)recorder;
	(void)info;
#endif
}

void Instance::completedPipelineCompilation()
{
#ifdef FOSSILIZE_LAYER_CAPTURE_SIGSEGV
	tls_recorder = nullptr;
	tls_graphics_create_info = nullptr;
	tls_compute_create_info = nullptr;
	tls_raytracing_create_info = nullptr;
#endif
}

bool Instance::queryPrecompileQA()
{
	const char *precompileQA = getenv(FOSSILIZE_PRECOMPILE_QA_ENV);
	return precompileQA && strtoul(precompileQA, nullptr, 0) != 0;
}

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

#ifndef ANDROID
	const char *sync = getenv(FOSSILIZE_DUMP_SYNC_ENV);
	if (sync && strtoul(sync, nullptr, 0) != 0)
		synchronized = true;
#endif

	enablePrecompileQA = queryPrecompileQA();
}

StateRecorder *Instance::getStateRecorderForDevice(const VkPhysicalDeviceProperties2 *props,
                                                   const VkApplicationInfo *appInfo,
                                                   const void *device_pnext)
{
	auto appInfoFeatureHash = Hashing::compute_application_feature_hash(appInfo, device_pnext);
	auto hash = Hashing::compute_combined_application_feature_hash(appInfoFeatureHash);

	std::lock_guard<std::mutex> lock(recorderLock);
	auto itr = globalRecorders.find(hash);
	if (itr != end(globalRecorders))
		return itr->second.recorder.get();

	auto &entry = globalRecorders[hash];

	std::string serializationPath;
	std::string lastUsePath;
	const char *extraPaths = nullptr;
#ifdef ANDROID
	serializationPath = "/sdcard/fossilize";
	auto logPath = getSystemProperty("debug.fossilize.dump_path");
	if (!logPath.empty())
	{
		serializationPath = logPath;
		LOGI("Overriding serialization path: \"%s\".\n", logPath.c_str());
	}
#else
	serializationPath = "fossilize";
	const char *path = getenv(FOSSILIZE_DUMP_PATH_ENV);
	if (path)
	{
		serializationPath = path;
		LOGI("Overriding serialization path: \"%s\".\n", path);
	}
	extraPaths = getenv(FOSSILIZE_DUMP_PATH_READ_ONLY_ENV);
#endif

	const char *lastUseTag = getenv(FOSSILIZE_LAST_USE_TAG_ENV);

	bool needsBucket = infoFilter && infoFilter->needs_buckets(appInfo);
	shouldRecordImmutableSamplers = !infoFilter || infoFilter->should_record_immutable_samplers(appInfo);

	// Don't write a bucket if we're going to filter out the application.
	if (needsBucket && appInfo && infoFilter && !infoFilter->test_application_info(appInfo))
		needsBucket = false;

	char hashString[17];
	sprintf(hashString, "%016" PRIx64, hash);

	// Try to normalize the path layouts for last use.
	// Without buckets:
	//  Write part: path.$suffix.$feature-hash.$counter.foz
	//  Read part: path.$suffix.$feature-hash.foz
	// With buckets:
	//  Write part: path.$bucket/path.$suffix.$feature-hash.$counter.foz
	//  Read part: path.$bucket/path.$suffix.$feature-hash.foz

	if (lastUseTag && !serializationPath.empty() && !needsBucket)
	{
		lastUsePath = serializationPath + '.' + lastUseTag;
		lastUsePath += '.';
		lastUsePath += hashString;
	}

	if (!serializationPath.empty() && !needsBucket)
	{
		serializationPath += '.';
		serializationPath += hashString;
	}

	entry.interface.reset(create_concurrent_database_with_encoded_extra_paths(serializationPath.c_str(),
	                                                                          DatabaseMode::Append,
	                                                                          extraPaths));

	if (lastUseTag)
	{
		entry.last_use_interface.reset(create_concurrent_database(
				lastUsePath.empty() ? serializationPath.c_str() : lastUsePath.c_str(),
				DatabaseMode::OverWrite, nullptr, 0));
	}

	if (needsBucket && infoFilter)
	{
		char bucketPath[17];
		Hash bucketHash = infoFilter->get_bucket_hash(props, appInfo, device_pnext);
		sprintf(bucketPath, "%016" PRIx64, bucketHash);

		// For convenience. Makes filenames similar in top-level directory and bucket directories.
		auto basename = Path::basename(serializationPath);
		auto prefix = basename;
		if (!prefix.empty())
			prefix += '.';
		prefix += hashString;

		entry.interface->set_bucket_path(bucketPath, prefix.c_str());

		if (entry.last_use_interface)
		{
			prefix = basename;
			if (!prefix.empty())
			{
				prefix += '.';
				prefix += lastUseTag;
				prefix += '.';
			}
			prefix += hashString;

			entry.last_use_interface->set_bucket_path(bucketPath, prefix.c_str());
		}
	}
	else
		needsBucket = false;

	if (const char *identifierPath = getenv(FOSSILIZE_IDENTIFIER_DUMP_PATH_ENV))
	{
		// If the application is using shader module identifiers, we need to save those as well as sideband information.
		// This allows us to resolve identifiers later.
		auto *identifier =
				static_cast<const VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT *>(
						findpNext(device_pnext,
						          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT));

		auto *identifierProps =
				static_cast<const VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT *>(
						findpNext(props, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT));

		if (identifier && identifierProps && identifier->shaderModuleIdentifier)
		{
			char uuidString[2 * VK_UUID_SIZE + 1];
			for (unsigned i = 0; i < VK_UUID_SIZE; i++)
				sprintf(uuidString + 2 * i, "%02x", identifierProps->shaderModuleIdentifierAlgorithmUUID[i]);

			std::string identifierDatabasePath = identifierPath;
			identifierDatabasePath += '.';
			identifierDatabasePath += uuidString;

			entry.module_identifier_interface.reset(
					create_concurrent_database(identifierDatabasePath.c_str(),
					                           DatabaseMode::AppendWithReadOnlyAccess,
					                           nullptr, 0));
		}
	}

	entry.recorder.reset(new StateRecorder);
	auto *recorder = entry.recorder.get();
	recorder->set_database_enable_compression(true);
	recorder->set_database_enable_checksum(true);
	recorder->set_application_info_filter(infoFilter);

	// Feature links are somewhat irrelevant if we're using bucket mechanism.
	if (needsBucket)
		recorder->set_database_enable_application_feature_links(false);

	if (appInfo)
		if (!recorder->record_application_info(*appInfo))
			LOGE_LEVEL("Failed to record application info.\n");
	if (device_pnext)
		if (!recorder->record_physical_device_features(device_pnext))
			LOGE_LEVEL("Failed to record physical device features.\n");

	recorder->set_module_identifier_database_interface(entry.module_identifier_interface.get());
	recorder->set_on_use_database_interface(entry.last_use_interface.get());

	if (synchronized)
		recorder->init_recording_synchronized(entry.interface.get());
	else
		recorder->init_recording_thread(entry.interface.get());

	return recorder;
}

}
