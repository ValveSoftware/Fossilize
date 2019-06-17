/* Copyright (c) 2018-2019 Hans-Kristian Arntzen
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

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <stddef.h>
#include "fossilize.hpp"
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <string.h>
#include "varint.hpp"
#include "path.hpp"
#include "fossilize_db.hpp"
#include "layer/utils.hpp"
#include "fossilize_errors.hpp"
#include "fossilize_application_filter.hpp"

#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/writer.h"
using namespace rapidjson;


#ifdef PRETTY_WRITER
using CustomWriter = PrettyWriter<StringBuffer>;
#else
using CustomWriter = Writer<StringBuffer>;
#endif

using namespace std;

namespace Fossilize
{
class Hasher
{
public:
	explicit Hasher(Hash h_)
		: h(h_)
	{
	}

	Hasher() = default;

	template <typename T>
	inline void data(const T *data_, size_t size)
	{
		size /= sizeof(*data_);
		for (size_t i = 0; i < size; i++)
			h = (h * 0x100000001b3ull) ^ data_[i];
	}

	inline void u32(uint32_t value)
	{
		h = (h * 0x100000001b3ull) ^ value;
	}

	inline void s32(int32_t value)
	{
		u32(uint32_t(value));
	}

	inline void f32(float value)
	{
		union
		{
			float f32;
			uint32_t u32;
		} u;
		u.f32 = value;
		u32(u.u32);
	}

	inline void u64(uint64_t value)
	{
		u32(value & 0xffffffffu);
		u32(value >> 32);
	}

	template <typename T>
	inline void pointer(T *ptr)
	{
		u64(reinterpret_cast<uintptr_t>(ptr));
	}

	inline void string(const char *str)
	{
		char c;
		u32(0xff);
		while ((c = *str++) != '\0')
			u32(uint8_t(c));
	}

	inline void string(const std::string &str)
	{
		u32(0xff);
		for (auto &c : str)
			u32(uint8_t(c));
	}

	inline Hash get() const
	{
		return h;
	}

private:
	Hash h = 0xcbf29ce484222325ull;
};

template <typename T>
struct HashedInfo
{
	Hash hash;
	T info;
};

template <typename Allocator>
static Value uint64_string(uint64_t value, Allocator &alloc)
{
	char str[17]; // 16 digits + null
	sprintf(str, "%016llx", static_cast<unsigned long long>(value));
	return Value(str, alloc);
}

struct StateReplayer::Impl
{
	bool parse(StateCreatorInterface &iface, DatabaseInterface *resolver, const void *buffer, size_t size) FOSSILIZE_WARN_UNUSED;
	ScratchAllocator allocator;

	std::unordered_map<Hash, VkSampler> replayed_samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> replayed_descriptor_set_layouts;
	std::unordered_map<Hash, VkPipelineLayout> replayed_pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> replayed_shader_modules;
	std::unordered_map<Hash, VkRenderPass> replayed_render_passes;
	std::unordered_map<Hash, VkPipeline> replayed_compute_pipelines;
	std::unordered_map<Hash, VkPipeline> replayed_graphics_pipelines;

	void copy_handle_references(const Impl &impl);
	bool parse_samplers(StateCreatorInterface &iface, const Value &samplers) FOSSILIZE_WARN_UNUSED;
	bool parse_descriptor_set_layouts(StateCreatorInterface &iface, const Value &layouts) FOSSILIZE_WARN_UNUSED;
	bool parse_pipeline_layouts(StateCreatorInterface &iface, const Value &layouts) FOSSILIZE_WARN_UNUSED;
	bool parse_shader_modules(StateCreatorInterface &iface, const Value &modules, const uint8_t *varint, size_t varint_size) FOSSILIZE_WARN_UNUSED;
	bool parse_render_passes(StateCreatorInterface &iface, const Value &passes) FOSSILIZE_WARN_UNUSED;
	bool parse_compute_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines) FOSSILIZE_WARN_UNUSED;
	bool parse_graphics_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines) FOSSILIZE_WARN_UNUSED;
	bool parse_compute_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines, const Value &member) FOSSILIZE_WARN_UNUSED;
	bool parse_graphics_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines, const Value &member) FOSSILIZE_WARN_UNUSED;
	bool parse_application_info(StateCreatorInterface &iface, const Value &app_info, const Value &pdf_info) FOSSILIZE_WARN_UNUSED;
	bool parse_application_info_link(StateCreatorInterface &iface, const Value &link) FOSSILIZE_WARN_UNUSED;

	bool parse_push_constant_ranges(const Value &ranges, const VkPushConstantRange **out_ranges) FOSSILIZE_WARN_UNUSED;
	bool parse_set_layouts(const Value &layouts, const VkDescriptorSetLayout **out_layouts) FOSSILIZE_WARN_UNUSED;
	bool parse_descriptor_set_bindings(const Value &bindings, const VkDescriptorSetLayoutBinding **out_bindings) FOSSILIZE_WARN_UNUSED;
	bool parse_immutable_samplers(const Value &samplers, const VkSampler **out_sampler) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_attachments(const Value &attachments, const VkAttachmentDescription **out_attachments) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_dependencies(const Value &dependencies, const VkSubpassDependency **out_dependencies) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_subpasses(const Value &subpass, const VkSubpassDescription **out_descriptions) FOSSILIZE_WARN_UNUSED;
	bool parse_attachment(const Value &value, const VkAttachmentReference **out_references) FOSSILIZE_WARN_UNUSED;
	bool parse_attachments(const Value &attachments, const VkAttachmentReference **out_references) FOSSILIZE_WARN_UNUSED;
	bool parse_specialization_info(const Value &spec_info, const VkSpecializationInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_map_entries(const Value &map_entries, const VkSpecializationMapEntry **out_entries) FOSSILIZE_WARN_UNUSED;
	bool parse_viewports(const Value &viewports, const VkViewport **out_viewports) FOSSILIZE_WARN_UNUSED;
	bool parse_scissors(const Value &scissors, const VkRect2D **out_rects) FOSSILIZE_WARN_UNUSED;
	bool parse_pnext_chain(const Value &pnext, const void **out_pnext) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_input_state(const Value &state, const VkPipelineVertexInputStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_color_blend_state(const Value &state, const VkPipelineColorBlendStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_depth_stencil_state(const Value &state, const VkPipelineDepthStencilStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_state(const Value &state, const VkPipelineRasterizationStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_input_assembly_state(const Value &state, const VkPipelineInputAssemblyStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_multisample_state(const Value &state, const VkPipelineMultisampleStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_viewport_state(const Value &state, const VkPipelineViewportStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_dynamic_state(const Value &state, const VkPipelineDynamicStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_tessellation_state(const Value &state, const VkPipelineTessellationStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_stages(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &stages, const VkPipelineShaderStageCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_attributes(const Value &attributes, const VkVertexInputAttributeDescription **out_desc) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_bindings(const Value &bindings, const VkVertexInputBindingDescription **out_desc) FOSSILIZE_WARN_UNUSED;
	bool parse_blend_attachments(const Value &attachments, const VkPipelineColorBlendAttachmentState **out_state) FOSSILIZE_WARN_UNUSED;
	bool parse_tessellation_domain_origin_state(const Value &state, VkPipelineTessellationDomainOriginStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_input_divisor_state(const Value &state, VkPipelineVertexInputDivisorStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_depth_clip_state(const Value &state, VkPipelineRasterizationDepthClipStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_stream_state(const Value &state, VkPipelineRasterizationStateStreamCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_uints(const Value &attachments, const uint32_t **out_uints) FOSSILIZE_WARN_UNUSED;
	const char *duplicate_string(const char *str, size_t len);
	bool resolve_derivative_pipelines = true;
	bool resolve_shader_modules = true;

	template <typename T>
	T *copy(const T *src, size_t count);
};

struct WorkItem
{
	uint64_t handle;
	void *create_info;
	Hash custom_hash;
};

struct StateRecorder::Impl
{
	~Impl();
	void sync_thread();
	void record_end();

	ScratchAllocator allocator;
	ScratchAllocator temp_allocator;
	DatabaseInterface *database_iface = nullptr;
	ApplicationInfoFilter *application_info_filter = nullptr;

	std::unordered_map<Hash, VkDescriptorSetLayoutCreateInfo *> descriptor_sets;
	std::unordered_map<Hash, VkPipelineLayoutCreateInfo *> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModuleCreateInfo *> shader_modules;
	std::unordered_map<Hash, VkGraphicsPipelineCreateInfo *> graphics_pipelines;
	std::unordered_map<Hash, VkComputePipelineCreateInfo *> compute_pipelines;
	std::unordered_map<Hash, VkRenderPassCreateInfo *> render_passes;
	std::unordered_map<Hash, VkSamplerCreateInfo *> samplers;

	std::unordered_map<VkDescriptorSetLayout, Hash> descriptor_set_layout_to_hash;
	std::unordered_map<VkPipelineLayout, Hash> pipeline_layout_to_hash;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_hash;
	std::unordered_map<VkPipeline, Hash> graphics_pipeline_to_hash;
	std::unordered_map<VkPipeline, Hash> compute_pipeline_to_hash;
	std::unordered_map<VkRenderPass, Hash> render_pass_to_hash;
	std::unordered_map<VkSampler, Hash> sampler_to_hash;

	VkApplicationInfo *application_info = nullptr;
	VkPhysicalDeviceFeatures2 *physical_device_features = nullptr;
	StateRecorderApplicationFeatureHash application_feature_hash = {};

	bool copy_descriptor_set_layout(const VkDescriptorSetLayoutCreateInfo *create_info, ScratchAllocator &alloc, VkDescriptorSetLayoutCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_pipeline_layout(const VkPipelineLayoutCreateInfo *create_info, ScratchAllocator &alloc, VkPipelineLayoutCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_shader_module(const VkShaderModuleCreateInfo *create_info, ScratchAllocator &alloc, VkShaderModuleCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo *create_info, ScratchAllocator &alloc,
	                            const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
	                            VkGraphicsPipelineCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_compute_pipeline(const VkComputePipelineCreateInfo *create_info, ScratchAllocator &alloc,
	                           const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
	                           VkComputePipelineCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_sampler(const VkSamplerCreateInfo *create_info, ScratchAllocator &alloc,
	                  VkSamplerCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_render_pass(const VkRenderPassCreateInfo *create_info, ScratchAllocator &alloc,
	                      VkRenderPassCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_application_info(const VkApplicationInfo *app_info, ScratchAllocator &alloc, VkApplicationInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_physical_device_features(const VkPhysicalDeviceFeatures2 *pdf, ScratchAllocator &alloc, VkPhysicalDeviceFeatures2 **out_features) FOSSILIZE_WARN_UNUSED;

	bool copy_specialization_info(const VkSpecializationInfo *info, ScratchAllocator &alloc, const VkSpecializationInfo **out_info) FOSSILIZE_WARN_UNUSED;

	void *copy_pnext_struct(const VkPipelineTessellationDomainOriginStateCreateInfo *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineVertexInputDivisorStateCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineRasterizationDepthClipStateCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineRasterizationStateStreamCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;

	bool remap_sampler_handle(VkSampler sampler, VkSampler *out_sampler) const FOSSILIZE_WARN_UNUSED;
	bool remap_descriptor_set_layout_handle(VkDescriptorSetLayout layout, VkDescriptorSetLayout *out_layout) const FOSSILIZE_WARN_UNUSED;
	bool remap_pipeline_layout_handle(VkPipelineLayout layout, VkPipelineLayout *out_layout) const FOSSILIZE_WARN_UNUSED;
	bool remap_render_pass_handle(VkRenderPass render_pass, VkRenderPass *out_render_pass) const FOSSILIZE_WARN_UNUSED;
	bool remap_shader_module_handle(VkShaderModule shader_module, VkShaderModule *out_shader_module) const FOSSILIZE_WARN_UNUSED;
	bool remap_compute_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const FOSSILIZE_WARN_UNUSED;
	bool remap_graphics_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const FOSSILIZE_WARN_UNUSED;

	bool remap_descriptor_set_layout_ci(VkDescriptorSetLayoutCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_pipeline_layout_ci(VkPipelineLayoutCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_shader_module_ci(VkShaderModuleCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_graphics_pipeline_ci(VkGraphicsPipelineCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_compute_pipeline_ci(VkComputePipelineCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_sampler_ci(VkSamplerCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_render_pass_ci(VkRenderPassCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;

	bool serialize_application_info(std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_application_blob_link(Hash hash, ResourceTag tag, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	Hash get_application_link_hash(ResourceTag tag, Hash hash) const;
	bool register_application_link_hash(ResourceTag tag, Hash hash, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_sampler(Hash hash, const VkSamplerCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_render_pass(Hash hash, const VkRenderPassCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_shader_module(Hash hash, const VkShaderModuleCreateInfo &create_info, std::vector<uint8_t> &blob, ScratchAllocator &allocator) const FOSSILIZE_WARN_UNUSED;
	bool serialize_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;

	std::mutex record_lock;
	std::condition_variable record_cv;
	std::queue<WorkItem> record_queue;
	std::thread worker_thread;

	bool compression = false;
	bool checksum = false;

	void record_task(StateRecorder *recorder, bool looping);

	template <typename T>
	T *copy(const T *src, size_t count, ScratchAllocator &alloc);
	bool copy_pnext_chain(const void *pNext, ScratchAllocator &alloc, const void **out_pnext) FOSSILIZE_WARN_UNUSED;
};

// reinterpret_cast does not work reliably on MSVC 2013 for Vulkan objects.
template <typename T, typename U>
static inline T api_object_cast(U obj)
{
	static_assert(sizeof(T) == sizeof(U), "Objects are not of same size.");
	return (T)obj;
}

namespace Hashing
{
static Hash compute_hash_application_info(const VkApplicationInfo &info)
{
	Hasher h;
	h.u32(info.applicationVersion);
	h.u32(info.apiVersion);
	h.u32(info.engineVersion);

	if (info.pApplicationName)
		h.string(info.pApplicationName);
	else
		h.u32(0);

	if (info.pEngineName)
		h.string(info.pEngineName);
	else
		h.u32(0);

	return h.get();
}

static Hash compute_hash_physical_device_features(const VkPhysicalDeviceFeatures2 &pdf)
{
	Hasher h;
	h.u32(pdf.features.robustBufferAccess);
	return h.get();
}

StateRecorderApplicationFeatureHash compute_application_feature_hash(const VkApplicationInfo *info,
                                                                     const VkPhysicalDeviceFeatures2 *features)
{
	StateRecorderApplicationFeatureHash hash = {};
	if (info)
		hash.application_info_hash = compute_hash_application_info(*info);
	if (features)
		hash.physical_device_features_hash = compute_hash_physical_device_features(*features);
	return hash;
}

static void hash_application_feature_info(Hasher &hasher, const StateRecorderApplicationFeatureHash &base_hash)
{
	// This makes it so two different applications won't conflict if they use the same pipelines.
	hasher.u64(base_hash.application_info_hash);
	hasher.u64(base_hash.physical_device_features_hash);
}

Hash compute_combined_application_feature_hash(const StateRecorderApplicationFeatureHash &base_hash)
{
	Hasher h;
	hash_application_feature_info(h, base_hash);
	return h.get();
}

static Hash compute_hash_application_info_link(const StateRecorderApplicationFeatureHash &app, ResourceTag tag, Hash hash)
{
	Hasher h;
	h.u64(compute_combined_application_feature_hash(app));
	h.s32(tag);
	h.u64(hash);
	return h.get();
}

static Hash compute_hash_application_info_link(Hash app_hash, ResourceTag tag, Hash hash)
{
	Hasher h;
	h.u64(app_hash);
	h.s32(tag);
	h.u64(hash);
	return h.get();
}

Hash compute_hash_sampler(const VkSamplerCreateInfo &sampler)
{
	Hasher h;

	h.u32(sampler.flags);
	h.f32(sampler.maxAnisotropy);
	h.f32(sampler.mipLodBias);
	h.f32(sampler.minLod);
	h.f32(sampler.maxLod);
	h.u32(sampler.minFilter);
	h.u32(sampler.magFilter);
	h.u32(sampler.mipmapMode);
	h.u32(sampler.compareEnable);
	h.u32(sampler.compareOp);
	h.u32(sampler.anisotropyEnable);
	h.u32(sampler.addressModeU);
	h.u32(sampler.addressModeV);
	h.u32(sampler.addressModeW);
	h.u32(sampler.borderColor);
	h.u32(sampler.unnormalizedCoordinates);

	return h.get();
}

bool compute_hash_descriptor_set_layout(const StateRecorder &recorder, const VkDescriptorSetLayoutCreateInfo &layout, Hash *out_hash)
{
	Hasher h;

	h.u32(layout.bindingCount);
	h.u32(layout.flags);
	for (uint32_t i = 0; i < layout.bindingCount; i++)
	{
		auto &binding = layout.pBindings[i];
		h.u32(binding.binding);
		h.u32(binding.descriptorCount);
		h.u32(binding.descriptorType);
		h.u32(binding.stageFlags);

		if (binding.pImmutableSamplers &&
			(binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
		    binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER))
		{
			for (uint32_t j = 0; j < binding.descriptorCount; j++)
			{
				Hash hash;
				if (!recorder.get_hash_for_sampler(binding.pImmutableSamplers[j], &hash))
					return false;
				h.u64(hash);
			}
		}
	}

	*out_hash = h.get();
	return true;
}

bool compute_hash_pipeline_layout(const StateRecorder &recorder, const VkPipelineLayoutCreateInfo &layout, Hash *out_hash)
{
	Hasher h;

	h.u32(layout.setLayoutCount);
	for (uint32_t i = 0; i < layout.setLayoutCount; i++)
	{
		if (layout.pSetLayouts[i])
		{
			Hash hash;
			if (!recorder.get_hash_for_descriptor_set_layout(layout.pSetLayouts[i], &hash))
				return false;
			h.u64(hash);
		}
		else
			h.u32(0);
	}

	h.u32(layout.pushConstantRangeCount);
	for (uint32_t i = 0; i < layout.pushConstantRangeCount; i++)
	{
		auto &push = layout.pPushConstantRanges[i];
		h.u32(push.stageFlags);
		h.u32(push.size);
		h.u32(push.offset);
	}

	h.u32(layout.flags);

	*out_hash = h.get();
	return true;
}

Hash compute_hash_shader_module(const VkShaderModuleCreateInfo &create_info)
{
	Hasher h;
	h.data(create_info.pCode, create_info.codeSize);
	h.u32(create_info.flags);
	return h.get();
}

static void hash_specialization_info(Hasher &h, const VkSpecializationInfo &spec)
{
	h.data(static_cast<const uint8_t *>(spec.pData), spec.dataSize);
	h.u64(spec.dataSize);
	h.u32(spec.mapEntryCount);
	for (uint32_t i = 0; i < spec.mapEntryCount; i++)
	{
		h.u32(spec.pMapEntries[i].offset);
		h.u64(spec.pMapEntries[i].size);
		h.u32(spec.pMapEntries[i].constantID);
	}
}

static void hash_pnext_struct(const StateRecorder &,
                              Hasher &h,
                              const VkPipelineTessellationDomainOriginStateCreateInfo &create_info)
{
	h.u32(create_info.domainOrigin);
}

static void hash_pnext_struct(const StateRecorder &,
                              Hasher &h,
                              const VkPipelineVertexInputDivisorStateCreateInfoEXT &create_info)
{
	h.u32(create_info.vertexBindingDivisorCount);
	for (uint32_t i = 0; i < create_info.vertexBindingDivisorCount; i++)
	{
		h.u32(create_info.pVertexBindingDivisors[i].binding);
		h.u32(create_info.pVertexBindingDivisors[i].divisor);
	}
}

static void hash_pnext_struct(const StateRecorder &,
                              Hasher &h,
                              const VkPipelineRasterizationDepthClipStateCreateInfoEXT &create_info)
{
	h.u32(create_info.flags);
	h.u32(create_info.depthClipEnable);
}

static void hash_pnext_struct(const StateRecorder &,
                              Hasher &h,
                              const VkPipelineRasterizationStateStreamCreateInfoEXT &create_info)
{
	h.u32(create_info.flags);
	h.u32(create_info.rasterizationStream);
}

static bool hash_pnext_chain(const StateRecorder &recorder, Hasher &h, const void *pNext) FOSSILIZE_WARN_UNUSED;

static bool hash_pnext_chain(const StateRecorder &recorder, Hasher &h, const void *pNext)
{
	while (pNext != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);
		h.s32(pin->sType);

		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineTessellationDomainOriginStateCreateInfo *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT *>(pNext));
			break;

		default:
			log_error_pnext_chain("Unsupported pNext found, cannot hash.", pNext);
			return false;
		}

		pNext = pin->pNext;
	}

	return true;
}

bool compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info, Hash *out_hash)
{
	Hasher h;
	Hash hash;

	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		if (!recorder.get_hash_for_graphics_pipeline_handle(create_info.basePipelineHandle, &hash))
			return false;
		h.u64(hash);
		h.s32(create_info.basePipelineIndex);
	}

	if (!recorder.get_hash_for_pipeline_layout(create_info.layout, &hash))
		return false;
	h.u64(hash);

	if (!recorder.get_hash_for_render_pass(create_info.renderPass, &hash))
		return false;
	h.u64(hash);

	h.u32(create_info.subpass);
	h.u32(create_info.stageCount);

	bool dynamic_stencil_compare = false;
	bool dynamic_stencil_reference = false;
	bool dynamic_stencil_write_mask = false;
	bool dynamic_depth_bounds = false;
	bool dynamic_depth_bias = false;
	bool dynamic_line_width = false;
	bool dynamic_blend_constants = false;
	bool dynamic_scissor = false;
	bool dynamic_viewport = false;
	if (create_info.pDynamicState)
	{
		auto &state = *create_info.pDynamicState;
		h.u32(state.dynamicStateCount);
		h.u32(state.flags);
		for (uint32_t i = 0; i < state.dynamicStateCount; i++)
		{
			h.u32(state.pDynamicStates[i]);
			switch (state.pDynamicStates[i])
			{
			case VK_DYNAMIC_STATE_DEPTH_BIAS:
				dynamic_depth_bias = true;
				break;
			case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
				dynamic_depth_bounds = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
				dynamic_stencil_write_mask = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
				dynamic_stencil_reference = true;
				break;
			case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
				dynamic_stencil_compare = true;
				break;
			case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
				dynamic_blend_constants = true;
				break;
			case VK_DYNAMIC_STATE_SCISSOR:
				dynamic_scissor = true;
				break;
			case VK_DYNAMIC_STATE_VIEWPORT:
				dynamic_viewport = true;
				break;
			case VK_DYNAMIC_STATE_LINE_WIDTH:
				dynamic_line_width = true;
				break;
			default:
				break;
			}
		}

		if (!hash_pnext_chain(recorder, h, state.pNext))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pDepthStencilState)
	{
		auto &ds = *create_info.pDepthStencilState;
		h.u32(ds.flags);
		h.u32(ds.depthBoundsTestEnable);
		h.u32(ds.depthCompareOp);
		h.u32(ds.depthTestEnable);
		h.u32(ds.depthWriteEnable);
		h.u32(ds.front.compareOp);
		h.u32(ds.front.depthFailOp);
		h.u32(ds.front.failOp);
		h.u32(ds.front.passOp);
		h.u32(ds.back.compareOp);
		h.u32(ds.back.depthFailOp);
		h.u32(ds.back.failOp);
		h.u32(ds.back.passOp);
		h.u32(ds.stencilTestEnable);

		if (!dynamic_depth_bounds && ds.depthBoundsTestEnable)
		{
			h.f32(ds.minDepthBounds);
			h.f32(ds.maxDepthBounds);
		}

		if (ds.stencilTestEnable)
		{
			if (!dynamic_stencil_compare)
			{
				h.u32(ds.front.compareMask);
				h.u32(ds.back.compareMask);
			}

			if (!dynamic_stencil_reference)
			{
				h.u32(ds.front.reference);
				h.u32(ds.back.reference);
			}

			if (!dynamic_stencil_write_mask)
			{
				h.u32(ds.front.writeMask);
				h.u32(ds.back.writeMask);
			}
		}

		if (!hash_pnext_chain(recorder, h, ds.pNext))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pInputAssemblyState)
	{
		auto &ia = *create_info.pInputAssemblyState;
		h.u32(ia.flags);
		h.u32(ia.primitiveRestartEnable);
		h.u32(ia.topology);

		if (!hash_pnext_chain(recorder, h, ia.pNext))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pRasterizationState)
	{
		auto &rs = *create_info.pRasterizationState;
		h.u32(rs.flags);
		h.u32(rs.cullMode);
		h.u32(rs.depthClampEnable);
		h.u32(rs.frontFace);
		h.u32(rs.rasterizerDiscardEnable);
		h.u32(rs.polygonMode);
		h.u32(rs.depthBiasEnable);

		if (rs.depthBiasEnable && !dynamic_depth_bias)
		{
			h.f32(rs.depthBiasClamp);
			h.f32(rs.depthBiasSlopeFactor);
			h.f32(rs.depthBiasConstantFactor);
		}

		if (!dynamic_line_width)
			h.f32(rs.lineWidth);

		if (!hash_pnext_chain(recorder, h, rs.pNext))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pMultisampleState)
	{
		auto &ms = *create_info.pMultisampleState;
		h.u32(ms.flags);
		h.u32(ms.alphaToCoverageEnable);
		h.u32(ms.alphaToOneEnable);
		h.f32(ms.minSampleShading);
		h.u32(ms.rasterizationSamples);
		h.u32(ms.sampleShadingEnable);
		if (ms.pSampleMask)
		{
			uint32_t elems = (ms.rasterizationSamples + 31) / 32;
			for (uint32_t i = 0; i < elems; i++)
				h.u32(ms.pSampleMask[i]);
		}
		else
			h.u32(0);

		if (!hash_pnext_chain(recorder, h, ms.pNext))
			return false;
	}

	if (create_info.pViewportState)
	{
		auto &vp = *create_info.pViewportState;
		h.u32(vp.flags);
		h.u32(vp.scissorCount);
		h.u32(vp.viewportCount);
		if (!dynamic_scissor)
		{
			for (uint32_t i = 0; i < vp.scissorCount; i++)
			{
				h.s32(vp.pScissors[i].offset.x);
				h.s32(vp.pScissors[i].offset.y);
				h.u32(vp.pScissors[i].extent.width);
				h.u32(vp.pScissors[i].extent.height);
			}
		}

		if (!dynamic_viewport)
		{
			for (uint32_t i = 0; i < vp.viewportCount; i++)
			{
				h.f32(vp.pViewports[i].x);
				h.f32(vp.pViewports[i].y);
				h.f32(vp.pViewports[i].width);
				h.f32(vp.pViewports[i].height);
				h.f32(vp.pViewports[i].minDepth);
				h.f32(vp.pViewports[i].maxDepth);
			}
		}

		if (!hash_pnext_chain(recorder, h, vp.pNext))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pVertexInputState)
	{
		auto &vi = *create_info.pVertexInputState;
		h.u32(vi.flags);
		h.u32(vi.vertexAttributeDescriptionCount);
		h.u32(vi.vertexBindingDescriptionCount);

		for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; i++)
		{
			h.u32(vi.pVertexAttributeDescriptions[i].offset);
			h.u32(vi.pVertexAttributeDescriptions[i].binding);
			h.u32(vi.pVertexAttributeDescriptions[i].format);
			h.u32(vi.pVertexAttributeDescriptions[i].location);
		}

		for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++)
		{
			h.u32(vi.pVertexBindingDescriptions[i].binding);
			h.u32(vi.pVertexBindingDescriptions[i].inputRate);
			h.u32(vi.pVertexBindingDescriptions[i].stride);
		}

		if (!hash_pnext_chain(recorder, h, vi.pNext))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pColorBlendState)
	{
		auto &b = *create_info.pColorBlendState;
		h.u32(b.flags);
		h.u32(b.attachmentCount);
		h.u32(b.logicOpEnable);
		h.u32(b.logicOp);

		bool need_blend_constants = false;

		for (uint32_t i = 0; i < b.attachmentCount; i++)
		{
			h.u32(b.pAttachments[i].blendEnable);
			if (b.pAttachments[i].blendEnable)
			{
				h.u32(b.pAttachments[i].colorWriteMask);
				h.u32(b.pAttachments[i].alphaBlendOp);
				h.u32(b.pAttachments[i].colorBlendOp);
				h.u32(b.pAttachments[i].dstAlphaBlendFactor);
				h.u32(b.pAttachments[i].srcAlphaBlendFactor);
				h.u32(b.pAttachments[i].dstColorBlendFactor);
				h.u32(b.pAttachments[i].srcColorBlendFactor);

				if (b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				    b.pAttachments[i].srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
					b.pAttachments[i].srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR)
				{
					need_blend_constants = true;
				}
			}
			else
				h.u32(0);
		}

		if (need_blend_constants && !dynamic_blend_constants)
			for (auto &blend_const : b.blendConstants)
				h.f32(blend_const);

		if (!hash_pnext_chain(recorder, h, b.pNext))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pTessellationState)
	{
		auto &tess = *create_info.pTessellationState;
		h.u32(tess.flags);
		h.u32(tess.patchControlPoints);

		if (!hash_pnext_chain(recorder, h, tess.pNext))
			return false;
	}
	else
		h.u32(0);

	for (uint32_t i = 0; i < create_info.stageCount; i++)
	{
		auto &stage = create_info.pStages[i];
		h.u32(stage.flags);
		h.string(stage.pName);
		h.u32(stage.stage);

		if (!recorder.get_hash_for_shader_module(stage.module, &hash))
			return false;
		h.u64(hash);

		if (stage.pSpecializationInfo)
			hash_specialization_info(h, *stage.pSpecializationInfo);
		else
			h.u32(0);

		if (!hash_pnext_chain(recorder, h, stage.pNext))
			return false;
	}

	*out_hash = h.get();
	return true;
}

bool compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info, Hash *out_hash)
{
	Hasher h;
	Hash hash;

	if (!recorder.get_hash_for_pipeline_layout(create_info.layout, &hash))
		return false;
	h.u64(hash);

	h.u32(create_info.flags);

	if (create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		if (!recorder.get_hash_for_compute_pipeline_handle(create_info.basePipelineHandle, &hash))
			return false;
		h.u64(hash);

		h.s32(create_info.basePipelineIndex);
	}
	else
		h.u32(0);

	if (!recorder.get_hash_for_shader_module(create_info.stage.module, &hash))
		return false;
	h.u64(hash);

	h.string(create_info.stage.pName);
	h.u32(create_info.stage.flags);
	h.u32(create_info.stage.stage);

	if (create_info.stage.pSpecializationInfo)
		hash_specialization_info(h, *create_info.stage.pSpecializationInfo);
	else
		h.u32(0);

	*out_hash = h.get();
	return true;
}

static void hash_attachment(Hasher &h, const VkAttachmentDescription &att)
{
	h.u32(att.flags);
	h.u32(att.initialLayout);
	h.u32(att.finalLayout);
	h.u32(att.format);
	h.u32(att.loadOp);
	h.u32(att.storeOp);
	h.u32(att.stencilLoadOp);
	h.u32(att.stencilStoreOp);
	h.u32(att.samples);
}

static void hash_dependency(Hasher &h, const VkSubpassDependency &dep)
{
	h.u32(dep.dependencyFlags);
	h.u32(dep.dstAccessMask);
	h.u32(dep.srcAccessMask);
	h.u32(dep.srcSubpass);
	h.u32(dep.dstSubpass);
	h.u32(dep.srcStageMask);
	h.u32(dep.dstStageMask);
}

static void hash_subpass(Hasher &h, const VkSubpassDescription &subpass)
{
	h.u32(subpass.flags);
	h.u32(subpass.colorAttachmentCount);
	h.u32(subpass.inputAttachmentCount);
	h.u32(subpass.preserveAttachmentCount);
	h.u32(subpass.pipelineBindPoint);

	for (uint32_t i = 0; i < subpass.preserveAttachmentCount; i++)
		h.u32(subpass.pPreserveAttachments[i]);

	for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
	{
		h.u32(subpass.pColorAttachments[i].attachment);
		h.u32(subpass.pColorAttachments[i].layout);
	}

	for (uint32_t i = 0; i < subpass.inputAttachmentCount; i++)
	{
		h.u32(subpass.pInputAttachments[i].attachment);
		h.u32(subpass.pInputAttachments[i].layout);
	}

	if (subpass.pResolveAttachments)
	{
		for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
		{
			h.u32(subpass.pResolveAttachments[i].attachment);
			h.u32(subpass.pResolveAttachments[i].layout);
		}
	}

	if (subpass.pDepthStencilAttachment)
	{
		h.u32(subpass.pDepthStencilAttachment->attachment);
		h.u32(subpass.pDepthStencilAttachment->layout);
	}
	else
		h.u32(0);
}

Hash compute_hash_render_pass(const VkRenderPassCreateInfo &create_info)
{
	Hasher h;

	h.u32(create_info.attachmentCount);
	h.u32(create_info.dependencyCount);
	h.u32(create_info.subpassCount);

	for (uint32_t i = 0; i < create_info.attachmentCount; i++)
	{
		auto &att = create_info.pAttachments[i];
		hash_attachment(h, att);
	}

	for (uint32_t i = 0; i < create_info.dependencyCount; i++)
	{
		auto &dep = create_info.pDependencies[i];
		hash_dependency(h, dep);
	}

	for (uint32_t i = 0; i < create_info.subpassCount; i++)
	{
		auto &subpass = create_info.pSubpasses[i];
		hash_subpass(h, subpass);
	}

	return h.get();
}
}

static uint8_t *decode_base64(ScratchAllocator &allocator, const char *data, size_t length)
{
	auto *buf = static_cast<uint8_t *>(allocator.allocate_raw(length, 16));
	auto *ptr = buf;

	const auto base64_index = [](char c) -> uint32_t {
		if (c >= 'A' && c <= 'Z')
			return uint32_t(c - 'A');
		else if (c >= 'a' && c <= 'z')
			return uint32_t(c - 'a') + 26;
		else if (c >= '0' && c <= '9')
			return uint32_t(c - '0') + 52;
		else if (c == '+')
			return 62;
		else if (c == '/')
			return 63;
		else
			return 0;
	};

	for (uint64_t i = 0; i < length; )
	{
		char c0 = *data++;
		if (c0 == '\0')
			break;
		char c1 = *data++;
		if (c1 == '\0')
			break;
		char c2 = *data++;
		if (c2 == '\0')
			break;
		char c3 = *data++;
		if (c3 == '\0')
			break;

		uint32_t values =
				(base64_index(c0) << 18) |
				(base64_index(c1) << 12) |
				(base64_index(c2) << 6) |
				(base64_index(c3) << 0);

		unsigned outbytes = 3;
		if (c2 == '=' && c3 == '=')
		{
			outbytes = 1;
			*ptr++ = uint8_t(values >> 16);
		}
		else if (c3 == '=')
		{
			outbytes = 2;
			*ptr++ = uint8_t(values >> 16);
			*ptr++ = uint8_t(values >> 8);
		}
		else
		{
			*ptr++ = uint8_t(values >> 16);
			*ptr++ = uint8_t(values >> 8);
			*ptr++ = uint8_t(values >> 0);
		}

		i += outbytes;
	}

	return buf;
}

static uint64_t string_to_uint64(const char* str)
{
	return strtoull(str, nullptr, 16);
}

const char *StateReplayer::Impl::duplicate_string(const char *str, size_t len)
{
	auto *c = allocator.allocate_n<char>(len + 1);
	memcpy(c, str, len);
	c[len] = '\0';
	return c;
}

bool StateReplayer::Impl::parse_immutable_samplers(const Value &samplers, const VkSampler **out_sampler)
{
	auto *samps = allocator.allocate_n<VkSampler>(samplers.Size());
	auto *ret = samps;
	for (auto itr = samplers.Begin(); itr != samplers.End(); ++itr, samps++)
	{
		auto index = string_to_uint64(itr->GetString());
		if (index > 0)
		{
			auto sampler_itr = replayed_samplers.find(index);
			if (sampler_itr == end(replayed_samplers))
			{
				log_missing_resource("Immutable sampler", index);
				return false;
			}
			else
				*samps = sampler_itr->second;
		}
	}

	*out_sampler = ret;
	return true;
}

void StateRecorder::Impl::sync_thread()
{
	if (worker_thread.joinable())
	{
		record_end();
		worker_thread.join();
	}
}

StateRecorder::Impl::~Impl()
{
	sync_thread();
}

bool StateReplayer::Impl::parse_descriptor_set_bindings(const Value &bindings,
                                                        const VkDescriptorSetLayoutBinding **out_bindings)
{
	auto *set_bindings = allocator.allocate_n_cleared<VkDescriptorSetLayoutBinding>(bindings.Size());
	auto *ret = set_bindings;
	for (auto itr = bindings.Begin(); itr != bindings.End(); ++itr, set_bindings++)
	{
		auto &b = *itr;
		set_bindings->binding = b["binding"].GetUint();
		set_bindings->descriptorCount = b["descriptorCount"].GetUint();
		set_bindings->descriptorType = static_cast<VkDescriptorType>(b["descriptorType"].GetUint());
		set_bindings->stageFlags = b["stageFlags"].GetUint();
		if (b.HasMember("immutableSamplers"))
			if (!parse_immutable_samplers(b["immutableSamplers"], &set_bindings->pImmutableSamplers))
				return false;
	}

	*out_bindings = ret;
	return true;
}

bool StateReplayer::Impl::parse_push_constant_ranges(const Value &ranges, const VkPushConstantRange **out_ranges)
{
	auto *infos = allocator.allocate_n_cleared<VkPushConstantRange>(ranges.Size());
	auto *ret = infos;

	for (auto itr = ranges.Begin(); itr != ranges.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->stageFlags = obj["stageFlags"].GetUint();
		infos->offset = obj["offset"].GetUint();
		infos->size = obj["size"].GetUint();
	}

	*out_ranges = ret;
	return true;
}

bool StateReplayer::Impl::parse_set_layouts(const Value &layouts, const VkDescriptorSetLayout **out_layout)
{
	auto *infos = allocator.allocate_n_cleared<VkDescriptorSetLayout>(layouts.Size());
	auto *ret = infos;

	for (auto itr = layouts.Begin(); itr != layouts.End(); ++itr, infos++)
	{
		auto index = string_to_uint64(itr->GetString());
		if (index > 0)
		{
			auto set_itr = replayed_descriptor_set_layouts.find(index);
			if (set_itr == end(replayed_descriptor_set_layouts))
			{
				log_missing_resource("Descriptor set layout", index);
				return false;
			}
			else
				*infos = set_itr->second;
		}
	}

	*out_layout = ret;
	return true;
}

bool StateReplayer::Impl::parse_shader_modules(StateCreatorInterface &iface, const Value &modules,
                                               const uint8_t *varint, size_t varint_size)
{
	auto *infos = allocator.allocate_n_cleared<VkShaderModuleCreateInfo>(modules.MemberCount());

	unsigned index = 0;
	for (auto itr = modules.MemberBegin(); itr != modules.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_shader_modules.count(hash))
			continue;

		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.flags = obj["flags"].GetUint();
		info.codeSize = obj["codeSize"].GetUint64();

		if (obj.HasMember("varintOffset") && obj.HasMember("varintSize"))
		{
			uint32_t *decoded = static_cast<uint32_t *>(allocator.allocate_raw(info.codeSize, 64));
			auto offset = obj["varintOffset"].GetUint64();
			auto size = obj["varintSize"].GetUint64();
			if (offset + size > varint_size)
			{
				LOGE("Binary varint buffer overflows payload.\n");
				return false;
			}

			if (!decode_varint(decoded, info.codeSize / 4, varint + offset, size))
			{
				LOGE("Invalid varint format.\n");
				return false;
			}

			info.pCode = decoded;
		}
		else
			info.pCode = reinterpret_cast<uint32_t *>(decode_base64(allocator, obj["code"].GetString(), info.codeSize));

		if (!iface.enqueue_create_shader_module(hash, &info, &replayed_shader_modules[hash]))
		{
			LOGE("Failed to create shader module.\n");
			return false;
		}
	}

	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_pipeline_layouts(StateCreatorInterface &iface, const Value &layouts)
{
	auto *infos = allocator.allocate_n_cleared<VkPipelineLayoutCreateInfo>(layouts.MemberCount());

	unsigned index = 0;
	for (auto itr = layouts.MemberBegin(); itr != layouts.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_pipeline_layouts.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

		info.flags = obj["flags"].GetUint();

		if (obj.HasMember("pushConstantRanges"))
		{
			info.pushConstantRangeCount = obj["pushConstantRanges"].Size();
			if (!parse_push_constant_ranges(obj["pushConstantRanges"], &info.pPushConstantRanges))
				return false;
		}

		if (obj.HasMember("setLayouts"))
		{
			info.setLayoutCount = obj["setLayouts"].Size();
			if (!parse_set_layouts(obj["setLayouts"], &info.pSetLayouts))
				return false;
		}

		if (!iface.enqueue_create_pipeline_layout(hash, &info, &replayed_pipeline_layouts[hash]))
		{
			LOGE("Failed to create pipeline layout.\n");
			return false;
		}
	}

	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_descriptor_set_layouts(StateCreatorInterface &iface, const Value &layouts)
{
	auto *infos = allocator.allocate_n_cleared<VkDescriptorSetLayoutCreateInfo>(layouts.MemberCount());

	unsigned index = 0;
	for (auto itr = layouts.MemberBegin(); itr != layouts.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_descriptor_set_layouts.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

		info.flags = obj["flags"].GetUint();
		if (obj.HasMember("bindings"))
		{
			auto &bindings = obj["bindings"];
			info.bindingCount = bindings.Size();
			if (!parse_descriptor_set_bindings(bindings, &info.pBindings))
				return false;
		}

		if (!iface.enqueue_create_descriptor_set_layout(hash, &info, &replayed_descriptor_set_layouts[hash]))
		{
			LOGE("Failed to create descriptor set layout.\n");
			return false;
		}
	}

	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_application_info(StateCreatorInterface &iface, const Value &app_info, const Value &pdf_info)
{
	if (app_info.HasMember("apiVersion") && pdf_info.HasMember("robustBufferAccess"))
	{
		auto *app = allocator.allocate_cleared<VkApplicationInfo>();
		app->sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app->apiVersion = app_info["apiVersion"].GetUint();
		app->applicationVersion = app_info["applicationVersion"].GetUint();
		app->engineVersion = app_info["engineVersion"].GetUint();

		if (app_info.HasMember("applicationName"))
		{
			auto len = app_info["applicationName"].GetStringLength();
			char *name = allocator.allocate_n_cleared<char>(len + 1);
			memcpy(name, app_info["applicationName"].GetString(), len);
			app->pApplicationName = name;
		}

		if (app_info.HasMember("engineName"))
		{
			auto len = app_info["engineName"].GetStringLength();
			char *name = allocator.allocate_n_cleared<char>(len + 1);
			memcpy(name, app_info["engineName"].GetString(), len);
			app->pEngineName = name;
		}

		auto *pdf = allocator.allocate_cleared<VkPhysicalDeviceFeatures2>();
		pdf->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		pdf->features.robustBufferAccess = pdf_info["robustBufferAccess"].GetUint();

		auto hash =
				Hashing::compute_combined_application_feature_hash(
						Hashing::compute_application_feature_hash(app, pdf));
		iface.set_application_info(hash, app, pdf);
	}
	else
	{
		auto hash =
				Hashing::compute_combined_application_feature_hash(
						Hashing::compute_application_feature_hash(nullptr, nullptr));
		iface.set_application_info(hash, nullptr, nullptr);
	}

	return true;
}

bool StateReplayer::Impl::parse_application_info_link(StateCreatorInterface &iface, const Value &link)
{
	Hash application_hash = string_to_uint64(link["application"].GetString());
	auto tag = static_cast<ResourceTag>(link["tag"].GetInt());
	Hash hash = string_to_uint64(link["hash"].GetString());
	Hash link_hash = Hashing::compute_hash_application_info_link(application_hash, tag, hash);
	iface.notify_application_info_link(link_hash, application_hash, tag, hash);
	return true;
}

bool StateReplayer::Impl::parse_samplers(StateCreatorInterface &iface, const Value &samplers)
{
	auto *infos = allocator.allocate_n_cleared<VkSamplerCreateInfo>(samplers.MemberCount());

	unsigned index = 0;
	for (auto itr = samplers.MemberBegin(); itr != samplers.MemberEnd(); ++itr)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_samplers.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

		info.addressModeU = static_cast<VkSamplerAddressMode>(obj["addressModeU"].GetUint());
		info.addressModeV = static_cast<VkSamplerAddressMode>(obj["addressModeV"].GetUint());
		info.addressModeW = static_cast<VkSamplerAddressMode>(obj["addressModeW"].GetUint());
		info.anisotropyEnable = obj["anisotropyEnable"].GetUint();
		info.borderColor = static_cast<VkBorderColor>(obj["borderColor"].GetUint());
		info.compareEnable = obj["compareEnable"].GetUint();
		info.compareOp = static_cast<VkCompareOp>(obj["compareOp"].GetUint());
		info.flags = obj["flags"].GetUint();
		info.magFilter = static_cast<VkFilter>(obj["magFilter"].GetUint());
		info.minFilter = static_cast<VkFilter>(obj["minFilter"].GetUint());
		info.maxAnisotropy = obj["maxAnisotropy"].GetFloat();
		info.mipmapMode = static_cast<VkSamplerMipmapMode>(obj["mipmapMode"].GetUint());
		info.maxLod = obj["maxLod"].GetFloat();
		info.minLod = obj["minLod"].GetFloat();
		info.mipLodBias = obj["mipLodBias"].GetFloat();
		info.unnormalizedCoordinates = obj["unnormalizedCoordinates"].GetUint();

		if (!iface.enqueue_create_sampler(hash, &info, &replayed_samplers[hash]))
		{
			LOGE("Failed to create sampler.\n");
			return false;
		}
	}

	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_render_pass_attachments(const Value &attachments, const VkAttachmentDescription **out_attachments)
{
	auto *infos = allocator.allocate_n_cleared<VkAttachmentDescription>(attachments.Size());
	auto *ret = infos;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->flags = obj["flags"].GetUint();
		infos->finalLayout = static_cast<VkImageLayout>(obj["finalLayout"].GetUint());
		infos->initialLayout = static_cast<VkImageLayout>(obj["initialLayout"].GetUint());
		infos->format = static_cast<VkFormat>(obj["format"].GetUint());
		infos->loadOp = static_cast<VkAttachmentLoadOp>(obj["loadOp"].GetUint());
		infos->storeOp = static_cast<VkAttachmentStoreOp>(obj["storeOp"].GetUint());
		infos->stencilLoadOp = static_cast<VkAttachmentLoadOp>(obj["stencilLoadOp"].GetUint());
		infos->stencilStoreOp = static_cast<VkAttachmentStoreOp>(obj["stencilStoreOp"].GetUint());
		infos->samples = static_cast<VkSampleCountFlagBits>(obj["samples"].GetUint());
	}

	*out_attachments = ret;
	return true;
}

bool StateReplayer::Impl::parse_render_pass_dependencies(const Value &dependencies, const VkSubpassDependency **out_deps)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDependency>(dependencies.Size());
	auto *ret = infos;

	for (auto itr = dependencies.Begin(); itr != dependencies.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->dependencyFlags = obj["dependencyFlags"].GetUint();
		infos->dstAccessMask = obj["dstAccessMask"].GetUint();
		infos->srcAccessMask = obj["srcAccessMask"].GetUint();
		infos->dstStageMask = obj["dstStageMask"].GetUint();
		infos->srcStageMask = obj["srcStageMask"].GetUint();
		infos->srcSubpass = obj["srcSubpass"].GetUint();
		infos->dstSubpass = obj["dstSubpass"].GetUint();
	}

	*out_deps = ret;
	return true;
}

bool StateReplayer::Impl::parse_attachment(const Value &value, const VkAttachmentReference **out_reference)
{
	auto *ret = allocator.allocate_cleared<VkAttachmentReference>();
	ret->attachment = value["attachment"].GetUint();
	ret->layout = static_cast<VkImageLayout>(value["layout"].GetUint());

	*out_reference = ret;
	return true;
}

bool StateReplayer::Impl::parse_attachments(const Value &attachments, const VkAttachmentReference **out_references)
{
	auto *refs = allocator.allocate_n_cleared<VkAttachmentReference>(attachments.Size());
	auto *ret = refs;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, refs++)
	{
		auto &value = *itr;
		refs->attachment = value["attachment"].GetUint();
		refs->layout = static_cast<VkImageLayout>(value["layout"].GetUint());
	}

	*out_references = ret;
	return true;
}

bool StateReplayer::Impl::parse_uints(const Value &uints, const uint32_t **out_uints)
{
	auto *u32s = allocator.allocate_n<uint32_t>(uints.Size());
	auto *ret = u32s;
	for (auto itr = uints.Begin(); itr != uints.End(); ++itr, u32s++)
		*u32s = itr->GetUint();

	*out_uints = ret;
	return true;
}

bool StateReplayer::Impl::parse_render_pass_subpasses(const Value &subpasses, const VkSubpassDescription **out_subpasses)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDescription>(subpasses.Size());
	auto *ret = infos;

	for (auto itr = subpasses.Begin(); itr != subpasses.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->flags = obj["flags"].GetUint();
		infos->pipelineBindPoint = static_cast<VkPipelineBindPoint>(obj["pipelineBindPoint"].GetUint());

		if (obj.HasMember("depthStencilAttachment"))
			if (!parse_attachment(obj["depthStencilAttachment"], &infos->pDepthStencilAttachment))
				return false;

		if (obj.HasMember("resolveAttachments"))
			if (!parse_attachments(obj["resolveAttachments"], &infos->pResolveAttachments))
				return false;

		if (obj.HasMember("inputAttachments"))
		{
			infos->inputAttachmentCount = obj["inputAttachments"].Size();
			if (!parse_attachments(obj["inputAttachments"], &infos->pInputAttachments))
				return false;
		}

		if (obj.HasMember("colorAttachments"))
		{
			infos->colorAttachmentCount = obj["colorAttachments"].Size();
			if (!parse_attachments(obj["colorAttachments"], &infos->pColorAttachments))
				return false;
		}

		if (obj.HasMember("preserveAttachments"))
		{
			infos->preserveAttachmentCount = obj["preserveAttachments"].Size();
			if (!parse_uints(obj["preserveAttachments"], &infos->pPreserveAttachments))
				return false;
		}
	}

	*out_subpasses = ret;
	return true;
}

bool StateReplayer::Impl::parse_render_passes(StateCreatorInterface &iface, const Value &passes)
{
	auto *infos = allocator.allocate_n_cleared<VkRenderPassCreateInfo>(passes.MemberCount());

	unsigned index = 0;
	for (auto itr = passes.MemberBegin(); itr != passes.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_samplers.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;

		info.flags = obj["flags"].GetUint();

		if (obj.HasMember("attachments"))
		{
			info.attachmentCount = obj["attachments"].Size();
			if (!parse_render_pass_attachments(obj["attachments"], &info.pAttachments))
				return false;
		}

		if (obj.HasMember("dependencies"))
		{
			info.dependencyCount = obj["dependencies"].Size();
			if (!parse_render_pass_dependencies(obj["dependencies"], &info.pDependencies))
				return false;
		}

		if (obj.HasMember("subpasses"))
		{
			info.subpassCount = obj["subpasses"].Size();
			if (!parse_render_pass_subpasses(obj["subpasses"], &info.pSubpasses))
				return false;
		}

		if (!iface.enqueue_create_render_pass(hash, &info, &replayed_render_passes[hash]))
		{
			LOGE("Failed to create render pass.\n");
			return false;
		}
	}

	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_map_entries(const Value &map_entries, const VkSpecializationMapEntry **out_entries)
{
	auto *entries = allocator.allocate_n_cleared<VkSpecializationMapEntry>(map_entries.Size());
	auto *ret = entries;

	for (auto itr = map_entries.Begin(); itr != map_entries.End(); ++itr, entries++)
	{
		auto &obj = *itr;
		entries->constantID = obj["constantID"].GetUint();
		entries->offset = obj["offset"].GetUint();
		entries->size = obj["size"].GetUint();
	}

	*out_entries = ret;
	return true;
}

bool StateReplayer::Impl::parse_specialization_info(const Value &spec_info, const VkSpecializationInfo **out_info)
{
	auto *spec = allocator.allocate_cleared<VkSpecializationInfo>();
	spec->dataSize = spec_info["dataSize"].GetUint();
	spec->pData = decode_base64(allocator, spec_info["data"].GetString(), spec->dataSize);
	if (spec_info.HasMember("mapEntries"))
	{
		spec->mapEntryCount = spec_info["mapEntries"].Size();
		if (!parse_map_entries(spec_info["mapEntries"], &spec->pMapEntries))
			return false;
	}

	*out_info = spec;
	return true;
}

bool StateReplayer::Impl::parse_compute_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver,
                                                 const Value &pipelines, const Value &member)
{
	Hash hash = string_to_uint64(member.GetString());
	if (replayed_compute_pipelines.count(hash))
		return true;

	auto *info_allocated = allocator.allocate_cleared<VkComputePipelineCreateInfo>();
	auto &obj = pipelines[member];
	auto &info = *info_allocated;
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.flags = obj["flags"].GetUint();
	info.basePipelineIndex = obj["basePipelineIndex"].GetInt();

	auto pipeline = string_to_uint64(obj["basePipelineHandle"].GetString());
	if (pipeline > 0 && resolve_derivative_pipelines)
	{
		// This is pretty bad for multithreaded replay, but this should be very rare.
		iface.sync_threads();
		auto pipeline_iter = replayed_compute_pipelines.find(pipeline);

		// If we don't have the pipeline, we might have it later in the array of graphics pipelines, queue up out of order.
		if (pipeline_iter == replayed_compute_pipelines.end() && pipelines.HasMember(obj["basePipelineHandle"].GetString()))
		{
			if (!parse_compute_pipeline(iface, resolver, pipelines, obj["basePipelineHandle"]))
				return false;
			iface.sync_threads();
			pipeline_iter = replayed_compute_pipelines.find(pipeline);
		}

		// Still don't have it? Look into database.
		if (pipeline_iter == replayed_compute_pipelines.end())
		{
			size_t external_state_size = 0;
			if (!resolver || !resolver->read_entry(RESOURCE_COMPUTE_PIPELINE, pipeline, &external_state_size, nullptr,
			                                       PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}

			vector<uint8_t> external_state(external_state_size);

			if (!resolver->read_entry(RESOURCE_COMPUTE_PIPELINE, pipeline, &external_state_size, external_state.data(),
			                          PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}

			if (!this->parse(iface, resolver, external_state.data(), external_state.size()))
				return false;
			iface.sync_threads();

			pipeline_iter = replayed_compute_pipelines.find(pipeline);
			if (pipeline_iter == replayed_compute_pipelines.end())
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}
		}
		info.basePipelineHandle = pipeline_iter->second;
	}
	else
		info.basePipelineHandle = api_object_cast<VkPipeline>(pipeline);

	auto layout = string_to_uint64(obj["layout"].GetString());
	if (layout > 0)
	{
		auto layout_itr = replayed_pipeline_layouts.find(layout);
		if (layout_itr == end(replayed_pipeline_layouts))
		{
			log_missing_resource("Pipeline layout", layout);
			return false;
		}
		else
			info.layout = layout_itr->second;
	}

	auto &stage = obj["stage"];
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = static_cast<VkShaderStageFlagBits>(stage["stage"].GetUint());

	auto module = string_to_uint64(stage["module"].GetString());
	if (module > 0 && resolve_shader_modules)
	{
		auto module_iter = replayed_shader_modules.find(module);
		if (module_iter == replayed_shader_modules.end())
		{
			size_t external_state_size = 0;
			if (!resolver || !resolver->read_entry(RESOURCE_SHADER_MODULE, module, &external_state_size, nullptr,
			                                       PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Shader module", module);
				return false;
			}

			vector<uint8_t> external_state(external_state_size);

			if (!resolver->read_entry(RESOURCE_SHADER_MODULE, module, &external_state_size, external_state.data(),
			                          PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Shader module", module);
				return false;
			}

			if (!this->parse(iface, resolver, external_state.data(), external_state.size()))
				return false;

			iface.sync_shader_modules();
			module_iter = replayed_shader_modules.find(module);
			if (module_iter == replayed_shader_modules.end())
			{
				log_missing_resource("Shader module", module);
				return false;
			}
		}
		else
			iface.sync_shader_modules();
		info.stage.module = module_iter->second;
	}
	else
		info.stage.module = api_object_cast<VkShaderModule>(module);

	info.stage.pName = duplicate_string(stage["name"].GetString(), stage["name"].GetStringLength());
	if (stage.HasMember("specializationInfo"))
		if (!parse_specialization_info(stage["specializationInfo"], &info.stage.pSpecializationInfo))
			return false;

	if (!iface.enqueue_create_compute_pipeline(hash, &info, &replayed_compute_pipelines[hash]))
	{
		LOGE("Failed to create compute pipeline.\n");
		return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_compute_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines)
{
	for (auto itr = pipelines.MemberBegin(); itr != pipelines.MemberEnd(); ++itr)
		if (!parse_compute_pipeline(iface, resolver, pipelines, itr->name))
			return false;
	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_vertex_attributes(const Value &attributes, const VkVertexInputAttributeDescription **out_desc)
{
	auto *attribs = allocator.allocate_n_cleared<VkVertexInputAttributeDescription>(attributes.Size());
	auto *ret = attribs;

	for (auto itr = attributes.Begin(); itr != attributes.End(); ++itr, attribs++)
	{
		auto &obj = *itr;
		attribs->location = obj["location"].GetUint();
		attribs->binding = obj["binding"].GetUint();
		attribs->offset = obj["offset"].GetUint();
		attribs->format = static_cast<VkFormat>(obj["format"].GetUint());
	}

	*out_desc = ret;
	return true;
}

bool StateReplayer::Impl::parse_vertex_bindings(const Value &bindings, const VkVertexInputBindingDescription **out_desc)
{
	auto *binds = allocator.allocate_n_cleared<VkVertexInputBindingDescription>(bindings.Size());
	auto *ret = binds;

	for (auto itr = bindings.Begin(); itr != bindings.End(); ++itr, binds++)
	{
		auto &obj = *itr;
		binds->binding = obj["binding"].GetUint();
		binds->inputRate = static_cast<VkVertexInputRate>(obj["inputRate"].GetUint());
		binds->stride = obj["stride"].GetUint();
	}

	*out_desc = ret;
	return true;
}

bool StateReplayer::Impl::parse_vertex_input_state(const Value &vi, const VkPipelineVertexInputStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineVertexInputStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	state->flags = vi["flags"].GetUint();

	if (vi.HasMember("attributes"))
	{
		state->vertexAttributeDescriptionCount = vi["attributes"].Size();
		if (!parse_vertex_attributes(vi["attributes"], &state->pVertexAttributeDescriptions))
			return false;
	}

	if (vi.HasMember("bindings"))
	{
		state->vertexBindingDescriptionCount = vi["bindings"].Size();
		if (!parse_vertex_bindings(vi["bindings"], &state->pVertexBindingDescriptions))
			return false;
	}

	if (vi.HasMember("pNext"))
		if (!parse_pnext_chain(vi["pNext"], &state->pNext))
			return false;

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_depth_stencil_state(const Value &ds, const VkPipelineDepthStencilStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineDepthStencilStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	state->flags = ds["flags"].GetUint();

	state->depthBoundsTestEnable = ds["depthBoundsTestEnable"].GetUint();
	state->depthCompareOp = static_cast<VkCompareOp>(ds["depthCompareOp"].GetUint());
	state->depthTestEnable = ds["depthTestEnable"].GetUint();
	state->depthWriteEnable = ds["depthWriteEnable"].GetUint();
	state->minDepthBounds = ds["minDepthBounds"].GetFloat();
	state->maxDepthBounds = ds["maxDepthBounds"].GetFloat();
	state->stencilTestEnable = ds["stencilTestEnable"].GetUint();
	state->front.compareMask = ds["front"]["compareMask"].GetUint();
	state->front.compareOp = static_cast<VkCompareOp>(ds["front"]["compareOp"].GetUint());
	state->front.depthFailOp = static_cast<VkStencilOp>(ds["front"]["depthFailOp"].GetUint());
	state->front.passOp = static_cast<VkStencilOp>(ds["front"]["passOp"].GetUint());
	state->front.failOp = static_cast<VkStencilOp>(ds["front"]["failOp"].GetUint());
	state->front.reference = ds["front"]["reference"].GetUint();
	state->front.writeMask = ds["front"]["writeMask"].GetUint();
	state->back.compareMask = ds["back"]["compareMask"].GetUint();
	state->back.compareOp = static_cast<VkCompareOp>(ds["back"]["compareOp"].GetUint());
	state->back.depthFailOp = static_cast<VkStencilOp>(ds["back"]["depthFailOp"].GetUint());
	state->back.passOp = static_cast<VkStencilOp>(ds["back"]["passOp"].GetUint());
	state->back.failOp = static_cast<VkStencilOp>(ds["back"]["failOp"].GetUint());
	state->back.reference = ds["back"]["reference"].GetUint();
	state->back.writeMask = ds["back"]["writeMask"].GetUint();

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_rasterization_state(const Value &rs, const VkPipelineRasterizationStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineRasterizationStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	state->flags = rs["flags"].GetUint();

	state->cullMode = static_cast<VkCullModeFlags>(rs["cullMode"].GetUint());
	state->depthBiasClamp = rs["depthBiasClamp"].GetFloat();
	state->depthBiasConstantFactor = rs["depthBiasConstantFactor"].GetFloat();
	state->depthBiasSlopeFactor = rs["depthBiasSlopeFactor"].GetFloat();
	state->lineWidth = rs["lineWidth"].GetFloat();
	state->rasterizerDiscardEnable = rs["rasterizerDiscardEnable"].GetUint();
	state->depthBiasEnable = rs["depthBiasEnable"].GetUint();
	state->depthClampEnable = rs["depthClampEnable"].GetUint();
	state->polygonMode = static_cast<VkPolygonMode>(rs["polygonMode"].GetUint());
	state->frontFace = static_cast<VkFrontFace>(rs["frontFace"].GetUint());

	if (rs.HasMember("pNext"))
		if (!parse_pnext_chain(rs["pNext"], &state->pNext))
			return false;

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_tessellation_state(const Value &tess, const VkPipelineTessellationStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineTessellationStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
	state->flags = tess["flags"].GetUint();
	state->patchControlPoints = tess["patchControlPoints"].GetUint();

	if (tess.HasMember("pNext"))
		if (!parse_pnext_chain(tess["pNext"], &state->pNext))
			return false;

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_input_assembly_state(const Value &ia, const VkPipelineInputAssemblyStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineInputAssemblyStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	state->flags = ia["flags"].GetUint();
	state->primitiveRestartEnable = ia["primitiveRestartEnable"].GetUint();
	state->topology = static_cast<VkPrimitiveTopology>(ia["topology"].GetUint());

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_blend_attachments(const Value &attachments, const VkPipelineColorBlendAttachmentState **out_info)
{
	auto *att = allocator.allocate_n_cleared<VkPipelineColorBlendAttachmentState>(attachments.Size());
	auto *ret = att;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, att++)
	{
		auto &obj = *itr;
		att->blendEnable = obj["blendEnable"].GetUint();
		att->colorWriteMask = obj["colorWriteMask"].GetUint();
		att->alphaBlendOp = static_cast<VkBlendOp>(obj["alphaBlendOp"].GetUint());
		att->colorBlendOp = static_cast<VkBlendOp>(obj["colorBlendOp"].GetUint());
		att->srcColorBlendFactor = static_cast<VkBlendFactor>(obj["srcColorBlendFactor"].GetUint());
		att->dstColorBlendFactor = static_cast<VkBlendFactor>(obj["dstColorBlendFactor"].GetUint());
		att->srcAlphaBlendFactor = static_cast<VkBlendFactor>(obj["srcAlphaBlendFactor"].GetUint());
		att->dstAlphaBlendFactor = static_cast<VkBlendFactor>(obj["dstAlphaBlendFactor"].GetUint());
	}

	*out_info = ret;
	return true;
}

bool StateReplayer::Impl::parse_color_blend_state(const Value &blend, const VkPipelineColorBlendStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineColorBlendStateCreateInfo>();
	state->sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	state->flags = blend["flags"].GetUint();

	state->logicOp = static_cast<VkLogicOp>(blend["logicOp"].GetUint());
	state->logicOpEnable = blend["logicOpEnable"].GetUint();
	for (unsigned i = 0; i < 4; i++)
		state->blendConstants[i] = blend["blendConstants"][i].GetFloat();

	if (blend.HasMember("attachments"))
	{
		state->attachmentCount = blend["attachments"].Size();
		if (!parse_blend_attachments(blend["attachments"], &state->pAttachments))
			return false;
	}

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_multisample_state(const Value &ms, const VkPipelineMultisampleStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineMultisampleStateCreateInfo>();

	state->sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	state->flags = ms["flags"].GetUint();

	state->alphaToCoverageEnable = ms["alphaToCoverageEnable"].GetUint();
	state->alphaToOneEnable = ms["alphaToOneEnable"].GetUint();
	state->minSampleShading = ms["minSampleShading"].GetFloat();

	if (ms.HasMember("sampleMask"))
		if (!parse_uints(ms["sampleMask"], &state->pSampleMask))
			return false;

	state->sampleShadingEnable = ms["sampleShadingEnable"].GetUint();
	state->rasterizationSamples = static_cast<VkSampleCountFlagBits>(ms["rasterizationSamples"].GetUint());

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_dynamic_state(const Value &dyn, const VkPipelineDynamicStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineDynamicStateCreateInfo>();

	state->sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	state->flags = dyn["flags"].GetUint();

	if (dyn.HasMember("dynamicState"))
	{
		state->dynamicStateCount = dyn["dynamicState"].Size();
		static_assert(sizeof(VkDynamicState) == sizeof(uint32_t), "Enum size is not 32-bit.");
		if (!parse_uints(dyn["dynamicState"], reinterpret_cast<const uint32_t **>(&state->pDynamicStates)))
			return false;
	}

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_viewports(const Value &viewports, const VkViewport **out_viewports)
{
	auto *vps = allocator.allocate_n_cleared<VkViewport>(viewports.Size());
	auto *ret = vps;

	for (auto itr = viewports.Begin(); itr != viewports.End(); ++itr, vps++)
	{
		auto &obj = *itr;
		vps->x = obj["x"].GetFloat();
		vps->y = obj["y"].GetFloat();
		vps->width = obj["width"].GetFloat();
		vps->height = obj["height"].GetFloat();
		vps->minDepth = obj["minDepth"].GetFloat();
		vps->maxDepth = obj["maxDepth"].GetFloat();
	}

	*out_viewports = ret;
	return true;
}

bool StateReplayer::Impl::parse_scissors(const Value &scissors, const VkRect2D **out_rects)
{
	auto *sci = allocator.allocate_n_cleared<VkRect2D>(scissors.Size());
	auto *ret = sci;

	for (auto itr = scissors.Begin(); itr != scissors.End(); ++itr, sci++)
	{
		auto &obj = *itr;
		sci->offset.x = obj["x"].GetInt();
		sci->offset.y = obj["y"].GetInt();
		sci->extent.width = obj["width"].GetUint();
		sci->extent.height = obj["height"].GetUint();
	}

	*out_rects = ret;
	return true;
}

bool StateReplayer::Impl::parse_viewport_state(const Value &vp, const VkPipelineViewportStateCreateInfo **out_info)
{
	auto *state = allocator.allocate_cleared<VkPipelineViewportStateCreateInfo>();

	state->sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	state->flags = vp["flags"].GetUint();

	state->scissorCount = vp["scissorCount"].GetUint();
	if (vp.HasMember("scissors"))
		if (!parse_scissors(vp["scissors"], &state->pScissors))
			return false;

	state->viewportCount = vp["viewportCount"].GetUint();
	if (vp.HasMember("viewports"))
		if (!parse_viewports(vp["viewports"], &state->pViewports))
			return false;

	*out_info = state;
	return true;
}

bool StateReplayer::Impl::parse_stages(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &stages,
                                       const VkPipelineShaderStageCreateInfo **out_info)
{
	auto *state = allocator.allocate_n_cleared<VkPipelineShaderStageCreateInfo>(stages.Size());
	auto *ret = state;

	for (auto itr = stages.Begin(); itr != stages.End(); ++itr, state++)
	{
		auto &obj = *itr;
		state->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		state->flags = obj["flags"].GetUint();
		state->stage = static_cast<VkShaderStageFlagBits>(obj["stage"].GetUint());
		state->pName = duplicate_string(obj["name"].GetString(), obj["name"].GetStringLength());
		if (obj.HasMember("specializationInfo"))
			if (!parse_specialization_info(obj["specializationInfo"], &state->pSpecializationInfo))
				return false;

		auto module = string_to_uint64(obj["module"].GetString());
		if (module > 0 && resolve_shader_modules)
		{
			auto module_iter = replayed_shader_modules.find(module);
			if (module_iter == replayed_shader_modules.end())
			{
				size_t external_state_size = 0;
				if (!resolver || !resolver->read_entry(RESOURCE_SHADER_MODULE, module, &external_state_size, nullptr,
				                                       PAYLOAD_READ_NO_FLAGS))
				{
					log_missing_resource("Shader module", module);
					return false;
				}

				vector<uint8_t> external_state(external_state_size);

				if (!resolver->read_entry(RESOURCE_SHADER_MODULE, module, &external_state_size, external_state.data(),
				                          PAYLOAD_READ_NO_FLAGS))
				{
					log_missing_resource("Shader module", module);
					return false;
				}

				if (!this->parse(iface, resolver, external_state.data(), external_state.size()))
					return false;

				iface.sync_shader_modules();
				module_iter = replayed_shader_modules.find(module);
				if (module_iter == replayed_shader_modules.end())
				{
					log_missing_resource("Shader module", module);
					return false;
				}
			}
			else
				iface.sync_shader_modules();

			state->module = module_iter->second;
		}
		else
			state->module = api_object_cast<VkShaderModule>(module);
	}

	*out_info = ret;
	return true;
}

bool StateReplayer::Impl::parse_graphics_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines, const Value &member)
{
	Hash hash = string_to_uint64(member.GetString());
	if (replayed_graphics_pipelines.count(hash))
		return true;

	auto *info_allocated = allocator.allocate_cleared<VkGraphicsPipelineCreateInfo>();
	auto &obj = pipelines[member];
	auto &info = *info_allocated;
	info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	info.flags = obj["flags"].GetUint();
	info.basePipelineIndex = obj["basePipelineIndex"].GetInt();

	auto pipeline = string_to_uint64(obj["basePipelineHandle"].GetString());
	if (pipeline > 0 && resolve_derivative_pipelines)
	{
		// This is pretty bad for multithreaded replay, but this should be very rare.
		iface.sync_threads();
		auto pipeline_iter = replayed_graphics_pipelines.find(pipeline);

		// If we don't have the pipeline, we might have it later in the array of graphics pipelines, queue up out of order.
		if (pipeline_iter == replayed_graphics_pipelines.end() && pipelines.HasMember(obj["basePipelineHandle"].GetString()))
		{
			if (!parse_graphics_pipeline(iface, resolver, pipelines, obj["basePipelineHandle"]))
				return false;
			iface.sync_threads();
			pipeline_iter = replayed_graphics_pipelines.find(pipeline);
		}

		// Still don't have it? Look into database.
		if (pipeline_iter == replayed_graphics_pipelines.end())
		{
			size_t external_state_size = 0;
			if (!resolver || !resolver->read_entry(RESOURCE_GRAPHICS_PIPELINE, pipeline, &external_state_size, nullptr,
			                                       PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}

			vector<uint8_t> external_state(external_state_size);

			if (!resolver->read_entry(RESOURCE_GRAPHICS_PIPELINE, pipeline, &external_state_size, external_state.data(),
			                          PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}

			if (!this->parse(iface, resolver, external_state.data(), external_state.size()))
				return false;

			iface.sync_threads();
			pipeline_iter = replayed_graphics_pipelines.find(pipeline);
			if (pipeline_iter == replayed_graphics_pipelines.end())
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}
		}
		info.basePipelineHandle = pipeline_iter->second;
	}
	else
		info.basePipelineHandle = api_object_cast<VkPipeline>(pipeline);

	auto layout = string_to_uint64(obj["layout"].GetString());
	if (layout > 0)
	{
		auto layout_itr = replayed_pipeline_layouts.find(layout);
		if (layout_itr == end(replayed_pipeline_layouts))
		{
			log_missing_resource("Pipeline layout", layout);
			return false;
		}
		else
			info.layout = layout_itr->second;
	}

	auto render_pass = string_to_uint64(obj["renderPass"].GetString());
	if (render_pass > 0)
	{
		auto rp_itr = replayed_render_passes.find(render_pass);
		if (rp_itr == end(replayed_render_passes))
		{
			log_missing_resource("Render pass", render_pass);
			return false;
		}
		else
			info.renderPass = rp_itr->second;
	}

	info.subpass = obj["subpass"].GetUint();

	if (obj.HasMember("stages"))
	{
		info.stageCount = obj["stages"].Size();
		if (!parse_stages(iface, resolver, obj["stages"], &info.pStages))
			return false;
	}

	if (obj.HasMember("rasterizationState"))
		if (!parse_rasterization_state(obj["rasterizationState"], &info.pRasterizationState))
			return false;

	if (obj.HasMember("tessellationState"))
		if (!parse_tessellation_state(obj["tessellationState"], &info.pTessellationState))
			return false;

	if (obj.HasMember("colorBlendState"))
		if (!parse_color_blend_state(obj["colorBlendState"], &info.pColorBlendState))
			return false;

	if (obj.HasMember("depthStencilState"))
		if (!parse_depth_stencil_state(obj["depthStencilState"], &info.pDepthStencilState))
			return false;

	if (obj.HasMember("dynamicState"))
		if (!parse_dynamic_state(obj["dynamicState"], &info.pDynamicState))
			return false;

	if (obj.HasMember("viewportState"))
		if (!parse_viewport_state(obj["viewportState"], &info.pViewportState))
			return false;

	if (obj.HasMember("multisampleState"))
		 if (!parse_multisample_state(obj["multisampleState"], &info.pMultisampleState))
		 	return false;

	if (obj.HasMember("inputAssemblyState"))
		if (!parse_input_assembly_state(obj["inputAssemblyState"], &info.pInputAssemblyState))
			return false;

	if (obj.HasMember("vertexInputState"))
		if (!parse_vertex_input_state(obj["vertexInputState"], &info.pVertexInputState))
			return false;

	if (!iface.enqueue_create_graphics_pipeline(hash, &info, &replayed_graphics_pipelines[hash]))
	{
		LOGE("Failed to create graphics pipeline.");
		return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_graphics_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines)
{
	for (auto itr = pipelines.MemberBegin(); itr != pipelines.MemberEnd(); ++itr)
		if (!parse_graphics_pipeline(iface, resolver, pipelines, itr->name))
			return false;
	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_tessellation_domain_origin_state(const Value &state, VkPipelineTessellationDomainOriginStateCreateInfo **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineTessellationDomainOriginStateCreateInfo>();
	info->domainOrigin = static_cast<VkTessellationDomainOrigin>(state["domainOrigin"].GetUint());
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_vertex_input_divisor_state(const Value &state, VkPipelineVertexInputDivisorStateCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineVertexInputDivisorStateCreateInfoEXT>();
	info->vertexBindingDivisorCount = state["vertexBindingDivisorCount"].GetUint();
	if (state.HasMember("vertexBindingDivisors"))
	{
		auto *new_divisors =
				allocator.allocate_n_cleared<VkVertexInputBindingDivisorDescriptionEXT>(info->vertexBindingDivisorCount);
		info->pVertexBindingDivisors = new_divisors;

		auto &divisors = state["vertexBindingDivisors"];
		for (auto divisor_itr = divisors.Begin(); divisor_itr != divisors.End(); ++divisor_itr, new_divisors++)
		{
			auto &divisor = *divisor_itr;
			new_divisors->binding = divisor["binding"].GetUint();
			new_divisors->divisor = divisor["divisor"].GetUint();
		}
	}

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_rasterization_depth_clip_state(const Value &state, VkPipelineRasterizationDepthClipStateCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineRasterizationDepthClipStateCreateInfoEXT>();

	info->flags = state["flags"].GetUint();
	info->depthClipEnable = state["depthClipEnable"].GetUint();

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_rasterization_stream_state(const Value &state, VkPipelineRasterizationStateStreamCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineRasterizationStateStreamCreateInfoEXT>();

	info->flags = state["flags"].GetUint();
	info->rasterizationStream = state["rasterizationStream"].GetUint();

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_pnext_chain(const Value &pnext, const void **outpNext)
{
	VkBaseInStructure *ret = nullptr;
	VkBaseInStructure *chain = nullptr;

	for (auto itr = pnext.Begin(); itr != pnext.End(); ++itr)
	{
		auto &next = *itr;
		auto sType = static_cast<VkStructureType>(next["sType"].GetInt());
		VkBaseInStructure *new_struct = nullptr;

		switch (sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
		{
			VkPipelineTessellationDomainOriginStateCreateInfo *info = nullptr;
			if (!parse_tessellation_domain_origin_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT:
		{
			VkPipelineVertexInputDivisorStateCreateInfoEXT *info = nullptr;
			if (!parse_vertex_input_divisor_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
		{
			VkPipelineRasterizationDepthClipStateCreateInfoEXT *info = nullptr;
			if (!parse_rasterization_depth_clip_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
		{
			VkPipelineRasterizationStateStreamCreateInfoEXT *info = nullptr;
			if (!parse_rasterization_stream_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		default:
			LOGE("Failed to parse pNext chain for sType: %d\n", int(sType));
			return false;
		}

		new_struct->sType = sType;
		new_struct->pNext = nullptr;

		if (!chain)
		{
			chain = new_struct;
			ret = chain;
		}
		else
		{
			chain->pNext = new_struct;
			chain = new_struct;
		}
	}

	*outpNext = ret;
	return true;
}

StateReplayer::StateReplayer()
{
	impl = new Impl;
}

StateReplayer::~StateReplayer()
{
	delete impl;
}

ScratchAllocator &StateReplayer::get_allocator()
{
	return impl->allocator;
}

bool StateReplayer::parse(StateCreatorInterface &iface, DatabaseInterface *resolver, const void *buffer, size_t size)
{
	return impl->parse(iface, resolver, buffer, size);
}

void StateReplayer::set_resolve_derivative_pipeline_handles(bool enable)
{
	impl->resolve_derivative_pipelines = enable;
}

void StateReplayer::set_resolve_shader_module_handles(bool enable)
{
	impl->resolve_shader_modules = enable;
}

void StateReplayer::copy_handle_references(const StateReplayer &replayer)
{
	impl->copy_handle_references(*replayer.impl);
}

void StateReplayer::Impl::copy_handle_references(const StateReplayer::Impl &other)
{
	replayed_samplers = other.replayed_samplers;
	replayed_descriptor_set_layouts = other.replayed_descriptor_set_layouts;
	replayed_pipeline_layouts = other.replayed_pipeline_layouts;
	replayed_shader_modules = other.replayed_shader_modules;
	replayed_render_passes = other.replayed_render_passes;
	replayed_compute_pipelines = other.replayed_compute_pipelines;
	replayed_graphics_pipelines = other.replayed_graphics_pipelines;
}

bool StateReplayer::Impl::parse(StateCreatorInterface &iface, DatabaseInterface *resolver, const void *buffer_, size_t total_size)
{
	// All data after a string terminating '\0' is considered binary payload
	// which can be read for various purposes (SPIR-V varint for example).
	const uint8_t *buffer = static_cast<const uint8_t *>(buffer_);
	auto itr = find(buffer, buffer + total_size, '\0');
	const uint8_t *varint_buffer = nullptr;
	size_t varint_size = 0;
	size_t json_size = itr - buffer;

	if (itr < buffer + total_size)
	{
		varint_buffer = itr + 1;
		varint_size = (buffer + total_size) - varint_buffer;
	}

	Document doc;
	doc.Parse(reinterpret_cast<const char *>(buffer), json_size);

	if (doc.HasParseError())
	{
		auto error = doc.GetParseError();
		LOGE("Got parse error: %d\n", int(error));
		return false;
	}

	int version = doc["version"].GetInt();
	if (version > FOSSILIZE_FORMAT_VERSION || version < FOSSILIZE_FORMAT_MIN_COMPAT_VERSION)
	{
		LOGE("JSON version mismatches.");
		return false;
	}

	if (doc.HasMember("applicationInfo") && doc.HasMember("physicalDeviceFeatures"))
		if (!parse_application_info(iface, doc["applicationInfo"], doc["physicalDeviceFeatures"]))
			return false;

	if (doc.HasMember("application"))
		iface.set_current_application_info(string_to_uint64(doc["application"].GetString()));

	if (doc.HasMember("link"))
		if (!parse_application_info_link(iface, doc["link"]))
			return false;

	if (doc.HasMember("shaderModules"))
		if (!parse_shader_modules(iface, doc["shaderModules"], varint_buffer, varint_size))
			return false;

	if (doc.HasMember("samplers"))
		if (!parse_samplers(iface, doc["samplers"]))
			return false;

	if (doc.HasMember("setLayouts"))
		if (!parse_descriptor_set_layouts(iface, doc["setLayouts"]))
			return false;

	if (doc.HasMember("pipelineLayouts"))
		if (!parse_pipeline_layouts(iface, doc["pipelineLayouts"]))
			return false;

	if (doc.HasMember("renderPasses"))
		if (!parse_render_passes(iface, doc["renderPasses"]))
			return false;

	if (doc.HasMember("computePipelines"))
		if (!parse_compute_pipelines(iface, resolver, doc["computePipelines"]))
			return false;

	if (doc.HasMember("graphicsPipelines"))
		if (!parse_graphics_pipelines(iface, resolver, doc["graphicsPipelines"]))
			return false;

	return true;
}

template <typename T>
T *StateRecorder::Impl::copy(const T *src, size_t count, ScratchAllocator &alloc)
{
	if (!count)
		return nullptr;

	auto *new_data = alloc.allocate_n<T>(count);
	if (new_data)
		std::copy(src, src + count, new_data);
	return new_data;
}

void *StateRecorder::Impl::copy_pnext_struct(
		const VkPipelineTessellationDomainOriginStateCreateInfo *create_info,
		ScratchAllocator &alloc)
{
	return copy(create_info, 1, alloc);
}

void *StateRecorder::Impl::copy_pnext_struct(
		const VkPipelineVertexInputDivisorStateCreateInfoEXT *create_info,
		ScratchAllocator &alloc)
{
	auto *info = copy(create_info, 1, alloc);
	if (info->pVertexBindingDivisors)
	{
		info->pVertexBindingDivisors = copy(create_info->pVertexBindingDivisors,
		                                    create_info->vertexBindingDivisorCount,
		                                    alloc);
	}

	return info;
}

void *StateRecorder::Impl::copy_pnext_struct(
		const VkPipelineRasterizationDepthClipStateCreateInfoEXT *create_info,
		ScratchAllocator &alloc)
{
	return copy(create_info, 1, alloc);
}

void *StateRecorder::Impl::copy_pnext_struct(
		const VkPipelineRasterizationStateStreamCreateInfoEXT *create_info,
		ScratchAllocator &alloc)
{
	return copy(create_info, 1, alloc);
}

bool StateRecorder::Impl::copy_pnext_chain(const void *pNext, ScratchAllocator &alloc, const void **out_pnext)
{
	VkBaseInStructure new_pnext = {};
	const VkBaseInStructure **ppNext = &new_pnext.pNext;

	while (pNext != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);

		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
		{
			auto ci = static_cast<const VkPipelineTessellationDomainOriginStateCreateInfo *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		default:
			LOGE("Cannot copy unknown pNext sType: %d.\n", int(pin->sType));
			return false;
		}

		pNext = pin->pNext;
		ppNext = const_cast<const VkBaseInStructure **>(&(*ppNext)->pNext);
		*ppNext = nullptr;
	}

	*out_pnext = new_pnext.pNext;
	return true;
}

struct ScratchAllocator::Impl
{
	struct Block
	{
		Block(size_t size);
		size_t offset = 0;
		std::vector<uint8_t> blob;
	};
	std::vector<Block> blocks;

	void add_block(size_t minimum_size);
	size_t peak_history_size = 0;
};

ScratchAllocator::ScratchAllocator()
{
	impl = new Impl;
}

ScratchAllocator::~ScratchAllocator()
{
	delete impl;
}

ScratchAllocator::Impl::Block::Block(size_t size)
{
	blob.resize(size);
}

void ScratchAllocator::Impl::add_block(size_t minimum_size)
{
	if (minimum_size < 64 * 1024)
		minimum_size = 64 * 1024;
	blocks.emplace_back(minimum_size);
}

void *ScratchAllocator::allocate_raw_cleared(size_t size, size_t alignment)
{
	void *ret = allocate_raw(size, alignment);
	if (ret)
		memset(ret, 0, size);
	return ret;
}

void *ScratchAllocator::allocate_raw(size_t size, size_t alignment)
{
	if (impl->blocks.empty())
		impl->add_block(size + alignment);

	auto &block = impl->blocks.back();

	size_t offset = (block.offset + alignment - 1) & ~(alignment - 1);
	size_t required_size = offset + size;
	if (required_size <= block.blob.size())
	{
		void *ret = block.blob.data() + offset;
		block.offset = required_size;
		return ret;
	}

	impl->add_block(size + alignment);
	return allocate_raw(size, alignment);
}

void ScratchAllocator::reset()
{
	// Keep track of how large the buffer can grow.
	size_t peak = get_peak_memory_consumption();
	if (peak > impl->peak_history_size)
		impl->peak_history_size = peak;

	if (impl->blocks.size() > 0)
	{
		// free all but first block
		if (impl->blocks.size() > 1)
			impl->blocks.erase(impl->blocks.begin() + 1, impl->blocks.end());
		// reset offset on first block
		impl->blocks[0].offset = 0;
	}
}

size_t ScratchAllocator::get_peak_memory_consumption() const
{
	size_t current_size = 0;
	for (auto &block : impl->blocks)
		current_size += block.blob.size();

	if (impl->peak_history_size > current_size)
		return impl->peak_history_size;
	else
		return current_size;
}

ScratchAllocator &StateRecorder::get_allocator()
{
	return impl->allocator;
}

void StateRecorder::set_database_enable_checksum(bool enable)
{
	impl->checksum = enable;
}

void StateRecorder::set_database_enable_compression(bool enable)
{
	impl->compression = enable;
}

bool StateRecorder::record_application_info(const VkApplicationInfo &info)
{
	if (info.pNext)
	{
		log_error_pnext_chain("pNext in VkApplicationInfo not supported.", info.pNext);
		return false;
	}

	std::lock_guard<std::mutex> lock(impl->record_lock);
	if (!impl->copy_application_info(&info, impl->allocator, &impl->application_info))
		return false;
	impl->application_feature_hash.application_info_hash = Hashing::compute_hash_application_info(*impl->application_info);
	return true;
}

bool StateRecorder::record_physical_device_features(const VkPhysicalDeviceFeatures2 &device_features)
{
	// We just ignore pNext, but it's okay to keep it. We will not need to serialize it for now.
	std::lock_guard<std::mutex> lock(impl->record_lock);
	if (!impl->copy_physical_device_features(&device_features, impl->allocator, &impl->physical_device_features))
		return false;
	impl->application_feature_hash.physical_device_features_hash = Hashing::compute_hash_physical_device_features(*impl->physical_device_features);
	return true;
}

void StateRecorder::set_application_info_filter(ApplicationInfoFilter *filter)
{
	impl->application_info_filter = filter;
}

const StateRecorderApplicationFeatureHash &StateRecorder::get_application_feature_hash() const
{
	return impl->application_feature_hash;
}

bool StateRecorder::record_physical_device_features(const VkPhysicalDeviceFeatures &device_features)
{
	VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	features.features = device_features;
	return record_physical_device_features(features);
}

bool StateRecorder::record_sampler(VkSampler sampler, const VkSamplerCreateInfo &create_info, Hash custom_hash)
{
	{
		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkSamplerCreateInfo not supported.", create_info.pNext);
			return false;
		}
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkSamplerCreateInfo *new_info = nullptr;
		if (!impl->copy_sampler(&create_info, impl->temp_allocator, &new_info))
			return false;

		impl->record_queue.push({api_object_cast<uint64_t>(sampler), new_info, custom_hash});
		impl->record_cv.notify_one();
	}

	// Thread is not running, drain the queue ourselves.
	if (!impl->worker_thread.joinable())
		impl->record_task(this, false);

	return true;
}

bool StateRecorder::record_descriptor_set_layout(VkDescriptorSetLayout set_layout, const VkDescriptorSetLayoutCreateInfo &create_info,
                                                 Hash custom_hash)
{
	{
		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkDescriptorSetLayoutCreateInfo not supported.", create_info.pNext);
			return false;
		}
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkDescriptorSetLayoutCreateInfo *new_info = nullptr;
		if (!impl->copy_descriptor_set_layout(&create_info, impl->temp_allocator, &new_info))
			return false;

		impl->record_queue.push({api_object_cast<uint64_t>(set_layout), new_info, custom_hash});
		impl->record_cv.notify_one();
	}

	// Thread is not running, drain the queue ourselves.
	if (!impl->worker_thread.joinable())
		impl->record_task(this, false);

	return true;
}

bool StateRecorder::record_pipeline_layout(VkPipelineLayout pipeline_layout, const VkPipelineLayoutCreateInfo &create_info,
                                           Hash custom_hash)
{
	{
		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineLayoutCreateInfo not supported.", create_info.pNext);
			return false;
		}
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkPipelineLayoutCreateInfo *new_info = nullptr;
		if (!impl->copy_pipeline_layout(&create_info, impl->temp_allocator, &new_info))
			return false;

		impl->record_queue.push({api_object_cast<uint64_t>(pipeline_layout), new_info, custom_hash});
		impl->record_cv.notify_one();
	}

	// Thread is not running, drain the queue ourselves.
	if (!impl->worker_thread.joinable())
		impl->record_task(this, false);

	return true;
}

bool StateRecorder::record_graphics_pipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo &create_info,
                                             const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                             Hash custom_hash)
{
	{
		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkGraphicsPipelineCreateInfo not supported.", create_info.pNext);
			return false;
		}
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkGraphicsPipelineCreateInfo *new_info = nullptr;
		if (!impl->copy_graphics_pipeline(&create_info, impl->temp_allocator, base_pipelines, base_pipeline_count, &new_info))
			return false;

		impl->record_queue.push({api_object_cast<uint64_t>(pipeline), new_info, custom_hash});
		impl->record_cv.notify_one();
	}

	// Thread is not running, drain the queue ourselves.
	if (!impl->worker_thread.joinable())
		impl->record_task(this, false);

	return true;
}

bool StateRecorder::record_compute_pipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo &create_info,
                                            const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                            Hash custom_hash)
{
	{
		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkComputePipelineCreateInfo not supported.", create_info.pNext);
			return false;
		}
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkComputePipelineCreateInfo *new_info = nullptr;
		if (!impl->copy_compute_pipeline(&create_info, impl->temp_allocator, base_pipelines, base_pipeline_count, &new_info))
			return false;

		impl->record_queue.push({api_object_cast<uint64_t>(pipeline), new_info, custom_hash});
		impl->record_cv.notify_one();
	}

	// Thread is not running, drain the queue ourselves.
	if (!impl->worker_thread.joinable())
		impl->record_task(this, false);

	return true;
}

bool StateRecorder::record_render_pass(VkRenderPass render_pass, const VkRenderPassCreateInfo &create_info,
                                       Hash custom_hash)
{
	{
		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkRenderPassCreateInfo not supported.", create_info.pNext);
			return false;
		}
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkRenderPassCreateInfo *new_info = nullptr;
		if (!impl->copy_render_pass(&create_info, impl->temp_allocator, &new_info))
			return false;

		impl->record_queue.push({api_object_cast<uint64_t>(render_pass), new_info, custom_hash});
		impl->record_cv.notify_one();
	}

	// Thread is not running, drain the queue ourselves.
	if (!impl->worker_thread.joinable())
		impl->record_task(this, false);

	return true;
}

bool StateRecorder::record_shader_module(VkShaderModule module, const VkShaderModuleCreateInfo &create_info,
                                         Hash custom_hash)
{
	{
		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkShaderModuleCreateInfo not supported.", create_info.pNext);
			return false;
		}
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkShaderModuleCreateInfo *new_info = nullptr;
		if (!impl->copy_shader_module(&create_info, impl->temp_allocator, &new_info))
			return false;

		impl->record_queue.push({api_object_cast<uint64_t>(module), new_info, custom_hash});
		impl->record_cv.notify_one();
	}

	// Thread is not running, drain the queue ourselves.
	if (!impl->worker_thread.joinable())
		impl->record_task(this, false);

	return true;
}

void StateRecorder::Impl::record_end()
{
	// Signal end of recording with empty work item
	std::lock_guard<std::mutex> lock(record_lock);
	record_queue.push({ 0, nullptr, 0 });
	record_cv.notify_one();
}

bool StateRecorder::get_hash_for_compute_pipeline_handle(VkPipeline pipeline, Hash *hash) const
{
	auto itr = impl->compute_pipeline_to_hash.find(pipeline);
	if (itr == end(impl->compute_pipeline_to_hash))
	{
		LOGE("Handle is not registered.\n");
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_graphics_pipeline_handle(VkPipeline pipeline, Hash *hash) const
{
	auto itr = impl->graphics_pipeline_to_hash.find(pipeline);
	if (itr == end(impl->graphics_pipeline_to_hash))
	{
		LOGE("Handle is not registered.\n");
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_sampler(VkSampler sampler, Hash *hash) const
{
	auto itr = impl->sampler_to_hash.find(sampler);
	if (itr == end(impl->sampler_to_hash))
	{
		LOGE("Handle is not registered.\n");
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_shader_module(VkShaderModule module, Hash *hash) const
{
	auto itr = impl->shader_module_to_hash.find(module);
	if (itr == end(impl->shader_module_to_hash))
	{
		LOGE("Handle is not registered.\n");
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_pipeline_layout(VkPipelineLayout layout, Hash *hash) const
{
	auto itr = impl->pipeline_layout_to_hash.find(layout);
	if (itr == end(impl->pipeline_layout_to_hash))
	{
		LOGE("Handle is not registered.\n");
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_descriptor_set_layout(VkDescriptorSetLayout layout, Hash *hash) const
{
	auto itr = impl->descriptor_set_layout_to_hash.find(layout);
	if (itr == end(impl->descriptor_set_layout_to_hash))
	{
		LOGE("Handle is not registered.\n");
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_render_pass(VkRenderPass render_pass, Hash *hash) const
{
	auto itr = impl->render_pass_to_hash.find(render_pass);
	if (itr == end(impl->render_pass_to_hash))
	{
		LOGE("Handle is not registered.\n");
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::Impl::copy_shader_module(const VkShaderModuleCreateInfo *create_info, ScratchAllocator &alloc,
                                             VkShaderModuleCreateInfo **out_create_info)
{
	if (create_info->pNext)
		return false;

	auto *info = copy(create_info, 1, alloc);
	info->pCode = copy(info->pCode, info->codeSize / sizeof(uint32_t), alloc);

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_sampler(const VkSamplerCreateInfo *create_info, ScratchAllocator &alloc,
                                       VkSamplerCreateInfo **out_create_info)
{
	if (create_info->pNext)
		return false;

	*out_create_info = copy(create_info, 1, alloc);
	return true;
}

bool StateRecorder::Impl::copy_physical_device_features(const VkPhysicalDeviceFeatures2 *pdf,
                                                        ScratchAllocator &alloc,
                                                        VkPhysicalDeviceFeatures2 **out_pdf)
{
	// Ignore pNext. We don't need to serialize it.
	auto *features = copy(pdf, 1, alloc);
	features->pNext = nullptr;

	*out_pdf = features;
	return true;
}

bool StateRecorder::Impl::copy_application_info(const VkApplicationInfo *app_info, ScratchAllocator &alloc,
                                                VkApplicationInfo **out_app_info)
{
	auto *app = copy(app_info, 1, alloc);
	if (app->pEngineName)
		app->pEngineName = copy(app->pEngineName, strlen(app->pEngineName) + 1, alloc);
	if (app->pApplicationName)
		app->pApplicationName = copy(app->pApplicationName, strlen(app->pApplicationName) + 1, alloc);

	*out_app_info = app;
	return true;
}

bool StateRecorder::Impl::copy_descriptor_set_layout(
	const VkDescriptorSetLayoutCreateInfo *create_info, ScratchAllocator &alloc,
	VkDescriptorSetLayoutCreateInfo **out_create_info)
{
	if (create_info->pNext)
		return false;

	auto *info = copy(create_info, 1, alloc);
	info->pBindings = copy(info->pBindings, info->bindingCount, alloc);

	for (uint32_t i = 0; i < info->bindingCount; i++)
	{
		auto &b = const_cast<VkDescriptorSetLayoutBinding &>(info->pBindings[i]);
		if (b.pImmutableSamplers &&
		    (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		{
			b.pImmutableSamplers = copy(b.pImmutableSamplers, b.descriptorCount, alloc);
		}
	}

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_pipeline_layout(const VkPipelineLayoutCreateInfo *create_info, ScratchAllocator &alloc,
                                               VkPipelineLayoutCreateInfo **out_create_info)
{
	if (create_info->pNext)
		return false;

	auto *info = copy(create_info, 1, alloc);
	info->pPushConstantRanges = copy(info->pPushConstantRanges, info->pushConstantRangeCount, alloc);
	info->pSetLayouts = copy(info->pSetLayouts, info->setLayoutCount, alloc);

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_specialization_info(const VkSpecializationInfo *info, ScratchAllocator &alloc,
                                                   const VkSpecializationInfo **out_info)
{
	auto *ret = copy(info, 1, alloc);
	ret->pMapEntries = copy(ret->pMapEntries, ret->mapEntryCount, alloc);
	ret->pData = copy(static_cast<const uint8_t *>(ret->pData), ret->dataSize, alloc);

	*out_info = ret;
	return true;
}

bool StateRecorder::Impl::copy_compute_pipeline(const VkComputePipelineCreateInfo *create_info, ScratchAllocator &alloc,
                                                const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                                VkComputePipelineCreateInfo **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);

	// Check for case where application made use of derivative pipelines and relied on the indexing behavior
	// into an array of pCreateInfos. In the replayer, we only do it one by one,
	// so we need to pass the correct handle to the create pipeline calls.
	if ((info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0)
	{
		if (info->basePipelineHandle == VK_NULL_HANDLE &&
		    info->basePipelineIndex >= 0)
		{
			if (uint32_t(info->basePipelineIndex) >= base_pipeline_count)
			{
				LOGE("Base pipeline index is out of range.\n");
				return false;
			}

			info->basePipelineHandle = base_pipelines[info->basePipelineIndex];
			info->basePipelineIndex = -1;
		}
	}

	if (info->stage.pSpecializationInfo)
		if (!copy_specialization_info(info->stage.pSpecializationInfo, alloc, &info->stage.pSpecializationInfo))
			return false;

	if (info->stage.pNext)
	{
		log_error_pnext_chain("pNext in VkPipelineShaderStageCreateInfo not supported.", info->stage.pNext);
		return false;
	}

	info->stage.pName = copy(info->stage.pName, strlen(info->stage.pName) + 1, alloc);
	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo *create_info, ScratchAllocator &alloc,
                                                 const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                                 VkGraphicsPipelineCreateInfo **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);

	// Check for case where application made use of derivative pipelines and relied on the indexing behavior
	// into an array of pCreateInfos. In the replayer, we only do it one by one,
	// so we need to pass the correct handle to the create pipeline calls.
	if ((info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0)
	{
		if (info->basePipelineHandle == VK_NULL_HANDLE &&
		    info->basePipelineIndex >= 0)
		{
			if (uint32_t(info->basePipelineIndex) >= base_pipeline_count)
			{
				LOGE("Base pipeline index is out of range.\n");
				return false;
			}

			info->basePipelineHandle = base_pipelines[info->basePipelineIndex];
			info->basePipelineIndex = -1;
		}
	}

	info->pStages = copy(info->pStages, info->stageCount, alloc);
	if (info->pTessellationState)
	{
		info->pTessellationState = copy(info->pTessellationState, 1, alloc);

		const void *pNext = nullptr;
		if (!copy_pnext_chain(info->pTessellationState->pNext, alloc, &pNext))
			return false;
		const_cast<VkPipelineTessellationStateCreateInfo *>(info->pTessellationState)->pNext = pNext;
	}

	if (info->pColorBlendState)
	{
		if (info->pColorBlendState->pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineColorBlendStateCreateInfo not supported.",
			                      info->pColorBlendState->pNext);
			return false;
		}
		info->pColorBlendState = copy(info->pColorBlendState, 1, alloc);
	}

	if (info->pVertexInputState)
	{
		info->pVertexInputState = copy(info->pVertexInputState, 1, alloc);

		const void *pNext = nullptr;
		if (!copy_pnext_chain(info->pVertexInputState->pNext, alloc, &pNext))
			return false;

		const_cast<VkPipelineVertexInputStateCreateInfo *>(info->pVertexInputState)->pNext = pNext;
	}

	if (info->pMultisampleState)
	{
		if (info->pMultisampleState->pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineMultisampleStateCreateInfo not supported.",
			                      info->pMultisampleState->pNext);
			return false;
		}
		info->pMultisampleState = copy(info->pMultisampleState, 1, alloc);
	}

	if (info->pViewportState)
	{
		if (info->pViewportState->pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineViewportStateCreateInfo not supported.",
			                      info->pViewportState->pNext);
			return false;
		}
		info->pViewportState = copy(info->pViewportState, 1, alloc);
	}

	if (info->pInputAssemblyState)
	{
		if (info->pInputAssemblyState->pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineInputAssemblyStateCreateInfo not supported.",
			                      info->pInputAssemblyState->pNext);
			return false;
		}
		info->pInputAssemblyState = copy(info->pInputAssemblyState, 1, alloc);
	}

	if (info->pDepthStencilState)
	{
		if (info->pDepthStencilState->pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineDepthStencilStateCreateInfo not supported.",
			                      info->pDepthStencilState->pNext);
			return false;
		}
		info->pDepthStencilState = copy(info->pDepthStencilState, 1, alloc);
	}

	if (info->pRasterizationState)
	{
		info->pRasterizationState = copy(info->pRasterizationState, 1, alloc);
		if (!info->pRasterizationState)
			return false;

		const void *pNext = nullptr;
		if (!copy_pnext_chain(info->pRasterizationState->pNext, alloc, &pNext))
			return false;

		const_cast<VkPipelineRasterizationStateCreateInfo *>(info->pRasterizationState)->pNext = pNext;
	}

	if (info->pDynamicState)
	{
		if (info->pDynamicState->pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineDynamicStateCreateInfo not supported.",
			                      info->pDynamicState->pNext);
			return false;
		}
		info->pDynamicState = copy(info->pDynamicState, 1, alloc);
	}

	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info->pStages[i]);
		if (stage.pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineShaderStageCreateInfo not supported.", stage.pNext);
			return false;
		}

		stage.pName = copy(stage.pName, strlen(stage.pName) + 1, alloc);
		if (stage.pSpecializationInfo)
			if (!copy_specialization_info(stage.pSpecializationInfo, alloc, &stage.pSpecializationInfo))
				return false;
	}

	if (info->pColorBlendState)
	{
		auto &blend = const_cast<VkPipelineColorBlendStateCreateInfo &>(*info->pColorBlendState);
		blend.pAttachments = copy(blend.pAttachments, blend.attachmentCount, alloc);
	}

	if (info->pVertexInputState)
	{
		auto &vs = const_cast<VkPipelineVertexInputStateCreateInfo &>(*info->pVertexInputState);
		vs.pVertexAttributeDescriptions = copy(vs.pVertexAttributeDescriptions, vs.vertexAttributeDescriptionCount, alloc);
		vs.pVertexBindingDescriptions = copy(vs.pVertexBindingDescriptions, vs.vertexBindingDescriptionCount, alloc);
	}

	if (info->pViewportState)
	{
		auto &vp = const_cast<VkPipelineViewportStateCreateInfo &>(*info->pViewportState);
		if (vp.pViewports)
			vp.pViewports = copy(vp.pViewports, vp.viewportCount, alloc);
		if (vp.pScissors)
			vp.pScissors = copy(vp.pScissors, vp.scissorCount, alloc);
	}

	if (info->pMultisampleState)
	{
		auto &ms = const_cast<VkPipelineMultisampleStateCreateInfo &>(*info->pMultisampleState);
		if (ms.pSampleMask)
			ms.pSampleMask = copy(ms.pSampleMask, (ms.rasterizationSamples + 31) / 32, alloc);
	}

	if (info->pDynamicState)
	{
		const_cast<VkPipelineDynamicStateCreateInfo *>(info->pDynamicState)->pDynamicStates =
				copy(info->pDynamicState->pDynamicStates, info->pDynamicState->dynamicStateCount, alloc);
	}

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_render_pass(const VkRenderPassCreateInfo *create_info, ScratchAllocator &alloc,
                                           VkRenderPassCreateInfo **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);
	info->pAttachments = copy(info->pAttachments, info->attachmentCount, alloc);
	info->pSubpasses = copy(info->pSubpasses, info->subpassCount, alloc);
	info->pDependencies = copy(info->pDependencies, info->dependencyCount, alloc);

	for (uint32_t i = 0; i < info->subpassCount; i++)
	{
		auto &sub = const_cast<VkSubpassDescription &>(info->pSubpasses[i]);
		if (sub.pDepthStencilAttachment)
			sub.pDepthStencilAttachment = copy(sub.pDepthStencilAttachment, 1, alloc);
		if (sub.pColorAttachments)
			sub.pColorAttachments = copy(sub.pColorAttachments, sub.colorAttachmentCount, alloc);
		if (sub.pResolveAttachments)
			sub.pResolveAttachments = copy(sub.pResolveAttachments, sub.colorAttachmentCount, alloc);
		if (sub.pInputAttachments)
			sub.pInputAttachments = copy(sub.pInputAttachments, sub.inputAttachmentCount, alloc);
		if (sub.pPreserveAttachments)
			sub.pPreserveAttachments = copy(sub.pPreserveAttachments, sub.preserveAttachmentCount, alloc);
	}

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::remap_sampler_handle(VkSampler sampler, VkSampler *out_sampler) const
{
	auto itr = sampler_to_hash.find(sampler);
	if (itr == end(sampler_to_hash))
	{
		LOGE("Cannot find sampler in hashmap.\n");
		return false;
	}
	else
	{
		*out_sampler = api_object_cast<VkSampler>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_descriptor_set_layout_handle(VkDescriptorSetLayout layout,
                                                             VkDescriptorSetLayout *out_layout) const
{
	auto itr = descriptor_set_layout_to_hash.find(layout);
	if (itr == end(descriptor_set_layout_to_hash))
	{
		LOGE("Cannot find descriptor set layout in hashmap.\n");
		return false;
	}
	else
	{
		*out_layout = api_object_cast<VkDescriptorSetLayout>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_pipeline_layout_handle(VkPipelineLayout layout, VkPipelineLayout *out_layout) const
{
	auto itr = pipeline_layout_to_hash.find(layout);
	if (itr == end(pipeline_layout_to_hash))
	{
		LOGE("Cannot find pipeline layout in hashmap.\n");
		return false;
	}
	else
	{
		*out_layout = api_object_cast<VkPipelineLayout>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_shader_module_handle(VkShaderModule module, VkShaderModule *out_module) const
{
	auto itr = shader_module_to_hash.find(module);
	if (itr == end(shader_module_to_hash))
	{
		LOGE("Cannot find shader module in hashmap.\n");
		return false;
	}
	else
	{
		*out_module = api_object_cast<VkShaderModule>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_render_pass_handle(VkRenderPass render_pass, VkRenderPass *out_render_pass) const
{
	auto itr = render_pass_to_hash.find(render_pass);
	if (itr == end(render_pass_to_hash))
	{
		LOGE("Cannot find render pass in hashmap.\n");
		return false;
	}
	else
	{
		*out_render_pass = api_object_cast<VkRenderPass>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_graphics_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const
{
	auto itr = graphics_pipeline_to_hash.find(pipeline);
	if (itr == end(graphics_pipeline_to_hash))
	{
		LOGE("Cannot find graphics pipeline in hashmap.\n");
		return false;
	}
	else
	{
		*out_pipeline = api_object_cast<VkPipeline>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_compute_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const
{
	auto itr = compute_pipeline_to_hash.find(pipeline);
	if (itr == end(compute_pipeline_to_hash))
	{
		LOGE("Cannot find compute pipeline in hashmap.\n");
		return false;
	}
	else
	{
		*out_pipeline = api_object_cast<VkPipeline>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_descriptor_set_layout_ci(VkDescriptorSetLayoutCreateInfo *info)
{
	for (uint32_t i = 0; i < info->bindingCount; i++)
	{
		auto &b = const_cast<VkDescriptorSetLayoutBinding &>(info->pBindings[i]);
		if (b.pImmutableSamplers &&
		    (b.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
		     b.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
		{
			auto *immutable_samplers = const_cast<VkSampler *>(b.pImmutableSamplers);
			for (uint32_t j = 0; j < b.descriptorCount; j++)
				if (!remap_sampler_handle(immutable_samplers[j], &immutable_samplers[j]))
					return false;
		}
	}

	return true;
}

bool StateRecorder::Impl::remap_pipeline_layout_ci(VkPipelineLayoutCreateInfo *info)
{
	for (uint32_t i = 0; i < info->setLayoutCount; i++)
		if (!remap_descriptor_set_layout_handle(info->pSetLayouts[i], &const_cast<VkDescriptorSetLayout *>(info->pSetLayouts)[i]))
			return false;

	return true;
}

bool StateRecorder::Impl::remap_shader_module_ci(VkShaderModuleCreateInfo *)
{
	// nothing to do
	return true;
}

bool StateRecorder::Impl::remap_graphics_pipeline_ci(VkGraphicsPipelineCreateInfo *info)
{
	if (!remap_render_pass_handle(info->renderPass, &info->renderPass))
		return false;
	if (!remap_pipeline_layout_handle(info->layout, &info->layout))
		return false;

	if (info->basePipelineHandle != VK_NULL_HANDLE)
		if (!remap_graphics_pipeline_handle(info->basePipelineHandle, &info->basePipelineHandle))
			return false;

	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info->pStages[i]);
		if (!remap_shader_module_handle(stage.module, &stage.module))
			return false;
	}

	return true;
}

bool StateRecorder::Impl::remap_compute_pipeline_ci(VkComputePipelineCreateInfo *info)
{
	if (!remap_shader_module_handle(info->stage.module, &info->stage.module))
		return false;

	if (info->basePipelineHandle != VK_NULL_HANDLE)
		if (!remap_compute_pipeline_handle(info->basePipelineHandle, &info->basePipelineHandle))
			return false;

	if (!remap_pipeline_layout_handle(info->layout, &info->layout))
		return false;

	return true;
}

bool StateRecorder::Impl::remap_sampler_ci(VkSamplerCreateInfo *)
{
	// nothing to do
	return true;
}

bool StateRecorder::Impl::remap_render_pass_ci(VkRenderPassCreateInfo *)
{
	// nothing to do
	return true;
}

void StateRecorder::Impl::record_task(StateRecorder *recorder, bool looping)
{
	PayloadWriteFlags payload_flags = 0;
	if (compression)
		payload_flags |= PAYLOAD_WRITE_COMPRESS_BIT;
	if (checksum)
		payload_flags |= PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT;

	bool write_database_entries = true;

	// Start by preparing in the thread since we need to parse an archive potentially, and that might block a little bit.
	if (database_iface)
	{
		assert(looping);
		if (!database_iface->prepare())
		{
			LOGE("Failed to prepare database, will not dump data to database.\n");
			database_iface = nullptr;
		}

		// Check here in the worker thread if we should write database entries for this application info.
		if (application_info_filter)
			write_database_entries = application_info_filter->test_application_info(application_info);
	}

	// Keep a single, pre-allocated buffer.
	vector<uint8_t> blob;
	blob.reserve(64 * 1024);

	if (database_iface && write_database_entries)
	{
		assert(looping);
		Hasher h;
		Hashing::hash_application_feature_info(h, application_feature_hash);
		if (serialize_application_info(blob))
			database_iface->write_entry(RESOURCE_APPLICATION_INFO, h.get(), blob.data(), blob.size(), payload_flags);
		else
			LOGE("Failed to serialize application info.\n");
	}

	bool need_flush = false;

	for (;;)
	{
		WorkItem record_item = {};
		{
			std::unique_lock<std::mutex> lock(record_lock);
			if (record_queue.empty())
				temp_allocator.reset();

			// Having this check here allows us to call record_task from a single threaded variant.
			// This is mostly used for testing purposes.
			if (!looping && record_queue.empty())
				break;

			// If we have written something to the database, wake up to flush whatever files are
			// necessary. Do not flush after every single write, as that might bog down the file system.
			// Once no new writes have occured for a second, we flush, and go to deep sleep.
			bool has_data;
			if (need_flush)
			{
				has_data = record_cv.wait_for(lock, std::chrono::seconds(1),
				                              [&]()
				                              {
					                              return !record_queue.empty();
				                              });
			}
			else
			{
				record_cv.wait(lock, [&]()
				{
					return !record_queue.empty();
				});
				has_data = true;
			}

			if (database_iface && !has_data && need_flush)
			{
				database_iface->flush();
				need_flush = false;
				continue;
			}
			else
			{
				record_item = record_queue.front();
				record_queue.pop();
			}
		}

		if (!record_item.create_info)
			break;

		switch (reinterpret_cast<VkBaseInStructure *>(record_item.create_info)->sType)
		{
		case VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkSamplerCreateInfo *>(record_item.create_info);
			auto hash = record_item.custom_hash;
			if (hash == 0)
				hash = Hashing::compute_hash_sampler(*create_info);
			sampler_to_hash[api_object_cast<VkSampler>(record_item.handle)] = hash;

			if (database_iface)
			{
				if (write_database_entries)
				{
					if (register_application_link_hash(RESOURCE_SAMPLER, hash, blob))
						need_flush = true;

					if (!database_iface->has_entry(RESOURCE_SAMPLER, hash))
					{
						if (serialize_sampler(hash, *create_info, blob))
						{
							database_iface->write_entry(RESOURCE_SAMPLER, hash, blob.data(), blob.size(),
							                            payload_flags);
							need_flush = true;
						}
					}
				}
			}
			else
			{
				// Retain for combined serialize() later.
				if (!samplers.count(hash))
				{
					VkSamplerCreateInfo *create_info_copy = nullptr;
					if (copy_sampler(create_info, allocator, &create_info_copy))
						samplers[hash] = create_info_copy;
				}
			}
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkRenderPassCreateInfo *>(record_item.create_info);
			auto hash = record_item.custom_hash;
			if (hash == 0)
				hash = Hashing::compute_hash_render_pass(*create_info);
			render_pass_to_hash[api_object_cast<VkRenderPass>(record_item.handle)] = hash;

			if (database_iface)
			{
				if (write_database_entries)
				{
					if (register_application_link_hash(RESOURCE_RENDER_PASS, hash, blob))
						need_flush = true;

					if (!database_iface->has_entry(RESOURCE_RENDER_PASS, hash))
					{
						if (serialize_render_pass(hash, *create_info, blob))
						{
							database_iface->write_entry(RESOURCE_RENDER_PASS, hash, blob.data(), blob.size(),
							                            payload_flags);
							need_flush = true;
						}
					}
				}
			}
			else
			{
				// Retain for combined serialize() later.
				if (!render_passes.count(hash))
				{
					VkRenderPassCreateInfo *create_info_copy = nullptr;
					if (copy_render_pass(create_info, allocator, &create_info_copy))
						render_passes[hash] = create_info_copy;
				}
			}
			break;
		}

		case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkShaderModuleCreateInfo *>(record_item.create_info);
			auto hash = record_item.custom_hash;
			if (hash == 0)
				hash = Hashing::compute_hash_shader_module(*create_info);
			shader_module_to_hash[api_object_cast<VkShaderModule>(record_item.handle)] = hash;

			if (database_iface)
			{
				if (write_database_entries)
				{
					if (register_application_link_hash(RESOURCE_SHADER_MODULE, hash, blob))
						need_flush = true;

					if (!database_iface->has_entry(RESOURCE_SHADER_MODULE, hash))
					{
						if (serialize_shader_module(hash, *create_info, blob, allocator))
						{
							database_iface->write_entry(RESOURCE_SHADER_MODULE, hash, blob.data(), blob.size(),
							                            payload_flags);
							need_flush = true;
						}
						allocator.reset();
					}
				}
			}
			else
			{
				// Retain for combined serialize() later.
				if (!shader_modules.count(hash))
				{
					VkShaderModuleCreateInfo *create_info_copy = nullptr;
					if (copy_shader_module(create_info, allocator, &create_info_copy))
						shader_modules[hash] = create_info_copy;
				}
			}
			break;
		}

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkDescriptorSetLayoutCreateInfo *>(record_item.create_info);
			auto hash = record_item.custom_hash;
			if (hash == 0)
				if (!Hashing::compute_hash_descriptor_set_layout(*recorder, *create_info, &hash))
					break;

			VkDescriptorSetLayoutCreateInfo *create_info_copy = nullptr;
			if (!copy_descriptor_set_layout(create_info, allocator, &create_info_copy))
				break;
			if (!remap_descriptor_set_layout_ci(create_info_copy))
				break;

			descriptor_set_layout_to_hash[api_object_cast<VkDescriptorSetLayout>(record_item.handle)] = hash;

			if (database_iface)
			{
				if (write_database_entries)
				{
					if (register_application_link_hash(RESOURCE_DESCRIPTOR_SET_LAYOUT, hash, blob))
						need_flush = true;

					if (!database_iface->has_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, hash))
					{
						if (serialize_descriptor_set_layout(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(RESOURCE_DESCRIPTOR_SET_LAYOUT, hash, blob.data(), blob.size(),
							                            payload_flags);
							need_flush = true;
						}
					}
				}

				// Don't need to keep copied data around, reset the allocator.
				allocator.reset();
			}
			else
			{

				// Retain for combined serialize() later.
				if (!descriptor_sets.count(hash))
					descriptor_sets[hash] = create_info_copy;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkPipelineLayoutCreateInfo *>(record_item.create_info);
			auto hash = record_item.custom_hash;
			if (hash == 0)
				if (!Hashing::compute_hash_pipeline_layout(*recorder, *create_info, &hash))
					break;

			VkPipelineLayoutCreateInfo *create_info_copy = nullptr;
			if (!copy_pipeline_layout(create_info, allocator, &create_info_copy))
				break;
			if (!remap_pipeline_layout_ci(create_info_copy))
				break;

			pipeline_layout_to_hash[api_object_cast<VkPipelineLayout>(record_item.handle)] = hash;

			if (database_iface)
			{
				if (write_database_entries)
				{
					if (register_application_link_hash(RESOURCE_PIPELINE_LAYOUT, hash, blob))
						need_flush = true;

					if (!database_iface->has_entry(RESOURCE_PIPELINE_LAYOUT, hash))
					{
						if (serialize_pipeline_layout(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(RESOURCE_PIPELINE_LAYOUT, hash, blob.data(), blob.size(),
							                            payload_flags);
							need_flush = true;
						}
					}
				}

				// Don't need to keep copied data around, reset the allocator.
				allocator.reset();
			}
			else
			{
				// Retain for combined serialize() later.
				if (!pipeline_layouts.count(hash))
					pipeline_layouts[hash] = create_info_copy;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkGraphicsPipelineCreateInfo *>(record_item.create_info);
			auto hash = record_item.custom_hash;
			if (hash == 0)
				if (!Hashing::compute_hash_graphics_pipeline(*recorder, *create_info, &hash))
					break;

			VkGraphicsPipelineCreateInfo *create_info_copy = nullptr;
			if (!copy_graphics_pipeline(create_info, allocator, nullptr, 0, &create_info_copy))
				break;
			if (!remap_graphics_pipeline_ci(create_info_copy))
				break;

			graphics_pipeline_to_hash[api_object_cast<VkPipeline>(record_item.handle)] = hash;

			if (database_iface)
			{
				if (write_database_entries)
				{
					if (register_application_link_hash(RESOURCE_GRAPHICS_PIPELINE, hash, blob))
						need_flush = true;

					if (!database_iface->has_entry(RESOURCE_GRAPHICS_PIPELINE, hash))
					{
						if (serialize_graphics_pipeline(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(RESOURCE_GRAPHICS_PIPELINE, hash, blob.data(), blob.size(),
							                            payload_flags);
							need_flush = true;
						}
					}
				}

				// Don't need to keep copied data around, reset the allocator.
				allocator.reset();
			}
			else
			{
				// Retain for combined serialize() later.
				if (!graphics_pipelines.count(hash))
					graphics_pipelines[hash] = create_info_copy;
			}
			break;
		}

		case VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkComputePipelineCreateInfo *>(record_item.create_info);
			auto hash = record_item.custom_hash;
			if (hash == 0)
				if (!Hashing::compute_hash_compute_pipeline(*recorder, *create_info, &hash))
					break;

			VkComputePipelineCreateInfo *create_info_copy = nullptr;
			if (!copy_compute_pipeline(create_info, allocator, nullptr, 0, &create_info_copy))
				break;
			if (!remap_compute_pipeline_ci(create_info_copy))
				break;

			compute_pipeline_to_hash[api_object_cast<VkPipeline>(record_item.handle)] = hash;

			if (database_iface)
			{
				if (write_database_entries)
				{
					if (register_application_link_hash(RESOURCE_COMPUTE_PIPELINE, hash, blob))
						need_flush = true;

					if (!database_iface->has_entry(RESOURCE_COMPUTE_PIPELINE, hash))
					{
						if (serialize_compute_pipeline(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(RESOURCE_COMPUTE_PIPELINE, hash, blob.data(), blob.size(),
							                            payload_flags);
							need_flush = true;
						}
					}
				}

				// Don't need to keep copied data around, reset the allocator.
				allocator.reset();
			}
			else
			{
				// Retain for combined serialize() later.
				if (!compute_pipelines.count(hash))
					compute_pipelines[hash] = create_info_copy;
			}
			break;
		}
		default:
			break;
		}
	}

	if (database_iface)
		database_iface->flush();

	// We no longer need a reference to this.
	// This should allow us to call init_recording_thread again if we want,
	// or emit some final single threaded recording tasks.
	database_iface = nullptr;
}

static char base64(uint32_t v)
{
	if (v == 63)
		return '/';
	else if (v == 62)
		return '+';
	else if (v >= 52)
		return char('0' + (v - 52));
	else if (v >= 26)
		return char('a' + (v - 26));
	else
		return char('A' + v);
}

static std::string encode_base64(const void *data_, size_t size)
{
	auto *data = static_cast<const uint8_t *>(data_);
	size_t num_chars = 4 * ((size + 2) / 3);
	std::string ret;
	ret.reserve(num_chars);

	for (size_t i = 0; i < size; i += 3)
	{
		uint32_t code = data[i] << 16;
		if (i + 1 < size)
			code |= data[i + 1] << 8;
		if (i + 2 < size)
			code |= data[i + 2] << 0;

		auto c0 = base64((code >> 18) & 63);
		auto c1 = base64((code >> 12) & 63);
		auto c2 = base64((code >>  6) & 63);
		auto c3 = base64((code >>  0) & 63);

		auto outbytes = std::min(size - i, size_t(3));
		if (outbytes == 1)
		{
			c2 = '=';
			c3 = '=';
		}
		else if (outbytes == 2)
			c3 = '=';

		ret.push_back(c0);
		ret.push_back(c1);
		ret.push_back(c2);
		ret.push_back(c3);
	}

	return ret;
}

template <typename Allocator>
static bool json_value(const VkSamplerCreateInfo& sampler, Allocator& alloc, Value *out_value)
{
	Value s(kObjectType);
	s.AddMember("flags", sampler.flags, alloc);
	s.AddMember("minFilter", sampler.minFilter, alloc);
	s.AddMember("magFilter", sampler.magFilter, alloc);
	s.AddMember("maxAnisotropy", sampler.maxAnisotropy, alloc);
	s.AddMember("compareOp", sampler.compareOp, alloc);
	s.AddMember("anisotropyEnable", sampler.anisotropyEnable, alloc);
	s.AddMember("mipmapMode", sampler.mipmapMode, alloc);
	s.AddMember("addressModeU", sampler.addressModeU, alloc);
	s.AddMember("addressModeV", sampler.addressModeV, alloc);
	s.AddMember("addressModeW", sampler.addressModeW, alloc);
	s.AddMember("borderColor", sampler.borderColor, alloc);
	s.AddMember("unnormalizedCoordinates", sampler.unnormalizedCoordinates, alloc);
	s.AddMember("compareEnable", sampler.compareEnable, alloc);
	s.AddMember("mipLodBias", sampler.mipLodBias, alloc);
	s.AddMember("minLod", sampler.minLod, alloc);
	s.AddMember("maxLod", sampler.maxLod, alloc);

	*out_value = s;
	return true;
}

template <typename Allocator>
static bool json_value(const VkDescriptorSetLayoutCreateInfo& layout, Allocator& alloc, Value *out_value)
{
	Value l(kObjectType);
	l.AddMember("flags", layout.flags, alloc);

	Value bindings(kArrayType);
	for (uint32_t i = 0; i < layout.bindingCount; i++)
	{
		auto &b = layout.pBindings[i];
		Value binding(kObjectType);
		binding.AddMember("descriptorType", b.descriptorType, alloc);
		binding.AddMember("descriptorCount", b.descriptorCount, alloc);
		binding.AddMember("stageFlags", b.stageFlags, alloc);
		binding.AddMember("binding", b.binding, alloc);
		if (b.pImmutableSamplers)
		{
			Value immutables(kArrayType);
			for (uint32_t j = 0; j < b.descriptorCount; j++)
				immutables.PushBack(uint64_string(api_object_cast<uint64_t>(b.pImmutableSamplers[j]), alloc), alloc);
			binding.AddMember("immutableSamplers", immutables, alloc);
		}
		bindings.PushBack(binding, alloc);
	}
	l.AddMember("bindings", bindings, alloc);

	*out_value = l;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineLayoutCreateInfo& layout, Allocator& alloc, Value *out_value)
{
	Value p(kObjectType);
	p.AddMember("flags", layout.flags, alloc);
	Value push(kArrayType);
	for (uint32_t i = 0; i < layout.pushConstantRangeCount; i++)
	{
		Value range(kObjectType);
		range.AddMember("stageFlags", layout.pPushConstantRanges[i].stageFlags, alloc);
		range.AddMember("size", layout.pPushConstantRanges[i].size, alloc);
		range.AddMember("offset", layout.pPushConstantRanges[i].offset, alloc);
		push.PushBack(range, alloc);
	}
	p.AddMember("pushConstantRanges", push, alloc);

	Value set_layouts(kArrayType);
	for (uint32_t i = 0; i < layout.setLayoutCount; i++)
		set_layouts.PushBack(uint64_string(api_object_cast<uint64_t>(layout.pSetLayouts[i]), alloc), alloc);
	p.AddMember("setLayouts", set_layouts, alloc);

	*out_value = p;
	return true;
}

template <typename Allocator>
static bool json_value(const VkShaderModuleCreateInfo& module, Allocator& alloc, Value *out_value)
{
	Value m(kObjectType);
	m.AddMember("flags", module.flags, alloc);
	m.AddMember("codeSize", uint64_t(module.codeSize), alloc);
	m.AddMember("code", encode_base64(module.pCode, module.codeSize), alloc);

	*out_value = m;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRenderPassCreateInfo& pass, Allocator& alloc, Value *out_value)
{
	Value json_object(kObjectType);
	json_object.AddMember("flags", pass.flags, alloc);

	Value deps(kArrayType);
	Value subpasses(kArrayType);
	Value attachments(kArrayType);

	if (pass.pDependencies)
	{
		for (uint32_t i = 0; i < pass.dependencyCount; i++)
		{
			auto &d = pass.pDependencies[i];
			Value dep(kObjectType);
			dep.AddMember("dependencyFlags", d.dependencyFlags, alloc);
			dep.AddMember("dstAccessMask", d.dstAccessMask, alloc);
			dep.AddMember("srcAccessMask", d.srcAccessMask, alloc);
			dep.AddMember("dstStageMask", d.dstStageMask, alloc);
			dep.AddMember("srcStageMask", d.srcStageMask, alloc);
			dep.AddMember("dstSubpass", d.dstSubpass, alloc);
			dep.AddMember("srcSubpass", d.srcSubpass, alloc);
			deps.PushBack(dep, alloc);
		}
		json_object.AddMember("dependencies", deps, alloc);
	}

	if (pass.pAttachments)
	{
		for (uint32_t i = 0; i < pass.attachmentCount; i++)
		{
			auto &a = pass.pAttachments[i];
			Value att(kObjectType);

			att.AddMember("flags", a.flags, alloc);
			att.AddMember("format", a.format, alloc);
			att.AddMember("finalLayout", a.finalLayout, alloc);
			att.AddMember("initialLayout", a.initialLayout, alloc);
			att.AddMember("loadOp", a.loadOp, alloc);
			att.AddMember("storeOp", a.storeOp, alloc);
			att.AddMember("samples", a.samples, alloc);
			att.AddMember("stencilLoadOp", a.stencilLoadOp, alloc);
			att.AddMember("stencilStoreOp", a.stencilStoreOp, alloc);

			attachments.PushBack(att, alloc);
		}
		json_object.AddMember("attachments", attachments, alloc);
	}

	for (uint32_t i = 0; i < pass.subpassCount; i++)
	{
		auto &sub = pass.pSubpasses[i];
		Value p(kObjectType);
		p.AddMember("flags", sub.flags, alloc);
		p.AddMember("pipelineBindPoint", sub.pipelineBindPoint, alloc);

		if (sub.pPreserveAttachments)
		{
			Value preserves(kArrayType);
			for (uint32_t j = 0; j < sub.preserveAttachmentCount; j++)
				preserves.PushBack(sub.pPreserveAttachments[j], alloc);
			p.AddMember("preserveAttachments", preserves, alloc);
		}

		if (sub.pInputAttachments)
		{
			Value inputs(kArrayType);
			for (uint32_t j = 0; j < sub.inputAttachmentCount; j++)
			{
				Value input(kObjectType);
				auto &ia = sub.pInputAttachments[j];
				input.AddMember("attachment", ia.attachment, alloc);
				input.AddMember("layout", ia.layout, alloc);
				inputs.PushBack(input, alloc);
			}
			p.AddMember("inputAttachments", inputs, alloc);
		}

		if (sub.pColorAttachments)
		{
			Value colors(kArrayType);
			for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
			{
				Value color(kObjectType);
				auto &c = sub.pColorAttachments[j];
				color.AddMember("attachment", c.attachment, alloc);
				color.AddMember("layout", c.layout, alloc);
				colors.PushBack(color, alloc);
			}
			p.AddMember("colorAttachments", colors, alloc);
		}

		if (sub.pResolveAttachments)
		{
			Value resolves(kArrayType);
			for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
			{
				Value resolve(kObjectType);
				auto &r = sub.pResolveAttachments[j];
				resolve.AddMember("attachment", r.attachment, alloc);
				resolve.AddMember("layout", r.layout, alloc);
				resolves.PushBack(resolve, alloc);
			}
			p.AddMember("resolveAttachments", resolves, alloc);
		}

		if (sub.pDepthStencilAttachment)
		{
			Value depth_stencil(kObjectType);
			depth_stencil.AddMember("attachment", sub.pDepthStencilAttachment->attachment, alloc);
			depth_stencil.AddMember("layout", sub.pDepthStencilAttachment->layout, alloc);
			p.AddMember("depthStencilAttachment", depth_stencil, alloc);
		}

		subpasses.PushBack(p, alloc);
	}
	json_object.AddMember("subpasses", subpasses, alloc);

	*out_value = json_object;
	return true;
}

template <typename Allocator>
static bool json_value(const VkComputePipelineCreateInfo& pipe, Allocator& alloc, Value *out_value)
{
	Value p(kObjectType);
	p.AddMember("flags", pipe.flags, alloc);
	p.AddMember("layout", uint64_string(api_object_cast<uint64_t>(pipe.layout), alloc), alloc);
	p.AddMember("basePipelineHandle", uint64_string(api_object_cast<uint64_t>(pipe.basePipelineHandle), alloc), alloc);
	p.AddMember("basePipelineIndex", pipe.basePipelineIndex, alloc);
	Value stage(kObjectType);
	stage.AddMember("flags", pipe.stage.flags, alloc);
	stage.AddMember("stage", pipe.stage.stage, alloc);
	stage.AddMember("module", uint64_string(api_object_cast<uint64_t>(pipe.stage.module), alloc), alloc);
	stage.AddMember("name", StringRef(pipe.stage.pName), alloc);
	if (pipe.stage.pSpecializationInfo)
	{
		Value spec(kObjectType);
		spec.AddMember("dataSize", uint64_t(pipe.stage.pSpecializationInfo->dataSize), alloc);
		spec.AddMember("data",
					   encode_base64(pipe.stage.pSpecializationInfo->pData,
									 pipe.stage.pSpecializationInfo->dataSize), alloc);
		Value map_entries(kArrayType);
		for (uint32_t i = 0; i < pipe.stage.pSpecializationInfo->mapEntryCount; i++)
		{
			auto &e = pipe.stage.pSpecializationInfo->pMapEntries[i];
			Value map_entry(kObjectType);
			map_entry.AddMember("offset", e.offset, alloc);
			map_entry.AddMember("size", uint64_t(e.size), alloc);
			map_entry.AddMember("constantID", e.constantID, alloc);
			map_entries.PushBack(map_entry, alloc);
		}
		spec.AddMember("mapEntries", map_entries, alloc);
		stage.AddMember("specializationInfo", spec, alloc);
	}
	p.AddMember("stage", stage, alloc);

	*out_value = p;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineTessellationDomainOriginStateCreateInfo &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("domainOrigin", create_info.domainOrigin, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineVertexInputDivisorStateCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("vertexBindingDivisorCount", create_info.vertexBindingDivisorCount, alloc);

	if (create_info.pVertexBindingDivisors)
	{
		Value divisors(kArrayType);
		for (uint32_t i = 0; i < create_info.vertexBindingDivisorCount; i++)
		{
			Value divisor(kObjectType);
			divisor.AddMember("binding", create_info.pVertexBindingDivisors[i].binding, alloc);
			divisor.AddMember("divisor", create_info.pVertexBindingDivisors[i].divisor, alloc);
			divisors.PushBack(divisor, alloc);
		}
		value.AddMember("vertexBindingDivisors", divisors, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineRasterizationDepthClipStateCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("flags", create_info.flags, alloc);
	value.AddMember("depthClipEnable", create_info.depthClipEnable, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineRasterizationStateStreamCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("flags", create_info.flags, alloc);
	value.AddMember("rasterizationStream", create_info.rasterizationStream, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool pnext_chain_json_value(const void *pNext, Allocator &alloc, Value *out_value)
{
	Value nexts(kArrayType);

	while (pNext != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);
		Value next;
		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
			if (!json_value(*static_cast<const VkPipelineTessellationDomainOriginStateCreateInfo *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		default:
			log_error_pnext_chain("Unsupported pNext found, cannot hash sType.", pNext);
			return false;
		}

		nexts.PushBack(next, alloc);
		pNext = pin->pNext;
	}

	*out_value = nexts;
	return true;
}

template <typename Allocator>
static bool json_value(const VkGraphicsPipelineCreateInfo& pipe, Allocator& alloc, Value *out_value)
{
	Value p(kObjectType);
	p.AddMember("flags", pipe.flags, alloc);
	p.AddMember("basePipelineHandle", uint64_string(api_object_cast<uint64_t>(pipe.basePipelineHandle), alloc), alloc);
	p.AddMember("basePipelineIndex", pipe.basePipelineIndex, alloc);
	p.AddMember("layout", uint64_string(api_object_cast<uint64_t>(pipe.layout), alloc), alloc);
	p.AddMember("renderPass", uint64_string(api_object_cast<uint64_t>(pipe.renderPass), alloc), alloc);
	p.AddMember("subpass", pipe.subpass, alloc);

	if (pipe.pTessellationState)
	{
		Value tess(kObjectType);
		tess.AddMember("flags", pipe.pTessellationState->flags, alloc);
		tess.AddMember("patchControlPoints", pipe.pTessellationState->patchControlPoints, alloc);

		if (pipe.pTessellationState->pNext)
		{
			Value nexts;
			if (!pnext_chain_json_value(pipe.pTessellationState->pNext, alloc, &nexts))
				return false;
			tess.AddMember("pNext", nexts, alloc);
		}

		p.AddMember("tessellationState", tess, alloc);
	}

	if (pipe.pDynamicState)
	{
		Value dyn(kObjectType);
		dyn.AddMember("flags", pipe.pDynamicState->flags, alloc);
		Value dynamics(kArrayType);
		for (uint32_t i = 0; i < pipe.pDynamicState->dynamicStateCount; i++)
			dynamics.PushBack(pipe.pDynamicState->pDynamicStates[i], alloc);
		dyn.AddMember("dynamicState", dynamics, alloc);
		p.AddMember("dynamicState", dyn, alloc);
	}

	if (pipe.pMultisampleState)
	{
		Value ms(kObjectType);
		ms.AddMember("flags", pipe.pMultisampleState->flags, alloc);
		ms.AddMember("rasterizationSamples", pipe.pMultisampleState->rasterizationSamples, alloc);
		ms.AddMember("sampleShadingEnable", pipe.pMultisampleState->sampleShadingEnable, alloc);
		ms.AddMember("minSampleShading", pipe.pMultisampleState->minSampleShading, alloc);
		ms.AddMember("alphaToOneEnable", pipe.pMultisampleState->alphaToOneEnable, alloc);
		ms.AddMember("alphaToCoverageEnable", pipe.pMultisampleState->alphaToCoverageEnable, alloc);

		Value sm(kArrayType);
		if (pipe.pMultisampleState->pSampleMask)
		{
			auto entries = uint32_t(pipe.pMultisampleState->rasterizationSamples + 31) / 32;
			for (uint32_t i = 0; i < entries; i++)
				sm.PushBack(pipe.pMultisampleState->pSampleMask[i], alloc);
			ms.AddMember("sampleMask", sm, alloc);
		}

		p.AddMember("multisampleState", ms, alloc);
	}

	if (pipe.pVertexInputState)
	{
		Value vi(kObjectType);

		Value attribs(kArrayType);
		Value bindings(kArrayType);
		vi.AddMember("flags", pipe.pVertexInputState->flags, alloc);

		for (uint32_t i = 0; i < pipe.pVertexInputState->vertexAttributeDescriptionCount; i++)
		{
			auto &a = pipe.pVertexInputState->pVertexAttributeDescriptions[i];
			Value attrib(kObjectType);
			attrib.AddMember("location", a.location, alloc);
			attrib.AddMember("binding", a.binding, alloc);
			attrib.AddMember("offset", a.offset, alloc);
			attrib.AddMember("format", a.format, alloc);
			attribs.PushBack(attrib, alloc);
		}

		for (uint32_t i = 0; i < pipe.pVertexInputState->vertexBindingDescriptionCount; i++)
		{
			auto &b = pipe.pVertexInputState->pVertexBindingDescriptions[i];
			Value binding(kObjectType);
			binding.AddMember("binding", b.binding, alloc);
			binding.AddMember("stride", b.stride, alloc);
			binding.AddMember("inputRate", b.inputRate, alloc);
			bindings.PushBack(binding, alloc);
		}
		vi.AddMember("attributes", attribs, alloc);
		vi.AddMember("bindings", bindings, alloc);

		if (pipe.pVertexInputState->pNext)
		{
			Value nexts;
			if (!pnext_chain_json_value(pipe.pVertexInputState->pNext, alloc, &nexts))
				return false;
			vi.AddMember("pNext", nexts, alloc);
		}

		p.AddMember("vertexInputState", vi, alloc);
	}

	if (pipe.pRasterizationState)
	{
		Value rs(kObjectType);
		rs.AddMember("flags", pipe.pRasterizationState->flags, alloc);
		rs.AddMember("depthBiasConstantFactor", pipe.pRasterizationState->depthBiasConstantFactor, alloc);
		rs.AddMember("depthBiasSlopeFactor", pipe.pRasterizationState->depthBiasSlopeFactor, alloc);
		rs.AddMember("depthBiasClamp", pipe.pRasterizationState->depthBiasClamp, alloc);
		rs.AddMember("depthBiasEnable", pipe.pRasterizationState->depthBiasEnable, alloc);
		rs.AddMember("depthClampEnable", pipe.pRasterizationState->depthClampEnable, alloc);
		rs.AddMember("polygonMode", pipe.pRasterizationState->polygonMode, alloc);
		rs.AddMember("rasterizerDiscardEnable", pipe.pRasterizationState->rasterizerDiscardEnable, alloc);
		rs.AddMember("frontFace", pipe.pRasterizationState->frontFace, alloc);
		rs.AddMember("lineWidth", pipe.pRasterizationState->lineWidth, alloc);
		rs.AddMember("cullMode", pipe.pRasterizationState->cullMode, alloc);

		if (pipe.pRasterizationState->pNext)
		{
			Value nexts;
			if (!pnext_chain_json_value(pipe.pRasterizationState->pNext, alloc, &nexts))
				return false;
			rs.AddMember("pNext", nexts, alloc);
		}

		p.AddMember("rasterizationState", rs, alloc);
	}

	if (pipe.pInputAssemblyState)
	{
		Value ia(kObjectType);
		ia.AddMember("flags", pipe.pInputAssemblyState->flags, alloc);
		ia.AddMember("topology", pipe.pInputAssemblyState->topology, alloc);
		ia.AddMember("primitiveRestartEnable", pipe.pInputAssemblyState->primitiveRestartEnable, alloc);
		p.AddMember("inputAssemblyState", ia, alloc);
	}

	if (pipe.pColorBlendState)
	{
		Value cb(kObjectType);
		cb.AddMember("flags", pipe.pColorBlendState->flags, alloc);
		cb.AddMember("logicOp", pipe.pColorBlendState->logicOp, alloc);
		cb.AddMember("logicOpEnable", pipe.pColorBlendState->logicOpEnable, alloc);
		Value blend_constants(kArrayType);
		for (auto &c : pipe.pColorBlendState->blendConstants)
			blend_constants.PushBack(c, alloc);
		cb.AddMember("blendConstants", blend_constants, alloc);
		Value attachments(kArrayType);
		for (uint32_t i = 0; i < pipe.pColorBlendState->attachmentCount; i++)
		{
			auto &a = pipe.pColorBlendState->pAttachments[i];
			Value att(kObjectType);
			att.AddMember("dstAlphaBlendFactor", a.dstAlphaBlendFactor, alloc);
			att.AddMember("srcAlphaBlendFactor", a.srcAlphaBlendFactor, alloc);
			att.AddMember("dstColorBlendFactor", a.dstColorBlendFactor, alloc);
			att.AddMember("srcColorBlendFactor", a.srcColorBlendFactor, alloc);
			att.AddMember("colorWriteMask", a.colorWriteMask, alloc);
			att.AddMember("alphaBlendOp", a.alphaBlendOp, alloc);
			att.AddMember("colorBlendOp", a.colorBlendOp, alloc);
			att.AddMember("blendEnable", a.blendEnable, alloc);
			attachments.PushBack(att, alloc);
		}
		cb.AddMember("attachments", attachments, alloc);
		p.AddMember("colorBlendState", cb, alloc);
	}

	if (pipe.pViewportState)
	{
		Value vp(kObjectType);
		vp.AddMember("flags", pipe.pViewportState->flags, alloc);
		vp.AddMember("viewportCount", pipe.pViewportState->viewportCount, alloc);
		vp.AddMember("scissorCount", pipe.pViewportState->scissorCount, alloc);
		if (pipe.pViewportState->pViewports)
		{
			Value viewports(kArrayType);
			for (uint32_t i = 0; i < pipe.pViewportState->viewportCount; i++)
			{
				Value viewport(kObjectType);
				viewport.AddMember("x", pipe.pViewportState->pViewports[i].x, alloc);
				viewport.AddMember("y", pipe.pViewportState->pViewports[i].y, alloc);
				viewport.AddMember("width", pipe.pViewportState->pViewports[i].width, alloc);
				viewport.AddMember("height", pipe.pViewportState->pViewports[i].height, alloc);
				viewport.AddMember("minDepth", pipe.pViewportState->pViewports[i].minDepth, alloc);
				viewport.AddMember("maxDepth", pipe.pViewportState->pViewports[i].maxDepth, alloc);
				viewports.PushBack(viewport, alloc);
			}
			vp.AddMember("viewports", viewports, alloc);
		}

		if (pipe.pViewportState->pScissors)
		{
			Value scissors(kArrayType);
			for (uint32_t i = 0; i < pipe.pViewportState->scissorCount; i++)
			{
				Value scissor(kObjectType);
				scissor.AddMember("x", pipe.pViewportState->pScissors[i].offset.x, alloc);
				scissor.AddMember("y", pipe.pViewportState->pScissors[i].offset.y, alloc);
				scissor.AddMember("width", pipe.pViewportState->pScissors[i].extent.width, alloc);
				scissor.AddMember("height", pipe.pViewportState->pScissors[i].extent.height, alloc);
				scissors.PushBack(scissor, alloc);
			}
			vp.AddMember("scissors", scissors, alloc);
		}
		p.AddMember("viewportState", vp, alloc);
	}

	if (pipe.pDepthStencilState)
	{
		Value ds(kObjectType);
		ds.AddMember("flags", pipe.pDepthStencilState->flags, alloc);
		ds.AddMember("stencilTestEnable", pipe.pDepthStencilState->stencilTestEnable, alloc);
		ds.AddMember("maxDepthBounds", pipe.pDepthStencilState->maxDepthBounds, alloc);
		ds.AddMember("minDepthBounds", pipe.pDepthStencilState->minDepthBounds, alloc);
		ds.AddMember("depthBoundsTestEnable", pipe.pDepthStencilState->depthBoundsTestEnable, alloc);
		ds.AddMember("depthWriteEnable", pipe.pDepthStencilState->depthWriteEnable, alloc);
		ds.AddMember("depthTestEnable", pipe.pDepthStencilState->depthTestEnable, alloc);
		ds.AddMember("depthCompareOp", pipe.pDepthStencilState->depthCompareOp, alloc);

		const auto serialize_stencil = [&](Value &v, const VkStencilOpState &state) {
			v.AddMember("compareOp", state.compareOp, alloc);
			v.AddMember("writeMask", state.writeMask, alloc);
			v.AddMember("reference", state.reference, alloc);
			v.AddMember("compareMask", state.compareMask, alloc);
			v.AddMember("passOp", state.passOp, alloc);
			v.AddMember("failOp", state.failOp, alloc);
			v.AddMember("depthFailOp", state.depthFailOp, alloc);
		};
		Value front(kObjectType);
		Value back(kObjectType);
		serialize_stencil(front, pipe.pDepthStencilState->front);
		serialize_stencil(back, pipe.pDepthStencilState->back);
		ds.AddMember("front", front, alloc);
		ds.AddMember("back", back, alloc);
		p.AddMember("depthStencilState", ds, alloc);
	}

	Value stages(kArrayType);
	for (uint32_t i = 0; i < pipe.stageCount; i++)
	{
		auto &s = pipe.pStages[i];
		Value stage(kObjectType);
		stage.AddMember("flags", s.flags, alloc);
		stage.AddMember("name", StringRef(s.pName), alloc);
		stage.AddMember("module", uint64_string(api_object_cast<uint64_t>(s.module), alloc), alloc);
		stage.AddMember("stage", s.stage, alloc);
		if (s.pSpecializationInfo)
		{
			Value spec(kObjectType);
			spec.AddMember("dataSize", uint64_t(s.pSpecializationInfo->dataSize), alloc);
			spec.AddMember("data",
						   encode_base64(s.pSpecializationInfo->pData,
										 s.pSpecializationInfo->dataSize), alloc);
			Value map_entries(kArrayType);
			for (uint32_t j = 0; j < s.pSpecializationInfo->mapEntryCount; j++)
			{
				auto &e = s.pSpecializationInfo->pMapEntries[j];
				Value map_entry(kObjectType);
				map_entry.AddMember("offset", e.offset, alloc);
				map_entry.AddMember("size", uint64_t(e.size), alloc);
				map_entry.AddMember("constantID", e.constantID, alloc);
				map_entries.PushBack(map_entry, alloc);
			}
			spec.AddMember("mapEntries", map_entries, alloc);
			stage.AddMember("specializationInfo", spec, alloc);
		}
		stages.PushBack(stage, alloc);
	}
	p.AddMember("stages", stages, alloc);

	*out_value = p;
	return true;
}

template <typename AllocType>
static void serialize_application_info_inline(Value &value, const VkApplicationInfo &info, AllocType &alloc)
{
	if (info.pApplicationName)
		value.AddMember("applicationName", StringRef(info.pApplicationName), alloc);
	if (info.pEngineName)
		value.AddMember("engineName", StringRef(info.pEngineName), alloc);
	value.AddMember("applicationVersion", info.applicationVersion, alloc);
	value.AddMember("engineVersion", info.engineVersion, alloc);
	value.AddMember("apiVersion", info.apiVersion, alloc);
}

template <typename AllocType>
static void serialize_physical_device_features_inline(Value &value, const VkPhysicalDeviceFeatures2 &features, AllocType &alloc)
{
	// TODO: For now, we only care about this feature, which can definitely affect compilation.
	// Deal with other device features if proven to be required.
	value.AddMember("robustBufferAccess", features.features.robustBufferAccess, alloc);
}

bool StateRecorder::Impl::serialize_application_info(vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value app_info(kObjectType);
	Value pdf_info(kObjectType);
	if (application_info)
		serialize_application_info_inline(app_info, *application_info, alloc);
	if (physical_device_features)
		serialize_physical_device_features_inline(pdf_info, *physical_device_features, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("applicationInfo", app_info, alloc);
	doc.AddMember("physicalDeviceFeatures", pdf_info, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

Hash StateRecorder::Impl::get_application_link_hash(ResourceTag tag, Hash hash) const
{
	return Hashing::compute_hash_application_info_link(application_feature_hash, tag, hash);
}

bool StateRecorder::Impl::register_application_link_hash(ResourceTag tag, Hash hash, vector<uint8_t> &blob) const
{
	PayloadWriteFlags payload_flags = 0;
	if (checksum)
		payload_flags |= PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT;

	Hash link_hash = get_application_link_hash(tag, hash);
	if (!database_iface->has_entry(RESOURCE_APPLICATION_BLOB_LINK, link_hash))
	{
		if (!serialize_application_blob_link(hash, tag, blob))
			return false;
		database_iface->write_entry(RESOURCE_APPLICATION_BLOB_LINK, link_hash, blob.data(), blob.size(), payload_flags);
		return true;
	}
	else
		return false;
}

bool StateRecorder::Impl::serialize_application_blob_link(Hash hash, ResourceTag tag, vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);

	Value links(kArrayType);
	Value link(kObjectType);

	Hasher h;
	Hashing::hash_application_feature_info(h, application_feature_hash);
	link.AddMember("application", uint64_string(h.get(), alloc), alloc);
	link.AddMember("tag", uint32_t(tag), alloc);
	link.AddMember("hash", uint64_string(hash, alloc), alloc);
	doc.AddMember("link", link, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

bool StateRecorder::Impl::serialize_sampler(Hash hash, const VkSamplerCreateInfo &create_info, vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value value;
	if (!json_value(create_info, alloc, &value))
		return false;

	Value serialized_samplers(kObjectType);
	serialized_samplers.AddMember(uint64_string(hash, alloc), value, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("samplers", serialized_samplers, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

bool StateRecorder::Impl::serialize_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo &create_info,
                                                          vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value value;
	if (!json_value(create_info, alloc, &value))
		return false;

	Value layouts(kObjectType);
	layouts.AddMember(uint64_string(hash, alloc), value, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("setLayouts", layouts, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

bool StateRecorder::Impl::serialize_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo &create_info,
                                                    vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value value;
	if (!json_value(create_info, alloc, &value))
		return false;

	Value layouts(kObjectType);
	layouts.AddMember(uint64_string(hash, alloc), value, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("pipelineLayouts", layouts, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

bool StateRecorder::Impl::serialize_render_pass(Hash hash, const VkRenderPassCreateInfo &create_info, vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value value;
	if (!json_value(create_info, alloc, &value))
		return false;

	Value serialized_render_passes(kObjectType);
	serialized_render_passes.AddMember(uint64_string(hash, alloc), value, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("renderPasses", serialized_render_passes, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

bool StateRecorder::Impl::serialize_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo &create_info, vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value value;
	if (!json_value(create_info, alloc, &value))
		return false;

	Value serialized_graphics_pipelines(kObjectType);
	serialized_graphics_pipelines.AddMember(uint64_string(hash, alloc), value, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("graphicsPipelines", serialized_graphics_pipelines, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

bool StateRecorder::Impl::serialize_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo &create_info, vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value value;
	if (!json_value(create_info, alloc, &value))
		return false;

	Value serialized_compute_pipelines(kObjectType);
	serialized_compute_pipelines.AddMember(uint64_string(hash, alloc), value, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("computePipelines", serialized_compute_pipelines, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize());
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	return true;
}

bool StateRecorder::Impl::serialize_shader_module(Hash hash, const VkShaderModuleCreateInfo &create_info,
                                                  vector<uint8_t> &blob, ScratchAllocator &blob_allocator) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value serialized_shader_modules(kObjectType);

	size_t size = compute_size_varint(create_info.pCode, create_info.codeSize / 4);
	uint8_t *encoded = static_cast<uint8_t *>(blob_allocator.allocate_raw(size, 64));
	encode_varint(encoded, create_info.pCode, create_info.codeSize / 4);

	Value varint(kObjectType);
	varint.AddMember("varintOffset", 0, alloc);
	varint.AddMember("varintSize", uint64_t(size), alloc);
	varint.AddMember("codeSize", uint64_t(create_info.codeSize), alloc);
	varint.AddMember("flags", 0, alloc);

	// Varint binary form, starts at offset 0 after the delim '\0' character.
	serialized_shader_modules.AddMember(uint64_string(hash, alloc), varint, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("shaderModules", serialized_shader_modules, alloc);

	StringBuffer buffer;
	CustomWriter writer(buffer);
	doc.Accept(writer);

	blob.resize(buffer.GetSize() + 1 + size);
	memcpy(blob.data(), buffer.GetString(), buffer.GetSize());
	blob[buffer.GetSize()] = '\0';
	memcpy(blob.data() + buffer.GetSize() + 1, encoded, size);
	return true;
}

bool StateRecorder::serialize(uint8_t **serialized_data, size_t *serialized_size)
{
	if (impl->database_iface)
		return false;

	impl->sync_thread();

	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);

	Value app_info(kObjectType);
	Value pdf_info(kObjectType);
	Value value;

	if (impl->application_info)
		serialize_application_info_inline(app_info, *impl->application_info, alloc);
	if (impl->physical_device_features)
		serialize_physical_device_features_inline(pdf_info, *impl->physical_device_features, alloc);

	doc.AddMember("applicationInfo", app_info, alloc);
	doc.AddMember("physicalDeviceFeatures", pdf_info, alloc);

	Value samplers(kObjectType);
	for (auto &sampler : impl->samplers)
	{
		if (!json_value(*sampler.second, alloc, &value))
			return false;
		samplers.AddMember(uint64_string(sampler.first, alloc), value, alloc);
	}
	doc.AddMember("samplers", samplers, alloc);

	Value set_layouts(kObjectType);
	for (auto &layout : impl->descriptor_sets)
	{
		if (!json_value(*layout.second, alloc, &value))
			return false;
		set_layouts.AddMember(uint64_string(layout.first, alloc), value, alloc);
	}
	doc.AddMember("setLayouts", set_layouts, alloc);

	Value pipeline_layouts(kObjectType);
	for (auto &layout : impl->pipeline_layouts)
	{
		if (!json_value(*layout.second, alloc, &value))
			return false;
		pipeline_layouts.AddMember(uint64_string(layout.first, alloc), value, alloc);
	}
	doc.AddMember("pipelineLayouts", pipeline_layouts, alloc);

	Value shader_modules(kObjectType);
	for (auto &module : impl->shader_modules)
	{
		if (!json_value(*module.second, alloc, &value))
			return false;
		shader_modules.AddMember(uint64_string(module.first, alloc), value, alloc);
	}
	doc.AddMember("shaderModules", shader_modules, alloc);

	Value render_passes(kObjectType);
	for (auto &pass : impl->render_passes)
	{
		if (!json_value(*pass.second, alloc, &value))
			return false;
		render_passes.AddMember(uint64_string(pass.first, alloc), value, alloc);
	}
	doc.AddMember("renderPasses", render_passes, alloc);

	Value compute_pipelines(kObjectType);
	for (auto &pipe : impl->compute_pipelines)
	{
		if (!json_value(*pipe.second, alloc, &value))
			return false;
		compute_pipelines.AddMember(uint64_string(pipe.first, alloc), value, alloc);
	}
	doc.AddMember("computePipelines", compute_pipelines, alloc);

	Value graphics_pipelines(kObjectType);
	for (auto &pipe : impl->graphics_pipelines)
	{
		if (!json_value(*pipe.second, alloc, &value))
			return false;
		graphics_pipelines.AddMember(uint64_string(pipe.first, alloc), value, alloc);
	}
	doc.AddMember("graphicsPipelines", graphics_pipelines, alloc);

	StringBuffer buffer;
	PrettyWriter<StringBuffer> writer(buffer);
	doc.Accept(writer);

	*serialized_size = buffer.GetSize();
	*serialized_data = new uint8_t[buffer.GetSize()];
	if (*serialized_data)
	{
		memcpy(*serialized_data, buffer.GetString(), buffer.GetSize());
		return true;
	}
	else
		return false;
}

void StateRecorder::free_serialized(uint8_t *serialized)
{
	delete[] serialized;
}

void StateRecorder::init_recording_thread(DatabaseInterface *iface)
{
	impl->database_iface = iface;
	impl->worker_thread = std::thread(&StateRecorder::Impl::record_task, impl, this, true);
}

void StateRecorder::tear_down_recording_thread()
{
	impl->sync_thread();
}

StateRecorder::StateRecorder()
{
	impl = new Impl;
}

StateRecorder::~StateRecorder()
{
	delete impl;
}

}
