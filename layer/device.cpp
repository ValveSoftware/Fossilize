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

#include "device.hpp"
#include "instance.hpp"
#include "path.hpp"
#include "utils.hpp"
#include <cinttypes>
#include <stdlib.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

namespace Fossilize
{
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

void Device::init(VkPhysicalDevice gpu_, VkDevice device_, Instance *pInstance,
                  const VkPhysicalDeviceFeatures2 &features,
                  VkLayerDispatchTable *pTable_)
{
	gpu = gpu_;
	device = device_;
	pInstanceTable = pInstance->getTable();
	pTable = pTable_;

#ifdef ANDROID
	auto logPath = getSystemProperty("debug.fossilize.dump_path");
	if (!logPath.empty())
	{
		serializationPath = logPath;
		LOGI("Overriding serialization path: \"%s\".\n", logPath.c_str());
	}
#else
	const char *path = getenv("STEAM_FOSSILIZE_DUMP_PATH");
	if (path)
	{
		serializationPath = path;
		LOGI("Overriding serialization path: \"%s\".\n", path);
	}
#endif

	recorder = Instance::getStateRecorderForDevice(serializationPath.c_str(), pInstance->getApplicationInfo(), &features);
}
}
