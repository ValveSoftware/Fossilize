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
#include <mutex>
#include <unordered_map>
#include <memory>

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
	std::unique_ptr<DatabaseInterface> interface;
	std::unique_ptr<StateRecorder> recorder;
};
static std::unordered_map<Hash, Recorder> globalRecorders;

StateRecorder *Instance::getStateRecorderForDevice(const char *basePath, const VkApplicationInfo *appInfo, const VkPhysicalDeviceFeatures2 *features)
{
	auto appInfoFeatureHash = Hashing::compute_application_feature_hash(appInfo, features);
	auto hash = Hashing::compute_combined_application_feature_hash(appInfoFeatureHash);

	std::lock_guard<std::mutex> lock(recorderLock);
	auto itr = globalRecorders.find(hash);
	if (itr != end(globalRecorders))
		return itr->second.recorder.get();

	auto &entry = globalRecorders[hash];

	std::string path = basePath;
	char hashString[17];
	sprintf(hashString, "%016llx", static_cast<unsigned long long>(hash));
	if (!path.empty())
		path += ".";
	path += hashString;
	entry.interface.reset(create_concurrent_database(path.c_str()));

	auto *recorder = new StateRecorder;
	entry.recorder.reset(recorder);
	recorder->set_database_enable_compression(true);
	recorder->set_database_enable_checksum(true);
	if (appInfo)
		recorder->record_application_info(*appInfo);
	if (features)
		recorder->record_physical_device_features(*features);
	recorder->init_recording_thread(entry.interface.get());
	return recorder;
}

}
