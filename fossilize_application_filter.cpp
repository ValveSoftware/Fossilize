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
#include "fossilize_hasher.hpp"
#include "layer/utils.hpp"
#include "vulkan.h"
#include <future>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <stdio.h>

#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/writer.h"
using namespace rapidjson;

namespace Fossilize
{
enum { FOSSILIZE_APPLICATION_INFO_FILTER_VERSION = 2 };

struct EnvInfo
{
	std::string env;
	std::string contains;
	std::string equals;
	bool nonnull = false;
};

enum class VariantDependency
{
	VendorID,
	MutableDescriptorType,
	BindlessUBO,
	BufferDeviceAddress,
	ApplicationVersion,
	EngineVersion,
	ApplicationVersionMajor,
	ApplicationVersionMinor,
	ApplicationVersionPatch,
	EngineVersionMajor,
	EngineVersionMinor,
	EngineVersionPatch,
	ApplicationName,
	EngineName
};

struct VariantDependencyMap
{
	const char *env;
	VariantDependency dep;
};
#define DEF(x) { #x, VariantDependency::x }
static const VariantDependencyMap variant_dependency_map[] = {
	DEF(VendorID),
	DEF(MutableDescriptorType),
	DEF(BindlessUBO),
	DEF(BufferDeviceAddress),
	DEF(ApplicationVersion),
	DEF(EngineVersion),
	DEF(ApplicationVersionMajor),
	DEF(ApplicationVersionMinor),
	DEF(ApplicationVersionPatch),
	DEF(EngineVersionMajor),
	DEF(EngineVersionMinor),
	DEF(EngineVersionPatch),
	DEF(ApplicationName),
	DEF(EngineName),
};
#undef DEF

struct AppInfo
{
	uint32_t minimum_api_version = VK_MAKE_VERSION(1, 0, 0);
	uint32_t minimum_application_version = 0;
	uint32_t minimum_engine_version = 0;
	std::vector<EnvInfo> env_infos;
	std::vector<VariantDependency> variant_dependencies;
};

struct ApplicationInfoFilter::Impl
{
	~Impl();
	std::unordered_set<std::string> blacklisted_application_names;
	std::unordered_set<std::string> blacklisted_engine_names;
	std::unordered_map<std::string, AppInfo> application_infos;
	std::unordered_map<std::string, AppInfo> engine_infos;
	std::vector<VariantDependency> default_variant_dependencies;

	bool parsing_done = false;
	bool parsing_success = false;
	std::future<void> task;

	void parse_async(const char *path);
	bool test_application_info(const VkApplicationInfo *info);
	bool filter_env_info(const EnvInfo &info) const;
	bool parse(const std::string &path);
	bool check_success();

	bool needs_buckets(const VkApplicationInfo *info);
	Hash get_bucket_hash(const VkPhysicalDeviceProperties2 *props,
	                     const VkApplicationInfo *info,
	                     const VkPhysicalDeviceFeatures2 *features2);

