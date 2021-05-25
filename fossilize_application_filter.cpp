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

#include "fossilize_application_filter.hpp"
#include "layer/utils.hpp"
#include "vulkan.h"
#include <mutex>
#include <condition_variable>
#include <future>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <stdio.h>

#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/writer.h"
using namespace rapidjson;

namespace Fossilize
{
enum { FOSSILIZE_APPLICATION_INFO_FILTER_VERSION = 1 };

struct AppInfo
{
	uint32_t minimum_api_version = VK_MAKE_VERSION(1, 0, 0);
	uint32_t minimum_application_version = 0;
	uint32_t minimum_engine_version = 0;
};

struct ApplicationInfoFilter::Impl
{
	~Impl();
	std::unordered_set<std::string> blacklisted_application_names;
	std::unordered_set<std::string> blacklisted_engine_names;
	std::unordered_map<std::string, AppInfo> application_infos;
	std::unordered_map<std::string, AppInfo> engine_infos;

	bool parsing_done = false;
	bool parsing_success = false;
	std::future<void> task;

	void parse_async(const char *path);
	bool test_application_info(const VkApplicationInfo *info);
	bool parse(const std::string &path);
	bool check_success();
};

bool ApplicationInfoFilter::Impl::check_success()
{
	if (task.valid())
		task.wait();
	return parsing_success;
}

bool ApplicationInfoFilter::Impl::test_application_info(const VkApplicationInfo *info)
{
	// No app info, just assume true.
	if (!info)
		return true;

	if (task.valid())
		task.wait();

	if (!parsing_success)
	{
		LOGE("Failed to parse ApplicationInfoFilter, letting recording go through.\n");
		return true;
	}

	// First, check for blacklists.
	if (info->pApplicationName && blacklisted_application_names.count(info->pApplicationName))
	{
		LOGI("pApplicationName %s is blacklisted for recording. Skipping.\n", info->pApplicationName);
		return false;
	}

	if (info->pEngineName && blacklisted_engine_names.count(info->pEngineName))
	{
		LOGI("pEngineName %s is blacklisted for recording. Skipping.\n", info->pEngineName);
		return false;
	}

	// Check versioning for applicationName.
	if (info->pApplicationName)
	{
		auto itr = application_infos.find(info->pApplicationName);
		if (itr != application_infos.end())
		{
			if (info->applicationVersion < itr->second.minimum_application_version)
			{
				LOGI("applicationVersion %u is too low for pApplicationName %s. Skipping.\n",
				     info->applicationVersion, info->pApplicationName);
				return false;
			}

			if (info->apiVersion < itr->second.minimum_api_version)
			{
				LOGI("apiVersion %u is too low for pApplicationName %s. Skipping.\n",
				     info->apiVersion, info->pApplicationName);
				return false;
			}
		}
	}

	// Check versioning for engineName.
	if (info->pEngineName)
	{
		auto itr = engine_infos.find(info->pEngineName);
		if (itr != engine_infos.end())
		{
			if (info->engineVersion < itr->second.minimum_engine_version)
			{
				LOGI("engineVersion %u is too low for pEngineName %s. Skipping.\n",
				     info->engineVersion, info->pEngineName);
				return false;
			}

			if (info->apiVersion < itr->second.minimum_api_version)
			{
				LOGI("apiVersion %u is too low for pEngineName %s. Skipping.\n",
				     info->apiVersion, info->pEngineName);
				return false;
			}
		}
	}

	// We didn't fail any filter, so we should record.
	return true;
}

static std::vector<char> read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (!file)
		return {};

	fseek(file, 0, SEEK_END);
	long len = ftell(file);

	if (len < 0)
	{
		fclose(file);
		return {};
	}

	rewind(file);
	std::vector<char> buffer(len);
	if (fread(buffer.data(), 1, len, file) != size_t(len))
	{
		fclose(file);
		return {};
	}

	fclose(file);
	return buffer;
}

static const Value *get_safe_member(const Value &value, const char *member)
{
	auto memb = value.FindMember(member);
	if (memb == value.MemberEnd())
	{
		LOGE("Member not found.\n");
		return nullptr;
	}
	else
		return &memb->value;
}

static const Value *maybe_get_member(const Value &value, const char *member)
{
	auto memb = value.FindMember(member);
	if (memb == value.MemberEnd())
		return nullptr;
	else
		return &memb->value;
}

