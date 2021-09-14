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

#include <stdint.h>

namespace Fossilize
{
enum ResourceTag
{
	RESOURCE_APPLICATION_INFO = 0,
	RESOURCE_SAMPLER = 1,
	RESOURCE_DESCRIPTOR_SET_LAYOUT = 2,
	RESOURCE_PIPELINE_LAYOUT = 3,
	RESOURCE_SHADER_MODULE = 4,
	RESOURCE_RENDER_PASS = 5,
	RESOURCE_GRAPHICS_PIPELINE = 6,
	RESOURCE_COMPUTE_PIPELINE = 7,
	RESOURCE_APPLICATION_BLOB_LINK = 8,
	RESOURCE_RAYTRACING_PIPELINE = 9,
	RESOURCE_COUNT = 10
};

enum
{
	FOSSILIZE_FORMAT_VERSION = 6,
	FOSSILIZE_FORMAT_MIN_COMPAT_VERSION = 5
};

enum LogLevel
{
	// Log everything
	LOG_INFO = 0,
	LOG_ALL = LOG_INFO,
	// Only report warnings and up, info messages are ignored
	LOG_WARNING = 1,
	LOG_DEFAULT = LOG_WARNING,
	// Only report errors and up, warnings are ignored
	LOG_ERROR = 2,
	// No logging at all, only used as a log level
	LOG_NONE = 3
};

using Hash = uint64_t;
}
