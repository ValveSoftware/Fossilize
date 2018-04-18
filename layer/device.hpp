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

#include "dispatch_helper.hpp"
#include "fossilize.hpp"

namespace Fossilize
{
class Device
{
public:
	void init(VkPhysicalDevice gpu, VkDevice device,
	          VkLayerInstanceDispatchTable *pInstanceTable,
	          VkLayerDispatchTable *pTable);

	VkLayerDispatchTable *getTable()
	{
		return pTable;
	}

	StateRecorder &getRecorder()
	{
		return recorder;
	}

	void serializeToPath(const std::string &path);
	const std::string &getSerializationPath() const
	{
		return serializationPath;
	}

	bool isParanoid() const
	{
		return paranoidMode;
	}

private:
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkLayerInstanceDispatchTable *pInstanceTable = nullptr;
	VkLayerDispatchTable *pTable = nullptr;

	StateRecorder recorder;

#ifdef ANDROID
	std::string serializationPath = "/sdcard/fossilize.json";
#else
	std::string serializationPath = "fossilize.json";
#endif

	bool paranoidMode = false;

#ifndef _WIN32
	void installSegfaultHandler();
#endif
};
}