static bool get_safe_member_string(const Value &value, const char *member, std::string &out_value)
{
	auto *memb = get_safe_member(value, member);
	if (memb && memb->IsString())
	{
		const char *str = memb->GetString();
		out_value = std::string(str, str + memb->GetStringLength());
		return true;
	}
	else
	{
		LOGE("Not a string.\n");
		return false;
	}
}

static bool get_safe_member_int(const Value &value, const char *member, int &out_value)
{
	auto *memb = get_safe_member(value, member);
	if (memb && memb->IsInt())
	{
		out_value = memb->GetInt();
		return true;
	}
	else
	{
		LOGE("Not an int.\n");
		return false;
	}
}

static unsigned default_get_member_uint(const Value &value, const char *member, unsigned default_value = 0)
{
	auto *memb = maybe_get_member(value, member);
	if (!memb)
		return default_value;
	if (!memb->IsUint())
		return default_value;
	return memb->GetUint();
}

static bool add_blacklists(std::unordered_set<std::string> &output, const Value *blacklist)
{
	if (!blacklist->IsArray())
	{
		LOGE("Not an array.\n");
		return false;
	}

	for (auto itr = blacklist->Begin(); itr != blacklist->End(); ++itr)
	{
		if (!itr->IsString())
		{
			LOGE("Not a string.\n");
			return false;
		}

		const char *str = itr->GetString();
		output.insert(std::string(str, str + itr->GetStringLength()));
	}

	return true;
}

static bool add_application_filters(std::unordered_map<std::string, AppInfo> &output, const Value *filters)
{
	if (!filters->IsObject())
	{
		LOGE("Not an object.\n");
		return false;
	}

	for (auto itr = filters->MemberBegin(); itr != filters->MemberEnd(); ++itr)
	{
		if (!itr->value.IsObject())
		{
			LOGE("Not an object.\n");
			return false;
		}

		AppInfo info;

		auto &value = itr->value;
		info.minimum_api_version = default_get_member_uint(value, "minimumApiVersion");
		info.minimum_engine_version = default_get_member_uint(value, "minimumEngineVersion");
		info.minimum_application_version = default_get_member_uint(value, "minimumApplicationVersion");
		output[itr->name.GetString()] = info;
	}

	return true;
}

bool ApplicationInfoFilter::Impl::parse(const std::string &path)
{
	auto buffer = read_file(path.c_str());

	Document doc;
	doc.Parse(buffer.data(), buffer.size());
	if (doc.HasParseError())
		return false;

	std::string json_str;
	int json_int;

	if (!get_safe_member_string(doc, "asset", json_str) || json_str != "FossilizeApplicationInfoFilter")
		return false;
	if (!get_safe_member_int(doc, "version", json_int) || json_int != FOSSILIZE_APPLICATION_INFO_FILTER_VERSION)
		return false;

	auto *blacklist = maybe_get_member(doc, "blacklistedApplicationNames");
	if (blacklist)
		if (!add_blacklists(blacklisted_application_names, blacklist))
			return false;
	blacklist = maybe_get_member(doc, "blacklistedEngineNames");
	if (blacklist)
		if (!add_blacklists(blacklisted_engine_names, blacklist))
			return false;

	auto *filters = maybe_get_member(doc, "applicationFilters");
	if (filters)
		if (!add_application_filters(application_infos, filters))
			return false;
	filters = maybe_get_member(doc, "engineFilters");
	if (filters)
		if (!add_application_filters(engine_infos, filters))
			return false;

	return true;
}

void ApplicationInfoFilter::Impl::parse_async(const char *path_)
{
	std::string path = path_;
	task = std::async(std::launch::async, [this, path]() {
		bool ret = parse(path);
		parsing_success = ret;
		parsing_done = true;
	});
}

ApplicationInfoFilter::Impl::~Impl()
{
	if (task.valid())
		task.wait();
}

ApplicationInfoFilter::ApplicationInfoFilter()
{
	impl = new Impl;
}

void ApplicationInfoFilter::parse_async(const char *path)
{
	impl->parse_async(path);
}

bool ApplicationInfoFilter::test_application_info(const VkApplicationInfo *info)
{
	return impl->test_application_info(info);
}

bool ApplicationInfoFilter::check_success()
{
	return impl->check_success();
}

ApplicationInfoFilter::~ApplicationInfoFilter()
{
	delete impl;
}
}
