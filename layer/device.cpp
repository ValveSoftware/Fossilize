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

void Device::init(VkPhysicalDevice gpu, VkDevice device, VkLayerInstanceDispatchTable *pInstanceTable,
                  VkLayerDispatchTable *pTable)
{
	this->gpu = gpu;
	this->device = device;
	this->pInstanceTable = pInstanceTable;
	this->pTable = pTable;

#ifdef ANDROID
	auto logPath = getSystemProperty("debug.fossilize.dump_path");
	if (!logPath.empty())
	{
		serializationPath = logPath;
		LOGI("Overriding serialization path: \"%s\".\n", logPath.c_str());
	}

	auto paranoid = getSystemProperty("debug.fossilize.paranoid_mode");
	if (!paranoid.empty() && strtoul(paranoid.c_str(), nullptr, 0) != 0)
	{
		paranoidMode = true;
		LOGI("Enabling paranoid serialization mode.\n");
	}
#else
	const char *path = getenv("STEAM_FOSSILIZE_DUMP_PATH");
	if (path)
	{
		serializationPath = path;
		LOGI("Overriding serialization path: \"%s\".\n", path);
	}

	const char *paranoid = getenv("STEAM_FOSSILIZE_PARANOID_MODE");
	if (paranoid && strtoul(paranoid, nullptr, 0) != 0)
	{
		paranoidMode = true;
		LOGI("Enabling paranoid serialization mode.\n");
	}
#endif

#ifndef _WIN32
#if ANDROID
	auto sigsegv = getSystemProperty("debug.fossilize.dump_sigsegv");
	if (!sigsegv.empty() && strtoul(sigsegv.c_str(), nullptr, 0) != 0)
		installSegfaultHandler();
#else
	const char *sigsegv = getenv("STEAM_FOSSILIZE_DUMP_SIGSEGV");
	if (sigsegv && strtoul(sigsegv, nullptr, 0) != 0)
		installSegfaultHandler();
#endif
#endif

	recorder.init(serializationPath);
}

#ifndef _WIN32
static Device *segfaultDevice = nullptr;

static void segfaultHandler(int, siginfo_t *, void *)
{
	LOGE("Caught segmentation fault!");

	// Now we can die properly.
	kill(getpid(), SIGSEGV);
}

void Device::installSegfaultHandler()
{
	segfaultDevice = this;

	struct sigaction sa;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = segfaultHandler;

	if (sigaction(SIGSEGV, &sa, nullptr) < 0)
		LOGE("Failed to install SIGSEGV handler!\n");
}
#endif

}
