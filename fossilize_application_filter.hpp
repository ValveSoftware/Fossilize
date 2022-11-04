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
#include "fossilize_types.hpp"

struct VkApplicationInfo;
struct VkPhysicalDeviceFeatures2;
struct VkPhysicalDeviceProperties2;

// Allows us to blacklist which applications and which app/engine-versions we don't want to capture.
namespace Fossilize
{
class ApplicationInfoFilter
{
public:
	~ApplicationInfoFilter();
	void operator=(const ApplicationInfoFilter &) = delete;
	ApplicationInfoFilter(const ApplicationInfoFilter &) = delete;

	// Path to a JSON file.
	// Called by layer when an instance is created.
	static ApplicationInfoFilter *parse(const char *path,
	                                    const char *(*getenv_wrapper)(const char *, void *),
	                                    void *userdata);

	// Tests if application should be recorded.
	// Blocks until parsing is complete. Called by recording thread when preparing for recording.
	bool test_application_info(const VkApplicationInfo *info);

	bool needs_buckets(const VkApplicationInfo *info);
	Hash get_bucket_hash(const VkPhysicalDeviceProperties2 *props,
	                     const VkApplicationInfo *info,
	                     const void *device_pnext);
	bool should_record_immutable_samplers(const VkApplicationInfo *info);

private:
	ApplicationInfoFilter();
	struct Impl;
	Impl *impl;
};
}