	const char *(*getenv_wrapper)(const char *, void *) = nullptr;
	void *getenv_userdata = nullptr;
};

bool ApplicationInfoFilter::Impl::check_success()
{
	if (task.valid())
		task.wait();
	return parsing_success;
}

bool ApplicationInfoFilter::Impl::filter_env_info(const EnvInfo &info) const
{
	if (!getenv_wrapper)
		return false;

	const char *env = getenv_wrapper(info.env.c_str(), getenv_userdata);
	if (!env)
		return false;

	if (info.nonnull)
		return true;
	else if (!info.equals.empty() && info.equals == env)
		return true;
	else if (!info.contains.empty() && strstr(env, info.contains.c_str()))
		return true;
	else
		return false;
}

bool ApplicationInfoFilter::Impl::needs_buckets(const VkApplicationInfo *info)
{
	if (task.valid())
		task.wait();
	if (!parsing_success)
		return false;

	if (info && info->pApplicationName)
	{
		auto itr = application_infos.find(info->pApplicationName);
		if (itr != application_infos.end() && !itr->second.variant_dependencies.empty())
			return true;
	}

	if (info && info->pEngineName)
	{
		auto itr = engine_infos.find(info->pEngineName);
		if (itr != engine_infos.end() && !itr->second.variant_dependencies.empty())
			return true;
	}

	return !default_variant_dependencies.empty();
}

template <typename T>
static inline const T *find_pnext(VkStructureType type, const void *pNext)
{
	while (pNext)
	{
		auto *sin = static_cast<const VkBaseInStructure *>(pNext);
		if (sin->sType == type)
			return static_cast<const T*>(pNext);

		pNext = sin->pNext;
	}

	return nullptr;
}

static void hash_variant(Hasher &h, VariantDependency dep,
                         const VkPhysicalDeviceProperties2 *props,
                         const VkApplicationInfo *info,
                         const VkPhysicalDeviceFeatures2 *features2)
{
	const VkApplicationInfo default_app_info = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	if (!info)
		info = &default_app_info;

	switch (dep)
	{
	case VariantDependency::VendorID:
		h.u32(props ? props->properties.vendorID : 0);
		break;

	case VariantDependency::MutableDescriptorType:
	{
		auto *mut = find_pnext<VkPhysicalDeviceMutableDescriptorTypeFeaturesVALVE>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_VALVE,
				features2 ? features2->pNext : nullptr);
		h.u32(uint32_t(mut && mut->mutableDescriptorType));
		break;
	}

	case VariantDependency::BufferDeviceAddress:
	{
		auto *bda = find_pnext<VkPhysicalDeviceBufferDeviceAddressFeatures>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
				features2 ? features2->pNext : nullptr);
		auto *features12 = find_pnext<VkPhysicalDeviceVulkan12Features>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
				features2 ? features2->pNext : nullptr);

		bool enabled = (bda && bda->bufferDeviceAddress) ||
		               (features12 && features12->bufferDeviceAddress);
		h.u32(uint32_t(enabled));
		break;
	}

	case VariantDependency::BindlessUBO:
	{
		auto *indexing = find_pnext<VkPhysicalDeviceDescriptorIndexingFeatures>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
				features2 ? features2->pNext : nullptr);
		auto *features12 = find_pnext<VkPhysicalDeviceVulkan12Features>(
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
				features2 ? features2->pNext : nullptr);
		bool enabled = (indexing && indexing->descriptorBindingUniformBufferUpdateAfterBind) ||
		               (features12 && features12->descriptorBindingUniformBufferUpdateAfterBind);
		h.u32(uint32_t(enabled));
		break;
	}

	case VariantDependency::ApplicationVersion:
		h.u32(info->applicationVersion);
		break;

	case VariantDependency::ApplicationVersionMajor:
		h.u32(VK_VERSION_MAJOR(info->applicationVersion));
		break;

	case VariantDependency::ApplicationVersionMinor:
		h.u32(VK_VERSION_MINOR(info->applicationVersion));
		break;

	case VariantDependency::ApplicationVersionPatch:
		h.u32(VK_VERSION_PATCH(info->applicationVersion));
		break;

	case VariantDependency::EngineVersion:
		h.u32(info->engineVersion);
		break;

	case VariantDependency::EngineVersionMajor:
		h.u32(VK_VERSION_MAJOR(info->engineVersion));
		break;

	case VariantDependency::EngineVersionMinor:
		h.u32(VK_VERSION_MINOR(info->engineVersion));
		break;

	case VariantDependency::EngineVersionPatch:
		h.u32(VK_VERSION_PATCH(info->engineVersion));
		break;

	case VariantDependency::ApplicationName:
		h.string(info->pApplicationName ? info->pApplicationName : "");
		break;

	case VariantDependency::EngineName:
		h.string(info->pEngineName ? info->pEngineName : "");
		break;

	default:
		break;
	}
}

Hash ApplicationInfoFilter::Impl::get_bucket_hash(const VkPhysicalDeviceProperties2 *props,
                                                  const VkApplicationInfo *info,
                                                  const VkPhysicalDeviceFeatures2 *features2)
{
	if (task.valid())
		task.wait();
	if (!parsing_success)
		return 0;

	Hasher h;
	bool use_default_variant = true;

	h.u32(0);
	if (info && info->pApplicationName)
	{
		auto itr = application_infos.find(info->pApplicationName);
		if (itr != application_infos.end())
		{
			use_default_variant = false;
			for (auto &dep : itr->second.variant_dependencies)
				hash_variant(h, dep, props, info, features2);
		}
	}

	h.u32(0);
	if (info && info->pEngineName)
	{
		auto itr = engine_infos.find(info->pEngineName);
		if (itr != engine_infos.end())
		{
			use_default_variant = false;
			for (auto &dep : itr->second.variant_dependencies)
				hash_variant(h, dep, props, info, features2);
		}
	}

	if (use_default_variant)
		for (auto &dep : default_variant_dependencies)
			hash_variant(h, dep, props, info, features2);

	return h.get();
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

			for (auto &env_info : itr->second.env_infos)
			{
				if (filter_env_info(env_info))
				{
					LOGI("Skipping recording due to environment rule for: %s.\n", env_info.env.c_str());
					return false;
				}
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

			for (auto &env_info : itr->second.env_infos)
			{
				if (filter_env_info(env_info))
				{
					LOGI("Skipping recording due to environment rule for: %s.\n", env_info.env.c_str());
					return false;
				}
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

static bool parse_blacklist_environments(const Value &envs, std::vector<EnvInfo> &infos)
{
	if (!envs.IsObject())
	{
		LOGE("blacklistedEnvironments must be an object.\n");
		return false;
	}

	infos.clear();

	for (auto itr = envs.MemberBegin(); itr != envs.MemberEnd(); ++itr)
	{
		auto &elem = itr->value;
		if (!elem.IsObject())
		{
			LOGE("blacklistEnvironment element must be object.\n");
			return false;
		}

		EnvInfo env_info;

		env_info.env = itr->name.GetString();
		if (elem.HasMember("contains") && elem["contains"].IsString())
			env_info.contains = elem["contains"].GetString();
		if (elem.HasMember("equals") && elem["equals"].IsString())
			env_info.equals = elem["equals"].GetString();
		if (elem.HasMember("nonnull") && elem["nonnull"].IsBool())
			env_info.nonnull = elem["nonnull"].GetBool();

		infos.push_back(std::move(env_info));
	}

	return true;
}

static bool parse_bucket_variant_dependencies(const Value &deps, std::vector<VariantDependency> &variant_deps)
{
	if (!deps.IsArray())
	{
		LOGE("bucketVariantDependencies must be an array.\n");
		return false;
	}

	variant_deps.clear();

	for (auto itr = deps.Begin(); itr != deps.End(); ++itr)
	{
		auto &elem = *itr;
		if (!elem.IsString())
			return false;

		const char *str = elem.GetString();

		auto find_itr = std::find_if(std::begin(variant_dependency_map), std::end(variant_dependency_map),
		                             [str](const VariantDependencyMap &m) { return strcmp(str, m.env) == 0; });
		if (find_itr != std::end(variant_dependency_map))
			variant_deps.push_back(find_itr->dep);
		else
			LOGW("Couldn't find variant dependency for %s, ignoring.\n", str);
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
		if (value.HasMember("blacklistedEnvironments"))
			if (!parse_blacklist_environments(value["blacklistedEnvironments"], info.env_infos))
				return false;
		if (value.HasMember("bucketVariantDependencies"))
			if (!parse_bucket_variant_dependencies(value["bucketVariantDependencies"], info.variant_dependencies))
				return false;
		output[itr->name.GetString()] = std::move(info);
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

	if (!get_safe_member_int(doc, "version", json_int) || json_int > FOSSILIZE_APPLICATION_INFO_FILTER_VERSION)
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

	auto *default_variants = maybe_get_member(doc, "defaultBucketVariantDependencies");
	if (default_variants)
		if (!parse_bucket_variant_dependencies(*default_variants, default_variant_dependencies))
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

bool ApplicationInfoFilter::needs_buckets(const VkApplicationInfo *info)
{
	return impl->needs_buckets(info);
}

Hash ApplicationInfoFilter::get_bucket_hash(const VkPhysicalDeviceProperties2 *props,
                                            const VkApplicationInfo *info,
                                            const VkPhysicalDeviceFeatures2 *features2)
{
	return impl->get_bucket_hash(props, info, features2);
}

bool ApplicationInfoFilter::check_success()
{
	return impl->check_success();
}

void ApplicationInfoFilter::set_environment_resolver(const char *(*getenv_wrapper)(const char *, void *), void *userdata)
{
	impl->getenv_wrapper = getenv_wrapper;
	impl->getenv_userdata = userdata;
}

ApplicationInfoFilter::~ApplicationInfoFilter()
{
	delete impl;
}
}
