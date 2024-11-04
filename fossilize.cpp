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
#include "fossilize_inttypes.h"
#include "fossilize.hpp"
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <string.h>
#include <stdarg.h>
#include "varint.hpp"
#include "path.hpp"
#include "fossilize_db.hpp"
#include "layer/utils.hpp"
#include "fossilize_errors.hpp"
#include "fossilize_application_filter.hpp"
#include "fossilize_hasher.hpp"
#include <time.h>

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

static inline bool operator==(const VkShaderModuleIdentifierEXT &a, const VkShaderModuleIdentifierEXT &b)
{
	return a.identifierSize == b.identifierSize &&
	       memcmp(a.identifier, b.identifier, a.identifierSize) == 0;
}

static inline bool operator!=(const VkShaderModuleIdentifierEXT &a, const VkShaderModuleIdentifierEXT &b)
{
	return !(a == b);
}

namespace std
{
template <> struct hash<VkShaderModuleIdentifierEXT>
{
	size_t operator()(const VkShaderModuleIdentifierEXT &i) const
	{
		Fossilize::Hasher h;
		h.u32(i.identifierSize);
		h.data(i.identifier, i.identifierSize);
		return h.get();
	}
};
}

using namespace std;

namespace Fossilize
{
static const void *pnext_chain_skip_ignored_entries(const void *pNext);
static bool pnext_chain_stype_is_hash_invariant(VkStructureType sType);
static const void *pnext_chain_pdf2_skip_ignored_entries(const void *pNext);

template <typename T>
static const T *find_pnext(VkStructureType sType, const void *pNext)
{
	while (pNext)
	{
		auto *base_in = static_cast<const VkBaseInStructure *>(pNext);
		if (base_in->sType == sType)
			return static_cast<const T *>(pNext);
		pNext = base_in->pNext;
	}
	return nullptr;
}

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
	sprintf(str, "%016" PRIx64, value);
	return Value(str, alloc);
}

struct GlobalStateInfo
{
	bool input_assembly;
	bool tessellation_state;
	bool viewport_state;
	bool multisample_state;
	bool depth_stencil_state;
	bool color_blend_state;
	bool vertex_input;
	bool rasterization_state;
	bool render_pass_state;
	bool layout_state;
	bool module_state;
};

struct DynamicStateInfo
{
	bool stencil_compare;
	bool stencil_reference;
	bool stencil_write_mask;
	bool depth_bounds;
	bool depth_bias;
	bool line_width;
	bool blend_constants;
	bool scissor;
	bool viewport;
	bool scissor_count;
	bool viewport_count;
	bool cull_mode;
	bool front_face;
	// Primitive topology isn't fully dynamic, so we need to hash it.
	// With dynamic state 3 unrestricted the topology is irrelevant, but that's a property.
	bool depth_test_enable;
	bool depth_write_enable;
	bool depth_compare_op;
	bool depth_bounds_test_enable;
	bool stencil_test_enable;
	bool stencil_op;
	bool vertex_input;
	bool vertex_input_binding_stride;
	bool patch_control_points;
	bool rasterizer_discard_enable;
	bool primitive_restart_enable;
	bool logic_op;
	bool color_write_enable;
	bool depth_bias_enable;
	bool discard_rectangle;
	bool discard_rectangle_mode;
	bool fragment_shading_rate;
	bool sample_locations;
	bool line_stipple;

	// Dynamic state 3
	bool tessellation_domain_origin;
	bool depth_clamp_enable;
	bool polygon_mode;
	bool rasterization_samples;
	bool sample_mask;
	bool alpha_to_coverage_enable;
	bool alpha_to_one_enable;
	bool logic_op_enable;
	bool color_blend_enable;
	bool color_blend_equation;
	bool color_write_mask;
	bool rasterization_stream;
	bool conservative_rasterization_mode;
	bool extra_primitive_overestimation_size;
	bool depth_clip_enable;
	bool sample_locations_enable;
	bool color_blend_advanced;
	bool provoking_vertex_mode;
	bool line_rasterization_mode;
	bool line_stipple_enable;
	bool depth_clip_negative_one_to_one;
	bool viewport_w_scaling_enable;
	bool viewport_swizzle;
	bool coverage_to_color_enable;
	bool coverage_to_color_location;
	bool coverage_modulation_mode;
	bool coverage_modulation_table_enable;
	bool coverage_modulation_table;
	bool shading_rate_image_enable;
	bool representative_fragment_test_enable;
	bool coverage_reduction_mode;

	bool depth_clamp_range;
};

static VkPipelineCreateFlags2KHR normalize_pipeline_creation_flags(VkPipelineCreateFlags2KHR flags)
{
	// Remove flags which do not meaningfully contribute to compilation.
	flags &= ~VkPipelineCreateFlags2KHR(VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
	                                    VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
	                                    VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT |
	                                    VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT |
	                                    VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR);
	return flags;
}

static VkGraphicsPipelineLibraryFlagsEXT graphics_pipeline_get_effective_state_flags(
		const VkGraphicsPipelineCreateInfo &create_info)
{
	VkGraphicsPipelineLibraryFlagsEXT state_flags =
			VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT |
			VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
			VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT |
			VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;

	auto *graphics_pipeline_library = find_pnext<VkGraphicsPipelineLibraryCreateInfoEXT>(
			VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
			create_info.pNext);

	// If we're not creating a library, assume we're defining a complete pipeline.
	if ((create_info.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0 && graphics_pipeline_library)
		state_flags = graphics_pipeline_library->flags;

	return state_flags;
}

static bool graphics_pipeline_library_state_flags_have_module_state(VkGraphicsPipelineLibraryFlagsEXT flags)
{
	return (flags & (VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
	                 VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT)) != 0;
}

static bool shader_stage_is_identifier_only(const VkPipelineShaderStageCreateInfo &stage)
{
	if (stage.module == VK_NULL_HANDLE)
	{
		auto *pnext = find_pnext<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, stage.pNext);
		if (!pnext)
			return true;
	}

	return false;
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
	std::unordered_map<Hash, VkPipeline> replayed_raytracing_pipelines;

	void copy_handle_references(const Impl &impl);
	void forget_handle_references();
	void forget_pipeline_handle_references();
	bool parse_samplers(StateCreatorInterface &iface, const Value &samplers) FOSSILIZE_WARN_UNUSED;
	bool parse_descriptor_set_layouts(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &layouts) FOSSILIZE_WARN_UNUSED;
	bool parse_pipeline_layouts(StateCreatorInterface &iface, const Value &layouts) FOSSILIZE_WARN_UNUSED;
	bool parse_shader_modules(StateCreatorInterface &iface, const Value &modules, const uint8_t *varint, size_t varint_size) FOSSILIZE_WARN_UNUSED;
	bool parse_render_passes(StateCreatorInterface &iface, const Value &passes) FOSSILIZE_WARN_UNUSED;
	bool parse_render_passes2(StateCreatorInterface &iface, const Value &passes) FOSSILIZE_WARN_UNUSED;
	bool parse_compute_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines) FOSSILIZE_WARN_UNUSED;
	bool parse_graphics_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines) FOSSILIZE_WARN_UNUSED;
	bool parse_raytracing_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines) FOSSILIZE_WARN_UNUSED;
	bool parse_compute_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines, const Value &member) FOSSILIZE_WARN_UNUSED;
	bool parse_graphics_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines, const Value &member) FOSSILIZE_WARN_UNUSED;
	bool parse_raytracing_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &pipelines, const Value &member) FOSSILIZE_WARN_UNUSED;
	bool parse_application_info(StateCreatorInterface &iface, const Value &app_info, const Value &pdf_info) FOSSILIZE_WARN_UNUSED;
	bool parse_application_info_link(StateCreatorInterface &iface, const Value &link) FOSSILIZE_WARN_UNUSED;

	bool parse_push_constant_ranges(const Value &ranges, const VkPushConstantRange **out_ranges) FOSSILIZE_WARN_UNUSED;
	bool parse_set_layouts(const Value &layouts, const VkDescriptorSetLayout **out_layouts) FOSSILIZE_WARN_UNUSED;
	bool parse_descriptor_set_bindings(StateCreatorInterface &iface, DatabaseInterface *resolver,
	                                   const Value &bindings, const VkDescriptorSetLayoutBinding **out_bindings) FOSSILIZE_WARN_UNUSED;
	bool parse_immutable_samplers(StateCreatorInterface &iface, DatabaseInterface *resolver,
	                              const Value &samplers, const VkSampler **out_sampler) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_attachments(const Value &attachments, const VkAttachmentDescription **out_attachments) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_dependencies(const Value &dependencies, const VkSubpassDependency **out_dependencies) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_subpasses(const Value &subpass, const VkSubpassDescription **out_descriptions) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_attachments2(const Value &attachments, const VkAttachmentDescription2 **out_attachments) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_dependencies2(const Value &dependencies, const VkSubpassDependency2 **out_dependencies) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_subpasses2(const Value &subpass, const VkSubpassDescription2 **out_descriptions) FOSSILIZE_WARN_UNUSED;
	bool parse_attachment(const Value &value, const VkAttachmentReference **out_references) FOSSILIZE_WARN_UNUSED;
	bool parse_attachments(const Value &attachments, const VkAttachmentReference **out_references) FOSSILIZE_WARN_UNUSED;
	bool parse_attachment2(const Value &value, const VkAttachmentReference2 **out_references) FOSSILIZE_WARN_UNUSED;
	bool parse_attachments2(const Value &attachments, const VkAttachmentReference2 **out_references) FOSSILIZE_WARN_UNUSED;
	bool parse_specialization_info(const Value &spec_info, const VkSpecializationInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_map_entries(const Value &map_entries, const VkSpecializationMapEntry **out_entries) FOSSILIZE_WARN_UNUSED;
	bool parse_viewports(const Value &viewports, const VkViewport **out_viewports) FOSSILIZE_WARN_UNUSED;
	bool parse_scissors(const Value &scissors, const VkRect2D **out_rects) FOSSILIZE_WARN_UNUSED;
	bool parse_pnext_chain(const Value &pnext, const void **out_pnext,
	                       StateCreatorInterface *iface = nullptr,
	                       DatabaseInterface *resolver = nullptr,
	                       const Value *pipelines = nullptr) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_input_state(const Value &state, const VkPipelineVertexInputStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_color_blend_state(const Value &state, const VkPipelineColorBlendStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_depth_stencil_state(const Value &state, const VkPipelineDepthStencilStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_state(const Value &state, const VkPipelineRasterizationStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_input_assembly_state(const Value &state, const VkPipelineInputAssemblyStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_multisample_state(const Value &state, const VkPipelineMultisampleStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_viewport_state(const Value &state, const VkPipelineViewportStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_dynamic_state(const Value &state, const VkPipelineDynamicStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_pipeline_layout_handle(const Value &state, VkPipelineLayout *out_layout) FOSSILIZE_WARN_UNUSED;
	bool parse_derived_pipeline_handle(StateCreatorInterface &iface, DatabaseInterface *resolver,
	                                   const Value &state, const Value &pipelines,
	                                   ResourceTag tag, VkPipeline *pipeline) FOSSILIZE_WARN_UNUSED;
	bool parse_raytracing_groups(const Value &state, const VkRayTracingShaderGroupCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_library_interface(const Value &state, const VkRayTracingPipelineInterfaceCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_tessellation_state(const Value &state, const VkPipelineTessellationStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_stages(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &stages, const VkPipelineShaderStageCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_attributes(const Value &attributes, const VkVertexInputAttributeDescription **out_desc) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_bindings(const Value &bindings, const VkVertexInputBindingDescription **out_desc) FOSSILIZE_WARN_UNUSED;
	bool parse_blend_attachments(const Value &attachments, const VkPipelineColorBlendAttachmentState **out_state) FOSSILIZE_WARN_UNUSED;
	bool parse_tessellation_domain_origin_state(const Value &state, VkPipelineTessellationDomainOriginStateCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_vertex_input_divisor_state(const Value &state, VkPipelineVertexInputDivisorStateCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_depth_clip_state(const Value &state, VkPipelineRasterizationDepthClipStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_stream_state(const Value &state, VkPipelineRasterizationStateStreamCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_multiview_state(const Value &state, VkRenderPassMultiviewCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_descriptor_set_binding_flags(const Value &state, VkDescriptorSetLayoutBindingFlagsCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_color_blend_advanced_state(const Value &state, VkPipelineColorBlendAdvancedStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_conservative_state(const Value &state, VkPipelineRasterizationConservativeStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rasterization_line_state(const Value &state, VkPipelineRasterizationLineStateCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_shader_stage_required_subgroup_size(const Value &state, VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_mutable_descriptor_type(const Value &state, VkMutableDescriptorTypeCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_attachment_description_stencil_layout(const Value &state, VkAttachmentDescriptionStencilLayout **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_attachment_reference_stencil_layout(const Value &state, VkAttachmentReferenceStencilLayout **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_subpass_description_depth_stencil_resolve(const Value &state, VkSubpassDescriptionDepthStencilResolve **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_fragment_shading_rate_attachment_info(const Value &state, VkFragmentShadingRateAttachmentInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_pipeline_rendering_info(const Value &state, VkPipelineRenderingCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;

	bool parse_pnext_chain_pdf2(const Value &pnext, void **out_pnext) FOSSILIZE_WARN_UNUSED;
	bool parse_robustness2_features(const Value &state, VkPhysicalDeviceRobustness2FeaturesEXT **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_image_robustness_features(const Value &state, VkPhysicalDeviceImageRobustnessFeaturesEXT **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_fragment_shading_rate_enums_features(const Value &state, VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_fragment_shading_rate_features(const Value &state, VkPhysicalDeviceFragmentShadingRateFeaturesKHR **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_mesh_shader_features(const Value &state, VkPhysicalDeviceMeshShaderFeaturesEXT **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_mesh_shader_features_nv(const Value &state, VkPhysicalDeviceMeshShaderFeaturesNV **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_descriptor_buffer_features(const Value &state, VkPhysicalDeviceDescriptorBufferFeaturesEXT **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_shader_object_features(const Value &state, VkPhysicalDeviceShaderObjectFeaturesEXT **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_primitives_generated_query_features(const Value &state, VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT **out_features) FOSSILIZE_WARN_UNUSED;
	bool parse_2d_view_of_3d_features(const Value &state, VkPhysicalDeviceImage2DViewOf3DFeaturesEXT **out_features) FOSSILIZE_WARN_UNUSED;

	bool parse_color_write(const Value &state, VkPipelineColorWriteCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_sample_locations(const Value &state, VkPipelineSampleLocationsStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_provoking_vertex(const Value &state, VkPipelineRasterizationProvokingVertexStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_sampler_custom_border_color(const Value &state, VkSamplerCustomBorderColorCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_sampler_reduction_mode(const Value &state, VkSamplerReductionModeCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_input_attachment_aspect(const Value &state, VkRenderPassInputAttachmentAspectCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_discard_rectangles(const Value &state, VkPipelineDiscardRectangleStateCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_memory_barrier2(const Value &state, VkMemoryBarrier2KHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_fragment_shading_rate(const Value &state, VkPipelineFragmentShadingRateStateCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_sampler_ycbcr_conversion(const Value &state,
	                                    VkSamplerYcbcrConversionCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_graphics_pipeline_library(const Value &state, VkGraphicsPipelineLibraryCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_pipeline_library(
			StateCreatorInterface &iface, DatabaseInterface *resolver,
			const Value &pipelines,
			const Value &state, ResourceTag tag,
			VkPipelineLibraryCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_viewport_depth_clip_control(
			const Value &state, VkPipelineViewportDepthClipControlCreateInfoEXT **clip) FOSSILIZE_WARN_UNUSED;
	bool parse_pipeline_create_flags2(
			const Value &state, VkPipelineCreateFlags2CreateInfoKHR **flags2) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_creation_control(const Value &state, VkRenderPassCreationControlEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_sampler_border_color_component_mapping(const Value &state, VkSamplerBorderColorComponentMappingCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_multisampled_render_to_single_sampled(const Value &state, VkMultisampledRenderToSingleSampledInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_depth_bias_representation(const Value &state, VkDepthBiasRepresentationInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_render_pass_fragment_density_map(const Value &state, VkRenderPassFragmentDensityMapCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_sample_locations_info(const Value &state, VkSampleLocationsInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_pipeline_robustness(const Value &state, VkPipelineRobustnessCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_depth_clamp_control(const Value &state, VkPipelineViewportDepthClampControlCreateInfoEXT **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rendering_attachment_location_info(const Value &state, VkRenderingAttachmentLocationInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_rendering_input_attachment_index_info(const Value &state, VkRenderingInputAttachmentIndexInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool parse_uints(const Value &attachments, const uint32_t **out_uints) FOSSILIZE_WARN_UNUSED;
	bool parse_sints(const Value &attachments, const int32_t **out_uints) FOSSILIZE_WARN_UNUSED;
	const char *duplicate_string(const char *str, size_t len);
	bool resolve_derivative_pipelines = true;
	bool resolve_shader_modules = true;

	template <typename T>
	T *copy(const T *src, size_t count);
};

struct WorkItem
{
	VkStructureType type;
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
	ScratchAllocator ycbcr_temp_allocator;
	DatabaseInterface *database_iface = nullptr;
	DatabaseInterface *module_identifier_database_iface = nullptr;
	DatabaseInterface *on_use_database_iface = nullptr;
	ApplicationInfoFilter *application_info_filter = nullptr;
	bool should_record_identifier_only = false;

	std::unordered_map<Hash, VkDescriptorSetLayoutCreateInfo *> descriptor_sets;
	std::unordered_map<Hash, VkPipelineLayoutCreateInfo *> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModuleCreateInfo *> shader_modules;
	std::unordered_map<Hash, VkGraphicsPipelineCreateInfo *> graphics_pipelines;
	std::unordered_map<Hash, VkComputePipelineCreateInfo *> compute_pipelines;
	std::unordered_map<Hash, VkRayTracingPipelineCreateInfoKHR *> raytracing_pipelines;
	std::unordered_map<Hash, void *> render_passes;
	std::unordered_map<Hash, VkSamplerCreateInfo *> samplers;
	std::unordered_map<VkSamplerYcbcrConversion, const VkSamplerYcbcrConversionCreateInfo *> ycbcr_conversions;

	std::unordered_map<VkDescriptorSetLayout, Hash> descriptor_set_layout_to_hash;
	std::unordered_map<VkPipelineLayout, Hash> pipeline_layout_to_hash;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_hash;
	std::unordered_map<VkPipeline, Hash> graphics_pipeline_to_hash;
	std::unordered_map<VkPipeline, Hash> compute_pipeline_to_hash;
	std::unordered_map<VkPipeline, Hash> raytracing_pipeline_to_hash;
	std::unordered_map<VkRenderPass, Hash> render_pass_to_hash;
	std::unordered_map<VkSampler, Hash> sampler_to_hash;

	struct SubpassMetaStorage
	{
		// Holds 16 subpasses' worth of state. Hits ~100% of the time.
		uint32_t embedded;
		uint32_t subpass_count;
		// Spillage
		std::vector<uint32_t> fallback;
	};
	std::unordered_map<Hash, SubpassMetaStorage> render_pass_hash_to_subpass_meta;
	template <typename CreateInfo>
	static SubpassMetaStorage analyze_subpass_meta_storage(const CreateInfo &render_pass_create_info);

	std::unordered_map<VkShaderModuleIdentifierEXT, VkShaderModule> identifier_to_module;

	VkApplicationInfo *application_info = nullptr;
	VkPhysicalDeviceFeatures2 *physical_device_features = nullptr;
	StateRecorderApplicationFeatureHash application_feature_hash = {};

	bool copy_descriptor_set_layout(const VkDescriptorSetLayoutCreateInfo *create_info, ScratchAllocator &alloc, VkDescriptorSetLayoutCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_pipeline_layout(const VkPipelineLayoutCreateInfo *create_info, ScratchAllocator &alloc, VkPipelineLayoutCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_shader_module(const VkShaderModuleCreateInfo *create_info, ScratchAllocator &alloc,
	                        bool ignore_pnext, VkShaderModuleCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo *create_info, ScratchAllocator &alloc,
	                            const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
	                            VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
	                            VkGraphicsPipelineCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_compute_pipeline(const VkComputePipelineCreateInfo *create_info, ScratchAllocator &alloc,
	                           const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
	                           VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
	                           VkComputePipelineCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_raytracing_pipeline(const VkRayTracingPipelineCreateInfoKHR *create_info, ScratchAllocator &alloc,
	                              const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
	                              VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
	                              VkRayTracingPipelineCreateInfoKHR **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_sampler(const VkSamplerCreateInfo *create_info, ScratchAllocator &alloc,
	                  VkSamplerCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_ycbcr_conversion(const VkSamplerYcbcrConversionCreateInfo *create_info, ScratchAllocator &alloc,
	                           VkSamplerYcbcrConversionCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_render_pass(const VkRenderPassCreateInfo *create_info, ScratchAllocator &alloc,
	                      VkRenderPassCreateInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_render_pass2(const VkRenderPassCreateInfo2 *create_info, ScratchAllocator &alloc,
	                       VkRenderPassCreateInfo2 **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_application_info(const VkApplicationInfo *app_info, ScratchAllocator &alloc, VkApplicationInfo **out_info) FOSSILIZE_WARN_UNUSED;
	bool copy_physical_device_features(const void *device_pnext, ScratchAllocator &alloc, VkPhysicalDeviceFeatures2 **out_features) FOSSILIZE_WARN_UNUSED;

	bool copy_specialization_info(const VkSpecializationInfo *info, ScratchAllocator &alloc, const VkSpecializationInfo **out_info) FOSSILIZE_WARN_UNUSED;

	template <typename CreateInfo>
	bool copy_stages(CreateInfo *info, ScratchAllocator &alloc,
	                 VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
	                 const DynamicStateInfo *dynamic_state_info) FOSSILIZE_WARN_UNUSED;

	static bool add_module_identifier(VkPipelineShaderStageCreateInfo *info, ScratchAllocator &alloc,
	                                  VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii);

	template <typename CreateInfo>
	bool copy_dynamic_state(CreateInfo *info, ScratchAllocator &alloc,
	                        const DynamicStateInfo *dynamic_state_info) FOSSILIZE_WARN_UNUSED;

	template <typename SubCreateInfo>
	bool copy_sub_create_info(const SubCreateInfo *&info, ScratchAllocator &alloc,
	                          const DynamicStateInfo *dynamic_state_info,
	                          VkGraphicsPipelineLibraryFlagsEXT state_flags) FOSSILIZE_WARN_UNUSED;

	void *copy_pnext_struct(const VkPipelineVertexInputDivisorStateCreateInfoKHR *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkRenderPassMultiviewCreateInfo *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkMutableDescriptorTypeCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkSubpassDescriptionDepthStencilResolve *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkFragmentShadingRateAttachmentInfoKHR *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineRenderingCreateInfoKHR *create_info,
	                        ScratchAllocator &alloc,
	                        VkGraphicsPipelineLibraryFlagsEXT state_flags) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineColorWriteCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc,
	                        const DynamicStateInfo *dynamic_state_info) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineSampleLocationsStateCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc,
	                        const DynamicStateInfo *dynamic_state_info) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkRenderPassInputAttachmentAspectCreateInfo *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineDiscardRectangleStateCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc,
	                        const DynamicStateInfo *dynamic_state_info) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineLibraryCreateInfoKHR *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkSampleLocationsInfoEXT *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkPipelineViewportDepthClampControlCreateInfoEXT *create_info,
	                        ScratchAllocator &alloc,
	                        const DynamicStateInfo *dynamic_state_info) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkRenderingAttachmentLocationInfoKHR *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	void *copy_pnext_struct(const VkRenderingInputAttachmentIndexInfoKHR *create_info,
	                        ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;
	template <typename T>
	void *copy_pnext_struct_simple(const T *create_info, ScratchAllocator &alloc) FOSSILIZE_WARN_UNUSED;

	bool remap_sampler_handle(VkSampler sampler, VkSampler *out_sampler) const FOSSILIZE_WARN_UNUSED;
	bool remap_descriptor_set_layout_handle(VkDescriptorSetLayout layout, VkDescriptorSetLayout *out_layout) const FOSSILIZE_WARN_UNUSED;
	bool remap_pipeline_layout_handle(VkPipelineLayout layout, VkPipelineLayout *out_layout) const FOSSILIZE_WARN_UNUSED;
	bool remap_render_pass_handle(VkRenderPass render_pass, VkRenderPass *out_render_pass) const FOSSILIZE_WARN_UNUSED;
	bool remap_shader_module_handle(VkShaderModule shader_module, VkShaderModule *out_shader_module) const FOSSILIZE_WARN_UNUSED;
	bool remap_compute_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const FOSSILIZE_WARN_UNUSED;
	bool remap_graphics_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const FOSSILIZE_WARN_UNUSED;
	bool remap_raytracing_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const FOSSILIZE_WARN_UNUSED;

	bool remap_descriptor_set_layout_ci(VkDescriptorSetLayoutCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_pipeline_layout_ci(VkPipelineLayoutCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_shader_module_ci(VkShaderModuleCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_graphics_pipeline_ci(VkGraphicsPipelineCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_compute_pipeline_ci(VkComputePipelineCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_raytracing_pipeline_ci(VkRayTracingPipelineCreateInfoKHR *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_sampler_ci(VkSamplerCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	bool remap_render_pass_ci(VkRenderPassCreateInfo *create_info) FOSSILIZE_WARN_UNUSED;
	template <typename CreateInfo>
	bool remap_shader_module_handles(CreateInfo *info) FOSSILIZE_WARN_UNUSED;
	bool remap_shader_module_handle(VkPipelineShaderStageCreateInfo &info) FOSSILIZE_WARN_UNUSED;
	void register_module_identifier(VkShaderModule module, const VkPipelineShaderStageModuleIdentifierCreateInfoEXT &ident);
	void register_on_use(ResourceTag tag, Hash hash) const;

	bool get_subpass_meta_for_render_pass_hash(Hash render_pass_hash,
	                                           uint32_t subpass,
	                                           SubpassMeta *meta) const FOSSILIZE_WARN_UNUSED;
	bool get_subpass_meta_for_pipeline(const VkGraphicsPipelineCreateInfo &create_info,
	                                   Hash render_pass_hash,
	                                   SubpassMeta *meta) const FOSSILIZE_WARN_UNUSED;

	bool get_hash_for_shader_module(
			const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *identifier, Hash *hash) const FOSSILIZE_WARN_UNUSED;

	bool serialize_application_info(std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_application_blob_link(Hash hash, ResourceTag tag, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	Hash get_application_link_hash(ResourceTag tag, Hash hash) const;
	bool register_application_link_hash(ResourceTag tag, Hash hash, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_sampler(Hash hash, const VkSamplerCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_render_pass(Hash hash, const VkRenderPassCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_render_pass2(Hash hash, const VkRenderPassCreateInfo2 &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_shader_module(Hash hash, const VkShaderModuleCreateInfo &create_info, std::vector<uint8_t> &blob, ScratchAllocator &allocator) const FOSSILIZE_WARN_UNUSED;
	bool serialize_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;
	bool serialize_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR &create_info, std::vector<uint8_t> &blob) const FOSSILIZE_WARN_UNUSED;

	std::mutex record_lock;
	std::mutex synchronized_record_lock;
	std::condition_variable record_cv;
	std::queue<WorkItem> record_queue;
	std::thread worker_thread;

	void push_work_locked(const WorkItem &item);
	template <typename T>
	void push_unregister_locked(VkStructureType sType, T obj);

	bool compression = false;
	bool checksum = false;
	bool application_feature_links = true;

	void record_task(StateRecorder *recorder, bool looping);
	void pump_synchronized_recording(StateRecorder *recorder);

	template <typename T>
	T *copy(const T *src, size_t count, ScratchAllocator &alloc);
	bool copy_pnext_chain(const void *pNext, ScratchAllocator &alloc, const void **out_pnext,
	                      const DynamicStateInfo *dynamic_state_info,
	                      VkGraphicsPipelineLibraryFlagsEXT state_flags) FOSSILIZE_WARN_UNUSED;
	template <typename T>
	bool copy_pnext_chains(const T *ts, uint32_t count, ScratchAllocator &alloc,
	                       const DynamicStateInfo *dynamic_state_info,
	                       VkGraphicsPipelineLibraryFlagsEXT state_flags) FOSSILIZE_WARN_UNUSED;

	bool copy_pnext_chain_pdf2(const void *pNext, ScratchAllocator &alloc, void **out_pnext) FOSSILIZE_WARN_UNUSED;

	Hash record_shader_module(const WorkItem &record_item, bool dependent_record);

	struct
	{
		bool write_database_entries = true;
		PayloadWriteFlags payload_flags = 0;
		bool need_flush = false;
		bool need_prepare = true;
		vector<uint8_t> blob;
	} record_data;
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

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceRobustness2FeaturesEXT &info)
{
	h.u32(info.robustBufferAccess2);
	h.u32(info.robustImageAccess2);
	h.u32(info.nullDescriptor);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceImageRobustnessFeaturesEXT &info)
{
	h.u32(info.robustImageAccess);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV &info)
{
	h.u32(info.noInvocationFragmentShadingRates);
	h.u32(info.fragmentShadingRateEnums);
	// Specifically known to affect shader compilation on NV.
	// Just hash the entire struct while we're at it though ...
	h.u32(info.supersampleFragmentShadingRates);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceFragmentShadingRateFeaturesKHR &info)
{
	h.u32(info.pipelineFragmentShadingRate);
	h.u32(info.primitiveFragmentShadingRate);
	h.u32(info.attachmentFragmentShadingRate);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceMeshShaderFeaturesNV &info)
{
	h.u32(info.taskShader);
	h.u32(info.meshShader);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceMeshShaderFeaturesEXT &info)
{
	h.u32(info.taskShader);
	h.u32(info.meshShader);
	h.u32(info.multiviewMeshShader);
	h.u32(info.primitiveFragmentShadingRateMeshShader);
	h.u32(info.meshShaderQueries);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceDescriptorBufferFeaturesEXT &info)
{
	h.u32(info.descriptorBuffer);
	h.u32(info.descriptorBufferCaptureReplay);
	h.u32(info.descriptorBufferImageLayoutIgnored);
	h.u32(info.descriptorBufferPushDescriptors);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceShaderObjectFeaturesEXT &info)
{
	h.u32(info.shaderObject);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT &info)
{
	h.u32(info.primitivesGeneratedQuery);
	h.u32(info.primitivesGeneratedQueryWithNonZeroStreams);
	h.u32(info.primitivesGeneratedQueryWithRasterizerDiscard);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPhysicalDeviceImage2DViewOf3DFeaturesEXT &info)
{
	h.u32(info.image2DViewOf3D);
	h.u32(info.sampler2DViewOf3D);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineCreateFlags2CreateInfoKHR &info)
{
	auto flags = normalize_pipeline_creation_flags(info.flags);
	h.u64(flags);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkRenderPassCreationControlEXT &info)
{
	h.u32(info.disallowMerging);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkSamplerBorderColorComponentMappingCreateInfoEXT &info)
{
	h.u32(info.srgb);
	h.u32(info.components.r);
	h.u32(info.components.g);
	h.u32(info.components.b);
	h.u32(info.components.a);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkMultisampledRenderToSingleSampledInfoEXT &info)
{
	h.u32(info.multisampledRenderToSingleSampledEnable);
	h.u32(info.rasterizationSamples);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkDepthBiasRepresentationInfoEXT &info, const DynamicStateInfo *dynamic_state)
{
	if (!dynamic_state || !dynamic_state->depth_bias)
	{
		h.u32(info.depthBiasExact);
		h.u32(info.depthBiasRepresentation);
	}
	else
		h.u32(0);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkRenderPassFragmentDensityMapCreateInfoEXT &info)
{
	h.u32(info.fragmentDensityMapAttachment.attachment);
	h.u32(info.fragmentDensityMapAttachment.layout);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkSampleLocationsInfoEXT &info)
{
	h.u32(info.sampleLocationsCount);
	h.u32(info.sampleLocationGridSize.width);
	h.u32(info.sampleLocationGridSize.height);
	h.u32(info.sampleLocationsPerPixel);
	for (uint32_t i = 0; i < info.sampleLocationsCount; i++)
	{
		h.f32(info.pSampleLocations[i].x);
		h.f32(info.pSampleLocations[i].y);
	}
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkPipelineRobustnessCreateInfoEXT &info)
{
	h.u32(info.images);
	h.u32(info.vertexInputs);
	h.u32(info.uniformBuffers);
	h.u32(info.storageBuffers);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkPipelineViewportDepthClampControlCreateInfoEXT &info,
							  const DynamicStateInfo *dynamic_state)
{
	if (dynamic_state && dynamic_state->depth_clamp_range)
		return;

	h.u32(info.depthClampMode);
	if (info.depthClampMode == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT && info.pDepthClampRange)
	{
		h.f32(info.pDepthClampRange->minDepthClamp);
		h.f32(info.pDepthClampRange->maxDepthClamp);
	}
	else
		h.u32(0);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkRenderingAttachmentLocationInfoKHR &info)
{
	h.u32(info.colorAttachmentCount);
	if (info.pColorAttachmentLocations)
	{
		for (uint32_t i = 0; i < info.colorAttachmentCount; i++)
			h.u32(info.pColorAttachmentLocations[i]);
	}
	else
		h.u32(0);
}

static void hash_pnext_struct(const StateRecorder *, Hasher &h,
                              const VkRenderingInputAttachmentIndexInfoKHR &info)
{
	h.u32(info.colorAttachmentCount);

	if (info.pColorAttachmentInputIndices)
	{
		for (uint32_t i = 0; i < info.colorAttachmentCount; i++)
			h.u32(info.pColorAttachmentInputIndices[i]);
	}
	else
		h.u32(0);

	if (info.pDepthInputAttachmentIndex)
		h.u32(*info.pDepthInputAttachmentIndex);
	else
		h.u32(0xffff); // Use an arbitrary invalid attachment value.

	if (info.pStencilInputAttachmentIndex)
		h.u32(*info.pStencilInputAttachmentIndex);
	else
		h.u32(0xffff); // Use an arbitrary invalid attachment value.
}

static bool hash_pnext_chain_pdf2(const StateRecorder *recorder, Hasher &h, const void *pNext)
{
	while ((pNext = pnext_chain_pdf2_skip_ignored_entries(pNext)) != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);
		h.s32(pin->sType);

		// Pull in any robustness-like feature and other types which are known to affect shader compilation.
		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceRobustness2FeaturesEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceImageRobustnessFeaturesEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceMeshShaderFeaturesEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceMeshShaderFeaturesNV *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceShaderObjectFeaturesEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *>(pNext));
			break;

		default:
			log_error_pnext_chain("Unsupported pNext found, cannot hash.", pNext);
			return false;
		}

		pNext = pin->pNext;
	}

	return true;
}

static Hash compute_hash_physical_device_features(const void *device_pnext)
{
	Hasher h;

	// For hash invariance, make sure we hash PDF2 first, since when we serialize and unserialize,
	// PDF2 will always come first in the chain.
	auto *pdf2 = find_pnext<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, device_pnext);
	if (pdf2)
		h.u32(pdf2->features.robustBufferAccess);
	else
		h.u32(0);

	hash_pnext_chain_pdf2(nullptr, h, device_pnext);
	return h.get();
}

StateRecorderApplicationFeatureHash compute_application_feature_hash(const VkApplicationInfo *info,
                                                                     const void *device_pnext)
{
	StateRecorderApplicationFeatureHash hash = {};
	if (info)
		hash.application_info_hash = compute_hash_application_info(*info);
	if (device_pnext)
		hash.physical_device_features_hash = compute_hash_physical_device_features(device_pnext);
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

static bool hash_pnext_chain(const StateRecorder *recorder, Hasher &h, const void *pNext,
                             const DynamicStateInfo *dynamic_state_info,
                             VkGraphicsPipelineLibraryFlagsEXT state_flags) FOSSILIZE_WARN_UNUSED;

bool compute_hash_sampler(const VkSamplerCreateInfo &sampler, Hash *out_hash)
{
	Hasher h;

	constexpr VkSamplerCreateFlagBits ignore_capture_replay_flags =
			VK_SAMPLER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT;

	h.u32(sampler.flags & ~ignore_capture_replay_flags);
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

	if (!hash_pnext_chain(nullptr, h, sampler.pNext, nullptr, 0))
		return false;

	*out_hash = h.get();
	return true;
}

static bool validate_pnext_chain(const void *pNext, const VkStructureType *expected, uint32_t count)
{
	while ((pNext = pnext_chain_skip_ignored_entries(pNext)) != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);
		auto sType = pin->sType;
		if (std::find(expected, expected + count, sType) == expected + count)
			return false;
		pNext = pin->pNext;
	}

	return true;
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

	if (!hash_pnext_chain(&recorder, h, layout.pNext, nullptr, 0))
		return false;

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

bool compute_hash_shader_module(const VkShaderModuleCreateInfo &create_info, Hash *out_hash)
{
	Hasher h;
	h.data(create_info.pCode, create_info.codeSize);
	h.u32(create_info.flags);
	*out_hash = h.get();
	return true;
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

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineTessellationDomainOriginStateCreateInfo &create_info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(dynamic_state_info && dynamic_state_info->tessellation_domain_origin ? 0 : create_info.domainOrigin);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineVertexInputDivisorStateCreateInfoKHR &create_info)
{
	h.u32(create_info.vertexBindingDivisorCount);
	for (uint32_t i = 0; i < create_info.vertexBindingDivisorCount; i++)
	{
		h.u32(create_info.pVertexBindingDivisors[i].binding);
		h.u32(create_info.pVertexBindingDivisors[i].divisor);
	}
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineRasterizationDepthClipStateCreateInfoEXT &create_info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(create_info.flags);
	h.u32(dynamic_state_info && dynamic_state_info->depth_clip_enable ? 0 : create_info.depthClipEnable);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineRasterizationStateStreamCreateInfoEXT &create_info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(create_info.flags);
	h.u32(dynamic_state_info && dynamic_state_info->rasterization_stream ? 0 : create_info.rasterizationStream);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkRenderPassMultiviewCreateInfo &create_info)
{
	h.u32(create_info.subpassCount);
	for (uint32_t i = 0; i < create_info.subpassCount; i++)
		h.u32(create_info.pViewMasks[i]);
	h.u32(create_info.dependencyCount);
	for (uint32_t i = 0; i < create_info.dependencyCount; i++)
		h.s32(create_info.pViewOffsets[i]);
	h.u32(create_info.correlationMaskCount);
	for (uint32_t i = 0; i < create_info.correlationMaskCount; i++)
		h.u32(create_info.pCorrelationMasks[i]);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT &create_info)
{
	h.u32(create_info.bindingCount);
	for (uint32_t i = 0; i < create_info.bindingCount; i++)
		h.u32(create_info.pBindingFlags[i]);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineColorBlendAdvancedStateCreateInfoEXT &create_info,
                              const DynamicStateInfo *dynamic_state_info)
{
	if (dynamic_state_info && dynamic_state_info->color_blend_advanced)
	{
		h.u32(0);
	}
	else
	{
		h.u32(create_info.srcPremultiplied);
		h.u32(create_info.dstPremultiplied);
		h.u32(create_info.blendOverlap);
	}
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineRasterizationConservativeStateCreateInfoEXT &create_info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(create_info.flags);
	h.u32(dynamic_state_info && dynamic_state_info->conservative_rasterization_mode ? 0 : create_info.conservativeRasterizationMode);
	h.f32(dynamic_state_info && dynamic_state_info->extra_primitive_overestimation_size ? 0.0f : create_info.extraPrimitiveOverestimationSize);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineRasterizationLineStateCreateInfoKHR &create_info,
                              const DynamicStateInfo *dynamic_state_info)
{
	bool can_enable_stipple = (dynamic_state_info && dynamic_state_info->line_stipple_enable) || create_info.stippledLineEnable;
	bool dynamic_stipple_values = dynamic_state_info && dynamic_state_info->line_stipple;
	bool enable_stipple_values = can_enable_stipple && !dynamic_stipple_values;

	h.u32(dynamic_state_info && dynamic_state_info->line_rasterization_mode ? 0 : create_info.lineRasterizationMode);
	h.u32(dynamic_state_info && dynamic_state_info->line_stipple_enable ? 0 : create_info.stippledLineEnable);
	h.u32(enable_stipple_values ? create_info.lineStippleFactor : 0);
	h.u32(enable_stipple_values ? create_info.lineStipplePattern : 0);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT &create_info)
{
	h.u32(create_info.requiredSubgroupSize);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkMutableDescriptorTypeCreateInfoEXT &mutable_info)
{
	h.u32(mutable_info.mutableDescriptorTypeListCount);
	for (uint32_t i = 0; i < mutable_info.mutableDescriptorTypeListCount; i++)
	{
		auto &l = mutable_info.pMutableDescriptorTypeLists[i];
		h.u32(l.descriptorTypeCount);
		for (uint32_t j = 0; j < l.descriptorTypeCount; j++)
			h.s32(l.pDescriptorTypes[j]);
	}
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkAttachmentDescriptionStencilLayoutKHR &info)
{
	h.u32(info.stencilInitialLayout);
	h.u32(info.stencilFinalLayout);
}

static bool hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkFragmentShadingRateAttachmentInfoKHR &info)
{
	if (info.pFragmentShadingRateAttachment)
	{
		h.u32(info.pFragmentShadingRateAttachment->attachment);
		h.u32(info.pFragmentShadingRateAttachment->layout);
		h.u32(info.pFragmentShadingRateAttachment->aspectMask);
		h.u32(info.shadingRateAttachmentTexelSize.width);
		h.u32(info.shadingRateAttachmentTexelSize.height);

		// Avoid potential stack overflow on intentionally broken input.
		// It is also meaningless, since the only pNext we can consider here
		// is stencil layout.
		if (info.pFragmentShadingRateAttachment->pNext)
			return false;
	}
	else
		h.u32(0);

	return true;
}

static bool hash_pnext_struct(const StateRecorder *recorder,
                              Hasher &h,
                              const VkSubpassDescriptionDepthStencilResolve &info)
{
	if (info.pDepthStencilResolveAttachment)
	{
		h.u32(info.depthResolveMode);
		h.u32(info.stencilResolveMode);

		h.u32(info.pDepthStencilResolveAttachment->attachment);
		h.u32(info.pDepthStencilResolveAttachment->layout);
		h.u32(info.pDepthStencilResolveAttachment->aspectMask);

		// Ensures we cannot get a recursive cycle.
		const VkStructureType expected = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT;
		if (!validate_pnext_chain(info.pDepthStencilResolveAttachment->pNext, &expected, 1))
			return false;
		if (!hash_pnext_chain(recorder, h, info.pDepthStencilResolveAttachment->pNext, nullptr, 0))
			return false;
	}
	else
		h.u32(0);

	return true;
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkAttachmentReferenceStencilLayoutKHR &info)
{
	h.u32(info.stencilLayout);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineRenderingCreateInfoKHR &info,
                              VkGraphicsPipelineLibraryFlagsEXT state_flags)
{
	bool format_sensitive = (state_flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) != 0;
	bool view_mask_sensitive = (state_flags & (VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT |
	                                           VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
	                                           VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT)) != 0;

	// Keep hash order for hash compat.

	if (format_sensitive)
		h.u32(info.colorAttachmentCount);

	if (view_mask_sensitive)
		h.u32(info.viewMask);

	if (format_sensitive)
	{
		for (uint32_t i = 0; i < info.colorAttachmentCount; i++)
			h.u32(info.pColorAttachmentFormats[i]);
		h.u32(info.depthAttachmentFormat);
		h.u32(info.stencilAttachmentFormat);
	}
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineColorWriteCreateInfoEXT &info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(info.attachmentCount);
	if (dynamic_state_info && !dynamic_state_info->color_write_enable)
		for (uint32_t i = 0; i < info.attachmentCount; i++)
			h.u32(info.pColorWriteEnables[i]);
}

static bool hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineSampleLocationsStateCreateInfoEXT &info,
                              const DynamicStateInfo *dynamic_state_info)
{
	bool dynamic_enable = dynamic_state_info && dynamic_state_info->sample_locations_enable;
	bool dynamic_locations = dynamic_state_info && dynamic_state_info->sample_locations;
	h.u32(dynamic_enable ? 0 : info.sampleLocationsEnable);
	if ((dynamic_enable || info.sampleLocationsEnable) && !dynamic_locations)
	{
		if (info.sampleLocationsInfo.pNext)
			return false;

		h.u32(info.sampleLocationsInfo.sampleLocationGridSize.width);
		h.u32(info.sampleLocationsInfo.sampleLocationGridSize.height);
		h.u32(info.sampleLocationsInfo.sampleLocationsPerPixel);
		h.u32(info.sampleLocationsInfo.sampleLocationsCount);
		for (uint32_t i = 0; i < info.sampleLocationsInfo.sampleLocationsCount; i++)
		{
			h.f32(info.sampleLocationsInfo.pSampleLocations[i].x);
			h.f32(info.sampleLocationsInfo.pSampleLocations[i].y);
		}
	}
	else
		h.u32(0);

	return true;
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT &info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(dynamic_state_info && dynamic_state_info->provoking_vertex_mode ? 0 : info.provokingVertexMode);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkSamplerCustomBorderColorCreateInfoEXT &info)
{
	h.u32(info.customBorderColor.uint32[0]);
	h.u32(info.customBorderColor.uint32[1]);
	h.u32(info.customBorderColor.uint32[2]);
	h.u32(info.customBorderColor.uint32[3]);
	h.u32(info.format);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkSamplerReductionModeCreateInfo &info)
{
	h.u32(info.reductionMode);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkRenderPassInputAttachmentAspectCreateInfo &info)
{
	h.u32(info.aspectReferenceCount);
	for (uint32_t i = 0; i < info.aspectReferenceCount; i++)
	{
		h.u32(info.pAspectReferences[i].subpass);
		h.u32(info.pAspectReferences[i].inputAttachmentIndex);
		h.u32(info.pAspectReferences[i].aspectMask);
	}
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineDiscardRectangleStateCreateInfoEXT &info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(info.flags);
	h.u32(dynamic_state_info->discard_rectangle_mode ? 0 : info.discardRectangleMode);
	h.u32(info.discardRectangleCount);
	if (dynamic_state_info && !dynamic_state_info->discard_rectangle)
	{
		for (uint32_t i = 0; i < info.discardRectangleCount; i++)
		{
			h.s32(info.pDiscardRectangles[i].offset.x);
			h.s32(info.pDiscardRectangles[i].offset.y);
			h.u32(info.pDiscardRectangles[i].extent.width);
			h.u32(info.pDiscardRectangles[i].extent.height);
		}
	}
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkMemoryBarrier2KHR &info)
{
	h.u64(info.srcStageMask);
	h.u64(info.srcAccessMask);
	h.u64(info.dstStageMask);
	h.u64(info.dstAccessMask);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineFragmentShadingRateStateCreateInfoKHR &info,
                              const DynamicStateInfo *dynamic_state_info)
{
	if (dynamic_state_info && !dynamic_state_info->fragment_shading_rate)
	{
		h.u32(info.fragmentSize.width);
		h.u32(info.fragmentSize.height);
		h.u32(info.combinerOps[0]);
		h.u32(info.combinerOps[1]);
	}
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkSamplerYcbcrConversionCreateInfo &info)
{
	h.u32(info.format);
	h.u32(info.ycbcrModel);
	h.u32(info.ycbcrRange);
	h.u32(info.components.r);
	h.u32(info.components.g);
	h.u32(info.components.b);
	h.u32(info.components.a);
	h.u32(info.xChromaOffset);
	h.u32(info.yChromaOffset);
	h.u32(info.chromaFilter);
	h.u32(info.forceExplicitReconstruction);
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkGraphicsPipelineLibraryCreateInfoEXT &info)
{
	h.u32(info.flags);
}

static bool hash_pnext_struct(const StateRecorder *recorder,
                              Hasher &h,
                              const VkPipelineLibraryCreateInfoKHR &info)
{
	Hash hash = 0;
	h.u32(info.libraryCount);

	for (uint32_t i = 0; i < info.libraryCount; i++)
	{
		if (!recorder->get_hash_for_pipeline_library_handle(info.pLibraries[i], &hash))
			return false;
		h.u64(hash);
	}

	return true;
}

static void hash_pnext_struct(const StateRecorder *,
                              Hasher &h,
                              const VkPipelineViewportDepthClipControlCreateInfoEXT &info,
                              const DynamicStateInfo *dynamic_state_info)
{
	h.u32(dynamic_state_info && dynamic_state_info->depth_clip_negative_one_to_one ? 0 : info.negativeOneToOne);
}

static bool hash_pnext_chain(const StateRecorder *recorder, Hasher &h, const void *pNext,
                             const DynamicStateInfo *dynamic_state_info,
                             VkGraphicsPipelineLibraryFlagsEXT state_flags)
{
	while ((pNext = pnext_chain_skip_ignored_entries(pNext)) != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);

		if (pnext_chain_stype_is_hash_invariant(pin->sType))
		{
			pNext = pin->pNext;
			continue;
		}

		h.s32(pin->sType);

		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineTessellationDomainOriginStateCreateInfo *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineVertexInputDivisorStateCreateInfoKHR *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
			hash_pnext_struct(recorder, h, *static_cast<const VkRenderPassMultiviewCreateInfo *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineColorBlendAdvancedStateCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRasterizationConservativeStateCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRasterizationLineStateCreateInfoKHR *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkMutableDescriptorTypeCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkAttachmentDescriptionStencilLayoutKHR *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
			if (!hash_pnext_struct(recorder, h, *static_cast<const VkFragmentShadingRateAttachmentInfoKHR *>(pNext)))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
			if (!hash_pnext_struct(recorder, h, *static_cast<const VkSubpassDescriptionDepthStencilResolve *>(pNext)))
				return false;
			break;

		case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkAttachmentReferenceStencilLayoutKHR *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRenderingCreateInfoKHR *>(pNext), state_flags);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineColorWriteCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT:
			if (!hash_pnext_struct(recorder, h, *static_cast<const VkPipelineSampleLocationsStateCreateInfoEXT *>(pNext), dynamic_state_info))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkSamplerCustomBorderColorCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO:
			hash_pnext_struct(recorder, h, *static_cast<const VkSamplerReductionModeCreateInfo *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
			hash_pnext_struct(recorder, h, *static_cast<const VkRenderPassInputAttachmentAspectCreateInfo *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineDiscardRectangleStateCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkMemoryBarrier2KHR *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO:
			hash_pnext_struct(recorder, h, *static_cast<const VkSamplerYcbcrConversionCreateInfo *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkGraphicsPipelineLibraryCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
			if (!hash_pnext_struct(recorder, h, *static_cast<const VkPipelineLibraryCreateInfoKHR *>(pNext)))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineCreateFlags2CreateInfoKHR *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkRenderPassCreationControlEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkSamplerBorderColorComponentMappingCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkMultisampledRenderToSingleSampledInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkDepthBiasRepresentationInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkRenderPassFragmentDensityMapCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkSampleLocationsInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineRobustnessCreateInfoEXT *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT:
			hash_pnext_struct(recorder, h, *static_cast<const VkPipelineViewportDepthClampControlCreateInfoEXT *>(pNext), dynamic_state_info);
			break;

		case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkRenderingAttachmentLocationInfoKHR *>(pNext));
			break;

		case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR:
			hash_pnext_struct(recorder, h, *static_cast<const VkRenderingInputAttachmentIndexInfoKHR *>(pNext));
			break;

		default:
			log_error_pnext_chain("Unsupported pNext found, cannot hash.", pNext);
			return false;
		}

		pNext = pin->pNext;
	}

	return true;
}

static bool compute_hash_stage(const StateRecorder &recorder, Hasher &h, const VkPipelineShaderStageCreateInfo &stage)
{
	if (!stage.pName)
		return false;

	h.u32(stage.flags);
	h.string(stage.pName);
	h.u32(stage.stage);

	Hash hash;
	if (stage.module != VK_NULL_HANDLE)
	{
		if (!recorder.get_hash_for_shader_module(stage.module, &hash))
			return false;
	}
	else if (const auto *module = find_pnext<VkShaderModuleCreateInfo>(
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, stage.pNext))
	{
		if (!compute_hash_shader_module(*module, &hash))
			return false;
	}
	else if (const auto *identifier = find_pnext<VkPipelineShaderStageModuleIdentifierCreateInfoEXT>(
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, stage.pNext))
	{
		if (!recorder.get_hash_for_shader_module(identifier, &hash))
			return false;
	}
	else
		return false;

	h.u64(hash);

	if (stage.pSpecializationInfo)
		hash_specialization_info(h, *stage.pSpecializationInfo);
	else
		h.u32(0);

	if (!hash_pnext_chain(&recorder, h, stage.pNext, nullptr, 0))
		return false;

	return true;
}

static GlobalStateInfo parse_global_state_info(const VkGraphicsPipelineCreateInfo &create_info,
                                               const DynamicStateInfo &dynamic_info,
                                               const StateRecorder::SubpassMeta &meta)
{
	GlobalStateInfo info = {};

	info.rasterization_state = create_info.pRasterizationState != nullptr;
	info.render_pass_state = true;
	info.module_state = true;
	info.layout_state = true;

	auto state_flags = graphics_pipeline_get_effective_state_flags(create_info);

	info.rasterization_state = create_info.pRasterizationState &&
	                           (state_flags & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) != 0;

	bool rasterizer_discard = !dynamic_info.rasterizer_discard_enable &&
	                          info.rasterization_state &&
	                          create_info.pRasterizationState->rasterizerDiscardEnable == VK_TRUE;

	if (!rasterizer_discard)
	{
		info.viewport_state = create_info.pViewportState != nullptr;
		info.multisample_state = create_info.pMultisampleState != nullptr;
		info.color_blend_state = create_info.pColorBlendState != nullptr && meta.uses_color;
		info.depth_stencil_state = create_info.pDepthStencilState != nullptr && meta.uses_depth_stencil;
	}

	info.input_assembly = create_info.pInputAssemblyState != nullptr;
	info.vertex_input = create_info.pVertexInputState != nullptr && !dynamic_info.vertex_input;

	info.module_state = graphics_pipeline_library_state_flags_have_module_state(state_flags);
	info.layout_state = info.module_state;

	if (info.module_state)
	{
		for (uint32_t i = 0; i < create_info.stageCount; i++)
		{
			switch (create_info.pStages[i].stage)
			{
			case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
				info.tessellation_state = create_info.pTessellationState != nullptr;
				break;

			case VK_SHADER_STAGE_MESH_BIT_EXT:
			case VK_SHADER_STAGE_TASK_BIT_EXT:
				info.input_assembly = false;
				info.vertex_input = false;
				break;

			default:
				break;
			}
		}
	}

	// If state is not part of the interface for a pipeline library, nop out that state explicitly.
	if ((state_flags & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) == 0)
	{
		info.input_assembly = false;
		info.vertex_input = false;
	}

	if ((state_flags & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) == 0)
	{
		info.viewport_state = false;
		info.rasterization_state = false;
		info.tessellation_state = false;
	}

	if ((state_flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) == 0)
	{
		info.depth_stencil_state = false;
	}

	if ((state_flags & (
			VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
			VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)) == 0)
	{
		info.multisample_state = false;
	}

	if ((state_flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) == 0)
		info.color_blend_state = false;

	// We can ignore formats for dynamic rendering if output interface isn't used, but that's too esoteric.
	// We're mostly interested in ignoring pointers which could be garbage.
	if ((state_flags & (
			VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
			VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
			VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT)) == 0)
	{
		info.render_pass_state = false;
	}

	return info;
}

static DynamicStateInfo parse_dynamic_state_info(const VkPipelineDynamicStateCreateInfo &dynamic_info)
{
	DynamicStateInfo info = {};
#define DYN_STATE(state, member) case VK_DYNAMIC_STATE_##state: info.member = true; break
	for (uint32_t i = 0; i < dynamic_info.dynamicStateCount; i++)
	{
		switch (dynamic_info.pDynamicStates[i])
		{
		DYN_STATE(DEPTH_BIAS, depth_bias);
		DYN_STATE(DEPTH_BOUNDS, depth_bounds);
		DYN_STATE(STENCIL_WRITE_MASK, stencil_write_mask);
		DYN_STATE(STENCIL_REFERENCE, stencil_reference);
		DYN_STATE(STENCIL_COMPARE_MASK, stencil_compare);
		DYN_STATE(BLEND_CONSTANTS, blend_constants);
		DYN_STATE(SCISSOR, scissor);
		DYN_STATE(VIEWPORT, viewport);
		DYN_STATE(LINE_WIDTH, line_width);
		DYN_STATE(CULL_MODE, cull_mode);
		DYN_STATE(FRONT_FACE, front_face);
		DYN_STATE(DEPTH_TEST_ENABLE_EXT, depth_test_enable);
		DYN_STATE(DEPTH_WRITE_ENABLE_EXT, depth_write_enable);
		DYN_STATE(DEPTH_COMPARE_OP_EXT, depth_compare_op);
		DYN_STATE(DEPTH_BOUNDS_TEST_ENABLE_EXT, depth_bounds_test_enable);
		DYN_STATE(STENCIL_TEST_ENABLE_EXT, stencil_test_enable);
		DYN_STATE(STENCIL_OP_EXT, stencil_op);
		DYN_STATE(VERTEX_INPUT_EXT, vertex_input);
		DYN_STATE(VERTEX_INPUT_BINDING_STRIDE_EXT, vertex_input_binding_stride);
		DYN_STATE(PATCH_CONTROL_POINTS_EXT, patch_control_points);
		DYN_STATE(RASTERIZER_DISCARD_ENABLE_EXT, rasterizer_discard_enable);
		DYN_STATE(DEPTH_BIAS_ENABLE_EXT, depth_bias_enable);
		DYN_STATE(LOGIC_OP_EXT, logic_op);
		DYN_STATE(COLOR_WRITE_ENABLE_EXT, color_write_enable);
		DYN_STATE(PRIMITIVE_RESTART_ENABLE_EXT, primitive_restart_enable);
		DYN_STATE(DISCARD_RECTANGLE_EXT, discard_rectangle);
		DYN_STATE(DISCARD_RECTANGLE_MODE_EXT, discard_rectangle_mode);
		DYN_STATE(FRAGMENT_SHADING_RATE_KHR, fragment_shading_rate);
		DYN_STATE(SAMPLE_LOCATIONS_EXT, sample_locations);
		DYN_STATE(LINE_STIPPLE_EXT, line_stipple);

		// Dynamic state 3
		DYN_STATE(TESSELLATION_DOMAIN_ORIGIN_EXT, tessellation_domain_origin);
		DYN_STATE(DEPTH_CLAMP_ENABLE_EXT, depth_clamp_enable);
		DYN_STATE(POLYGON_MODE_EXT, polygon_mode);
		DYN_STATE(RASTERIZATION_SAMPLES_EXT, rasterization_samples);
		DYN_STATE(SAMPLE_MASK_EXT, sample_mask);
		DYN_STATE(ALPHA_TO_COVERAGE_ENABLE_EXT, alpha_to_coverage_enable);
		DYN_STATE(ALPHA_TO_ONE_ENABLE_EXT, alpha_to_one_enable);
		DYN_STATE(LOGIC_OP_ENABLE_EXT, logic_op_enable);
		DYN_STATE(COLOR_BLEND_ENABLE_EXT, color_blend_enable);
		DYN_STATE(COLOR_BLEND_EQUATION_EXT, color_blend_equation);
		DYN_STATE(COLOR_WRITE_MASK_EXT, color_write_mask);
		DYN_STATE(RASTERIZATION_STREAM_EXT, rasterization_stream);
		DYN_STATE(CONSERVATIVE_RASTERIZATION_MODE_EXT, conservative_rasterization_mode);
		DYN_STATE(EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT, extra_primitive_overestimation_size);
		DYN_STATE(DEPTH_CLIP_ENABLE_EXT, depth_clip_enable);
		DYN_STATE(SAMPLE_LOCATIONS_ENABLE_EXT, sample_locations_enable);
		DYN_STATE(COLOR_BLEND_ADVANCED_EXT, color_blend_advanced);
		DYN_STATE(PROVOKING_VERTEX_MODE_EXT, provoking_vertex_mode);
		DYN_STATE(LINE_RASTERIZATION_MODE_EXT, line_rasterization_mode);
		DYN_STATE(LINE_STIPPLE_ENABLE_EXT, line_stipple_enable);
		DYN_STATE(DEPTH_CLIP_NEGATIVE_ONE_TO_ONE_EXT, depth_clip_negative_one_to_one);
		DYN_STATE(VIEWPORT_W_SCALING_ENABLE_NV, viewport_w_scaling_enable);
		DYN_STATE(VIEWPORT_SWIZZLE_NV, viewport_swizzle);
		DYN_STATE(COVERAGE_TO_COLOR_ENABLE_NV, coverage_to_color_enable);
		DYN_STATE(COVERAGE_TO_COLOR_LOCATION_NV, coverage_to_color_location);
		DYN_STATE(COVERAGE_MODULATION_MODE_NV, coverage_modulation_mode);
		DYN_STATE(COVERAGE_MODULATION_TABLE_ENABLE_NV, coverage_modulation_table_enable);
		DYN_STATE(COVERAGE_MODULATION_TABLE_NV, coverage_modulation_table);
		DYN_STATE(SHADING_RATE_IMAGE_ENABLE_NV, shading_rate_image_enable);
		DYN_STATE(REPRESENTATIVE_FRAGMENT_TEST_ENABLE_NV, representative_fragment_test_enable);
		DYN_STATE(COVERAGE_REDUCTION_MODE_NV, coverage_reduction_mode);

		case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT_EXT:
			info.scissor_count = true;
			info.scissor = true;
			break;

		case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT_EXT:
			info.viewport_count = true;
			info.viewport = true;
			break;

		DYN_STATE(DEPTH_CLAMP_RANGE_EXT, depth_clamp_range);

		default:
			break;
		}
	}
#undef DYN_STATE

	return info;
}

bool compute_hash_graphics_pipeline(const StateRecorder &recorder, const VkGraphicsPipelineCreateInfo &create_info, Hash *out_hash)
{
	// Ignore pipelines that cannot result in meaningful replay.
	auto state_flags = graphics_pipeline_get_effective_state_flags(create_info);

	Hasher h;
	Hash hash;

	if (find_pnext<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, create_info.pNext))
		h.u32(0);
	else
		h.u32(normalize_pipeline_creation_flags(create_info.flags));

	if ((create_info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
	    create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		if (!recorder.get_hash_for_graphics_pipeline_handle(create_info.basePipelineHandle, &hash))
			return false;
		h.u64(hash);
		h.s32(create_info.basePipelineIndex);
	}

	DynamicStateInfo dynamic_info = {};
	if (create_info.pDynamicState)
		dynamic_info = parse_dynamic_state_info(*create_info.pDynamicState);
	GlobalStateInfo global_info = parse_global_state_info(create_info, dynamic_info, { true, true });

	if (global_info.layout_state)
	{
		if (!recorder.get_hash_for_pipeline_layout(create_info.layout, &hash))
			return false;
	}
	else
		hash = 0;

	h.u64(hash);

	// Need to query state info in two stages. Do we ignore render pass? If so, query a 0 hash.
	// If we don't ignore render pass, we can query for subpass meta.
	hash = 0;
	if (global_info.render_pass_state && !recorder.get_hash_for_render_pass(create_info.renderPass, &hash))
		return false;
	h.u64(hash);

	StateRecorder::SubpassMeta meta = {};
	if (!recorder.get_subpass_meta_for_pipeline(create_info, hash, &meta))
		return false;

	global_info = parse_global_state_info(create_info, dynamic_info, meta);

	h.u32(global_info.render_pass_state ? create_info.subpass : 0u);
	h.u32(global_info.module_state ? create_info.stageCount : 0u);

	if (create_info.pDynamicState)
	{
		auto &state = *create_info.pDynamicState;
		h.u32(state.dynamicStateCount);
		h.u32(state.flags);
		for (uint32_t i = 0; i < state.dynamicStateCount; i++)
			h.u32(state.pDynamicStates[i]);
		if (!hash_pnext_chain(&recorder, h, state.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.depth_stencil_state)
	{
		auto &ds = *create_info.pDepthStencilState;
		h.u32(ds.flags);
		h.u32(dynamic_info.depth_bounds_test_enable ? 0 : ds.depthBoundsTestEnable);
		h.u32(dynamic_info.depth_compare_op ? 0 : ds.depthCompareOp);
		h.u32(dynamic_info.depth_test_enable ? 0 : ds.depthTestEnable);
		h.u32(dynamic_info.depth_write_enable ? 0 : ds.depthWriteEnable);
		h.u32(dynamic_info.stencil_op ? 0 : ds.front.compareOp);
		h.u32(dynamic_info.stencil_op ? 0 : ds.front.depthFailOp);
		h.u32(dynamic_info.stencil_op ? 0 : ds.front.failOp);
		h.u32(dynamic_info.stencil_op ? 0 : ds.front.passOp);
		h.u32(dynamic_info.stencil_op ? 0 : ds.back.compareOp);
		h.u32(dynamic_info.stencil_op ? 0 : ds.back.depthFailOp);
		h.u32(dynamic_info.stencil_op ? 0 : ds.back.failOp);
		h.u32(dynamic_info.stencil_op ? 0 : ds.back.passOp);
		h.u32(dynamic_info.stencil_test_enable ? 0 : ds.stencilTestEnable);

		if (!dynamic_info.depth_bounds && (ds.depthBoundsTestEnable || dynamic_info.depth_bounds_test_enable))
		{
			h.f32(ds.minDepthBounds);
			h.f32(ds.maxDepthBounds);
		}

		if (ds.stencilTestEnable || dynamic_info.stencil_test_enable)
		{
			if (!dynamic_info.stencil_compare)
			{
				h.u32(ds.front.compareMask);
				h.u32(ds.back.compareMask);
			}

			if (!dynamic_info.stencil_reference)
			{
				h.u32(ds.front.reference);
				h.u32(ds.back.reference);
			}

			if (!dynamic_info.stencil_write_mask)
			{
				h.u32(ds.front.writeMask);
				h.u32(ds.back.writeMask);
			}
		}

		if (!hash_pnext_chain(&recorder, h, ds.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.input_assembly)
	{
		auto &ia = *create_info.pInputAssemblyState;
		h.u32(ia.flags);
		h.u32(dynamic_info.primitive_restart_enable ? 0 : ia.primitiveRestartEnable);
		// Dynamic 3 makes this fully ignored, but it depends on a property. For sanity's sake we need to hash this.
		h.u32(ia.topology);

		if (!hash_pnext_chain(&recorder, h, ia.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.rasterization_state)
	{
		auto &rs = *create_info.pRasterizationState;
		h.u32(rs.flags);
		h.u32(dynamic_info.cull_mode ? 0 : rs.cullMode);
		h.u32(dynamic_info.depth_clamp_enable ? 0 : rs.depthClampEnable);
		h.u32(dynamic_info.front_face ? 0 : rs.frontFace);
		h.u32(dynamic_info.rasterizer_discard_enable ? 0 : rs.rasterizerDiscardEnable);
		h.u32(dynamic_info.polygon_mode ? 0 : rs.polygonMode);
		h.u32(dynamic_info.depth_bias_enable ? 0 : rs.depthBiasEnable);

		if ((rs.depthBiasEnable || dynamic_info.depth_bias_enable) && !dynamic_info.depth_bias)
		{
			h.f32(rs.depthBiasClamp);
			h.f32(rs.depthBiasSlopeFactor);
			h.f32(rs.depthBiasConstantFactor);
		}

		if (!dynamic_info.line_width)
			h.f32(rs.lineWidth);

		if (!hash_pnext_chain(&recorder, h, rs.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.multisample_state)
	{
		auto &ms = *create_info.pMultisampleState;
		h.u32(ms.flags);
		h.u32(dynamic_info.alpha_to_coverage_enable ? 0 : ms.alphaToCoverageEnable);
		h.u32(dynamic_info.alpha_to_one_enable ? 0 : ms.alphaToOneEnable);
		h.f32(ms.minSampleShading);
		h.u32(dynamic_info.rasterization_samples ? 0 : ms.rasterizationSamples);
		h.u32(ms.sampleShadingEnable);
		if (!dynamic_info.sample_mask && ms.pSampleMask)
		{
			uint32_t elems = (ms.rasterizationSamples + 31) / 32;
			for (uint32_t i = 0; i < elems; i++)
				h.u32(ms.pSampleMask[i]);
		}
		else
			h.u32(0);

		if (!hash_pnext_chain(&recorder, h, ms.pNext, &dynamic_info, state_flags))
			return false;
	}

	if (global_info.viewport_state)
	{
		auto &vp = *create_info.pViewportState;
		h.u32(vp.flags);
		h.u32(dynamic_info.scissor_count ? 0 : vp.scissorCount);
		h.u32(dynamic_info.viewport_count ? 0 : vp.viewportCount);

		if (!dynamic_info.scissor)
		{
			for (uint32_t i = 0; i < vp.scissorCount; i++)
			{
				h.s32(vp.pScissors[i].offset.x);
				h.s32(vp.pScissors[i].offset.y);
				h.u32(vp.pScissors[i].extent.width);
				h.u32(vp.pScissors[i].extent.height);
			}
		}

		if (!dynamic_info.viewport)
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

		if (!hash_pnext_chain(&recorder, h, vp.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.vertex_input)
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
			h.u32(dynamic_info.vertex_input_binding_stride ? 0 : vi.pVertexBindingDescriptions[i].stride);
		}

		if (!hash_pnext_chain(&recorder, h, vi.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.color_blend_state)
	{
		auto &b = *create_info.pColorBlendState;
		h.u32(b.flags);
		h.u32(b.attachmentCount);
		h.u32(dynamic_info.logic_op_enable ? 0 : b.logicOpEnable);
		h.u32(dynamic_info.logic_op || (!b.logicOpEnable && !dynamic_info.logic_op_enable) ? 0 : b.logicOp);

		bool need_blend_constants = false;

		// Special EDS3 rule. If all of these are set, we must ignore the pAttachments pointer.
		const bool dynamic_attachments = dynamic_info.color_blend_enable &&
		                                 dynamic_info.color_write_mask &&
		                                 dynamic_info.color_blend_equation;

		// If we have dynamic equation, but not dynamic blend constants, we can dynamically set blend constant
		// as input, so we might need to know those constants to hash the PSO.
		if (dynamic_info.color_blend_equation)
			need_blend_constants = true;

		if (dynamic_attachments)
			h.u32(0);

		for (uint32_t i = 0; !dynamic_attachments && i < b.attachmentCount; i++)
		{
			h.u32(dynamic_info.color_blend_enable ? 0 : b.pAttachments[i].blendEnable);
			h.u32(dynamic_info.color_write_mask ? 0 : b.pAttachments[i].colorWriteMask);
			if (b.pAttachments[i].blendEnable || dynamic_info.color_blend_enable)
			{
				if (!dynamic_info.color_blend_equation)
				{
					h.u32(dynamic_info.color_blend_advanced ? 0 : b.pAttachments[i].alphaBlendOp);
					h.u32(dynamic_info.color_blend_advanced ? 0 : b.pAttachments[i].colorBlendOp);
					h.u32(b.pAttachments[i].dstAlphaBlendFactor);
					h.u32(b.pAttachments[i].srcAlphaBlendFactor);
					h.u32(b.pAttachments[i].dstColorBlendFactor);
					h.u32(b.pAttachments[i].srcColorBlendFactor);
				}
				else
					h.u32(0);

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

		if (need_blend_constants && !dynamic_info.blend_constants)
			for (auto &blend_const : b.blendConstants)
				h.f32(blend_const);

		if (!hash_pnext_chain(&recorder, h, b.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.tessellation_state)
	{
		auto &tess = *create_info.pTessellationState;
		h.u32(tess.flags);
		h.u32(dynamic_info.patch_control_points ? 0 : tess.patchControlPoints);

		if (!hash_pnext_chain(&recorder, h, tess.pNext, &dynamic_info, state_flags))
			return false;
	}
	else
		h.u32(0);

	if (global_info.module_state)
	{
		for (uint32_t i = 0; i < create_info.stageCount; i++)
		{
			auto &stage = create_info.pStages[i];
			if (!compute_hash_stage(recorder, h, stage))
				return false;
		}
	}

	if (!hash_pnext_chain(&recorder, h, create_info.pNext, &dynamic_info, state_flags))
		return false;

	*out_hash = h.get();
	return true;
}

bool compute_hash_compute_pipeline(const StateRecorder &recorder, const VkComputePipelineCreateInfo &create_info, Hash *out_hash)
{
	// Ignore pipelines that cannot result in meaningful replay.
	if (!create_info.stage.pName)
		return false;

	Hasher h;
	Hash hash;

	if (!recorder.get_hash_for_pipeline_layout(create_info.layout, &hash))
		return false;
	h.u64(hash);

	if (find_pnext<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, create_info.pNext))
		h.u32(0);
	else
		h.u32(normalize_pipeline_creation_flags(create_info.flags));

	if ((create_info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
	    create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		if (!recorder.get_hash_for_compute_pipeline_handle(create_info.basePipelineHandle, &hash))
			return false;
		h.u64(hash);
		h.s32(create_info.basePipelineIndex);
	}
	else
		h.u32(0);

	// Unfortunately, the hash order is incompatible with compute_hash_stage().
	// For compatibility, cannot change this without a clean break.
	if (create_info.stage.module != VK_NULL_HANDLE)
	{
		if (!recorder.get_hash_for_shader_module(create_info.stage.module, &hash))
			return false;
	}
	else if (const auto *module = find_pnext<VkShaderModuleCreateInfo>(
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, create_info.stage.pNext))
	{
		if (!compute_hash_shader_module(*module, &hash))
			return false;
	}
	else if (const auto *identifier = find_pnext<VkPipelineShaderStageModuleIdentifierCreateInfoEXT>(
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, create_info.stage.pNext))
	{
		if (!recorder.get_hash_for_shader_module(identifier, &hash))
			return false;
	}
	else
		return false;

	h.u64(hash);

	h.string(create_info.stage.pName);
	h.u32(create_info.stage.flags);
	h.u32(create_info.stage.stage);

	if (create_info.stage.pSpecializationInfo)
		hash_specialization_info(h, *create_info.stage.pSpecializationInfo);
	else
		h.u32(0);

	if (!hash_pnext_chain(&recorder, h, create_info.stage.pNext, nullptr, 0))
		return false;

	if (!hash_pnext_chain(&recorder, h, create_info.pNext, nullptr, 0))
		return false;

	*out_hash = h.get();
	return true;
}

bool compute_hash_raytracing_pipeline(const StateRecorder &recorder,
                                      const VkRayTracingPipelineCreateInfoKHR &create_info,
                                      Hash *out_hash)
{
	Hasher h;
	Hash hash;

	if (find_pnext<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, create_info.pNext))
		h.u32(0);
	else
		h.u32(normalize_pipeline_creation_flags(create_info.flags));

	h.u32(create_info.maxPipelineRayRecursionDepth);

	if (!recorder.get_hash_for_pipeline_layout(create_info.layout, &hash))
		return false;
	h.u64(hash);

	if ((create_info.flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0 &&
	    create_info.basePipelineHandle != VK_NULL_HANDLE)
	{
		if (!recorder.get_hash_for_raytracing_pipeline_handle(create_info.basePipelineHandle, &hash))
			return false;
		h.u64(hash);
		h.s32(create_info.basePipelineIndex);
	}
	else
		h.u32(0);

	h.u32(create_info.stageCount);
	for (uint32_t i = 0; i < create_info.stageCount; i++)
	{
		auto &stage = create_info.pStages[i];
		if (!compute_hash_stage(recorder, h, stage))
			return false;
	}

	if (create_info.pLibraryInterface)
	{
		h.u32(create_info.pLibraryInterface->maxPipelineRayHitAttributeSize);
		h.u32(create_info.pLibraryInterface->maxPipelineRayPayloadSize);
		if (!hash_pnext_chain(&recorder, h, create_info.pLibraryInterface->pNext, nullptr, 0))
			return false;
	}
	else
		h.u32(0);

	if (create_info.pDynamicState)
	{
		h.u32(create_info.pDynamicState->dynamicStateCount);
		h.u32(create_info.pDynamicState->flags);
		for (uint32_t i = 0; i < create_info.pDynamicState->dynamicStateCount; i++)
			h.u32(create_info.pDynamicState->pDynamicStates[i]);
		if (!hash_pnext_chain(&recorder, h, create_info.pDynamicState->pNext, nullptr, 0))
			return false;
	}
	else
		h.u32(0);

	h.u32(create_info.groupCount);
	for (uint32_t i = 0; i < create_info.groupCount; i++)
	{
		auto &group = create_info.pGroups[i];
		h.u32(group.type);
		h.u32(group.anyHitShader);
		h.u32(group.closestHitShader);
		h.u32(group.generalShader);
		h.u32(group.intersectionShader);
		if (!hash_pnext_chain(&recorder, h, group.pNext, nullptr, 0))
			return false;
	}

	if (create_info.pLibraryInfo)
	{
		if (!hash_pnext_struct(&recorder, h, *create_info.pLibraryInfo))
			return false;
		if (!hash_pnext_chain(&recorder, h, create_info.pLibraryInfo->pNext, nullptr, 0))
			return false;
	}
	else
		h.u32(0);

	if (!hash_pnext_chain(&recorder, h, create_info.pNext, nullptr, 0))
		return false;

	*out_hash = h.get();
	return true;
}

template <typename Att>
static void hash_attachment_base(Hasher &h, const Att &att)
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

static void hash_attachment(Hasher &h, const VkAttachmentDescription &att)
{
	hash_attachment_base(h, att);
}

static bool hash_attachment2(Hasher &h, const VkAttachmentDescription2 &att)
{
	hash_attachment_base(h, att);
	return hash_pnext_chain(nullptr, h, att.pNext, nullptr, 0);
}

template <typename Dep>
static void hash_dependency_base(Hasher &h, const Dep &dep)
{
	h.u32(dep.dependencyFlags);
	h.u32(dep.dstAccessMask);
	h.u32(dep.srcAccessMask);
	h.u32(dep.srcSubpass);
	h.u32(dep.dstSubpass);
	h.u32(dep.srcStageMask);
	h.u32(dep.dstStageMask);
}

static void hash_dependency(Hasher &h, const VkSubpassDependency &dep)
{
	hash_dependency_base(h, dep);
}

static bool hash_dependency2(Hasher &h, const VkSubpassDependency2 &dep)
{
	hash_dependency_base(h, dep);
	h.s32(dep.viewOffset);
	return hash_pnext_chain(nullptr, h, dep.pNext, nullptr, 0);
}

static bool hash_reference_base(Hasher &h, const VkAttachmentReference &ref)
{
	h.u32(ref.attachment);
	h.u32(ref.layout);
	return true;
}

static bool hash_reference_base(Hasher &h, const VkAttachmentReference2 &ref)
{
	h.u32(ref.attachment);
	h.u32(ref.layout);
	h.u32(ref.aspectMask);
	return hash_pnext_chain(nullptr, h, ref.pNext, nullptr, 0);
}

template <typename Subpass>
static bool hash_subpass_base(Hasher &h, const Subpass &subpass)
{
	h.u32(subpass.flags);
	h.u32(subpass.colorAttachmentCount);
	h.u32(subpass.inputAttachmentCount);
	h.u32(subpass.preserveAttachmentCount);
	h.u32(subpass.pipelineBindPoint);

	for (uint32_t i = 0; i < subpass.preserveAttachmentCount; i++)
		h.u32(subpass.pPreserveAttachments[i]);

	for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
		if (!hash_reference_base(h, subpass.pColorAttachments[i]))
			return false;
	for (uint32_t i = 0; i < subpass.inputAttachmentCount; i++)
		if (!hash_reference_base(h, subpass.pInputAttachments[i]))
			return false;

	if (subpass.pResolveAttachments)
		for (uint32_t i = 0; i < subpass.colorAttachmentCount; i++)
			if (!hash_reference_base(h, subpass.pResolveAttachments[i]))
				return false;

	if (subpass.pDepthStencilAttachment)
	{
		if (!hash_reference_base(h, *subpass.pDepthStencilAttachment))
			return false;
	}
	else
		h.u32(0);

	return true;
}

static void hash_subpass(Hasher &h, const VkSubpassDescription &subpass)
{
	hash_subpass_base(h, subpass);
}

static bool hash_subpass2(Hasher &h, const VkSubpassDescription2 &subpass)
{
	if (!hash_subpass_base(h, subpass))
		return false;
	h.u32(subpass.viewMask);
	return hash_pnext_chain(nullptr, h, subpass.pNext, nullptr, 0);
}

bool compute_hash_render_pass(const VkRenderPassCreateInfo &create_info, Hash *out_hash)
{
	Hasher h;

	// Conditionally branch to remain hash compatible.
	if (create_info.flags != 0)
		h.u32(create_info.flags);

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

	if (!hash_pnext_chain(nullptr, h, create_info.pNext, nullptr, 0))
		return false;

	*out_hash = h.get();
	return true;
}

bool compute_hash_render_pass2(const VkRenderPassCreateInfo2 &create_info, Hash *out_hash)
{
	Hasher h;

	h.u32(create_info.flags);
	h.u32(create_info.attachmentCount);
	h.u32(create_info.dependencyCount);
	h.u32(create_info.subpassCount);
	h.u32(create_info.correlatedViewMaskCount);
	h.u32(2);

	for (uint32_t i = 0; i < create_info.attachmentCount; i++)
	{
		auto &att = create_info.pAttachments[i];
		if (!hash_attachment2(h, att))
			return false;
	}

	for (uint32_t i = 0; i < create_info.dependencyCount; i++)
	{
		auto &dep = create_info.pDependencies[i];
		if (!hash_dependency2(h, dep))
			return false;
	}

	for (uint32_t i = 0; i < create_info.subpassCount; i++)
	{
		auto &subpass = create_info.pSubpasses[i];
		if (!hash_subpass2(h, subpass))
			return false;
	}

	for (uint32_t i = 0; i < create_info.correlatedViewMaskCount; i++)
		h.u32(create_info.pCorrelatedViewMasks[i]);

	if (!hash_pnext_chain(nullptr, h, create_info.pNext, nullptr, 0))
		return false;

	*out_hash = h.get();
	return true;
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

bool StateReplayer::Impl::parse_immutable_samplers(StateCreatorInterface &iface, DatabaseInterface *resolver,
                                                   const Value &samplers, const VkSampler **out_sampler)
{
	auto *samps = allocator.allocate_n<VkSampler>(samplers.Size());
	auto *ret = samps;
	for (auto itr = samplers.Begin(); itr != samplers.End(); ++itr, samps++)
	{
		auto sampler_hash = string_to_uint64(itr->GetString());
		if (sampler_hash > 0)
		{
			auto sampler_itr = replayed_samplers.find(sampler_hash);
			if (sampler_itr == end(replayed_samplers))
			{
				size_t external_state_size = 0;
				if (!resolver || !resolver->read_entry(RESOURCE_SAMPLER, sampler_hash,
				                                       &external_state_size, nullptr,
				                                       PAYLOAD_READ_NO_FLAGS))
				{
					log_missing_resource("Immutable sampler", sampler_hash);
					return false;
				}

				vector<uint8_t> external_state(external_state_size);

				if (!resolver->read_entry(RESOURCE_SAMPLER, sampler_hash,
				                          &external_state_size, external_state.data(),
				                          PAYLOAD_READ_NO_FLAGS))
				{
					log_missing_resource("Immutable sampler", sampler_hash);
					return false;
				}

				if (!this->parse(iface, resolver, external_state.data(), external_state.size()))
					return false;

				iface.sync_samplers();
				sampler_itr = replayed_samplers.find(sampler_hash);
			}
			else
				iface.sync_samplers();

			if (sampler_itr == replayed_samplers.end())
			{
				log_missing_resource("Immutable sampler", sampler_hash);
				return false;
			}
			else if (sampler_itr->second == VK_NULL_HANDLE)
			{
				log_invalid_resource("Immutable sampler", sampler_hash);
				return false;
			}

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

bool StateReplayer::Impl::parse_descriptor_set_bindings(StateCreatorInterface &iface, DatabaseInterface *resolver,
                                                        const Value &bindings,
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
		{
			if (!parse_immutable_samplers(iface, resolver,
			                              b["immutableSamplers"], &set_bindings->pImmutableSamplers))
			{
				return false;
			}
		}
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
			else if (set_itr->second == VK_NULL_HANDLE)
			{
				log_invalid_resource("Descriptor set layout", index);
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
				LOGE_LEVEL("Binary varint buffer overflows payload.\n");
				return false;
			}

			if (!decode_varint(decoded, info.codeSize / 4, varint + offset, size))
			{
				LOGE_LEVEL("Invalid varint format.\n");
				return false;
			}

			info.pCode = decoded;
		}
		else
			info.pCode = reinterpret_cast<uint32_t *>(decode_base64(allocator, obj["code"].GetString(), info.codeSize));

		if (!iface.enqueue_create_shader_module(hash, &info, &replayed_shader_modules[hash]))
			return false;
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
			return false;
	}

	iface.notify_replayed_resources_for_type();
	return true;
}

bool StateReplayer::Impl::parse_descriptor_set_layouts(StateCreatorInterface &iface, DatabaseInterface *resolver, const Value &layouts)
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
			if (!parse_descriptor_set_bindings(iface, resolver, bindings, &info.pBindings))
				return false;
		}

		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &info.pNext))
				return false;

		if (!iface.enqueue_create_descriptor_set_layout(hash, &info, &replayed_descriptor_set_layouts[hash]))
			return false;
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

		if (pdf_info.HasMember("pNext"))
			if (!parse_pnext_chain_pdf2(pdf_info["pNext"], &pdf->pNext))
				return false;

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
	for (auto itr = samplers.MemberBegin(); itr != samplers.MemberEnd(); ++itr, index++)
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

		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &info.pNext))
				return false;

		if (!iface.enqueue_create_sampler(hash, &info, &replayed_samplers[hash]))
			return false;
	}

	iface.notify_replayed_resources_for_type();
	return true;
}

template <typename Desc>
static void parse_render_pass_attachments_base(Desc &desc, const Value &obj)
{
	desc.flags = obj["flags"].GetUint();
	desc.finalLayout = static_cast<VkImageLayout>(obj["finalLayout"].GetUint());
	desc.initialLayout = static_cast<VkImageLayout>(obj["initialLayout"].GetUint());
	desc.format = static_cast<VkFormat>(obj["format"].GetUint());
	desc.loadOp = static_cast<VkAttachmentLoadOp>(obj["loadOp"].GetUint());
	desc.storeOp = static_cast<VkAttachmentStoreOp>(obj["storeOp"].GetUint());
	desc.stencilLoadOp = static_cast<VkAttachmentLoadOp>(obj["stencilLoadOp"].GetUint());
	desc.stencilStoreOp = static_cast<VkAttachmentStoreOp>(obj["stencilStoreOp"].GetUint());
	desc.samples = static_cast<VkSampleCountFlagBits>(obj["samples"].GetUint());
}

bool StateReplayer::Impl::parse_render_pass_attachments(const Value &attachments, const VkAttachmentDescription **out_attachments)
{
	auto *infos = allocator.allocate_n_cleared<VkAttachmentDescription>(attachments.Size());
	auto *ret = infos;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, infos++)
		parse_render_pass_attachments_base(*infos, *itr);

	*out_attachments = ret;
	return true;
}

bool StateReplayer::Impl::parse_render_pass_attachments2(const Value &attachments, const VkAttachmentDescription2 **out_attachments)
{
	auto *infos = allocator.allocate_n_cleared<VkAttachmentDescription2>(attachments.Size());
	auto *ret = infos;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
		parse_render_pass_attachments_base(*infos, *itr);
		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &infos->pNext))
				return false;
	}

	*out_attachments = ret;
	return true;
}

template <typename Dep>
static void parse_render_pass_dependencies_base(Dep &dep, const Value &obj)
{
	dep.dependencyFlags = obj["dependencyFlags"].GetUint();
	dep.dstAccessMask = obj["dstAccessMask"].GetUint();
	dep.srcAccessMask = obj["srcAccessMask"].GetUint();
	dep.dstStageMask = obj["dstStageMask"].GetUint();
	dep.srcStageMask = obj["srcStageMask"].GetUint();
	dep.srcSubpass = obj["srcSubpass"].GetUint();
	dep.dstSubpass = obj["dstSubpass"].GetUint();
}

bool StateReplayer::Impl::parse_render_pass_dependencies(const Value &dependencies, const VkSubpassDependency **out_deps)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDependency>(dependencies.Size());
	auto *ret = infos;

	for (auto itr = dependencies.Begin(); itr != dependencies.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		parse_render_pass_dependencies_base(*infos, obj);
	}

	*out_deps = ret;
	return true;
}

bool StateReplayer::Impl::parse_render_pass_dependencies2(const Value &dependencies, const VkSubpassDependency2 **out_deps)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDependency2>(dependencies.Size());
	auto *ret = infos;

	for (auto itr = dependencies.Begin(); itr != dependencies.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
		parse_render_pass_dependencies_base(*infos, obj);
		infos->viewOffset = obj["viewOffset"].GetInt();
		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &infos->pNext))
				return false;
	}

	*out_deps = ret;
	return true;
}

template <typename Ref>
static void parse_attachment_base(Ref &ref, const Value &value)
{
	ref.attachment = value["attachment"].GetUint();
	ref.layout = static_cast<VkImageLayout>(value["layout"].GetUint());
}

bool StateReplayer::Impl::parse_attachment(const Value &value, const VkAttachmentReference **out_reference)
{
	auto *ret = allocator.allocate_cleared<VkAttachmentReference>();
	parse_attachment_base(*ret, value);
	*out_reference = ret;
	return true;
}

bool StateReplayer::Impl::parse_attachment2(const Value &value, const VkAttachmentReference2 **out_reference)
{
	auto *ret = allocator.allocate_cleared<VkAttachmentReference2>();
	ret->sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
	parse_attachment_base(*ret, value);
	ret->aspectMask = value["aspectMask"].GetUint();

	if (value.HasMember("pNext"))
		if (!parse_pnext_chain(value["pNext"], &ret->pNext))
			return false;

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
		parse_attachment_base(*refs, value);
	}

	*out_references = ret;
	return true;
}

bool StateReplayer::Impl::parse_attachments2(const Value &attachments, const VkAttachmentReference2 **out_references)
{
	auto *refs = allocator.allocate_n_cleared<VkAttachmentReference2>(attachments.Size());
	auto *ret = refs;

	for (auto itr = attachments.Begin(); itr != attachments.End(); ++itr, refs++)
	{
		auto &value = *itr;
		refs->sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		parse_attachment_base(*refs, value);
		refs->aspectMask = value["aspectMask"].GetUint();

		if (value.HasMember("pNext"))
			if (!parse_pnext_chain(value["pNext"], &refs->pNext))
				return false;
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

bool StateReplayer::Impl::parse_sints(const Value &ints, const int32_t **out_sints)
{
	auto *s32s = allocator.allocate_n<int32_t>(ints.Size());
	auto *ret = s32s;
	for (auto itr = ints.Begin(); itr != ints.End(); ++itr, s32s++)
		*s32s = itr->GetInt();

	*out_sints = ret;
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

bool StateReplayer::Impl::parse_render_pass_subpasses2(const Value &subpasses, const VkSubpassDescription2 **out_subpasses)
{
	auto *infos = allocator.allocate_n_cleared<VkSubpassDescription2>(subpasses.Size());
	auto *ret = infos;

	for (auto itr = subpasses.Begin(); itr != subpasses.End(); ++itr, infos++)
	{
		auto &obj = *itr;
		infos->sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
		infos->flags = obj["flags"].GetUint();
		infos->pipelineBindPoint = static_cast<VkPipelineBindPoint>(obj["pipelineBindPoint"].GetUint());
		infos->viewMask = obj["viewMask"].GetUint();

		if (obj.HasMember("depthStencilAttachment"))
			if (!parse_attachment2(obj["depthStencilAttachment"], &infos->pDepthStencilAttachment))
				return false;

		if (obj.HasMember("resolveAttachments"))
			if (!parse_attachments2(obj["resolveAttachments"], &infos->pResolveAttachments))
				return false;

		if (obj.HasMember("inputAttachments"))
		{
			infos->inputAttachmentCount = obj["inputAttachments"].Size();
			if (!parse_attachments2(obj["inputAttachments"], &infos->pInputAttachments))
				return false;
		}

		if (obj.HasMember("colorAttachments"))
		{
			infos->colorAttachmentCount = obj["colorAttachments"].Size();
			if (!parse_attachments2(obj["colorAttachments"], &infos->pColorAttachments))
				return false;
		}

		if (obj.HasMember("preserveAttachments"))
		{
			infos->preserveAttachmentCount = obj["preserveAttachments"].Size();
			if (!parse_uints(obj["preserveAttachments"], &infos->pPreserveAttachments))
				return false;
		}

		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &infos->pNext))
				return false;
	}

	*out_subpasses = ret;
	return true;
}

bool StateReplayer::Impl::parse_render_passes2(StateCreatorInterface &iface, const Value &passes)
{
	auto *infos = allocator.allocate_n_cleared<VkRenderPassCreateInfo2>(passes.MemberCount());

	unsigned index = 0;
	for (auto itr = passes.MemberBegin(); itr != passes.MemberEnd(); ++itr, index++)
	{
		Hash hash = string_to_uint64(itr->name.GetString());
		if (replayed_samplers.count(hash))
			continue;
		auto &obj = itr->value;
		auto &info = infos[index];
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;

		info.flags = obj["flags"].GetUint();

		if (obj.HasMember("correlatedViewMasks"))
		{
			info.correlatedViewMaskCount = obj["correlatedViewMasks"].Size();
			if (!parse_uints(obj["correlatedViewMasks"], &info.pCorrelatedViewMasks))
				return false;
		}

		if (obj.HasMember("attachments"))
		{
			info.attachmentCount = obj["attachments"].Size();
			if (!parse_render_pass_attachments2(obj["attachments"], &info.pAttachments))
				return false;
		}

		if (obj.HasMember("dependencies"))
		{
			info.dependencyCount = obj["dependencies"].Size();
			if (!parse_render_pass_dependencies2(obj["dependencies"], &info.pDependencies))
				return false;
		}

		if (obj.HasMember("subpasses"))
		{
			info.subpassCount = obj["subpasses"].Size();
			if (!parse_render_pass_subpasses2(obj["subpasses"], &info.pSubpasses))
				return false;
		}

		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &info.pNext))
				return false;

		if (!iface.enqueue_create_render_pass2(hash, &info, &replayed_render_passes[hash]))
			return false;
	}

	iface.notify_replayed_resources_for_type();
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

		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &info.pNext))
				return false;

		if (!iface.enqueue_create_render_pass(hash, &info, &replayed_render_passes[hash]))
			return false;
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

	// For older captures, we might have captured these flags.
	info.flags = normalize_pipeline_creation_flags(info.flags);

	if (!parse_derived_pipeline_handle(iface, resolver, obj["basePipelineHandle"], pipelines,
	                                   RESOURCE_COMPUTE_PIPELINE, &info.basePipelineHandle))
		return false;

	if (!parse_pipeline_layout_handle(obj["layout"], &info.layout))
		return false;

	auto &stage = obj["stage"];
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = static_cast<VkShaderStageFlagBits>(stage["stage"].GetUint());
	info.stage.flags = stage["flags"].GetUint();

	if (stage.HasMember("pNext"))
		if (!parse_pnext_chain(stage["pNext"], &info.stage.pNext))
			return false;

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

	if (obj.HasMember("pNext"))
		if (!parse_pnext_chain(obj["pNext"], &info.pNext))
			return false;

	if (!iface.enqueue_create_compute_pipeline(hash, &info, &replayed_compute_pipelines[hash]))
		return false;

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

	if (ds.HasMember("pNext"))
		if (!parse_pnext_chain(ds["pNext"], &state->pNext))
			return false;

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

	if (ia.HasMember("pNext"))
		if (!parse_pnext_chain(ia["pNext"], &state->pNext))
			return false;

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

	if (blend.HasMember("pNext"))
		if (!parse_pnext_chain(blend["pNext"], &state->pNext))
			return false;

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

	if (ms.HasMember("pNext"))
		if (!parse_pnext_chain(ms["pNext"], &state->pNext))
			return false;

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

bool StateReplayer::Impl::parse_raytracing_groups(const Value &groups,
                                                  const VkRayTracingShaderGroupCreateInfoKHR **out_info)
{
	auto *state = allocator.allocate_n_cleared<VkRayTracingShaderGroupCreateInfoKHR>(groups.Size());
	*out_info = state;

	for (auto itr = groups.Begin(); itr != groups.End(); ++itr, state++)
	{
		auto &group = *itr;
		state->sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
		state->intersectionShader = group["intersectionShader"].GetUint();
		state->anyHitShader = group["anyHitShader"].GetUint();
		state->closestHitShader = group["closestHitShader"].GetUint();
		state->generalShader = group["generalShader"].GetUint();
		state->type = static_cast<VkRayTracingShaderGroupTypeKHR>(group["type"].GetUint());

		if (group.HasMember("pNext"))
			if (!parse_pnext_chain(group["pNext"], &state->pNext))
				return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_library_interface(const Value &lib,
                                                  const VkRayTracingPipelineInterfaceCreateInfoKHR **out_info)
{
	auto *state = allocator.allocate_cleared<VkRayTracingPipelineInterfaceCreateInfoKHR>();

	state->sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR;
	state->maxPipelineRayPayloadSize = lib["maxPipelineRayPayloadSize"].GetUint();
	state->maxPipelineRayHitAttributeSize = lib["maxPipelineRayHitAttributeSize"].GetUint();

	if (lib.HasMember("pNext"))
		if (!parse_pnext_chain(lib["pNext"], &state->pNext))
			return false;

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

	if (vp.HasMember("pNext"))
		if (!parse_pnext_chain(vp["pNext"], &state->pNext))
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

		if (obj.HasMember("pNext"))
			if (!parse_pnext_chain(obj["pNext"], &state->pNext))
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

bool StateReplayer::Impl::parse_pipeline_layout_handle(const Value &state, VkPipelineLayout *out_layout)
{
	auto layout = string_to_uint64(state.GetString());
	if (layout > 0)
	{
		auto layout_itr = replayed_pipeline_layouts.find(layout);
		if (layout_itr == end(replayed_pipeline_layouts) || layout_itr->second == VK_NULL_HANDLE)
		{
			log_missing_resource("Pipeline layout", layout);
			return false;
		}
		else
			*out_layout = layout_itr->second;
	}
	else
		*out_layout = VK_NULL_HANDLE;

	return true;
}

bool StateReplayer::Impl::parse_derived_pipeline_handle(StateCreatorInterface &iface, DatabaseInterface *resolver,
                                                        const Value &state, const Value &pipelines,
                                                        ResourceTag tag, VkPipeline *out_pipeline)
{
	unordered_map<Hash, VkPipeline> *replayed_pipelines = nullptr;
	if (tag == RESOURCE_GRAPHICS_PIPELINE)
		replayed_pipelines = &replayed_graphics_pipelines;
	else if (tag == RESOURCE_COMPUTE_PIPELINE)
		replayed_pipelines = &replayed_compute_pipelines;
	else if (tag == RESOURCE_RAYTRACING_PIPELINE)
		replayed_pipelines = &replayed_raytracing_pipelines;
	else
		return false;

	auto pipeline = string_to_uint64(state.GetString());
	if (pipeline > 0 && resolve_derivative_pipelines)
	{
		// This is pretty bad for multithreaded replay, but this should be very rare.
		iface.sync_threads();
		auto pipeline_iter = replayed_pipelines->find(pipeline);

		// If we don't have the pipeline, we might have it later in the array of graphics pipelines, queue up out of order.
		if (pipeline_iter == replayed_pipelines->end() && pipelines.HasMember(state.GetString()))
		{
			switch (tag)
			{
			case RESOURCE_GRAPHICS_PIPELINE:
				if (!parse_graphics_pipeline(iface, resolver, pipelines, state))
					return false;
				break;

			case RESOURCE_COMPUTE_PIPELINE:
				if (!parse_compute_pipeline(iface, resolver, pipelines, state))
					return false;
				break;

			case RESOURCE_RAYTRACING_PIPELINE:
				if (!parse_raytracing_pipeline(iface, resolver, pipelines, state))
					return false;
				break;

			default:
				return false;
			}

			iface.sync_threads();
			pipeline_iter = replayed_pipelines->find(pipeline);
		}

		// Still don't have it? Look into database.
		if (pipeline_iter == replayed_pipelines->end())
		{
			size_t external_state_size = 0;
			if (!resolver || !resolver->read_entry(tag, pipeline, &external_state_size, nullptr,
			                                       PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}

			vector<uint8_t> external_state(external_state_size);

			if (!resolver->read_entry(tag, pipeline, &external_state_size, external_state.data(),
			                          PAYLOAD_READ_NO_FLAGS))
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}

			if (!this->parse(iface, resolver, external_state.data(), external_state.size()))
				return false;

			iface.sync_threads();
			pipeline_iter = replayed_pipelines->find(pipeline);
			if (pipeline_iter == replayed_pipelines->end())
			{
				log_missing_resource("Base pipeline", pipeline);
				return false;
			}
			else if (pipeline_iter->second == VK_NULL_HANDLE)
			{
				log_invalid_resource("Base pipeline", pipeline);
				return false;
			}
		}
		*out_pipeline = pipeline_iter->second;
	}
	else
		*out_pipeline = api_object_cast<VkPipeline>(pipeline);

	return true;
}

bool StateReplayer::Impl::parse_raytracing_pipeline(StateCreatorInterface &iface, DatabaseInterface *resolver,
                                                    const Value &pipelines, const Value &member)
{
	Hash hash = string_to_uint64(member.GetString());
	if (replayed_raytracing_pipelines.count(hash))
		return true;

	auto *info_allocated = allocator.allocate_cleared<VkRayTracingPipelineCreateInfoKHR>();
	auto &obj = pipelines[member];
	auto &info = *info_allocated;
	info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
	info.flags = obj["flags"].GetUint();
	info.basePipelineIndex = obj["basePipelineIndex"].GetInt();
	info.maxPipelineRayRecursionDepth = obj["maxPipelineRayRecursionDepth"].GetUint();

	// For older captures, we might have captured these flags.
	info.flags = normalize_pipeline_creation_flags(info.flags);

	if (obj.HasMember("stages"))
	{
		info.stageCount = obj["stages"].Size();
		if (!parse_stages(iface, resolver, obj["stages"], &info.pStages))
			return false;
	}

	if (obj.HasMember("groups"))
	{
		info.groupCount = obj["groups"].Size();
		if (!parse_raytracing_groups(obj["groups"], &info.pGroups))
			return false;
	}

	if (obj.HasMember("libraryInterface"))
		if (!parse_library_interface(obj["libraryInterface"], &info.pLibraryInterface))
			return false;

	if (obj.HasMember("libraryInfo"))
	{
		auto &lib_info = obj["libraryInfo"];
		VkPipelineLibraryCreateInfoKHR *library_info = nullptr;
		if (!parse_pipeline_library(iface, resolver, pipelines, lib_info, RESOURCE_RAYTRACING_PIPELINE, &library_info))
			return false;
		info.pLibraryInfo = library_info;
	}

	if (obj.HasMember("dynamicState"))
		if (!parse_dynamic_state(obj["dynamicState"], &info.pDynamicState))
			return false;

	if (!parse_derived_pipeline_handle(iface, resolver, obj["basePipelineHandle"], pipelines,
	                                   RESOURCE_RAYTRACING_PIPELINE, &info.basePipelineHandle))
		return false;

	if (!parse_pipeline_layout_handle(obj["layout"], &info.layout))
		return false;

	if (obj.HasMember("pNext"))
		if (!parse_pnext_chain(obj["pNext"], &info.pNext))
			return false;

	if (!iface.enqueue_create_raytracing_pipeline(hash, &info, &replayed_raytracing_pipelines[hash]))
		return false;

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

	// For older captures, we might have captured these flags.
	info.flags = normalize_pipeline_creation_flags(info.flags);

	if (!parse_derived_pipeline_handle(iface, resolver, obj["basePipelineHandle"], pipelines,
	                                   RESOURCE_GRAPHICS_PIPELINE, &info.basePipelineHandle))
		return false;

	if (!parse_pipeline_layout_handle(obj["layout"], &info.layout))
		return false;

	auto render_pass = string_to_uint64(obj["renderPass"].GetString());
	if (render_pass > 0)
	{
		auto rp_itr = replayed_render_passes.find(render_pass);
		if (rp_itr == end(replayed_render_passes))
		{
			log_missing_resource("Render pass", render_pass);
			return false;
		}
		else if (rp_itr->second == VK_NULL_HANDLE)
		{
			log_invalid_resource("Render pass", render_pass);
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

	if (obj.HasMember("pNext"))
		if (!parse_pnext_chain(obj["pNext"], &info.pNext, &iface, resolver, &pipelines))
			return false;

	if (!iface.enqueue_create_graphics_pipeline(hash, &info, &replayed_graphics_pipelines[hash]))
		return false;

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

bool StateReplayer::Impl::parse_raytracing_pipelines(StateCreatorInterface &iface, DatabaseInterface *resolver,
                                                     const Value &pipelines)
{
	for (auto itr = pipelines.MemberBegin(); itr != pipelines.MemberEnd(); ++itr)
		if (!parse_raytracing_pipeline(iface, resolver, pipelines, itr->name))
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

bool StateReplayer::Impl::parse_vertex_input_divisor_state(const Value &state, VkPipelineVertexInputDivisorStateCreateInfoKHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineVertexInputDivisorStateCreateInfoKHR>();
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

bool StateReplayer::Impl::parse_descriptor_set_binding_flags(const Value &state,
                                                             VkDescriptorSetLayoutBindingFlagsCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkDescriptorSetLayoutBindingFlagsCreateInfoEXT>();
	if (state.HasMember("bindingFlags"))
	{
		info->bindingCount = state["bindingFlags"].Size();
		if (!parse_uints(state["bindingFlags"], &info->pBindingFlags))
			return false;
	}

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_color_blend_advanced_state(const Value &state,
                                                           VkPipelineColorBlendAdvancedStateCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineColorBlendAdvancedStateCreateInfoEXT>();

	info->blendOverlap = static_cast<VkBlendOverlapEXT>(state["blendOverlap"].GetUint());
	info->srcPremultiplied = state["srcPremultiplied"].GetUint();
	info->dstPremultiplied = state["dstPremultiplied"].GetUint();

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_rasterization_conservative_state(const Value &state,
                                                                 VkPipelineRasterizationConservativeStateCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineRasterizationConservativeStateCreateInfoEXT>();

	info->flags = state["flags"].GetUint();
	info->conservativeRasterizationMode = static_cast<VkConservativeRasterizationModeEXT>(state["conservativeRasterizationMode"].GetUint());
	info->extraPrimitiveOverestimationSize = state["extraPrimitiveOverestimationSize"].GetFloat();

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_rasterization_line_state(const Value &state,
                                                         VkPipelineRasterizationLineStateCreateInfoKHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineRasterizationLineStateCreateInfoKHR>();

	info->lineRasterizationMode = static_cast<VkLineRasterizationModeEXT>(state["lineRasterizationMode"].GetUint());
	info->stippledLineEnable = state["stippledLineEnable"].GetUint();
	info->lineStippleFactor = state["lineStippleFactor"].GetUint();
	info->lineStipplePattern = state["lineStipplePattern"].GetUint();

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_shader_stage_required_subgroup_size(const Value &state,
                                                                    VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT>();

	info->requiredSubgroupSize = state["requiredSubgroupSize"].GetUint();

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_attachment_description_stencil_layout(const Value &state,
                                                                      VkAttachmentDescriptionStencilLayout **out_info)
{
	auto *info = allocator.allocate_cleared<VkAttachmentDescriptionStencilLayout>();
	*out_info = info;

	info->stencilInitialLayout = static_cast<VkImageLayout>(state["stencilInitialLayout"].GetUint());
	info->stencilFinalLayout = static_cast<VkImageLayout>(state["stencilFinalLayout"].GetUint());
	return true;
}

bool StateReplayer::Impl::parse_attachment_reference_stencil_layout(const Value &state,
                                                                    VkAttachmentReferenceStencilLayout **out_info)
{
	auto *info = allocator.allocate_cleared<VkAttachmentReferenceStencilLayout>();
	*out_info = info;

	info->stencilLayout = static_cast<VkImageLayout>(state["stencilLayout"].GetUint());
	return true;
}

bool StateReplayer::Impl::parse_subpass_description_depth_stencil_resolve(const Value &state,
                                                                          VkSubpassDescriptionDepthStencilResolve **out_info)
{
	auto *info = allocator.allocate_cleared<VkSubpassDescriptionDepthStencilResolve>();
	*out_info = info;

	info->depthResolveMode = static_cast<VkResolveModeFlagBits>(state["depthResolveMode"].GetUint());
	info->stencilResolveMode = static_cast<VkResolveModeFlagBits>(state["stencilResolveMode"].GetUint());

	if (state.HasMember("depthStencilResolveAttachment"))
	{
		if (!parse_attachment2(state["depthStencilResolveAttachment"], &info->pDepthStencilResolveAttachment))
			return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_fragment_shading_rate_attachment_info(const Value &state,
                                                                      VkFragmentShadingRateAttachmentInfoKHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkFragmentShadingRateAttachmentInfoKHR>();
	*out_info = info;

	info->shadingRateAttachmentTexelSize.width = state["shadingRateAttachmentTexelSize"]["width"].GetUint();
	info->shadingRateAttachmentTexelSize.height = state["shadingRateAttachmentTexelSize"]["height"].GetUint();

	if (state.HasMember("fragmentShadingRateAttachment"))
	{
		if (!parse_attachment2(state["fragmentShadingRateAttachment"], &info->pFragmentShadingRateAttachment))
			return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_pipeline_rendering_info(const Value &state,
                                                        VkPipelineRenderingCreateInfoKHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineRenderingCreateInfoKHR>();
	*out_info = info;

	info->depthAttachmentFormat = static_cast<VkFormat>(state["depthAttachmentFormat"].GetUint());
	info->stencilAttachmentFormat = static_cast<VkFormat>(state["stencilAttachmentFormat"].GetUint());
	info->viewMask = state["viewMask"].GetUint();

	if (state.HasMember("colorAttachmentFormats"))
	{
		auto &atts = state["colorAttachmentFormats"];
		info->colorAttachmentCount = atts.Size();
		static_assert(sizeof(VkFormat) == sizeof(uint32_t), "Enum size is not 32-bit.");
		if (!parse_uints(atts, reinterpret_cast<const uint32_t **>(&info->pColorAttachmentFormats)))
			return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_color_write(const Value &state,
                                            VkPipelineColorWriteCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineColorWriteCreateInfoEXT>();
	*out_info = info;

	info->attachmentCount = state["attachmentCount"].GetUint();

	// Could be null for dynamic state.
	if (state.HasMember("colorWriteEnables"))
	{
		auto &enables = state["colorWriteEnables"];
		static_assert(sizeof(VkBool32) == sizeof(uint32_t), "VkBool32 is not 32-bit.");
		if (!parse_uints(enables, reinterpret_cast<const VkBool32 **>(&info->pColorWriteEnables)))
			return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_sample_locations(const Value &state,
                                                 VkPipelineSampleLocationsStateCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineSampleLocationsStateCreateInfoEXT>();
	*out_info = info;

	info->sampleLocationsEnable = state["sampleLocationsEnable"].GetUint();
	if (state.HasMember("sampleLocationsInfo"))
	{
		auto &loc_info = state["sampleLocationsInfo"];
		info->sampleLocationsInfo.sType = static_cast<VkStructureType>(loc_info["sType"].GetUint());
		info->sampleLocationsInfo.sampleLocationsPerPixel = static_cast<VkSampleCountFlagBits>(loc_info["sampleLocationsPerPixel"].GetUint());
		info->sampleLocationsInfo.sampleLocationGridSize.width = loc_info["sampleLocationGridSize"]["width"].GetUint();
		info->sampleLocationsInfo.sampleLocationGridSize.height = loc_info["sampleLocationGridSize"]["height"].GetUint();

		if (loc_info.HasMember("sampleLocations"))
		{
			auto &locs = loc_info["sampleLocations"];

			auto *locations = allocator.allocate_n<VkSampleLocationEXT>(locs.Size());
			info->sampleLocationsInfo.sampleLocationsCount = uint32_t(locs.Size());
			info->sampleLocationsInfo.pSampleLocations = locations;

			for (auto itr = locs.Begin(); itr != locs.End(); ++itr)
			{
				auto &elem = *itr;
				locations->x = elem["x"].GetFloat();
				locations->y = elem["y"].GetFloat();
				locations++;
			}
		}
	}

	return true;
}

bool StateReplayer::Impl::parse_provoking_vertex(const Value &state,
						 VkPipelineRasterizationProvokingVertexStateCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineRasterizationProvokingVertexStateCreateInfoEXT>();
	*out_info = info;

	info->provokingVertexMode = static_cast<VkProvokingVertexModeEXT>(state["provokingVertexMode"].GetUint());
	return true;
}

bool StateReplayer::Impl::parse_sampler_custom_border_color(const Value &state,
							    VkSamplerCustomBorderColorCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkSamplerCustomBorderColorCreateInfoEXT>();
	*out_info = info;

	for (uint32_t i = 0; i < 4; i++)
		info->customBorderColor.uint32[i] = state["customBorderColor"][i].GetUint();
	info->format = static_cast<VkFormat>(state["format"].GetUint());

	return true;
}

bool StateReplayer::Impl::parse_sampler_reduction_mode(const Value &state,
						       VkSamplerReductionModeCreateInfo **out_info)
{
	auto *info = allocator.allocate_cleared<VkSamplerReductionModeCreateInfo>();
	*out_info = info;

	info->reductionMode = static_cast<VkSamplerReductionMode>(state["reductionMode"].GetUint());

	return true;
}

bool StateReplayer::Impl::parse_input_attachment_aspect(const Value &state,
						        VkRenderPassInputAttachmentAspectCreateInfo **out_info)
{
	auto *info = allocator.allocate_cleared<VkRenderPassInputAttachmentAspectCreateInfo>();
	*out_info = info;

	info->aspectReferenceCount = state["aspectReferences"].Size();

	auto *new_aspects = allocator.allocate_n_cleared<VkInputAttachmentAspectReference>(info->aspectReferenceCount);
	info->pAspectReferences = new_aspects;

	auto &aspects = state["aspectReferences"];
	for (auto aspect_itr = aspects.Begin(); aspect_itr != aspects.End(); ++aspect_itr, new_aspects++)
	{
		auto &aspect = *aspect_itr;
		new_aspects->subpass = aspect["subpass"].GetUint();
		new_aspects->inputAttachmentIndex = aspect["inputAttachmentIndex"].GetUint();
		new_aspects->aspectMask = static_cast<VkImageAspectFlags>(aspect["aspectMask"].GetUint());
	}

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_discard_rectangles(const Value &state,
                                                   VkPipelineDiscardRectangleStateCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineDiscardRectangleStateCreateInfoEXT>();
	*out_info = info;

	info->flags = static_cast<VkPipelineDiscardRectangleStateCreateFlagsEXT>(state["flags"].GetUint());
	info->discardRectangleMode = static_cast<VkDiscardRectangleModeEXT>(state["discardRectangleMode"].GetUint());
	info->discardRectangleCount = state["discardRectangleCount"].GetUint();

	// Could be null for dynamic state.
	if (state.HasMember("discardRectangles"))
	{
		if (!parse_scissors(state["discardRectangles"], &info->pDiscardRectangles))
			return false;
	}

	return true;
}

bool StateReplayer::Impl::parse_memory_barrier2(const Value &state,
                                                VkMemoryBarrier2KHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkMemoryBarrier2KHR>();
	*out_info = info;

	info->srcStageMask = static_cast<VkPipelineStageFlags2KHR>(state["srcStageMask"].GetUint64());
	info->srcAccessMask = static_cast<VkAccessFlags2KHR>(state["srcAccessMask"].GetUint64());
	info->dstStageMask = static_cast<VkPipelineStageFlags2KHR>(state["dstStageMask"].GetUint64());
	info->dstAccessMask = static_cast<VkAccessFlags2KHR>(state["dstAccessMask"].GetUint64());

	return true;
}

bool StateReplayer::Impl::parse_graphics_pipeline_library(const Value &state,
                                                          VkGraphicsPipelineLibraryCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkGraphicsPipelineLibraryCreateInfoEXT>();
	*out_info = info;
	info->flags = state["flags"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_pipeline_library(
		StateCreatorInterface &iface, DatabaseInterface *resolver,
		const Value &pipelines,
		const Value &lib_info, ResourceTag tag,
		VkPipelineLibraryCreateInfoKHR **out_info)
{
	auto &library_list = lib_info["libraries"];
	auto *library_info = allocator.allocate_cleared<VkPipelineLibraryCreateInfoKHR>();
	auto *libraries = allocator.allocate_n<VkPipeline>(library_list.Size());

	library_info->sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
	library_info->libraryCount = library_list.Size();
	library_info->pLibraries = libraries;

	for (auto itr = library_list.Begin(); itr != library_list.End(); ++itr, libraries++)
		if (!parse_derived_pipeline_handle(iface, resolver, *itr, pipelines, tag, libraries))
			return false;

	if (lib_info.HasMember("pNext"))
		if (!parse_pnext_chain(lib_info["pNext"], &library_info->pNext))
			return false;

	*out_info = library_info;
	return true;
}

bool StateReplayer::Impl::parse_fragment_shading_rate(const Value &state,
                                                      VkPipelineFragmentShadingRateStateCreateInfoKHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineFragmentShadingRateStateCreateInfoKHR>();
	*out_info = info;

	// Could be null for dynamic state.
	if (state.HasMember("fragmentSize"))
	{
		info->fragmentSize.width = state["fragmentSize"]["width"].GetUint();
		info->fragmentSize.height = state["fragmentSize"]["height"].GetUint();
	}

	// Could be null for dynamic state.
	if (state.HasMember("combinerOps"))
	{
		for (uint32_t i = 0; i < 2; i++)
			info->combinerOps[i] = static_cast<VkFragmentShadingRateCombinerOpKHR>(state["combinerOps"][i].GetUint());
	}

	return true;
}

bool StateReplayer::Impl::parse_sampler_ycbcr_conversion(const Value &state,
                                                         VkSamplerYcbcrConversionCreateInfo **out_info)
{
	auto *info = allocator.allocate_cleared<VkSamplerYcbcrConversionCreateInfo>();
	*out_info = info;

	info->format = static_cast<VkFormat>(state["format"].GetUint());
	info->ycbcrModel = static_cast<VkSamplerYcbcrModelConversion>(state["ycbcrModel"].GetUint());
	info->ycbcrRange = static_cast<VkSamplerYcbcrRange>(state["ycbcrRange"].GetUint());
	info->components.r = static_cast<VkComponentSwizzle>(state["components"][0].GetUint());
	info->components.g = static_cast<VkComponentSwizzle>(state["components"][1].GetUint());
	info->components.b = static_cast<VkComponentSwizzle>(state["components"][2].GetUint());
	info->components.a = static_cast<VkComponentSwizzle>(state["components"][3].GetUint());
	info->xChromaOffset = static_cast<VkChromaLocation>(state["xChromaOffset"].GetUint());
	info->yChromaOffset = static_cast<VkChromaLocation>(state["yChromaOffset"].GetUint());
	info->chromaFilter = static_cast<VkFilter>(state["chromaFilter"].GetUint());
	info->forceExplicitReconstruction = state["forceExplicitReconstruction"].GetUint();

	return true;
}

bool StateReplayer::Impl::parse_viewport_depth_clip_control(const Value &state,
                                                            VkPipelineViewportDepthClipControlCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineViewportDepthClipControlCreateInfoEXT>();
	*out_info = info;
	info->negativeOneToOne = state["negativeOneToOne"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_pipeline_create_flags2(const Value &state,
                                                       VkPipelineCreateFlags2CreateInfoKHR **flags2)
{
	auto *info = allocator.allocate_cleared<VkPipelineCreateFlags2CreateInfoKHR>();
	*flags2 = info;
	info->flags = state["flags"].GetUint64();
	return true;
}

bool StateReplayer::Impl::parse_render_pass_creation_control(const Value &state, VkRenderPassCreationControlEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkRenderPassCreationControlEXT>();
	info->disallowMerging = state["disallowMerging"].GetUint();
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_sampler_border_color_component_mapping(const Value &state, VkSamplerBorderColorComponentMappingCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkSamplerBorderColorComponentMappingCreateInfoEXT>();
	info->srgb = state["srgb"].GetUint();
	info->components.r = static_cast<VkComponentSwizzle>(state["components"]["r"].GetUint());
	info->components.g = static_cast<VkComponentSwizzle>(state["components"]["g"].GetUint());
	info->components.b = static_cast<VkComponentSwizzle>(state["components"]["b"].GetUint());
	info->components.a = static_cast<VkComponentSwizzle>(state["components"]["a"].GetUint());
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_multisampled_render_to_single_sampled(const Value &state, VkMultisampledRenderToSingleSampledInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkMultisampledRenderToSingleSampledInfoEXT>();
	info->multisampledRenderToSingleSampledEnable = state["multisampledRenderToSingleSampledEnable"].GetUint();
	info->rasterizationSamples = static_cast<VkSampleCountFlagBits>(state["rasterizationSamples"].GetUint());
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_depth_bias_representation(const Value &state, VkDepthBiasRepresentationInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkDepthBiasRepresentationInfoEXT>();
	info->depthBiasExact = state["depthBiasExact"].GetUint();
	info->depthBiasRepresentation = static_cast<VkDepthBiasRepresentationEXT>(state["depthBiasRepresentation"].GetUint());
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_render_pass_fragment_density_map(const Value &state, VkRenderPassFragmentDensityMapCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkRenderPassFragmentDensityMapCreateInfoEXT>();
	info->fragmentDensityMapAttachment.attachment = state["fragmentDensityMapAttachment"]["attachment"].GetUint();
	info->fragmentDensityMapAttachment.layout = static_cast<VkImageLayout>(state["fragmentDensityMapAttachment"]["layout"].GetUint());
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_sample_locations_info(const Value &state, VkSampleLocationsInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkSampleLocationsInfoEXT>();
	info->sampleLocationGridSize.width = state["sampleLocationGridSize"]["width"].GetUint();
	info->sampleLocationGridSize.height = state["sampleLocationGridSize"]["height"].GetUint();
	info->sampleLocationsPerPixel = static_cast<VkSampleCountFlagBits>(state["sampleLocationsPerPixel"].GetUint());

	if (state.HasMember("sampleLocations"))
	{
		auto &locs = state["sampleLocations"];
		auto *locations = allocator.allocate_n<VkSampleLocationEXT>(locs.Size());
		info->sampleLocationsCount = uint32_t(locs.Size());
		info->pSampleLocations = locations;

		for (auto itr = locs.Begin(); itr != locs.End(); ++itr)
		{
			auto &elem = *itr;
			locations->x = elem["x"].GetFloat();
			locations->y = elem["y"].GetFloat();
			locations++;
		}
	}

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_pipeline_robustness(const Value &state, VkPipelineRobustnessCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineRobustnessCreateInfoEXT>();
	info->images = static_cast<VkPipelineRobustnessImageBehaviorEXT>(state["images"].GetUint());
	info->uniformBuffers = static_cast<VkPipelineRobustnessBufferBehaviorEXT>(state["uniformBuffers"].GetUint());
	info->storageBuffers = static_cast<VkPipelineRobustnessBufferBehaviorEXT>(state["storageBuffers"].GetUint());
	info->vertexInputs = static_cast<VkPipelineRobustnessBufferBehaviorEXT>(state["vertexInputs"].GetUint());
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_depth_clamp_control(const Value &state, VkPipelineViewportDepthClampControlCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkPipelineViewportDepthClampControlCreateInfoEXT>();
	info->depthClampMode = static_cast<VkDepthClampModeEXT>(state["depthClampMode"].GetUint());
	if (state.HasMember("depthClampRange"))
	{
		auto *range = allocator.allocate_cleared<VkDepthClampRangeEXT>();
		info->pDepthClampRange = range;
		auto &v = state["depthClampRange"];
		range->minDepthClamp = v["minDepthClamp"].GetFloat();
		range->maxDepthClamp = v["maxDepthClamp"].GetFloat();
	}
	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_rendering_attachment_location_info(const Value &state, VkRenderingAttachmentLocationInfoKHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkRenderingAttachmentLocationInfoKHR>();
	info->colorAttachmentCount = state["colorAttachmentCount"].GetUint();
	if (state.HasMember("colorAttachmentLocations"))
	{
		auto *locs = allocator.allocate_n_cleared<uint32_t>(info->colorAttachmentCount);
		for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
			locs[i] = state["colorAttachmentLocations"][i].GetUint();
		info->pColorAttachmentLocations = locs;
	}

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_rendering_input_attachment_index_info(const Value &state, VkRenderingInputAttachmentIndexInfoKHR **out_info)
{
	auto *info = allocator.allocate_cleared<VkRenderingInputAttachmentIndexInfoKHR>();
	info->colorAttachmentCount = state["colorAttachmentCount"].GetUint();
	if (state.HasMember("colorAttachmentInputIndices"))
	{
		auto *locs = allocator.allocate_n<uint32_t>(info->colorAttachmentCount);
		for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
			locs[i] = state["colorAttachmentInputIndices"][i].GetUint();
		info->pColorAttachmentInputIndices = locs;
	}

	if (state.HasMember("depthInputAttachmentIndex"))
	{
		auto *loc = allocator.allocate<uint32_t>();
		*loc = state["depthInputAttachmentIndex"].GetUint();
		info->pDepthInputAttachmentIndex = loc;
	}

	if (state.HasMember("stencilInputAttachmentIndex"))
	{
		auto *loc = allocator.allocate<uint32_t>();
		*loc = state["stencilInputAttachmentIndex"].GetUint();
		info->pStencilInputAttachmentIndex = loc;
	}

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_mutable_descriptor_type(const Value &state,
                                                        VkMutableDescriptorTypeCreateInfoEXT **out_info)
{
	auto *info = allocator.allocate_cleared<VkMutableDescriptorTypeCreateInfoEXT>();
	*out_info = info;

	if (!state.HasMember("mutableDescriptorTypeLists"))
		return true;

	auto &lists = state["mutableDescriptorTypeLists"];
	if (lists.Empty())
		return true;

	auto out_count = lists.Size();
	auto *out_lists = allocator.allocate_n_cleared<VkMutableDescriptorTypeListEXT>(out_count);

	info->mutableDescriptorTypeListCount = out_count;
	info->pMutableDescriptorTypeLists = out_lists;

	for (uint32_t i = 0; i < out_count; i++)
	{
		auto &list = lists[i];
		auto list_count = list.Size();
		out_lists[i].descriptorTypeCount = list_count;

		if (!list.Empty())
		{
			auto *desc_types = allocator.allocate_n<VkDescriptorType>(list_count);
			out_lists[i].pDescriptorTypes = desc_types;
			for (auto itr = list.Begin(); itr != list.End(); ++itr)
				*desc_types++ = static_cast<VkDescriptorType>(itr->GetUint());
		}
	}

	return true;
}

bool StateReplayer::Impl::parse_multiview_state(const Value &state, VkRenderPassMultiviewCreateInfo **out_info)
{
	auto *info = allocator.allocate_cleared<VkRenderPassMultiviewCreateInfo>();

	if (state.HasMember("viewMasks"))
	{
		info->subpassCount = state["viewMasks"].Size();
		if (!parse_uints(state["viewMasks"], &info->pViewMasks))
			return false;
	}

	if (state.HasMember("viewOffsets"))
	{
		info->dependencyCount = state["viewOffsets"].Size();
		if (!parse_sints(state["viewOffsets"], &info->pViewOffsets))
			return false;
	}

	if (state.HasMember("correlationMasks"))
	{
		info->correlationMaskCount = state["correlationMasks"].Size();
		if (!parse_uints(state["correlationMasks"], &info->pCorrelationMasks))
			return false;
	}

	*out_info = info;
	return true;
}

bool StateReplayer::Impl::parse_pnext_chain(
		const Value &pnext, const void **outpNext,
		StateCreatorInterface *iface, DatabaseInterface *resolver,
		const Value *pipelines)
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

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR:
		{
			VkPipelineVertexInputDivisorStateCreateInfoKHR *info = nullptr;
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

		case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
		{
			VkRenderPassMultiviewCreateInfo *info = nullptr;
			if (!parse_multiview_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT:
		{
			VkDescriptorSetLayoutBindingFlagsCreateInfoEXT *info = nullptr;
			if (!parse_descriptor_set_binding_flags(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
		{
			VkPipelineColorBlendAdvancedStateCreateInfoEXT *info = nullptr;
			if (!parse_color_blend_advanced_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
		{
			VkPipelineRasterizationConservativeStateCreateInfoEXT *info = nullptr;
			if (!parse_rasterization_conservative_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_KHR:
		{
			VkPipelineRasterizationLineStateCreateInfoKHR *info = nullptr;
			if (!parse_rasterization_line_state(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT:
		{
			VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT *info = nullptr;
			if (!parse_shader_stage_required_subgroup_size(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT:
		{
			VkMutableDescriptorTypeCreateInfoEXT *info = nullptr;
			if (!parse_mutable_descriptor_type(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
		{
			VkAttachmentDescriptionStencilLayout *info = nullptr;
			if (!parse_attachment_description_stencil_layout(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
		{
			VkAttachmentReferenceStencilLayout *info = nullptr;
			if (!parse_attachment_reference_stencil_layout(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
		{
			VkSubpassDescriptionDepthStencilResolve *resolve = nullptr;
			if (!parse_subpass_description_depth_stencil_resolve(next, &resolve))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(resolve);
			break;
		}

		case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
		{
			VkFragmentShadingRateAttachmentInfoKHR *rate = nullptr;
			if (!parse_fragment_shading_rate_attachment_info(next, &rate))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(rate);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR:
		{
			VkPipelineRenderingCreateInfoKHR *rendering = nullptr;
			if (!parse_pipeline_rendering_info(next, &rendering))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(rendering);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT:
		{
			VkPipelineColorWriteCreateInfoEXT *color_write = nullptr;
			if (!parse_color_write(next, &color_write))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(color_write);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT:
		{
			VkPipelineSampleLocationsStateCreateInfoEXT *sample_locations = nullptr;
			if (!parse_sample_locations(next, &sample_locations))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(sample_locations);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
		{
			VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *provoking_vertex = nullptr;
			if (!parse_provoking_vertex(next, &provoking_vertex))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(provoking_vertex);
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT:
		{
			VkSamplerCustomBorderColorCreateInfoEXT *custom_border_color = nullptr;
			if (!parse_sampler_custom_border_color(next, &custom_border_color))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(custom_border_color);
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO:
		{
			VkSamplerReductionModeCreateInfo *reduction_mode = nullptr;
			if (!parse_sampler_reduction_mode(next, &reduction_mode))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(reduction_mode);
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
		{
			VkRenderPassInputAttachmentAspectCreateInfo *input_att = nullptr;
			if (!parse_input_attachment_aspect(next, &input_att))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(input_att);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
		{
			VkPipelineDiscardRectangleStateCreateInfoEXT *discard_rectangles = nullptr;
			if (!parse_discard_rectangles(next, &discard_rectangles))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(discard_rectangles);
			break;
		}

		case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR:
		{
			VkMemoryBarrier2KHR *memory_barrier2 = nullptr;
			if (!parse_memory_barrier2(next, &memory_barrier2))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(memory_barrier2);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
		{
			VkPipelineFragmentShadingRateStateCreateInfoKHR *fragment_shading_rate = nullptr;
			if (!parse_fragment_shading_rate(next, &fragment_shading_rate))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(fragment_shading_rate);
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO:
		{
			VkSamplerYcbcrConversionCreateInfo *conv = nullptr;
			if (!parse_sampler_ycbcr_conversion(next, &conv))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(conv);
			break;
		}

		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
		{
			VkGraphicsPipelineLibraryCreateInfoEXT *graphics_pipeline_library = nullptr;
			if (!parse_graphics_pipeline_library(next, &graphics_pipeline_library))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(graphics_pipeline_library);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
		{
			VkPipelineLibraryCreateInfoKHR *library = nullptr;
			if (!iface || !pipelines)
				return false;
			if (!parse_pipeline_library(*iface, resolver, *pipelines, next, RESOURCE_GRAPHICS_PIPELINE, &library))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(library);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
		{
			VkPipelineViewportDepthClipControlCreateInfoEXT *clip = nullptr;
			if (!parse_viewport_depth_clip_control(next, &clip))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(clip);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR:
		{
			VkPipelineCreateFlags2CreateInfoKHR *flags2 = nullptr;
			if (!parse_pipeline_create_flags2(next, &flags2))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(flags2);
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT:
		{
			VkRenderPassCreationControlEXT *info = nullptr;
			if (!parse_render_pass_creation_control(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT:
		{
			VkSamplerBorderColorComponentMappingCreateInfoEXT *info = nullptr;
			if (!parse_sampler_border_color_component_mapping(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
		{
			VkMultisampledRenderToSingleSampledInfoEXT *info = nullptr;
			if (!parse_multisampled_render_to_single_sampled(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT:
		{
			VkDepthBiasRepresentationInfoEXT *info = nullptr;
			if (!parse_depth_bias_representation(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
		{
			VkRenderPassFragmentDensityMapCreateInfoEXT *info = nullptr;
			if (!parse_render_pass_fragment_density_map(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT:
		{
			VkSampleLocationsInfoEXT *info = nullptr;
			if (!parse_sample_locations_info(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT:
		{
			VkPipelineRobustnessCreateInfoEXT *info = nullptr;
			if (!parse_pipeline_robustness(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT:
		{
			VkPipelineViewportDepthClampControlCreateInfoEXT *info = nullptr;
			if (!parse_depth_clamp_control(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR:
		{
			VkRenderingAttachmentLocationInfoKHR *info = nullptr;
			if (!parse_rendering_attachment_location_info(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR:
		{
			VkRenderingInputAttachmentIndexInfoKHR *info = nullptr;
			if (!parse_rendering_input_attachment_index_info(next, &info))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(info);
			break;
		}

		default:
			LOGE_LEVEL("Failed to parse pNext chain for sType: %d\n", int(sType));
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

bool StateReplayer::Impl::parse_robustness2_features(const Value &state,
                                                     VkPhysicalDeviceRobustness2FeaturesEXT **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceRobustness2FeaturesEXT>();
	*out_features = features;

	features->robustBufferAccess2 = state["robustBufferAccess2"].GetUint();
	features->robustImageAccess2 = state["robustImageAccess2"].GetUint();
	features->nullDescriptor = state["nullDescriptor"].GetUint();

	return true;
}

bool StateReplayer::Impl::parse_image_robustness_features(const Value &state,
                                                          VkPhysicalDeviceImageRobustnessFeaturesEXT **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceImageRobustnessFeaturesEXT>();
	*out_features = features;

	features->robustImageAccess = state["robustImageAccess"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_fragment_shading_rate_enums_features(
		const Value &state,
		VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV>();
	*out_features = features;

	features->fragmentShadingRateEnums = state["fragmentShadingRateEnums"].GetUint();
	features->noInvocationFragmentShadingRates = state["noInvocationFragmentShadingRates"].GetUint();
	features->supersampleFragmentShadingRates = state["supersampleFragmentShadingRates"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_fragment_shading_rate_features(
		const Value &state,
		VkPhysicalDeviceFragmentShadingRateFeaturesKHR **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>();
	*out_features = features;

	features->pipelineFragmentShadingRate = state["pipelineFragmentShadingRate"].GetUint();
	features->primitiveFragmentShadingRate = state["primitiveFragmentShadingRate"].GetUint();
	features->attachmentFragmentShadingRate = state["attachmentFragmentShadingRate"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_mesh_shader_features(
		const Value &state,
		VkPhysicalDeviceMeshShaderFeaturesEXT **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceMeshShaderFeaturesEXT>();
	*out_features = features;

	features->taskShader = state["taskShader"].GetUint();
	features->meshShader = state["meshShader"].GetUint();
	features->multiviewMeshShader = state["multiviewMeshShader"].GetUint();
	features->primitiveFragmentShadingRateMeshShader = state["primitiveFragmentShadingRateMeshShader"].GetUint();
	features->meshShaderQueries = state["meshShaderQueries"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_mesh_shader_features_nv(
		const Value &state,
		VkPhysicalDeviceMeshShaderFeaturesNV **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceMeshShaderFeaturesNV>();
	*out_features = features;

	features->taskShader = state["taskShader"].GetUint();
	features->meshShader = state["meshShader"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_descriptor_buffer_features(
		const Value &state,
		VkPhysicalDeviceDescriptorBufferFeaturesEXT **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceDescriptorBufferFeaturesEXT>();
	*out_features = features;

	features->descriptorBuffer = state["descriptorBuffer"].GetUint();
	features->descriptorBufferCaptureReplay = state["descriptorBufferCaptureReplay"].GetUint();
	features->descriptorBufferImageLayoutIgnored = state["descriptorBufferImageLayoutIgnored"].GetUint();
	features->descriptorBufferPushDescriptors = state["descriptorBufferPushDescriptors"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_shader_object_features(
		const Value &state,
		VkPhysicalDeviceShaderObjectFeaturesEXT **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceShaderObjectFeaturesEXT>();
	*out_features = features;

	features->shaderObject = state["shaderObject"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_primitives_generated_query_features(
		const Value &state,
		VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT>();
	*out_features = features;

	features->primitivesGeneratedQuery = state["primitivesGeneratedQuery"].GetUint();
	features->primitivesGeneratedQueryWithNonZeroStreams = state["primitivesGeneratedQueryWithNonZeroStreams"].GetUint();
	features->primitivesGeneratedQueryWithRasterizerDiscard = state["primitivesGeneratedQueryWithRasterizerDiscard"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_2d_view_of_3d_features(
		const Value &state,
		VkPhysicalDeviceImage2DViewOf3DFeaturesEXT **out_features)
{
	auto *features = allocator.allocate_cleared<VkPhysicalDeviceImage2DViewOf3DFeaturesEXT>();
	*out_features = features;

	features->image2DViewOf3D = state["image2DViewOf3D"].GetUint();
	features->sampler2DViewOf3D = state["sampler2DViewOf3D"].GetUint();
	return true;
}

bool StateReplayer::Impl::parse_pnext_chain_pdf2(const Value &pnext, void **outpNext)
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
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
		{
			VkPhysicalDeviceRobustness2FeaturesEXT *robustness2 = nullptr;
			if (!parse_robustness2_features(next, &robustness2))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(robustness2);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
		{
			VkPhysicalDeviceImageRobustnessFeaturesEXT *image_robustness = nullptr;
			if (!parse_image_robustness_features(next, &image_robustness))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(image_robustness);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
		{
			VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV *fragment_shading_rate_enums = nullptr;
			if (!parse_fragment_shading_rate_enums_features(next, &fragment_shading_rate_enums))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(fragment_shading_rate_enums);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
		{
			VkPhysicalDeviceFragmentShadingRateFeaturesKHR *fragment_shading_rate = nullptr;
			if (!parse_fragment_shading_rate_features(next, &fragment_shading_rate))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(fragment_shading_rate);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
		{
			VkPhysicalDeviceMeshShaderFeaturesEXT *mesh_shader = nullptr;
			if (!parse_mesh_shader_features(next, &mesh_shader))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(mesh_shader);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
		{
			VkPhysicalDeviceMeshShaderFeaturesNV *mesh_shader = nullptr;
			if (!parse_mesh_shader_features_nv(next, &mesh_shader))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(mesh_shader);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
		{
			VkPhysicalDeviceDescriptorBufferFeaturesEXT *descriptor_buffer = nullptr;
			if (!parse_descriptor_buffer_features(next, &descriptor_buffer))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(descriptor_buffer);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
		{
			VkPhysicalDeviceShaderObjectFeaturesEXT *shader_object = nullptr;
			if (!parse_shader_object_features(next, &shader_object))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(shader_object);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
		{
			VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *prim_generated = nullptr;
			if (!parse_primitives_generated_query_features(next, &prim_generated))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(prim_generated);
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
		{
			VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *view_2d_of_3d = nullptr;
			if (!parse_2d_view_of_3d_features(next, &view_2d_of_3d))
				return false;
			new_struct = reinterpret_cast<VkBaseInStructure *>(view_2d_of_3d);
			break;
		}

		default:
			LOGE_LEVEL("Failed to parse pNext chain for sType: %d\n", int(sType));
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

void StateReplayer::forget_handle_references()
{
	impl->forget_handle_references();
}

void StateReplayer::forget_pipeline_handle_references()
{
	impl->forget_pipeline_handle_references();
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
	replayed_raytracing_pipelines = other.replayed_raytracing_pipelines;
}

void StateReplayer::Impl::forget_pipeline_handle_references()
{
	replayed_compute_pipelines.clear();
	replayed_graphics_pipelines.clear();
	replayed_raytracing_pipelines.clear();
}

void StateReplayer::Impl::forget_handle_references()
{
	replayed_samplers.clear();
	replayed_descriptor_set_layouts.clear();
	replayed_pipeline_layouts.clear();
	replayed_shader_modules.clear();
	replayed_render_passes.clear();
	forget_pipeline_handle_references();
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
		LOGE_LEVEL("Got parse error: %d\n", int(error));
		return false;
	}

	int version = doc["version"].GetInt();
	if (version > FOSSILIZE_FORMAT_VERSION || version < FOSSILIZE_FORMAT_MIN_COMPAT_VERSION)
	{
		LOGE_LEVEL("JSON version mismatches.");
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
		if (!parse_descriptor_set_layouts(iface, resolver, doc["setLayouts"]))
			return false;

	if (doc.HasMember("pipelineLayouts"))
		if (!parse_pipeline_layouts(iface, doc["pipelineLayouts"]))
			return false;

	if (doc.HasMember("renderPasses"))
		if (!parse_render_passes(iface, doc["renderPasses"]))
			return false;

	if (doc.HasMember("renderPasses2"))
		if (!parse_render_passes2(iface, doc["renderPasses2"]))
			return false;

	if (doc.HasMember("computePipelines"))
		if (!parse_compute_pipelines(iface, resolver, doc["computePipelines"]))
			return false;

	if (doc.HasMember("graphicsPipelines"))
		if (!parse_graphics_pipelines(iface, resolver, doc["graphicsPipelines"]))
			return false;

	if (doc.HasMember("raytracingPipelines"))
		if (!parse_raytracing_pipelines(iface, resolver, doc["raytracingPipelines"]))
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
		const VkPipelineVertexInputDivisorStateCreateInfoKHR *create_info,
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
		const VkRenderPassMultiviewCreateInfo *create_info,
		ScratchAllocator &alloc)
{
	auto *info = copy(create_info, 1, alloc);
	if (info->pViewMasks)
		info->pViewMasks = copy(create_info->pViewMasks, create_info->subpassCount, alloc);
	if (info->pViewOffsets)
		info->pViewOffsets = copy(create_info->pViewOffsets, create_info->dependencyCount, alloc);
	if (info->pCorrelationMasks)
		info->pCorrelationMasks = copy(create_info->pCorrelationMasks, create_info->correlationMaskCount, alloc);

	return info;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT *create_info,
                                             ScratchAllocator &alloc)
{
	auto *info = copy(create_info, 1, alloc);
	if (info->pBindingFlags)
		info->pBindingFlags = copy(create_info->pBindingFlags, create_info->bindingCount, alloc);
	return info;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkMutableDescriptorTypeCreateInfoEXT *create_info,
                                             ScratchAllocator &alloc)
{
	auto *info = copy(create_info, 1, alloc);
	if (info->pMutableDescriptorTypeLists)
		info->pMutableDescriptorTypeLists = copy(create_info->pMutableDescriptorTypeLists, create_info->mutableDescriptorTypeListCount, alloc);

	for (uint32_t i = 0; i < info->mutableDescriptorTypeListCount; i++)
	{
		auto &l = const_cast<VkMutableDescriptorTypeListEXT &>(info->pMutableDescriptorTypeLists[i]);
		if (l.pDescriptorTypes)
			l.pDescriptorTypes = copy(l.pDescriptorTypes, l.descriptorTypeCount, alloc);
	}

	return info;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkSubpassDescriptionDepthStencilResolve *create_info,
                                             ScratchAllocator &alloc)
{
	auto *resolve = copy(create_info, 1, alloc);
	if (resolve->pDepthStencilResolveAttachment)
	{
		auto *att = copy(resolve->pDepthStencilResolveAttachment, 1, alloc);
		if (!copy_pnext_chain(att->pNext, alloc, &att->pNext, nullptr, 0))
			return nullptr;
		resolve->pDepthStencilResolveAttachment = att;
	}

	return resolve;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkFragmentShadingRateAttachmentInfoKHR *create_info,
                                             ScratchAllocator &alloc)
{
	auto *resolve = copy(create_info, 1, alloc);
	if (resolve->pFragmentShadingRateAttachment)
	{
		auto *att = copy(resolve->pFragmentShadingRateAttachment, 1, alloc);
		if (!copy_pnext_chain(att->pNext, alloc, &att->pNext, nullptr, 0))
			return nullptr;
		resolve->pFragmentShadingRateAttachment = att;
	}

	return resolve;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkPipelineRenderingCreateInfoKHR *create_info,
                                             ScratchAllocator &alloc,
                                             VkGraphicsPipelineLibraryFlagsEXT state_flags)
{
	auto *rendering = copy(create_info, 1, alloc);

	bool view_mask_sensitive = (state_flags & (VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT |
	                                           VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
	                                           VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT)) != 0;
	bool format_sensitive = (state_flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) != 0;

	if (format_sensitive)
	{
		rendering->pColorAttachmentFormats =
				copy(rendering->pColorAttachmentFormats, rendering->colorAttachmentCount, alloc);
	}
	else
	{
		rendering->colorAttachmentCount = 0;
		rendering->pColorAttachmentFormats = nullptr;
		rendering->depthAttachmentFormat = VK_FORMAT_UNDEFINED;
		rendering->stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
		if (!view_mask_sensitive)
			rendering->viewMask = 0;
	}

	return rendering;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkPipelineColorWriteCreateInfoEXT *create_info,
                                             ScratchAllocator &alloc,
                                             const DynamicStateInfo *dynamic_state_info)
{
	auto *color_write = copy(create_info, 1, alloc);
	if (dynamic_state_info && !dynamic_state_info->color_write_enable)
		color_write->pColorWriteEnables = copy(color_write->pColorWriteEnables, color_write->attachmentCount, alloc);
	else
		color_write->pColorWriteEnables = nullptr;
	return color_write;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkPipelineSampleLocationsStateCreateInfoEXT *create_info,
                                             ScratchAllocator &alloc,
                                             const DynamicStateInfo *dynamic_state_info)
{
	// This is only a thing for VkImageMemoryBarrier pNext.
	if (create_info->sampleLocationsInfo.pNext)
		return nullptr;

	bool dynamic_enable = dynamic_state_info && dynamic_state_info->sample_locations_enable;
	auto *sample_locations = copy(create_info, 1, alloc);
	if (dynamic_state_info && !dynamic_state_info->sample_locations &&
	    (sample_locations->sampleLocationsEnable || dynamic_enable))
	{
		sample_locations->sampleLocationsInfo.pSampleLocations =
				copy(sample_locations->sampleLocationsInfo.pSampleLocations,
				     sample_locations->sampleLocationsInfo.sampleLocationsCount, alloc);
	}
	else
	{
		// Otherwise, ignore the location info.
		// Either it's dynamic, or ignored due to sampleLocationsEnable being VK_FALSE.
		sample_locations->sampleLocationsInfo = { VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT };
	}

	return sample_locations;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkRenderPassInputAttachmentAspectCreateInfo *create_info,
                                             ScratchAllocator &alloc)
{
	auto *input_att = copy(create_info, 1, alloc);
	input_att->pAspectReferences = copy(input_att->pAspectReferences, input_att->aspectReferenceCount, alloc);
	return input_att;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkPipelineDiscardRectangleStateCreateInfoEXT *create_info,
                                             ScratchAllocator &alloc,
                                             const DynamicStateInfo *dynamic_state_info)
{
	auto *discard_rectangles = copy(create_info, 1, alloc);
	if (dynamic_state_info && dynamic_state_info->discard_rectangle)
		discard_rectangles->pDiscardRectangles = nullptr;
	else
		discard_rectangles->pDiscardRectangles = copy(discard_rectangles->pDiscardRectangles, discard_rectangles->discardRectangleCount, alloc);
	return discard_rectangles;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkPipelineLibraryCreateInfoKHR *create_info,
                                             ScratchAllocator &alloc)
{
	auto *libraries = copy(create_info, 1, alloc);
	libraries->pLibraries = copy(libraries->pLibraries, libraries->libraryCount, alloc);
	return libraries;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *create_info,
                                             ScratchAllocator &alloc)
{
	auto *identifier = copy(create_info, 1, alloc);
	// Safeguard since we'll be copying these to stack variables later.
	identifier->identifierSize =
			std::min<uint32_t>(identifier->identifierSize, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
	identifier->pIdentifier = copy(identifier->pIdentifier, identifier->identifierSize, alloc);
	return identifier;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkSampleLocationsInfoEXT *info, ScratchAllocator &alloc)
{
	auto *loc = copy(info, 1, alloc);
	loc->pSampleLocations = copy(loc->pSampleLocations, loc->sampleLocationsCount, alloc);
	return loc;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkPipelineViewportDepthClampControlCreateInfoEXT *info, ScratchAllocator &alloc,
                                             const DynamicStateInfo *dynamic_state)
{
	auto *clamp_control = copy(info, 1, alloc);
	if ((!dynamic_state || !dynamic_state->depth_clamp_range) &&
	    clamp_control->depthClampMode == VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT &&
	    clamp_control->pDepthClampRange)
	{
		clamp_control->pDepthClampRange = copy(clamp_control->pDepthClampRange, 1, alloc);
	}
	else
		clamp_control->pDepthClampRange = nullptr;

	return clamp_control;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkRenderingAttachmentLocationInfoKHR *info, ScratchAllocator &alloc)
{
	auto *loc_info = copy(info, 1, alloc);
	if (loc_info->pColorAttachmentLocations)
		loc_info->pColorAttachmentLocations = copy(loc_info->pColorAttachmentLocations, info->colorAttachmentCount, alloc);
	return loc_info;
}

void *StateRecorder::Impl::copy_pnext_struct(const VkRenderingInputAttachmentIndexInfoKHR *info, ScratchAllocator &alloc)
{
	auto *loc_info = copy(info, 1, alloc);
	if (loc_info->pColorAttachmentInputIndices)
		loc_info->pColorAttachmentInputIndices = copy(loc_info->pColorAttachmentInputIndices, info->colorAttachmentCount, alloc);
	if (loc_info->pDepthInputAttachmentIndex)
		loc_info->pDepthInputAttachmentIndex = copy(loc_info->pDepthInputAttachmentIndex, 1, alloc);
	if (loc_info->pStencilInputAttachmentIndex)
		loc_info->pStencilInputAttachmentIndex = copy(loc_info->pStencilInputAttachmentIndex, 1, alloc);
	return loc_info;
}

template <typename T>
void *StateRecorder::Impl::copy_pnext_struct_simple(const T *create_info, ScratchAllocator &alloc)
{
	return copy(create_info, 1, alloc);
}

template <typename T>
bool StateRecorder::Impl::copy_pnext_chains(const T *ts, uint32_t count, ScratchAllocator &alloc,
                                            const DynamicStateInfo *dynamic_state_info,
                                            VkGraphicsPipelineLibraryFlagsEXT state_flags)
{
	for (uint32_t i = 0; i < count; i++)
		if (!copy_pnext_chain(ts[i].pNext, alloc, &const_cast<T&>(ts[i]).pNext, dynamic_state_info, state_flags))
			return false;
	return true;
}

bool StateRecorder::Impl::copy_pnext_chain(const void *pNext, ScratchAllocator &alloc, const void **out_pnext,
                                           const DynamicStateInfo *dynamic_state_info,
                                           VkGraphicsPipelineLibraryFlagsEXT state_flags)
{
	VkBaseInStructure new_pnext = {};
	const VkBaseInStructure **ppNext = &new_pnext.pNext;

	while ((pNext = pnext_chain_skip_ignored_entries(pNext)) != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);

		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
		{
			auto ci = static_cast<const VkPipelineTessellationDomainOriginStateCreateInfo *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR:
		{
			auto *ci = static_cast<const VkPipelineVertexInputDivisorStateCreateInfoKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineRasterizationDepthClipStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineRasterizationStateStreamCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
		{
			auto *ci = static_cast<const VkRenderPassMultiviewCreateInfo *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineColorBlendAdvancedStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineRasterizationConservativeStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_KHR:
		{
			auto *ci = static_cast<const VkPipelineRasterizationLineStateCreateInfoKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkMutableDescriptorTypeCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
		{
			auto *ci = static_cast<const VkAttachmentDescriptionStencilLayout *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
		{
			auto *ci = static_cast<const VkAttachmentReferenceStencilLayout *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
		{
			auto *ci = static_cast<const VkSubpassDescriptionDepthStencilResolve *>(pNext);
			auto *resolve = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			if (!resolve)
				return false;
			*ppNext = resolve;
			break;
		}

		case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
		{
			auto *ci = static_cast<const VkFragmentShadingRateAttachmentInfoKHR *>(pNext);
			auto *rate = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			if (!rate)
				return false;
			*ppNext = rate;
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR:
		{
			auto *ci = static_cast<const VkPipelineRenderingCreateInfoKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc, state_flags));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineColorWriteCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc, dynamic_state_info));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineSampleLocationsStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc, dynamic_state_info));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkSamplerCustomBorderColorCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO:
		{
			auto *ci = static_cast<const VkSamplerReductionModeCreateInfo *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
		{
			auto *ci = static_cast<const VkRenderPassInputAttachmentAspectCreateInfo *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineDiscardRectangleStateCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc, dynamic_state_info));
			break;
		}

		case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR:
		{
			auto *ci = static_cast<const VkMemoryBarrier2KHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
		{
			auto *ci = static_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO:
		{
			auto *ci = static_cast<const VkSamplerYcbcrConversionCreateInfo *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO:
		{
			// Special case. Instead of serializing the YCbCr object link, serialize the original create info.
			// Reduces excessive churn for supporting an extremely niche object type.
			auto *ci = static_cast<const VkSamplerYcbcrConversionInfo *>(pNext);
			auto ycbcr = ycbcr_conversions.find(ci->conversion);
			if (ycbcr == ycbcr_conversions.end())
				return false;

			VkSamplerYcbcrConversionCreateInfo *new_create_info;
			if (!copy_ycbcr_conversion(ycbcr->second, alloc, &new_create_info))
				return false;

			*ppNext = reinterpret_cast<VkBaseInStructure *>(new_create_info);
			break;
		}

		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkGraphicsPipelineLibraryCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
		{
			auto *ci = static_cast<const VkShaderModuleCreateInfo *>(pNext);
			VkShaderModuleCreateInfo *new_module = nullptr;
			if (!copy_shader_module(ci, alloc, true, &new_module))
				return false;
			*ppNext = reinterpret_cast<VkBaseInStructure *>(new_module);
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
		{
			auto *ci = static_cast<const VkPipelineLibraryCreateInfoKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR:
		{
			auto *ci = static_cast<const VkPipelineCreateFlags2CreateInfoKHR *>(pNext);
			auto *flags2 = static_cast<VkPipelineCreateFlags2CreateInfoKHR *>(copy_pnext_struct_simple(ci, alloc));
			flags2->flags = normalize_pipeline_creation_flags(flags2->flags);
			*ppNext = reinterpret_cast<VkBaseInStructure *>(flags2);
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT:
		{
			auto *ci = static_cast<const VkRenderPassCreationControlEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkSamplerBorderColorComponentMappingCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
		{
			auto *ci = static_cast<const VkMultisampledRenderToSingleSampledInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT:
		{
			auto *ci = static_cast<const VkDepthBiasRepresentationInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkRenderPassFragmentDensityMapCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT:
		{
			auto *ci = static_cast<const VkSampleLocationsInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineRobustnessCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT:
		{
			auto *ci = static_cast<const VkPipelineViewportDepthClampControlCreateInfoEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc, dynamic_state_info));
			break;
		}

		case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR:
		{
			auto *ci = static_cast<const VkRenderingAttachmentLocationInfoKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR:
		{
			auto *ci = static_cast<const VkRenderingInputAttachmentIndexInfoKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct(ci, alloc));
			break;
		}

		default:
			LOGE_LEVEL("Cannot copy unknown pNext sType: %d.\n", int(pin->sType));
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

void StateRecorder::set_database_enable_application_feature_links(bool enable)
{
	impl->application_feature_links = enable;
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

bool StateRecorder::record_physical_device_features(const void *device_pnext)
{
	// We just ignore pNext, but it's okay to keep it. We will not need to serialize it for now.
	std::lock_guard<std::mutex> lock(impl->record_lock);
	if (!impl->copy_physical_device_features(device_pnext, impl->allocator, &impl->physical_device_features))
		return false;
	impl->application_feature_hash.physical_device_features_hash =
			Hashing::compute_hash_physical_device_features(impl->physical_device_features);
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
	return record_physical_device_features(&features);
}

void StateRecorder::Impl::pump_synchronized_recording(StateRecorder *recorder)
{
	// Thread is not running, drain the queue ourselves.
	if (!worker_thread.joinable())
	{
		std::lock_guard<std::mutex> lock(synchronized_record_lock);
		record_task(recorder, false);
	}
}

void StateRecorder::Impl::push_work_locked(const WorkItem &item)
{
	record_queue.push(item);
	record_cv.notify_one();
}

template <typename T>
void StateRecorder::Impl::push_unregister_locked(VkStructureType sType, T obj)
{
	record_queue.push({sType, api_object_cast<uint64_t>(obj), nullptr, 0});
	record_cv.notify_one();
}

bool StateRecorder::record_sampler(VkSampler sampler, const VkSamplerCreateInfo &create_info, Hash custom_hash)
{
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkSamplerCreateInfo *new_info = nullptr;
		if (!impl->copy_sampler(&create_info, impl->temp_allocator, &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, sampler);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, api_object_cast<uint64_t>(sampler),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_ycbcr_conversion(VkSamplerYcbcrConversion conv,
                                            const VkSamplerYcbcrConversionCreateInfo &create_info)
{
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkSamplerYcbcrConversionCreateInfo *new_info = nullptr;
		if (!impl->copy_ycbcr_conversion(&create_info, impl->ycbcr_temp_allocator, &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			impl->ycbcr_conversions.erase(conv);
			return false;
		}

		// We don't directly serialize these objects. Just remember it for later if a VkSampler is created.
		impl->ycbcr_conversions[conv] = new_info;
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_descriptor_set_layout(VkDescriptorSetLayout set_layout, const VkDescriptorSetLayoutCreateInfo &create_info,
                                                 Hash custom_hash)
{
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkDescriptorSetLayoutCreateInfo *new_info = nullptr;
		if (!impl->copy_descriptor_set_layout(&create_info, impl->temp_allocator, &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, set_layout);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, api_object_cast<uint64_t>(set_layout),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_pipeline_layout(VkPipelineLayout pipeline_layout, const VkPipelineLayoutCreateInfo &create_info,
                                           Hash custom_hash)
{
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		if (create_info.pNext)
		{
			log_error_pnext_chain("pNext in VkPipelineLayoutCreateInfo not supported.", create_info.pNext);
			// Have to forget any reference if this API handle is recycled.
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, pipeline_layout);
			return false;
		}

		VkPipelineLayoutCreateInfo *new_info = nullptr;
		if (!impl->copy_pipeline_layout(&create_info, impl->temp_allocator, &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, pipeline_layout);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, api_object_cast<uint64_t>(pipeline_layout),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_graphics_pipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo &create_info,
                                             const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                             Hash custom_hash,
                                             VkDevice device,
                                             PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii)
{
	// Silently ignore if we have pipeline binaries.
	// We have no way of recording these unless we go the extra mile to
	// override the global key and add extra metadata blocks.
	// Hopefully applications are well-behaved ...
	auto *binaries = find_pnext<VkPipelineBinaryInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, create_info.pNext);
	if (binaries && binaries->binaryCount)
		return true;

	// Ignore pipelines that cannot result in meaningful replay.
	// We are not allowed to look at pStages unless we're emitting pre-raster / fragment shader interface.
	// If we are using module identifier data + on use, we need to push it for record.
	auto state_flags = graphics_pipeline_get_effective_state_flags(create_info);
	if (graphics_pipeline_library_state_flags_have_module_state(state_flags))
	{
		for (uint32_t i = 0; i < create_info.stageCount; i++)
		{
			if (shader_stage_is_identifier_only(create_info.pStages[i]) &&
			    !impl->should_record_identifier_only)
			{
				// Have to forget any reference if this API handle is recycled.
				std::lock_guard<std::mutex> lock(impl->record_lock);
				if (pipeline != VK_NULL_HANDLE)
					impl->push_unregister_locked(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, pipeline);
				return true;
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkGraphicsPipelineCreateInfo *new_info = nullptr;
		if (!impl->copy_graphics_pipeline(&create_info, impl->temp_allocator,
		                                  base_pipelines, base_pipeline_count,
		                                  device, gsmcii,
		                                  &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			if (pipeline != VK_NULL_HANDLE)
				impl->push_unregister_locked(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, pipeline);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, api_object_cast<uint64_t>(pipeline),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_compute_pipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo &create_info,
                                            const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                            Hash custom_hash,
                                            VkDevice device,
                                            PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii)
{
	// Silently ignore if we have pipeline binaries.
	// We have no way of recording these unless we go the extra mile to
	// override the global key and add extra metadata blocks.
	// Hopefully applications are well-behaved ...
	auto *binaries = find_pnext<VkPipelineBinaryInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, create_info.pNext);
	if (binaries && binaries->binaryCount)
		return true;

	// Ignore pipelines that cannot result in meaningful replay.
	// If we are using module identifier data + on use, we need to push it for record.
	if (shader_stage_is_identifier_only(create_info.stage) &&
	    !impl->should_record_identifier_only)
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);
		// Have to forget any reference if this API handle is recycled.
		if (pipeline != VK_NULL_HANDLE)
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, pipeline);
		return true;
	}

	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkComputePipelineCreateInfo *new_info = nullptr;
		if (!impl->copy_compute_pipeline(&create_info, impl->temp_allocator,
		                                 base_pipelines, base_pipeline_count,
		                                 device, gsmcii,
		                                 &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			if (pipeline != VK_NULL_HANDLE)
				impl->push_unregister_locked(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, pipeline);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, api_object_cast<uint64_t>(pipeline),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_raytracing_pipeline(
		VkPipeline pipeline, const VkRayTracingPipelineCreateInfoKHR &create_info,
		const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
		Hash custom_hash,
		VkDevice device,
		PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii)
{
	// Silently ignore if we have pipeline binaries.
	// We have no way of recording these unless we go the extra mile to
	// override the global key and add extra metadata blocks.
	// Hopefully applications are well-behaved ...
	auto *binaries = find_pnext<VkPipelineBinaryInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, create_info.pNext);
	if (binaries && binaries->binaryCount)
		return true;

	// Ignore pipelines that cannot result in meaningful replay.
	// If we are using module identifier data + on use, we need to push it for record.
	for (uint32_t i = 0; i < create_info.stageCount; i++)
	{
		if (shader_stage_is_identifier_only(create_info.pStages[i]) &&
		    !impl->should_record_identifier_only)
		{
			std::lock_guard<std::mutex> lock(impl->record_lock);
			// Have to forget any reference if this API handle is recycled.
			if (pipeline != VK_NULL_HANDLE)
				impl->push_unregister_locked(VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR, pipeline);
			return true;
		}
	}

	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkRayTracingPipelineCreateInfoKHR *new_info = nullptr;
		if (!impl->copy_raytracing_pipeline(&create_info, impl->temp_allocator,
		                                    base_pipelines, base_pipeline_count,
		                                    device, gsmcii,
		                                    &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			if (pipeline != VK_NULL_HANDLE)
				impl->push_unregister_locked(VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR, pipeline);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
		                        api_object_cast<uint64_t>(pipeline), new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_render_pass(VkRenderPass render_pass, const VkRenderPassCreateInfo &create_info,
                                       Hash custom_hash)
{
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkRenderPassCreateInfo *new_info = nullptr;
		if (!impl->copy_render_pass(&create_info, impl->temp_allocator, &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, render_pass);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		                        api_object_cast<uint64_t>(render_pass),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_render_pass2(VkRenderPass render_pass, const VkRenderPassCreateInfo2 &create_info,
                                        Hash custom_hash)
{
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkRenderPassCreateInfo2 *new_info = nullptr;
		if (!impl->copy_render_pass2(&create_info, impl->temp_allocator, &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, render_pass);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
		                        api_object_cast<uint64_t>(render_pass),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

bool StateRecorder::record_shader_module(VkShaderModule module, const VkShaderModuleCreateInfo &create_info,
                                         Hash custom_hash)
{
	{
		std::lock_guard<std::mutex> lock(impl->record_lock);

		VkShaderModuleCreateInfo *new_info = nullptr;
		if (!impl->copy_shader_module(&create_info, impl->temp_allocator, false, &new_info))
		{
			// Have to forget any reference if this API handle is recycled.
			impl->push_unregister_locked(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, module);
			return false;
		}

		impl->push_work_locked({VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, api_object_cast<uint64_t>(module),
		                        new_info, custom_hash});
	}

	impl->pump_synchronized_recording(this);
	return true;
}

void StateRecorder::Impl::record_end()
{
	// Signal end of recording with empty work item
	std::lock_guard<std::mutex> lock(record_lock);
	push_work_locked({ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO /* dummy value */, 0, nullptr, 0 });
}

bool StateRecorder::get_hash_for_compute_pipeline_handle(VkPipeline pipeline, Hash *hash) const
{
	auto itr = impl->compute_pipeline_to_hash.find(pipeline);
	if (itr == end(impl->compute_pipeline_to_hash))
	{
		log_failed_hash("Compute pipeline", pipeline);
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_pipeline_library_handle(VkPipeline pipeline, Hash *hash) const
{
	auto itr = impl->raytracing_pipeline_to_hash.find(pipeline);
	if (itr != end(impl->raytracing_pipeline_to_hash))
	{
		*hash = itr->second;
		return true;
	}

	itr = impl->graphics_pipeline_to_hash.find(pipeline);
	if (itr != end(impl->graphics_pipeline_to_hash))
	{
		*hash = itr->second;
		return true;
	}

	log_failed_hash("Pipeline library", pipeline);
	return false;
}

bool StateRecorder::get_hash_for_raytracing_pipeline_handle(VkPipeline pipeline, Hash *hash) const
{
	auto itr = impl->raytracing_pipeline_to_hash.find(pipeline);
	if (itr == end(impl->raytracing_pipeline_to_hash))
	{
		log_failed_hash("Raytracing pipeline", pipeline);
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
		log_failed_hash("Graphics pipeline", pipeline);
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
		log_failed_hash("Sampler", sampler);
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
		log_failed_hash("Shader module", module);
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_hash_for_shader_module(
		const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *info, Hash *hash) const
{
	return impl->get_hash_for_shader_module(info, hash);
}

bool StateRecorder::get_hash_for_pipeline_layout(VkPipelineLayout layout, Hash *hash) const
{
	if (layout == VK_NULL_HANDLE)
	{
		// Allowed by VK_EXT_graphics_pipeline_library.
		*hash = 0;
		return true;
	}

	auto itr = impl->pipeline_layout_to_hash.find(layout);
	if (itr == end(impl->pipeline_layout_to_hash))
	{
		log_failed_hash("Pipeline layout", layout);
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
	if (layout == VK_NULL_HANDLE)
	{
		// Allowed by VK_EXT_graphics_pipeline_library.
		*hash = 0;
		return true;
	}

	auto itr = impl->descriptor_set_layout_to_hash.find(layout);
	if (itr == end(impl->descriptor_set_layout_to_hash))
	{
		log_failed_hash("Descriptor set layout", layout);
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
	if (render_pass == VK_NULL_HANDLE)
	{
		// Dynamic rendering.
		*hash = 0;
		return true;
	}

	auto itr = impl->render_pass_to_hash.find(render_pass);
	if (itr == end(impl->render_pass_to_hash))
	{
		log_failed_hash("Render pass", render_pass);
		return false;
	}
	else
	{
		*hash = itr->second;
		return true;
	}
}

bool StateRecorder::get_subpass_meta_for_pipeline(const VkGraphicsPipelineCreateInfo &create_info,
                                                  Hash render_pass_hash,
                                                  SubpassMeta *meta) const
{
	return impl->get_subpass_meta_for_pipeline(create_info, render_pass_hash, meta);
}

bool StateRecorder::Impl::copy_shader_module(const VkShaderModuleCreateInfo *create_info, ScratchAllocator &alloc,
                                             bool ignore_pnext, VkShaderModuleCreateInfo **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);

	if (ignore_pnext)
		info->pNext = nullptr;
	else if (!copy_pnext_chain(info->pNext, alloc, &info->pNext, nullptr, 0))
		return false;

	info->pCode = copy(info->pCode, info->codeSize / sizeof(uint32_t), alloc);

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_sampler(const VkSamplerCreateInfo *create_info, ScratchAllocator &alloc,
                                       VkSamplerCreateInfo **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);

	constexpr VkSamplerCreateFlagBits ignore_capture_replay_flags =
			VK_SAMPLER_CREATE_DESCRIPTOR_BUFFER_CAPTURE_REPLAY_BIT_EXT;
	info->flags &= ~ignore_capture_replay_flags;

	if (!copy_pnext_chain(info->pNext, alloc, &info->pNext, nullptr, 0))
		return false;

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_ycbcr_conversion(const VkSamplerYcbcrConversionCreateInfo *create_info,
                                                ScratchAllocator &alloc, VkSamplerYcbcrConversionCreateInfo **out_info)
{
	auto *info = copy(create_info, 1, alloc);

	// Don't support pNext in this struct.
	if (create_info->pNext)
		return false;

	*out_info = info;
	return true;
}

bool StateRecorder::Impl::copy_pnext_chain_pdf2(const void *pNext, ScratchAllocator &alloc, void **out_pnext)
{
	VkBaseInStructure new_pnext = {};
	const VkBaseInStructure **ppNext = &new_pnext.pNext;

	while ((pNext = pnext_chain_pdf2_skip_ignored_entries(pNext)) != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);

		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
		{
			auto *ci = static_cast<const VkPhysicalDeviceRobustness2FeaturesEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
		{
			auto *ci = static_cast<const VkPhysicalDeviceImageRobustnessFeaturesEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
		{
			auto *ci = static_cast<const VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
		{
			auto *ci = static_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
		{
			auto *ci = static_cast<const VkPhysicalDeviceMeshShaderFeaturesNV *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
		{
			auto *ci = static_cast<const VkPhysicalDeviceMeshShaderFeaturesEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
		{
			auto *ci = static_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
		{
			auto *ci = static_cast<const VkPhysicalDeviceShaderObjectFeaturesEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
		{
			auto *ci = static_cast<const VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
		{
			auto *ci = static_cast<const VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *>(pNext);
			*ppNext = static_cast<VkBaseInStructure *>(copy_pnext_struct_simple(ci, alloc));
			break;
		}

		default:
			LOGE_LEVEL("Cannot copy unknown pNext sType: %d.\n", int(pin->sType));
			return false;
		}

		pNext = pin->pNext;
		ppNext = const_cast<const VkBaseInStructure **>(&(*ppNext)->pNext);
		*ppNext = nullptr;
	}

	*out_pnext = (void *)new_pnext.pNext;
	return true;
}

bool StateRecorder::Impl::copy_physical_device_features(const void *device_pnext,
                                                        ScratchAllocator &alloc,
                                                        VkPhysicalDeviceFeatures2 **out_pdf)
{
	// Reorder so that PDF2 comes first in the chain.
	// Caller guarantees that PDF2 is part of the device_pnext.
	// If original application did not have PDF2, just synthesize one based on pEnabledFeatures instead.
	const auto *pdf = find_pnext<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
	                                                        device_pnext);

	if (!pdf)
		return false;

	auto *features = copy(pdf, 1, alloc);
	features->pNext = nullptr;

	if (!copy_pnext_chain_pdf2(device_pnext, alloc, &features->pNext))
		return false;

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
		else
		{
			b.pImmutableSamplers = nullptr;
		}
	}

	if (!copy_pnext_chain(create_info->pNext, alloc, &info->pNext, nullptr, 0))
		return false;

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

template <typename CreateInfo>
static bool update_derived_pipeline(CreateInfo *info, const VkPipeline *base_pipelines, uint32_t base_pipeline_count)
{
	// Check for case where application made use of derivative pipelines and relied on the indexing behavior
	// into an array of pCreateInfos. In the replayer, we only do it one by one,
	// so we need to pass the correct handle to the create pipeline calls.
	if ((info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0)
	{
		if (info->basePipelineHandle == VK_NULL_HANDLE && info->basePipelineIndex >= 0)
		{
			if (uint32_t(info->basePipelineIndex) >= base_pipeline_count)
			{
				LOGE_LEVEL("Base pipeline index is out of range.\n");
				return false;
			}

			info->basePipelineHandle = base_pipelines[info->basePipelineIndex];
		}
		info->basePipelineIndex = -1;
	}
	else
	{
		// Explicitly ignore parameter.
		info->basePipelineHandle = VK_NULL_HANDLE;
		info->basePipelineIndex = -1;
	}

	return true;
}

template <typename CreateInfo>
bool StateRecorder::Impl::copy_stages(CreateInfo *info, ScratchAllocator &alloc,
                                      VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
                                      const DynamicStateInfo *dynamic_state_info)
{
	info->pStages = copy(info->pStages, info->stageCount, alloc);
	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info->pStages[i]);

		if (!stage.pName)
			return false;

		stage.pName = copy(stage.pName, strlen(stage.pName) + 1, alloc);
		if (stage.pSpecializationInfo)
			if (!copy_specialization_info(stage.pSpecializationInfo, alloc, &stage.pSpecializationInfo))
				return false;

		const void *pNext = nullptr;
		if (!copy_pnext_chain(stage.pNext, alloc, &pNext, dynamic_state_info, 0))
			return false;

		stage.pNext = pNext;

		if (!add_module_identifier(&stage, alloc, device, gsmcii))
			return false;
	}

	return true;
}

bool StateRecorder::Impl::add_module_identifier(
		VkPipelineShaderStageCreateInfo *info, ScratchAllocator &alloc,
		VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii)
{
	if (!device || !gsmcii)
		return true;

	if (auto *module_info = find_pnext<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	                                                             info->pNext))
	{
		auto *ident = alloc.allocate_cleared<VkShaderModuleIdentifierEXT>();
		if (!ident)
			return false;
		ident->sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT;
		gsmcii(device, module_info, ident);

		auto *identifier_create_info = alloc.allocate_cleared<VkPipelineShaderStageModuleIdentifierCreateInfoEXT>();
		if (!identifier_create_info)
			return false;

		identifier_create_info->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT;
		identifier_create_info->pIdentifier = ident->identifier;
		identifier_create_info->identifierSize = ident->identifierSize;

		identifier_create_info->pNext = info->pNext;
		info->pNext = identifier_create_info;
	}

	return true;
}

template <typename CreateInfo>
bool StateRecorder::Impl::copy_dynamic_state(CreateInfo *info, ScratchAllocator &alloc,
                                             const DynamicStateInfo *)
{
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

	if (info->pDynamicState)
	{
		const_cast<VkPipelineDynamicStateCreateInfo *>(info->pDynamicState)->pDynamicStates =
				copy(info->pDynamicState->pDynamicStates, info->pDynamicState->dynamicStateCount, alloc);
	}

	return true;
}

template <typename SubCreateInfo>
bool StateRecorder::Impl::copy_sub_create_info(const SubCreateInfo *&sub_info, ScratchAllocator &alloc,
                                               const DynamicStateInfo *dynamic_state_info,
                                               VkGraphicsPipelineLibraryFlagsEXT state_flags)
{
	if (sub_info)
	{
		sub_info = copy(sub_info, 1, alloc);
		const void *pNext = nullptr;
		if (!copy_pnext_chain(sub_info->pNext, alloc, &pNext, dynamic_state_info, state_flags))
			return false;
		const_cast<SubCreateInfo *>(sub_info)->pNext = pNext;
	}

	return true;
}

bool StateRecorder::Impl::copy_compute_pipeline(const VkComputePipelineCreateInfo *create_info, ScratchAllocator &alloc,
                                                const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                                VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
                                                VkComputePipelineCreateInfo **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);
	info->flags = normalize_pipeline_creation_flags(info->flags);
	if (find_pnext<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, info->pNext))
		info->flags = 0;

	if (!update_derived_pipeline(info, base_pipelines, base_pipeline_count))
		return false;

	if (info->stage.pSpecializationInfo)
		if (!copy_specialization_info(info->stage.pSpecializationInfo, alloc, &info->stage.pSpecializationInfo))
			return false;

	info->stage.pName = copy(info->stage.pName, strlen(info->stage.pName) + 1, alloc);

	if (!copy_pnext_chain(info->stage.pNext, alloc, &info->stage.pNext, nullptr, 0))
		return false;

	if (!add_module_identifier(&info->stage, alloc, device, gsmcii))
		return false;

	if (!copy_pnext_chain(info->pNext, alloc, &info->pNext, nullptr, 0))
		return false;

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_raytracing_pipeline(const VkRayTracingPipelineCreateInfoKHR *create_info,
                                                   ScratchAllocator &alloc, const VkPipeline *base_pipelines,
                                                   uint32_t base_pipeline_count,
                                                   VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
                                                   VkRayTracingPipelineCreateInfoKHR **out_info)
{
	auto *info = copy(create_info, 1, alloc);
	info->flags = normalize_pipeline_creation_flags(info->flags);
	if (find_pnext<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, info->pNext))
		info->flags = 0;

	if (!update_derived_pipeline(info, base_pipelines, base_pipeline_count))
		return false;

	if (!copy_stages(info, alloc, device, gsmcii, nullptr))
		return false;
	if (!copy_dynamic_state(info, alloc, nullptr))
		return false;

	if (!copy_sub_create_info(info->pLibraryInfo, alloc, nullptr, 0))
		return false;
	if (!copy_sub_create_info(info->pLibraryInterface, alloc, nullptr, 0))
		return false;

	if (info->pLibraryInfo)
	{
		const_cast<VkPipelineLibraryCreateInfoKHR *>(info->pLibraryInfo)->pLibraries =
				copy(info->pLibraryInfo->pLibraries, info->pLibraryInfo->libraryCount, alloc);
	}

	info->pGroups = copy(info->pGroups, info->groupCount, alloc);
	for (uint32_t i = 0; i < info->groupCount; i++)
	{
		auto &group = const_cast<VkRayTracingShaderGroupCreateInfoKHR &>(info->pGroups[i]);
		const void *pNext = nullptr;
		if (!copy_pnext_chain(group.pNext, alloc, &pNext, nullptr, 0))
			return false;
		group.pNext = pNext;
		group.pShaderGroupCaptureReplayHandle = nullptr;
	}

	if (!copy_pnext_chain(info->pNext, alloc, &info->pNext, nullptr, 0))
		return false;

	*out_info = info;
	return true;
}

bool StateRecorder::Impl::copy_graphics_pipeline(const VkGraphicsPipelineCreateInfo *create_info, ScratchAllocator &alloc,
                                                 const VkPipeline *base_pipelines, uint32_t base_pipeline_count,
                                                 VkDevice device, PFN_vkGetShaderModuleCreateInfoIdentifierEXT gsmcii,
                                                 VkGraphicsPipelineCreateInfo **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);

	// At the time of copy we don't really know the subpass meta state since the render pass is still being
	// parsed in a thread.
	// Assume that the subpass uses color or depth when copying.
	// This could in theory break if app passes pointer to invalid data while taking advantage of this
	// rule, but it has never happened so far ...
	const SubpassMeta subpass_meta = { true, true };

	DynamicStateInfo dynamic_info = {};
	if (create_info->pDynamicState)
		dynamic_info = Hashing::parse_dynamic_state_info(*create_info->pDynamicState);
	GlobalStateInfo global_info = Hashing::parse_global_state_info(*create_info, dynamic_info, subpass_meta);
	auto state_flags = graphics_pipeline_get_effective_state_flags(*info);

	// Remove pointers which a driver must ignore depending on other state.
	if (!global_info.input_assembly)
		info->pInputAssemblyState = nullptr;
	if (!global_info.vertex_input)
		info->pVertexInputState = nullptr;
	if (!global_info.depth_stencil_state)
		info->pDepthStencilState = nullptr;
	if (!global_info.color_blend_state)
		info->pColorBlendState = nullptr;
	if (!global_info.tessellation_state)
		info->pTessellationState = nullptr;
	if (!global_info.viewport_state)
		info->pViewportState = nullptr;
	if (!global_info.multisample_state)
		info->pMultisampleState = nullptr;
	if (!global_info.rasterization_state)
		info->pRasterizationState = nullptr;

	// If we can ignore render pass state, do just that.
	if (!global_info.render_pass_state)
	{
		info->renderPass = VK_NULL_HANDLE;
		info->subpass = 0;
	}

	if (!global_info.layout_state)
		info->layout = VK_NULL_HANDLE;

	info->flags = normalize_pipeline_creation_flags(info->flags);
	if (find_pnext<VkPipelineCreateFlags2CreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, info->pNext))
		info->flags = 0;

	if (!update_derived_pipeline(info, base_pipelines, base_pipeline_count))
		return false;

	if (!copy_sub_create_info(info->pTessellationState, alloc, &dynamic_info, state_flags))
		return false;
	if (!copy_sub_create_info(info->pColorBlendState, alloc, &dynamic_info, state_flags))
		return false;
	if (!copy_sub_create_info(info->pVertexInputState, alloc, &dynamic_info, state_flags))
		return false;
	if (!copy_sub_create_info(info->pMultisampleState, alloc, &dynamic_info, state_flags))
		return false;
	if (!copy_sub_create_info(info->pViewportState, alloc, &dynamic_info, state_flags))
		return false;
	if (!copy_sub_create_info(info->pInputAssemblyState, alloc, &dynamic_info, state_flags))
		return false;
	if (!copy_sub_create_info(info->pDepthStencilState, alloc, &dynamic_info, state_flags))
		return false;
	if (!copy_sub_create_info(info->pRasterizationState, alloc, &dynamic_info, state_flags))
		return false;

	if (global_info.module_state)
	{
		if (!copy_stages(info, alloc, device, gsmcii, &dynamic_info))
			return false;
	}
	else
	{
		info->stageCount = 0;
		info->pStages = nullptr;
	}

	if (!copy_dynamic_state(info, alloc, &dynamic_info))
		return false;

	if (info->pColorBlendState)
	{
		auto &blend = const_cast<VkPipelineColorBlendStateCreateInfo &>(*info->pColorBlendState);

		// Special EDS3 rule. If all of these are set, we must ignore the pAttachments pointer.
		const bool dynamic_attachments = dynamic_info.color_blend_enable &&
		                                 dynamic_info.color_write_mask &&
		                                 dynamic_info.color_blend_equation;

		if (dynamic_attachments)
			blend.pAttachments = nullptr;
		else
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
		if (dynamic_info.sample_mask)
			ms.pSampleMask = nullptr;

		// If rasterizationSamples is dynamic, but not sample mask,
		// rasterizationSamples still provides the size of the sample mask array.
		if (ms.pSampleMask)
			ms.pSampleMask = copy(ms.pSampleMask, (ms.rasterizationSamples + 31) / 32, alloc);
	}

	if (!copy_pnext_chain(info->pNext, alloc, &info->pNext, &dynamic_info, state_flags))
		return false;

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

	if (!copy_pnext_chain(create_info->pNext, alloc, &info->pNext, nullptr, 0))
		return false;

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::copy_render_pass2(const VkRenderPassCreateInfo2 *create_info, ScratchAllocator &alloc,
                                            VkRenderPassCreateInfo2 **out_create_info)
{
	auto *info = copy(create_info, 1, alloc);
	info->pAttachments = copy(info->pAttachments, info->attachmentCount, alloc);
	info->pSubpasses = copy(info->pSubpasses, info->subpassCount, alloc);
	info->pDependencies = copy(info->pDependencies, info->dependencyCount, alloc);
	info->pCorrelatedViewMasks = copy(info->pCorrelatedViewMasks, info->correlatedViewMaskCount, alloc);

	if (info->pAttachments && !copy_pnext_chains(info->pAttachments, info->attachmentCount, alloc, nullptr, 0))
		return false;
	if (info->pSubpasses && !copy_pnext_chains(info->pSubpasses, info->subpassCount, alloc, nullptr, 0))
		return false;
	if (info->pDependencies && !copy_pnext_chains(info->pDependencies, info->dependencyCount, alloc, nullptr, 0))
		return false;

	for (uint32_t i = 0; i < info->subpassCount; i++)
	{
		auto &sub = const_cast<VkSubpassDescription2 &>(info->pSubpasses[i]);
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

		if (sub.pColorAttachments && !copy_pnext_chains(sub.pColorAttachments, sub.colorAttachmentCount, alloc, nullptr, 0))
			return false;
		if (sub.pInputAttachments && !copy_pnext_chains(sub.pInputAttachments, sub.inputAttachmentCount, alloc, nullptr, 0))
			return false;
		if (sub.pResolveAttachments && !copy_pnext_chains(sub.pResolveAttachments, sub.colorAttachmentCount, alloc, nullptr, 0))
			return false;
		if (sub.pDepthStencilAttachment && !copy_pnext_chains(sub.pDepthStencilAttachment, 1, alloc, nullptr, 0))
			return false;
	}

	if (!copy_pnext_chain(create_info->pNext, alloc, &info->pNext, nullptr, 0))
		return false;

	*out_create_info = info;
	return true;
}

bool StateRecorder::Impl::remap_sampler_handle(VkSampler sampler, VkSampler *out_sampler) const
{
	auto itr = sampler_to_hash.find(sampler);
	if (itr == end(sampler_to_hash))
	{
		LOGW_LEVEL("Cannot find sampler in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
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
	if (layout == VK_NULL_HANDLE)
	{
		// This is allowed in VK_EXT_graphics_pipeline_library.
		*out_layout = VK_NULL_HANDLE;
		return true;
	}

	auto itr = descriptor_set_layout_to_hash.find(layout);
	if (itr == end(descriptor_set_layout_to_hash))
	{
		LOGW_LEVEL("Cannot find descriptor set layout in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
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
	if (layout == VK_NULL_HANDLE)
	{
		// This is allowed in VK_EXT_graphics_pipeline_library.
		*out_layout = VK_NULL_HANDLE;
		return true;
	}

	auto itr = pipeline_layout_to_hash.find(layout);
	if (itr == end(pipeline_layout_to_hash))
	{
		LOGW_LEVEL("Cannot find pipeline layout in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
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
		LOGW_LEVEL("Cannot find shader module in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
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
	if (render_pass == VK_NULL_HANDLE)
	{
		// Dynamic rendering.
		*out_render_pass = VK_NULL_HANDLE;
		return true;
	}

	auto itr = render_pass_to_hash.find(render_pass);
	if (itr == end(render_pass_to_hash))
	{
		LOGW_LEVEL("Cannot find render pass in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
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
		LOGW_LEVEL("Cannot find graphics pipeline in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
		return false;
	}
	else
	{
		*out_pipeline = api_object_cast<VkPipeline>(uint64_t(itr->second));
		return true;
	}
}

bool StateRecorder::Impl::remap_raytracing_pipeline_handle(VkPipeline pipeline, VkPipeline *out_pipeline) const
{
	auto itr = raytracing_pipeline_to_hash.find(pipeline);
	if (itr == end(raytracing_pipeline_to_hash))
	{
		LOGW_LEVEL("Cannot find raytracing pipeline in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
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
		LOGW_LEVEL("Cannot find compute pipeline in hashmap.\n"
		           "Object has either not been recorded, or it was not supported by Fossilize.\n");
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

void StateRecorder::Impl::register_on_use(ResourceTag tag, Hash hash) const
{
	if (record_data.write_database_entries && on_use_database_iface &&
	    !on_use_database_iface->has_entry(tag, hash))
	{
		uint64_t t = time(nullptr);
		on_use_database_iface->write_entry(tag, hash, &t, sizeof(t), PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT);
	}
}

void StateRecorder::Impl::register_module_identifier(
		VkShaderModule module, const VkPipelineShaderStageModuleIdentifierCreateInfoEXT &ident)
{
	auto hash = (uint64_t)module;
	if (record_data.write_database_entries &&
	    module_identifier_database_iface &&
	    ident.identifierSize && !module_identifier_database_iface->has_entry(RESOURCE_SHADER_MODULE, hash))
	{
		module_identifier_database_iface->write_entry(
				RESOURCE_SHADER_MODULE, hash, ident.pIdentifier, ident.identifierSize,
				PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT);

		VkShaderModuleIdentifierEXT m = { VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT };
		m.identifierSize = ident.identifierSize;
		memcpy(m.identifier, ident.pIdentifier, ident.identifierSize);
		identifier_to_module[m] = module;
	}
}

bool StateRecorder::Impl::remap_shader_module_handle(VkPipelineShaderStageCreateInfo &info)
{
	const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *identifier = nullptr;
	if (module_identifier_database_iface)
	{
		identifier = find_pnext<VkPipelineShaderStageModuleIdentifierCreateInfoEXT>(
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, info.pNext);
	}

	if (info.module != VK_NULL_HANDLE)
	{
		if (!remap_shader_module_handle(info.module, &info.module))
			return false;
	}
	else if (const auto *module = find_pnext<VkShaderModuleCreateInfo>(
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, info.pNext))
	{
		WorkItem record_item = {};
		record_item.create_info = const_cast<VkShaderModuleCreateInfo *>(module);
		Hash h = record_shader_module(record_item, true);
		info.module = api_object_cast<VkShaderModule>(h);
	}
	else if (identifier)
	{
		Hash h = 0;
		if (!get_hash_for_shader_module(identifier, &h))
			return false;
		info.module = api_object_cast<VkShaderModule>(h);
		register_on_use(RESOURCE_SHADER_MODULE, h);
	}
	else
		return false;

	if (identifier)
		register_module_identifier(info.module, *identifier);

	return true;
}

template <typename CreateInfo>
bool StateRecorder::Impl::remap_shader_module_handles(CreateInfo *info)
{
	for (uint32_t i = 0; i < info->stageCount; i++)
	{
		auto &stage = const_cast<VkPipelineShaderStageCreateInfo &>(info->pStages[i]);
		if (!remap_shader_module_handle(stage))
			return false;
	}

	return true;
}

bool StateRecorder::Impl::remap_graphics_pipeline_ci(VkGraphicsPipelineCreateInfo *info)
{
	if (!remap_render_pass_handle(info->renderPass, &info->renderPass))
		return false;
	if (!remap_pipeline_layout_handle(info->layout, &info->layout))
		return false;

	auto *library = find_pnext<VkPipelineLibraryCreateInfoKHR>(
			VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, info->pNext);

	if (library)
	{
		for (uint32_t i = 0; i < library->libraryCount; i++)
			if (!remap_graphics_pipeline_handle(library->pLibraries[i], const_cast<VkPipeline *>(&library->pLibraries[i])))
				return false;
	}

	if (info->basePipelineHandle != VK_NULL_HANDLE)
		if (!remap_graphics_pipeline_handle(info->basePipelineHandle, &info->basePipelineHandle))
			return false;

	if (!remap_shader_module_handles(info))
		return false;

	return true;
}

bool StateRecorder::Impl::remap_compute_pipeline_ci(VkComputePipelineCreateInfo *info)
{
	if (!remap_shader_module_handle(info->stage))
		return false;

	if (info->basePipelineHandle != VK_NULL_HANDLE)
		if (!remap_compute_pipeline_handle(info->basePipelineHandle, &info->basePipelineHandle))
			return false;

	if (!remap_pipeline_layout_handle(info->layout, &info->layout))
		return false;

	return true;
}

bool StateRecorder::Impl::remap_raytracing_pipeline_ci(VkRayTracingPipelineCreateInfoKHR *info)
{
	if (!remap_shader_module_handles(info))
		return false;

	if (info->basePipelineHandle != VK_NULL_HANDLE)
		if (!remap_raytracing_pipeline_handle(info->basePipelineHandle, &info->basePipelineHandle))
			return false;

	if (!remap_pipeline_layout_handle(info->layout, &info->layout))
		return false;

	if (info->pLibraryInfo)
	{
		auto *libraries = const_cast<VkPipeline *>(info->pLibraryInfo->pLibraries);
		for (uint32_t i = 0; i < info->pLibraryInfo->libraryCount; i++)
		{
			if (!remap_raytracing_pipeline_handle(libraries[i], &libraries[i]))
				return false;
		}
	}

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

bool StateRecorder::Impl::get_subpass_meta_for_render_pass_hash(Hash render_pass_hash, uint32_t subpass,
                                                                SubpassMeta *meta) const
{
	auto itr = render_pass_hash_to_subpass_meta.find(render_pass_hash);
	if (itr != render_pass_hash_to_subpass_meta.end() && subpass < itr->second.subpass_count)
	{
		uint32_t mask;
		if (subpass < 16)
			mask = itr->second.embedded >> (2 * subpass);
		else
			mask = itr->second.fallback[(subpass - 16) / 16] >> (2 * (subpass & 15));
		meta->uses_color = (mask & 1) != 0;
		meta->uses_depth_stencil = (mask & 2) != 0;
		return true;
	}
	else
		return false;
}

template <typename CreateInfo>
StateRecorder::Impl::SubpassMetaStorage StateRecorder::Impl::analyze_subpass_meta_storage(const CreateInfo &info)
{
	SubpassMetaStorage storage = {};

	storage.subpass_count = info.subpassCount;
	if (info.subpassCount > 16)
		storage.fallback.resize(((info.subpassCount - 16) + 15) / 16);

	for (uint32_t i = 0; i < info.subpassCount; i++)
	{
		bool uses_color = info.pSubpasses[i].colorAttachmentCount > 0;
		bool uses_depth_stencil =
				info.pSubpasses[i].pDepthStencilAttachment != nullptr &&
				info.pSubpasses[i].pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED;

		uint32_t &mask = i < 16 ? storage.embedded : storage.fallback[(i - 16) / 16];
		if (uses_color)
			mask |= 1u << (2 * (i & 15) + 0);
		if (uses_depth_stencil)
			mask |= 1u << (2 * (i & 15) + 1);
	}

	return storage;
}

bool StateRecorder::Impl::get_subpass_meta_for_pipeline(const VkGraphicsPipelineCreateInfo &create_info,
                                                        Hash render_pass_hash,
                                                        SubpassMeta *meta) const
{
	auto *library_info = find_pnext<VkGraphicsPipelineLibraryCreateInfoEXT>(
			VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, create_info.pNext);
	bool force_depth_stencil = false;

	if (!render_pass_hash && library_info)
	{
		// If we have fragment shaders, but no idea what our render pass looks like,
		// we might be forced to emit depth stencil state, even if we don't end up needing it.
		// VUID we consider here:
		// If renderPass is VK_NULL_HANDLE and the pipeline is being created with fragment shader state
		// but not fragment output interface state,
		// pDepthStencilState must be a valid pointer to a valid VkPipelineDepthStencilStateCreateInfo structure
		bool fragment = (library_info->flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) != 0;
		bool output = (library_info->flags & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) != 0;
		force_depth_stencil = fragment && !output;
	}

	// If a render pass is present, use that.
	if (render_pass_hash)
	{
		if (!get_subpass_meta_for_render_pass_hash(render_pass_hash, create_info.subpass, meta))
			return false;
	}
	else if (auto *rendering_create_info = find_pnext<VkPipelineRenderingCreateInfoKHR>(
			VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR, create_info.pNext))
	{
		meta->uses_color = rendering_create_info->colorAttachmentCount > 0;
		meta->uses_depth_stencil = rendering_create_info->depthAttachmentFormat != VK_FORMAT_UNDEFINED ||
		                           rendering_create_info->stencilAttachmentFormat != VK_FORMAT_UNDEFINED;
	}
	else
	{
		meta->uses_color = false;
		meta->uses_depth_stencil = false;
	}

	if (force_depth_stencil)
		meta->uses_depth_stencil = true;

	return true;
}

bool StateRecorder::Impl::get_hash_for_shader_module(
		const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *info, Hash *hash) const
{
	VkShaderModuleIdentifierEXT ident = { VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT };
	ident.identifierSize = std::min<uint32_t>(info->identifierSize, VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT);
	memcpy(ident.identifier, info->pIdentifier, ident.identifierSize);
	auto itr = identifier_to_module.find(ident);

	if (itr == identifier_to_module.end())
	{
		return false;
	}
	else
	{
		*hash = api_object_cast<Hash>(itr->second);
		return true;
	}
}

Hash StateRecorder::Impl::record_shader_module(const WorkItem &record_item, bool dependent_record)
{
	auto *create_info = reinterpret_cast<VkShaderModuleCreateInfo *>(record_item.create_info);
	Hash hash = record_item.custom_hash;
	auto vk_object = api_object_cast<VkShaderModule>(record_item.handle);

	if (hash == 0)
	{
		if (!create_info || !Hashing::compute_hash_shader_module(*create_info, &hash))
		{
			shader_module_to_hash.erase(vk_object);
			return hash;
		}
	}

	if (hash)
		register_on_use(RESOURCE_SHADER_MODULE, hash);

	if (record_item.handle != 0)
		shader_module_to_hash[vk_object] = hash;

	auto &blob = record_data.blob;

	if (database_iface)
	{
		if (record_data.write_database_entries)
		{
			if (register_application_link_hash(RESOURCE_SHADER_MODULE, hash, blob))
				record_data.need_flush = true;

			if (!database_iface->has_entry(RESOURCE_SHADER_MODULE, hash))
			{
				if (serialize_shader_module(hash, *create_info, blob, allocator))
				{
					database_iface->write_entry(RESOURCE_SHADER_MODULE, hash, blob.data(), blob.size(),
												record_data.payload_flags);
					record_data.need_flush = true;
				}
			}
		}

		// If this is called as part of a graphics pipeline library call, we must be very careful not
		// to reclaim the scratch allocator memory (yet). Otherwise we risk corruption.
		if (!dependent_record)
			allocator.reset();
	}
	else
	{
		// Retain for combined serialize() later.
		if (!shader_modules.count(hash))
		{
			VkShaderModuleCreateInfo *create_info_copy = nullptr;
			if (copy_shader_module(create_info, allocator, false, &create_info_copy))
				shader_modules[hash] = create_info_copy;
		}
	}

	// If we're a dependent record, the pipeline remapping logic will ensure to register the module.
	if (module_identifier_database_iface && !dependent_record)
	{
		auto *identifier = find_pnext<VkPipelineShaderStageModuleIdentifierCreateInfoEXT>(
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, create_info->pNext);
		if (identifier)
			register_module_identifier(api_object_cast<VkShaderModule>(hash), *identifier);
	}

	return hash;
}

void StateRecorder::Impl::record_task(StateRecorder *recorder, bool looping)
{
	auto &blob = record_data.blob;

	if (record_data.need_prepare)
	{
		record_data.payload_flags = 0;
		if (compression)
			record_data.payload_flags |= PAYLOAD_WRITE_COMPRESS_BIT;
		if (checksum)
			record_data.payload_flags |= PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT;

		// Keep a single, pre-allocated buffer.
		record_data.write_database_entries = true;
		blob.reserve(64 * 1024);

		// Start by preparing in the thread since we need to parse an archive potentially,
		// and that might block a little bit.
		if (database_iface)
		{
			if (!database_iface->prepare())
			{
				LOGE_LEVEL("Failed to prepare database, will not dump data to database.\n");
				database_iface = nullptr;
			}

			// Check here in the worker thread if we should write database entries for this application info.
			if (application_info_filter)
				record_data.write_database_entries = application_info_filter->test_application_info(application_info);
		}

		if (on_use_database_iface && !on_use_database_iface->prepare())
		{
			LOGE_LEVEL("Failed to prepare on-use database, will not dump those.\n");
			on_use_database_iface = nullptr;
		}

		if (database_iface && record_data.write_database_entries)
		{
			Hasher h;
			Hashing::hash_application_feature_info(h, application_feature_hash);
			if (serialize_application_info(blob))
			{
				database_iface->write_entry(RESOURCE_APPLICATION_INFO, h.get(), blob.data(), blob.size(),
											record_data.payload_flags);

				register_on_use(RESOURCE_APPLICATION_INFO, h.get());
			}
			else
				LOGE_LEVEL("Failed to serialize application info.\n");
		}

		if (module_identifier_database_iface && !module_identifier_database_iface->prepare())
		{
			LOGE_LEVEL("Failed to prepare module identifier database, will not dump identifiers.\n");
			module_identifier_database_iface = nullptr;
		}

		if (module_identifier_database_iface)
		{
			size_t num_hashes = 0;
			module_identifier_database_iface->get_hash_list_for_resource_tag(
					RESOURCE_SHADER_MODULE, &num_hashes, nullptr);
			std::vector<Hash> hashes(num_hashes);
			if (module_identifier_database_iface->get_hash_list_for_resource_tag(
					RESOURCE_SHADER_MODULE, &num_hashes, hashes.data()))
			{
				for (auto hash : hashes)
				{
					VkShaderModuleIdentifierEXT m = { VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT };
					size_t size = VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT;
					if (module_identifier_database_iface->read_entry(
							RESOURCE_SHADER_MODULE, hash, &size, m.identifier, PAYLOAD_READ_NO_FLAGS))
					{
						m.identifierSize = uint32_t(size);
						identifier_to_module[m] = api_object_cast<VkShaderModule>(hash);
					}
				}
			}
		}
	}

	record_data.need_prepare = false;
	record_data.need_flush = false;

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
			// Once no new writes have occurred for a second, we flush, and go to deep sleep.
			bool has_data;
			if (record_data.need_flush)
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

			if (database_iface && !has_data && record_data.need_flush)
			{
				database_iface->flush();
				record_data.need_flush = false;
				continue;
			}
			else
			{
				record_item = record_queue.front();
				record_queue.pop();
			}
		}

		if (!record_item.create_info && record_item.handle == 0)
			break;

		auto record_type = record_item.type;
		ResourceTag tag = RESOURCE_COUNT;
		Hash hash = 0;

		switch (record_type)
		{
		case VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkSamplerCreateInfo *>(record_item.create_info);
			hash = record_item.custom_hash;
			auto vk_object = api_object_cast<VkSampler>(record_item.handle);
			tag = RESOURCE_SAMPLER;

			if (hash == 0)
			{
				if (!create_info || !Hashing::compute_hash_sampler(*create_info, &hash))
				{
					// Forget this reference if we had one with same pointer value.
					sampler_to_hash.erase(vk_object);
					break;
				}
			}

			sampler_to_hash[vk_object] = hash;

			if (database_iface)
			{
				if (record_data.write_database_entries)
				{
					if (register_application_link_hash(tag, hash, blob))
						record_data.need_flush = true;

					if (!database_iface->has_entry(tag, hash))
					{
						if (serialize_sampler(hash, *create_info, blob))
						{
							database_iface->write_entry(tag, hash, blob.data(), blob.size(),
														record_data.payload_flags);
							record_data.need_flush = true;
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
		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2:
		{
			VkRenderPassCreateInfo *create_info = nullptr;
			VkRenderPassCreateInfo2 *create_info2 = nullptr;
			SubpassMetaStorage subpass_meta = {};
			tag = RESOURCE_RENDER_PASS;

			if (record_item.create_info)
			{
				if (record_type == VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2)
				{
					create_info2 = reinterpret_cast<VkRenderPassCreateInfo2 *>(record_item.create_info);
					subpass_meta = analyze_subpass_meta_storage(*create_info2);
				}
				else
				{
					create_info = reinterpret_cast<VkRenderPassCreateInfo *>(record_item.create_info);
					subpass_meta = analyze_subpass_meta_storage(*create_info);
				}
			}

			auto vk_object = api_object_cast<VkRenderPass>(record_item.handle);
			hash = record_item.custom_hash;
			if (hash == 0)
			{
				if (!record_item.create_info ||
				    (create_info && !Hashing::compute_hash_render_pass(*create_info, &hash)) ||
				    (create_info2 && !Hashing::compute_hash_render_pass2(*create_info2, &hash)))
				{
					render_pass_to_hash.erase(vk_object);
					break;
				}
			}

			render_pass_to_hash[vk_object] = hash;
			render_pass_hash_to_subpass_meta[hash] = std::move(subpass_meta);

			if (database_iface)
			{
				if (record_data.write_database_entries)
				{
					if (register_application_link_hash(tag, hash, blob))
						record_data.need_flush = true;

					if (!database_iface->has_entry(tag, hash))
					{
						if ((create_info && serialize_render_pass(hash, *create_info, blob)) ||
						    (create_info2 && serialize_render_pass2(hash, *create_info2, blob)))
						{
							database_iface->write_entry(tag, hash, blob.data(), blob.size(),
														record_data.payload_flags);
							record_data.need_flush = true;
						}
					}
				}
			}
			else
			{
				// Retain for combined serialize() later.
				if (!render_passes.count(hash))
				{
					if (create_info)
					{
						VkRenderPassCreateInfo *create_info_copy = nullptr;
						if (copy_render_pass(create_info, allocator, &create_info_copy))
							render_passes[hash] = create_info_copy;
					}
					else if (create_info2)
					{
						VkRenderPassCreateInfo2 *create_info_copy = nullptr;
						if (copy_render_pass2(create_info2, allocator, &create_info_copy))
							render_passes[hash] = create_info_copy;
					}
				}
			}
			break;
		}

		case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
		{
			record_shader_module(record_item, false);
			break;
		}

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkDescriptorSetLayoutCreateInfo *>(record_item.create_info);
			hash = record_item.custom_hash;
			auto vk_object = api_object_cast<VkDescriptorSetLayout>(record_item.handle);
			tag = RESOURCE_DESCRIPTOR_SET_LAYOUT;

			if (hash == 0)
			{
				if (!create_info || !Hashing::compute_hash_descriptor_set_layout(*recorder, *create_info, &hash))
				{
					descriptor_set_layout_to_hash.erase(vk_object);
					break;
				}
			}

			VkDescriptorSetLayoutCreateInfo *create_info_copy = nullptr;
			if (!copy_descriptor_set_layout(create_info, allocator, &create_info_copy) ||
			    !remap_descriptor_set_layout_ci(create_info_copy))
			{
				descriptor_set_layout_to_hash.erase(vk_object);
				break;
			}

			descriptor_set_layout_to_hash[vk_object] = hash;

			if (database_iface)
			{
				if (record_data.write_database_entries)
				{
					if (register_application_link_hash(tag, hash, blob))
						record_data.need_flush = true;

					if (!database_iface->has_entry(tag, hash))
					{
						if (serialize_descriptor_set_layout(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(tag, hash, blob.data(), blob.size(),
														record_data.payload_flags);
							record_data.need_flush = true;
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
			hash = record_item.custom_hash;
			auto vk_object = api_object_cast<VkPipelineLayout>(record_item.handle);
			tag = RESOURCE_PIPELINE_LAYOUT;

			if (hash == 0)
			{
				if (!create_info || !Hashing::compute_hash_pipeline_layout(*recorder, *create_info, &hash))
				{
					pipeline_layout_to_hash.erase(vk_object);
					break;
				}
			}

			VkPipelineLayoutCreateInfo *create_info_copy = nullptr;
			if (!copy_pipeline_layout(create_info, allocator, &create_info_copy) ||
			    !remap_pipeline_layout_ci(create_info_copy))
			{
				pipeline_layout_to_hash.erase(vk_object);
				break;
			}

			pipeline_layout_to_hash[vk_object] = hash;

			if (database_iface)
			{
				if (record_data.write_database_entries)
				{
					if (register_application_link_hash(tag, hash, blob))
						record_data.need_flush = true;

					if (!database_iface->has_entry(tag, hash))
					{
						if (serialize_pipeline_layout(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(tag, hash, blob.data(), blob.size(),
														record_data.payload_flags);
							record_data.need_flush = true;
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

		case VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR:
		{
			auto *create_info = reinterpret_cast<VkRayTracingPipelineCreateInfoKHR *>(record_item.create_info);
			hash = record_item.custom_hash;
			auto vk_object = api_object_cast<VkPipeline>(record_item.handle);
			tag = RESOURCE_RAYTRACING_PIPELINE;

			if (hash == 0)
			{
				if (!create_info || !Hashing::compute_hash_raytracing_pipeline(*recorder, *create_info, &hash))
				{
					if (vk_object)
						raytracing_pipeline_to_hash.erase(vk_object);
					break;
				}
			}

			VkRayTracingPipelineCreateInfoKHR *create_info_copy = nullptr;
			if (!copy_raytracing_pipeline(create_info, allocator, nullptr, 0, nullptr, nullptr, &create_info_copy) ||
			    !remap_raytracing_pipeline_ci(create_info_copy))
			{
				if (vk_object)
					raytracing_pipeline_to_hash.erase(vk_object);
				break;
			}

			if (vk_object)
				raytracing_pipeline_to_hash[vk_object] = hash;

			if (database_iface)
			{
				if (record_data.write_database_entries)
				{
					if (register_application_link_hash(tag, hash, blob))
						record_data.need_flush = true;

					if (!database_iface->has_entry(tag, hash))
					{
						if (serialize_raytracing_pipeline(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(tag, hash, blob.data(), blob.size(),
														record_data.payload_flags);
							record_data.need_flush = true;
						}
					}
				}

				// Don't need to keep copied data around, reset the allocator.
				allocator.reset();
			}
			else
			{
				// Retain for combined serialize() later.
				if (!raytracing_pipelines.count(hash))
					raytracing_pipelines[hash] = create_info_copy;
			}

			break;
		}

		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:
		{
			auto *create_info = reinterpret_cast<VkGraphicsPipelineCreateInfo *>(record_item.create_info);
			hash = record_item.custom_hash;
			auto vk_object = api_object_cast<VkPipeline>(record_item.handle);
			tag = RESOURCE_GRAPHICS_PIPELINE;

			if (hash == 0)
			{
				if (!create_info || !Hashing::compute_hash_graphics_pipeline(*recorder, *create_info, &hash))
				{
					if (vk_object)
						graphics_pipeline_to_hash.erase(vk_object);
					break;
				}
			}

			VkGraphicsPipelineCreateInfo *create_info_copy = nullptr;
			if (!copy_graphics_pipeline(create_info, allocator, nullptr, 0, nullptr, nullptr, &create_info_copy) ||
			    !remap_graphics_pipeline_ci(create_info_copy))
			{
				if (vk_object)
					graphics_pipeline_to_hash.erase(vk_object);
				break;
			}

			if (vk_object)
				graphics_pipeline_to_hash[vk_object] = hash;

			if (database_iface)
			{
				if (record_data.write_database_entries)
				{
					if (register_application_link_hash(tag, hash, blob))
						record_data.need_flush = true;

					if (!database_iface->has_entry(tag, hash))
					{
						if (serialize_graphics_pipeline(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(tag, hash, blob.data(), blob.size(),
														record_data.payload_flags);
							record_data.need_flush = true;
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
			hash = record_item.custom_hash;
			auto vk_object = api_object_cast<VkPipeline>(record_item.handle);
			tag = RESOURCE_COMPUTE_PIPELINE;

			if (hash == 0)
			{
				if (!create_info || !Hashing::compute_hash_compute_pipeline(*recorder, *create_info, &hash))
				{
					if (vk_object)
						compute_pipeline_to_hash.erase(vk_object);
					break;
				}
			}

			VkComputePipelineCreateInfo *create_info_copy = nullptr;
			if (!copy_compute_pipeline(create_info, allocator, nullptr, 0, nullptr, nullptr, &create_info_copy) ||
			    !remap_compute_pipeline_ci(create_info_copy))
			{
				if (vk_object)
					compute_pipeline_to_hash.erase(vk_object);
				break;
			}

			if (vk_object)
				compute_pipeline_to_hash[vk_object] = hash;

			if (database_iface)
			{
				if (record_data.write_database_entries)
				{
					if (register_application_link_hash(tag, hash, blob))
						record_data.need_flush = true;

					if (!database_iface->has_entry(tag, hash))
					{
						if (serialize_compute_pipeline(hash, *create_info_copy, blob))
						{
							database_iface->write_entry(tag, hash, blob.data(), blob.size(),
														record_data.payload_flags);
							record_data.need_flush = true;
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

		if (hash)
			register_on_use(tag, hash);
	}

	if (looping)
	{
		if (database_iface)
			database_iface->flush();
		if (module_identifier_database_iface)
			module_identifier_database_iface->flush();
		if (on_use_database_iface)
			on_use_database_iface->flush();

		// We no longer need a reference to this.
		// This should allow us to call init_recording_thread again if we want,
		// or emit some final single threaded recording tasks.
		database_iface = nullptr;
		module_identifier_database_iface = nullptr;
		on_use_database_iface = nullptr;
	}
	else if (database_iface)
	{
		database_iface->flush();
		if (module_identifier_database_iface)
			module_identifier_database_iface->flush();
		if (on_use_database_iface)
			on_use_database_iface->flush();
	}
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

template <typename T, typename Allocator>
static bool pnext_chain_add_json_value(Value &base, const T &t, Allocator &alloc,
                                       const DynamicStateInfo *dynamic_state_info);

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

	if (!pnext_chain_add_json_value(s, sampler, alloc, nullptr))
		return false;

	*out_value = s;
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
static bool json_value(const VkPipelineTessellationDomainOriginStateCreateInfo &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("domainOrigin", create_info.domainOrigin, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineVertexInputDivisorStateCreateInfoKHR &create_info, Allocator &alloc, Value *out_value)
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
static bool json_value(const VkRenderPassMultiviewCreateInfo &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	if (create_info.subpassCount)
	{
		Value view_masks(kArrayType);
		for (uint32_t i = 0; i < create_info.subpassCount; i++)
			view_masks.PushBack(create_info.pViewMasks[i], alloc);
		value.AddMember("viewMasks", view_masks, alloc);
	}

	if (create_info.dependencyCount)
	{
		Value view_offsets(kArrayType);
		for (uint32_t i = 0; i < create_info.dependencyCount; i++)
			view_offsets.PushBack(create_info.pViewOffsets[i], alloc);
		value.AddMember("viewOffsets", view_offsets, alloc);
	}

	if (create_info.correlationMaskCount)
	{
		Value correlation_masks(kArrayType);
		for (uint32_t i = 0; i < create_info.correlationMaskCount; i++)
			correlation_masks.PushBack(create_info.pCorrelationMasks[i], alloc);
		value.AddMember("correlationMasks", correlation_masks, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	if (create_info.bindingCount)
	{
		Value binding_flags(kArrayType);
		for (uint32_t i = 0; i < create_info.bindingCount; i++)
			binding_flags.PushBack(create_info.pBindingFlags[i], alloc);
		value.AddMember("bindingFlags", binding_flags, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineColorBlendAdvancedStateCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	value.AddMember("srcPremultiplied", uint32_t(create_info.srcPremultiplied), alloc);
	value.AddMember("dstPremultiplied", uint32_t(create_info.dstPremultiplied), alloc);
	value.AddMember("blendOverlap", create_info.blendOverlap, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineRasterizationConservativeStateCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	value.AddMember("flags", create_info.flags, alloc);
	value.AddMember("conservativeRasterizationMode", create_info.conservativeRasterizationMode, alloc);
	value.AddMember("extraPrimitiveOverestimationSize", create_info.extraPrimitiveOverestimationSize, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineRasterizationLineStateCreateInfoKHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	value.AddMember("lineRasterizationMode", create_info.lineRasterizationMode, alloc);
	value.AddMember("stippledLineEnable", create_info.stippledLineEnable, alloc);
	value.AddMember("lineStippleFactor", create_info.lineStippleFactor, alloc);
	value.AddMember("lineStipplePattern", create_info.lineStipplePattern, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("requiredSubgroupSize", create_info.requiredSubgroupSize, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkMutableDescriptorTypeCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	Value lists(kArrayType);
	for (uint32_t i = 0; i < create_info.mutableDescriptorTypeListCount; i++)
	{
		Value list(kArrayType);
		auto &l = create_info.pMutableDescriptorTypeLists[i];
		for (uint32_t j = 0; j < l.descriptorTypeCount; j++)
			list.PushBack(l.pDescriptorTypes[j], alloc);
		lists.PushBack(list, alloc);
	}
	value.AddMember("mutableDescriptorTypeLists", lists, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkAttachmentDescriptionStencilLayout &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("stencilInitialLayout", create_info.stencilInitialLayout, alloc);
	value.AddMember("stencilFinalLayout", create_info.stencilFinalLayout, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkAttachmentReferenceStencilLayout &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("stencilLayout", create_info.stencilLayout, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineRenderingCreateInfoKHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("depthAttachmentFormat", uint32_t(create_info.depthAttachmentFormat), alloc);
	value.AddMember("stencilAttachmentFormat", uint32_t(create_info.stencilAttachmentFormat), alloc);
	value.AddMember("viewMask", create_info.viewMask, alloc);

	if (create_info.colorAttachmentCount)
	{
		Value colors(kArrayType);
		for (uint32_t i = 0; i < create_info.colorAttachmentCount; i++)
			colors.PushBack(uint32_t(create_info.pColorAttachmentFormats[i]), alloc);
		value.AddMember("colorAttachmentFormats", colors, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineColorWriteCreateInfoEXT &create_info, Allocator &alloc, Value *out_value,
                       const DynamicStateInfo *dynamic_state_info)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("attachmentCount", create_info.attachmentCount, alloc);

	if (!dynamic_state_info)
		return false;

	if (create_info.pColorWriteEnables && !dynamic_state_info->color_write_enable)
	{
		Value enables(kArrayType);
		for (uint32_t i = 0; i < create_info.attachmentCount; i++)
			enables.PushBack(uint32_t(create_info.pColorWriteEnables[i]), alloc);
		value.AddMember("colorWriteEnables", enables, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("provokingVertexMode", create_info.provokingVertexMode, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkSamplerCustomBorderColorCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	Value customBorderColor(kArrayType);
	for (uint32_t i = 0; i < 4; i++)
		customBorderColor.PushBack(create_info.customBorderColor.uint32[i], alloc);
	value.AddMember("customBorderColor", customBorderColor, alloc);
	value.AddMember("format", create_info.format, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkSamplerReductionModeCreateInfo &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("reductionMode", create_info.reductionMode, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRenderPassInputAttachmentAspectCreateInfo &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	Value aspects(kArrayType);
	for (uint32_t i = 0; i < create_info.aspectReferenceCount; i++)
	{
		Value aspect(kObjectType);
		aspect.AddMember("subpass", create_info.pAspectReferences[i].subpass, alloc);
		aspect.AddMember("inputAttachmentIndex", create_info.pAspectReferences[i].inputAttachmentIndex, alloc);
		aspect.AddMember("aspectMask", create_info.pAspectReferences[i].aspectMask, alloc);
		aspects.PushBack(aspect, alloc);
	}
	value.AddMember("aspectReferences", aspects, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineDiscardRectangleStateCreateInfoEXT &create_info, Allocator &alloc, Value *out_value,
                       const DynamicStateInfo *dynamic_state_info)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("flags", create_info.flags, alloc);
	value.AddMember("discardRectangleMode", create_info.discardRectangleMode, alloc);
	value.AddMember("discardRectangleCount", create_info.discardRectangleCount, alloc);

	if (dynamic_state_info && !dynamic_state_info->discard_rectangle)
	{
		Value discardRectangles(kArrayType);
		for (uint32_t i = 0; i < create_info.discardRectangleCount; i++)
		{
			Value discardRectangle(kObjectType);
			discardRectangle.AddMember("x", create_info.pDiscardRectangles[i].offset.x, alloc);
			discardRectangle.AddMember("y", create_info.pDiscardRectangles[i].offset.y, alloc);
			discardRectangle.AddMember("width", create_info.pDiscardRectangles[i].extent.width, alloc);
			discardRectangle.AddMember("height", create_info.pDiscardRectangles[i].extent.height, alloc);
			discardRectangles.PushBack(discardRectangle, alloc);
		}
		value.AddMember("discardRectangles", discardRectangles, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkMemoryBarrier2KHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("srcStageMask", create_info.srcStageMask, alloc);
	value.AddMember("srcAccessMask", create_info.srcAccessMask, alloc);
	value.AddMember("dstStageMask", create_info.dstStageMask, alloc);
	value.AddMember("dstAccessMask", create_info.dstAccessMask, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkGraphicsPipelineLibraryCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("flags", create_info.flags, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineFragmentShadingRateStateCreateInfoKHR &create_info, Allocator &alloc, Value *out_value,
                       const DynamicStateInfo *dynamic_state_info)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	if (dynamic_state_info && !dynamic_state_info->fragment_shading_rate)
	{
		Value extent(kObjectType);
		extent.AddMember("width", create_info.fragmentSize.width, alloc);
		extent.AddMember("height", create_info.fragmentSize.height, alloc);
		value.AddMember("fragmentSize", extent, alloc);

		Value combinerOps(kArrayType);
		for (uint32_t i = 0; i < 2; i++)
			combinerOps.PushBack(create_info.combinerOps[i], alloc);
		value.AddMember("combinerOps", combinerOps, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkSamplerYcbcrConversionCreateInfo &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	Value comp(kArrayType);

	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("format", create_info.format, alloc);
	value.AddMember("ycbcrModel", create_info.ycbcrModel, alloc);
	value.AddMember("ycbcrRange", create_info.ycbcrRange, alloc);
	comp.PushBack(create_info.components.r, alloc);
	comp.PushBack(create_info.components.g, alloc);
	comp.PushBack(create_info.components.b, alloc);
	comp.PushBack(create_info.components.a, alloc);
	value.AddMember("components", comp, alloc);
	value.AddMember("xChromaOffset", create_info.xChromaOffset, alloc);
	value.AddMember("yChromaOffset", create_info.yChromaOffset, alloc);
	value.AddMember("chromaFilter", create_info.chromaFilter, alloc);
	value.AddMember("forceExplicitReconstruction", create_info.forceExplicitReconstruction, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineLibraryCreateInfoKHR &create_info, Allocator &alloc,
                       bool in_pnext_chain, Value *out_value)
{
	Value library_info(kObjectType);
	Value libraries(kArrayType);
	for (uint32_t i = 0; i < create_info.libraryCount; i++)
		libraries.PushBack(uint64_string(api_object_cast<uint64_t>(create_info.pLibraries[i]), alloc), alloc);
	library_info.AddMember("libraries", libraries, alloc);

	if (in_pnext_chain)
	{
		library_info.AddMember("sType", create_info.sType, alloc);
	}
	else
	{
		if (!pnext_chain_add_json_value(library_info, create_info, alloc, nullptr))
			return false;
	}

	*out_value = library_info;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineViewportDepthClipControlCreateInfoEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("negativeOneToOne", create_info.negativeOneToOne, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRenderPassCreationControlEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("disallowMerging", create_info.disallowMerging, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkSamplerBorderColorComponentMappingCreateInfoEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("srgb", create_info.srgb, alloc);
	Value components(kObjectType);
	components.AddMember("r", create_info.components.r, alloc);
	components.AddMember("g", create_info.components.g, alloc);
	components.AddMember("b", create_info.components.b, alloc);
	components.AddMember("a", create_info.components.a, alloc);
	value.AddMember("components", components, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkMultisampledRenderToSingleSampledInfoEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("rasterizationSamples", create_info.rasterizationSamples, alloc);
	value.AddMember("multisampledRenderToSingleSampledEnable", create_info.multisampledRenderToSingleSampledEnable, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkDepthBiasRepresentationInfoEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("depthBiasRepresentation", create_info.depthBiasRepresentation, alloc);
	value.AddMember("depthBiasExact", create_info.depthBiasExact, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRenderPassFragmentDensityMapCreateInfoEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	Value attachment(kObjectType);
	attachment.AddMember("attachment", create_info.fragmentDensityMapAttachment.attachment, alloc);
	attachment.AddMember("layout", create_info.fragmentDensityMapAttachment.layout, alloc);
	value.AddMember("fragmentDensityMapAttachment", attachment, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkSampleLocationsInfoEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	value.AddMember("sampleLocationsPerPixel", create_info.sampleLocationsPerPixel, alloc);
	{
		Value extent(kObjectType);
		extent.AddMember("width", create_info.sampleLocationGridSize.width, alloc);
		extent.AddMember("height", create_info.sampleLocationGridSize.height, alloc);
		value.AddMember("sampleLocationGridSize", extent, alloc);
	}

	if (create_info.sampleLocationsCount)
	{
		Value locs(kArrayType);
		for (uint32_t i = 0; i < create_info.sampleLocationsCount; i++)
		{
			Value loc(kObjectType);
			loc.AddMember("x", create_info.pSampleLocations[i].x, alloc);
			loc.AddMember("y", create_info.pSampleLocations[i].y, alloc);
			locs.PushBack(loc, alloc);
		}
		value.AddMember("sampleLocations", locs, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineRobustnessCreateInfoEXT &create_info,
                       Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("storageBuffers", create_info.storageBuffers, alloc);
	value.AddMember("vertexInputs", create_info.vertexInputs, alloc);
	value.AddMember("uniformBuffers", create_info.uniformBuffers, alloc);
	value.AddMember("images", create_info.images, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineCreateFlags2CreateInfoKHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("flags", create_info.flags, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineViewportDepthClampControlCreateInfoEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("depthClampMode", create_info.depthClampMode, alloc);

	if (create_info.pDepthClampRange)
	{
		Value range(kObjectType);
		range.AddMember("minDepthClamp", create_info.pDepthClampRange->minDepthClamp, alloc);
		range.AddMember("maxDepthClamp", create_info.pDepthClampRange->maxDepthClamp, alloc);
		value.AddMember("depthClampRange", range, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRenderingAttachmentLocationInfoKHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("colorAttachmentCount", create_info.colorAttachmentCount, alloc);

	if (create_info.pColorAttachmentLocations)
	{
		Value range(kArrayType);
		for (uint32_t i = 0; i < create_info.colorAttachmentCount; i++)
			range.PushBack(create_info.pColorAttachmentLocations[i], alloc);
		value.AddMember("colorAttachmentLocations", range, alloc);
	}

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRenderingInputAttachmentIndexInfoKHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("colorAttachmentCount", create_info.colorAttachmentCount, alloc);

	if (create_info.pColorAttachmentInputIndices)
	{
		Value range(kArrayType);
		for (uint32_t i = 0; i < create_info.colorAttachmentCount; i++)
			range.PushBack(create_info.pColorAttachmentInputIndices[i], alloc);
		value.AddMember("colorAttachmentInputIndices", range, alloc);
	}

	if (create_info.pDepthInputAttachmentIndex)
		value.AddMember("depthInputAttachmentIndex", *create_info.pDepthInputAttachmentIndex, alloc);
	if (create_info.pStencilInputAttachmentIndex)
		value.AddMember("stencilInputAttachmentIndex", *create_info.pStencilInputAttachmentIndex, alloc);

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkSubpassDescriptionDepthStencilResolve &create_info, Allocator &alloc, Value *out_value);
template <typename Allocator>
static bool json_value(const VkFragmentShadingRateAttachmentInfoKHR &create_info, Allocator &alloc, Value *out_value);

template <typename Allocator>
static bool pnext_chain_json_value(const void *pNext, Allocator &alloc, Value *out_value,
                                   const DynamicStateInfo *dynamic_state_info)
{
	Value nexts(kArrayType);

	while ((pNext = pnext_chain_skip_ignored_entries(pNext)) != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);
		bool ignored = false;
		Value next;

		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO:
			if (!json_value(*static_cast<const VkPipelineTessellationDomainOriginStateCreateInfo *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR:
			if (!json_value(*static_cast<const VkPipelineVertexInputDivisorStateCreateInfoKHR *>(pNext), alloc, &next))
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

		case VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO:
			if (!json_value(*static_cast<const VkRenderPassMultiviewCreateInfo *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineColorBlendAdvancedStateCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineRasterizationConservativeStateCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_KHR:
			if (!json_value(*static_cast<const VkPipelineRasterizationLineStateCreateInfoKHR *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkMutableDescriptorTypeCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT:
			if (!json_value(*static_cast<const VkAttachmentDescriptionStencilLayout *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT:
			if (!json_value(*static_cast<const VkAttachmentReferenceStencilLayout *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE:
			if (!json_value(*static_cast<const VkSubpassDescriptionDepthStencilResolve *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR:
			if (!json_value(*static_cast<const VkFragmentShadingRateAttachmentInfoKHR *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR:
			if (!json_value(*static_cast<const VkPipelineRenderingCreateInfoKHR *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineColorWriteCreateInfoEXT *>(pNext), alloc, &next, dynamic_state_info))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineSampleLocationsStateCreateInfoEXT *>(pNext), alloc, &next, dynamic_state_info))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkSamplerCustomBorderColorCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO:
			if (!json_value(*static_cast<const VkSamplerReductionModeCreateInfo *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO:
			if (!json_value(*static_cast<const VkRenderPassInputAttachmentAspectCreateInfo *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineDiscardRectangleStateCreateInfoEXT *>(pNext), alloc, &next, dynamic_state_info))
				return false;
			break;

		case VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR:
			if (!json_value(*static_cast<const VkMemoryBarrier2KHR *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR:
			if (!json_value(*static_cast<const VkPipelineFragmentShadingRateStateCreateInfoKHR *>(pNext), alloc, &next, dynamic_state_info))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO:
			if (!json_value(*static_cast<const VkSamplerYcbcrConversionCreateInfo *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkGraphicsPipelineLibraryCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
		case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT:
			// Ignored.
			ignored = true;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR:
			if (!json_value(*static_cast<const VkPipelineLibraryCreateInfoKHR *>(pNext), alloc, true, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR:
			if (!json_value(*static_cast<const VkPipelineCreateFlags2CreateInfoKHR *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT:
			if (!json_value(*static_cast<const VkRenderPassCreationControlEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkSamplerBorderColorComponentMappingCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT:
			if (!json_value(*static_cast<const VkMultisampledRenderToSingleSampledInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT:
			if (!json_value(*static_cast<const VkDepthBiasRepresentationInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkRenderPassFragmentDensityMapCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT:
			if (!json_value(*static_cast<const VkSampleLocationsInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineRobustnessCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT:
			if (!json_value(*static_cast<const VkPipelineViewportDepthClampControlCreateInfoEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO_KHR:
			if (!json_value(*static_cast<const VkRenderingAttachmentLocationInfoKHR *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO_KHR:
			if (!json_value(*static_cast<const VkRenderingInputAttachmentIndexInfoKHR *>(pNext), alloc, &next))
				return false;
			break;

		default:
			log_error_pnext_chain("Unsupported pNext found, cannot hash sType.", pNext);
			return false;
		}

		if (!ignored)
			nexts.PushBack(next, alloc);
		pNext = pin->pNext;
	}

	*out_value = nexts;
	return true;
}

template <typename T, typename Allocator>
static bool pnext_chain_add_json_value(Value &base, const T &t, Allocator &alloc,
                                       const DynamicStateInfo *dynamic_state_info)
{
	if (t.pNext)
	{
		Value nexts;
		if (!pnext_chain_json_value(t.pNext, alloc, &nexts, dynamic_state_info))
			return false;
		base.AddMember("pNext", nexts, alloc);
	}
	return true;
}

template <typename Allocator>
static bool json_value(const VkAttachmentReference2 &att, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("attachment", att.attachment, alloc);
	value.AddMember("layout", att.layout, alloc);
	value.AddMember("aspectMask", att.aspectMask, alloc);
	if (!pnext_chain_add_json_value(value, att, alloc, nullptr))
		return false;

	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkSubpassDescriptionDepthStencilResolve &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("depthResolveMode", create_info.depthResolveMode, alloc);
	value.AddMember("stencilResolveMode", create_info.stencilResolveMode, alloc);
	if (create_info.pDepthStencilResolveAttachment)
	{
		Value att;
		if (!json_value(*create_info.pDepthStencilResolveAttachment, alloc, &att))
			return false;
		value.AddMember("depthStencilResolveAttachment", att, alloc);
	}
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkFragmentShadingRateAttachmentInfoKHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);

	Value extent(kObjectType);
	extent.AddMember("width", create_info.shadingRateAttachmentTexelSize.width, alloc);
	extent.AddMember("height", create_info.shadingRateAttachmentTexelSize.height, alloc);
	value.AddMember("shadingRateAttachmentTexelSize", extent, alloc);
	if (create_info.pFragmentShadingRateAttachment)
	{
		Value att;
		if (!json_value(*create_info.pFragmentShadingRateAttachment, alloc, &att))
			return false;
		value.AddMember("fragmentShadingRateAttachment", att, alloc);
	}
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineSampleLocationsStateCreateInfoEXT &create_info, Allocator &alloc, Value *out_value,
                       const DynamicStateInfo *dynamic_state_info)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("sampleLocationsEnable", create_info.sampleLocationsEnable, alloc);

	bool dynamic_enable = dynamic_state_info && dynamic_state_info->sample_locations_enable;
	if ((dynamic_enable || create_info.sampleLocationsEnable) &&
	    dynamic_state_info && !dynamic_state_info->sample_locations)
	{
		Value locations(kObjectType);
		auto &info = create_info.sampleLocationsInfo;

		locations.AddMember("sType", info.sType, alloc);
		locations.AddMember("sampleLocationsPerPixel", info.sampleLocationsPerPixel, alloc);

		{
			Value extent(kObjectType);
			extent.AddMember("width", info.sampleLocationGridSize.width, alloc);
			extent.AddMember("height", info.sampleLocationGridSize.height, alloc);
			locations.AddMember("sampleLocationGridSize", extent, alloc);
		}

		if (info.sampleLocationsCount)
		{
			Value locs(kArrayType);
			for (uint32_t i = 0; i < info.sampleLocationsCount; i++)
			{
				Value loc(kObjectType);
				loc.AddMember("x", info.pSampleLocations[i].x, alloc);
				loc.AddMember("y", info.pSampleLocations[i].y, alloc);
				locs.PushBack(loc, alloc);
			}
			locations.AddMember("sampleLocations", locs, alloc);
		}

		value.AddMember("sampleLocationsInfo", locations, alloc);
	}

	*out_value = value;
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

	if (!pnext_chain_add_json_value(stage, pipe.stage, alloc, nullptr))
		return false;
	p.AddMember("stage", stage, alloc);

	if (!pnext_chain_add_json_value(p, pipe, alloc, nullptr))
		return false;

	*out_value = p;
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

	if (!pnext_chain_add_json_value(l, layout, alloc, nullptr))
		return false;

	*out_value = l;
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

	if (!pnext_chain_add_json_value(json_object, pass, alloc, nullptr))
		return false;

	*out_value = json_object;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRenderPassCreateInfo2 &pass, Allocator &alloc, Value *out_value)
{
	Value json_object(kObjectType);
	json_object.AddMember("flags", pass.flags, alloc);

	Value deps(kArrayType);
	Value subpasses(kArrayType);
	Value attachments(kArrayType);

	if (pass.pCorrelatedViewMasks)
	{
		Value view_masks(kArrayType);
		for (uint32_t i = 0; i < pass.correlatedViewMaskCount; i++)
			view_masks.PushBack(pass.pCorrelatedViewMasks[i], alloc);
		json_object.AddMember("correlatedViewMasks", view_masks, alloc);
	}

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
			dep.AddMember("viewOffset", d.viewOffset, alloc);
			if (!pnext_chain_add_json_value(dep, d, alloc, nullptr))
				return false;
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
			if (!pnext_chain_add_json_value(att, a, alloc, nullptr))
				return false;
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
		p.AddMember("viewMask", sub.viewMask, alloc);

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
				Value input;
				if (!json_value(sub.pInputAttachments[j], alloc, &input))
					return false;
				inputs.PushBack(input, alloc);
			}
			p.AddMember("inputAttachments", inputs, alloc);
		}

		if (sub.pColorAttachments)
		{
			Value colors(kArrayType);
			for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
			{
				Value color;
				if (!json_value(sub.pColorAttachments[j], alloc, &color))
					return false;
				colors.PushBack(color, alloc);
			}
			p.AddMember("colorAttachments", colors, alloc);
		}

		if (sub.pResolveAttachments)
		{
			Value resolves(kArrayType);
			for (uint32_t j = 0; j < sub.colorAttachmentCount; j++)
			{
				Value resolve;
				if (!json_value(sub.pResolveAttachments[j], alloc, &resolve))
					return false;
				resolves.PushBack(resolve, alloc);
			}
			p.AddMember("resolveAttachments", resolves, alloc);
		}

		if (sub.pDepthStencilAttachment)
		{
			Value depth_stencil;
			if (!json_value(*sub.pDepthStencilAttachment, alloc, &depth_stencil))
				return false;
			p.AddMember("depthStencilAttachment", depth_stencil, alloc);
		}

		if (!pnext_chain_add_json_value(p, sub, alloc, nullptr))
			return false;
		subpasses.PushBack(p, alloc);
	}
	json_object.AddMember("subpasses", subpasses, alloc);

	if (!pnext_chain_add_json_value(json_object, pass, alloc, nullptr))
		return false;

	*out_value = json_object;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineShaderStageCreateInfo *pStages, uint32_t stageCount,
                       Allocator &alloc, Value *out_value)
{
	Value stages(kArrayType);
	for (uint32_t i = 0; i < stageCount; i++)
	{
		auto &s = pStages[i];
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

		if (!pnext_chain_add_json_value(stage, s, alloc, nullptr))
			return false;
		stages.PushBack(stage, alloc);
	}

	*out_value = stages;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPipelineDynamicStateCreateInfo &dynamic, Allocator &alloc, Value *out_value)
{
	Value dyn(kObjectType);
	dyn.AddMember("flags", dynamic.flags, alloc);
	Value dynamics(kArrayType);
	for (uint32_t i = 0; i < dynamic.dynamicStateCount; i++)
		dynamics.PushBack(dynamic.pDynamicStates[i], alloc);
	dyn.AddMember("dynamicState", dynamics, alloc);

	*out_value = dyn;
	return true;
}

template <typename Allocator>
static bool json_value(const VkRayTracingPipelineCreateInfoKHR &pipe, Allocator &alloc, Value *out_value)
{
	Value p(kObjectType);
	p.AddMember("flags", pipe.flags, alloc);
	p.AddMember("layout", uint64_string(api_object_cast<uint64_t>(pipe.layout), alloc), alloc);
	p.AddMember("basePipelineHandle", uint64_string(api_object_cast<uint64_t>(pipe.basePipelineHandle), alloc), alloc);
	p.AddMember("basePipelineIndex", pipe.basePipelineIndex, alloc);
	p.AddMember("maxPipelineRayRecursionDepth", pipe.maxPipelineRayRecursionDepth, alloc);

	if (pipe.pDynamicState)
	{
		Value dyn;
		if (!json_value(*pipe.pDynamicState, alloc, &dyn))
			return false;
		p.AddMember("dynamicState", dyn, alloc);
	}

	Value stages;
	if (!json_value(pipe.pStages, pipe.stageCount, alloc, &stages))
		return false;
	p.AddMember("stages", stages, alloc);

	if (pipe.pLibraryInterface)
	{
		Value iface(kObjectType);
		iface.AddMember("maxPipelineRayPayloadSize", pipe.pLibraryInterface->maxPipelineRayPayloadSize, alloc);
		iface.AddMember("maxPipelineRayHitAttributeSize", pipe.pLibraryInterface->maxPipelineRayHitAttributeSize, alloc);
		if (!pnext_chain_add_json_value(iface, *pipe.pLibraryInterface, alloc, nullptr))
			return false;
		p.AddMember("libraryInterface", iface, alloc);
	}

	if (pipe.pLibraryInfo)
	{
		Value library_info;
		if (!json_value(*pipe.pLibraryInfo, alloc, false, &library_info))
			return false;
		p.AddMember("libraryInfo", library_info, alloc);
	}

	Value groups(kArrayType);
	for (uint32_t i = 0; i < pipe.groupCount; i++)
	{
		Value group(kObjectType);
		group.AddMember("anyHitShader", pipe.pGroups[i].anyHitShader, alloc);
		group.AddMember("intersectionShader", pipe.pGroups[i].intersectionShader, alloc);
		group.AddMember("generalShader", pipe.pGroups[i].generalShader, alloc);
		group.AddMember("closestHitShader", pipe.pGroups[i].closestHitShader, alloc);
		group.AddMember("type", pipe.pGroups[i].type, alloc);
		if (!pnext_chain_add_json_value(group, pipe.pGroups[i], alloc, nullptr))
			return false;
		groups.PushBack(group, alloc);
	}
	p.AddMember("groups", groups, alloc);

	if (!pnext_chain_add_json_value(p, pipe, alloc, nullptr))
		return false;

	*out_value = p;
	return true;
}

template <typename Allocator>
static bool json_value(const VkGraphicsPipelineCreateInfo &pipe,
                       const StateRecorder::SubpassMeta &subpass_meta,
                       Allocator &alloc, Value *out_value)
{
	Value p(kObjectType);
	p.AddMember("flags", pipe.flags, alloc);
	p.AddMember("basePipelineHandle", uint64_string(api_object_cast<uint64_t>(pipe.basePipelineHandle), alloc), alloc);
	p.AddMember("basePipelineIndex", pipe.basePipelineIndex, alloc);
	p.AddMember("layout", uint64_string(api_object_cast<uint64_t>(pipe.layout), alloc), alloc);
	p.AddMember("renderPass", uint64_string(api_object_cast<uint64_t>(pipe.renderPass), alloc), alloc);
	p.AddMember("subpass", pipe.subpass, alloc);

	DynamicStateInfo dynamic_info = {};
	if (pipe.pDynamicState)
		dynamic_info = Hashing::parse_dynamic_state_info(*pipe.pDynamicState);
	GlobalStateInfo global_info = Hashing::parse_global_state_info(pipe, dynamic_info, subpass_meta);

	if (global_info.tessellation_state)
	{
		Value tess(kObjectType);
		tess.AddMember("flags", pipe.pTessellationState->flags, alloc);
		tess.AddMember("patchControlPoints", pipe.pTessellationState->patchControlPoints, alloc);
		if (!pnext_chain_add_json_value(tess, *pipe.pTessellationState, alloc, &dynamic_info))
			return false;
		p.AddMember("tessellationState", tess, alloc);
	}

	if (pipe.pDynamicState)
	{
		Value dyn;
		if (!json_value(*pipe.pDynamicState, alloc, &dyn))
			return false;
		p.AddMember("dynamicState", dyn, alloc);
	}

	if (global_info.multisample_state)
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

		if (!pnext_chain_add_json_value(ms, *pipe.pMultisampleState, alloc, &dynamic_info))
			return false;
		p.AddMember("multisampleState", ms, alloc);
	}

	if (global_info.vertex_input)
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
		if (!pnext_chain_add_json_value(vi, *pipe.pVertexInputState, alloc, &dynamic_info))
			return false;
		p.AddMember("vertexInputState", vi, alloc);
	}

	if (global_info.rasterization_state)
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
		if (!pnext_chain_add_json_value(rs, *pipe.pRasterizationState, alloc, &dynamic_info))
			return false;
		p.AddMember("rasterizationState", rs, alloc);
	}

	if (global_info.input_assembly)
	{
		Value ia(kObjectType);
		ia.AddMember("flags", pipe.pInputAssemblyState->flags, alloc);
		ia.AddMember("topology", pipe.pInputAssemblyState->topology, alloc);
		ia.AddMember("primitiveRestartEnable", pipe.pInputAssemblyState->primitiveRestartEnable, alloc);
		if (!pnext_chain_add_json_value(ia, *pipe.pInputAssemblyState, alloc, &dynamic_info))
			return false;
		p.AddMember("inputAssemblyState", ia, alloc);
	}

	if (global_info.color_blend_state)
	{
		Value cb(kObjectType);
		cb.AddMember("flags", pipe.pColorBlendState->flags, alloc);
		cb.AddMember("logicOp", pipe.pColorBlendState->logicOp, alloc);
		cb.AddMember("logicOpEnable", pipe.pColorBlendState->logicOpEnable, alloc);

		bool need_blend_constants = false;
		// Special EDS3 rule. If all of these are set, we must ignore the pAttachments pointer.
		const bool dynamic_attachments = dynamic_info.color_blend_enable &&
		                                 dynamic_info.color_write_mask &&
		                                 dynamic_info.color_blend_equation;

		if (dynamic_attachments)
		{
			need_blend_constants = true;
		}
		else
		{
			for (uint32_t i = 0; i < pipe.pColorBlendState->attachmentCount; i++)
			{
				auto &a = pipe.pColorBlendState->pAttachments[i];

				if (a.blendEnable &&
				    (a.dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				     a.dstAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
				     a.srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				     a.srcAlphaBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
				     a.dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				     a.dstColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR ||
				     a.srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_ALPHA ||
				     a.srcColorBlendFactor == VK_BLEND_FACTOR_CONSTANT_COLOR))
				{
					need_blend_constants = true;
				}
			}
		}

		const VkPipelineColorBlendAttachmentState blank_attachment = {};

		Value blend_constants(kArrayType);
		for (auto &c : pipe.pColorBlendState->blendConstants)
			blend_constants.PushBack(dynamic_info.blend_constants || !need_blend_constants ? 0.0f : c, alloc);
		cb.AddMember("blendConstants", blend_constants, alloc);
		Value attachments(kArrayType);
		for (uint32_t i = 0; i < pipe.pColorBlendState->attachmentCount; i++)
		{
			// We cannot completely decouple pAttachments from attachmentCount.
			// Just serialize dummy entries.

			auto &a = dynamic_attachments ? blank_attachment : pipe.pColorBlendState->pAttachments[i];
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
		if (!pnext_chain_add_json_value(cb, *pipe.pColorBlendState, alloc, &dynamic_info))
			return false;
		p.AddMember("colorBlendState", cb, alloc);
	}

	if (global_info.viewport_state)
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
		if (!pnext_chain_add_json_value(vp, *pipe.pViewportState, alloc, &dynamic_info))
			return false;
		p.AddMember("viewportState", vp, alloc);
	}

	if (global_info.depth_stencil_state)
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
		if (!pnext_chain_add_json_value(ds, *pipe.pDepthStencilState, alloc, &dynamic_info))
			return false;
		p.AddMember("depthStencilState", ds, alloc);
	}

	if (global_info.module_state)
	{
		Value stages;
		if (!json_value(pipe.pStages, pipe.stageCount, alloc, &stages))
			return false;
		p.AddMember("stages", stages, alloc);
	}

	if (!pnext_chain_add_json_value(p, pipe, alloc, &dynamic_info))
		return false;

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

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceRobustness2FeaturesEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("robustBufferAccess2", uint32_t(create_info.robustBufferAccess2), alloc);
	value.AddMember("robustImageAccess2", uint32_t(create_info.robustImageAccess2), alloc);
	value.AddMember("nullDescriptor", uint32_t(create_info.nullDescriptor), alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceImageRobustnessFeaturesEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("robustImageAccess", uint32_t(create_info.robustImageAccess), alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("fragmentShadingRateEnums", uint32_t(create_info.fragmentShadingRateEnums), alloc);
	value.AddMember("supersampleFragmentShadingRates", uint32_t(create_info.supersampleFragmentShadingRates), alloc);
	value.AddMember("noInvocationFragmentShadingRates", uint32_t(create_info.noInvocationFragmentShadingRates), alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceFragmentShadingRateFeaturesKHR &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("pipelineFragmentShadingRate", uint32_t(create_info.pipelineFragmentShadingRate), alloc);
	value.AddMember("primitiveFragmentShadingRate", uint32_t(create_info.primitiveFragmentShadingRate), alloc);
	value.AddMember("attachmentFragmentShadingRate", uint32_t(create_info.attachmentFragmentShadingRate), alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceMeshShaderFeaturesEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("taskShader", uint32_t(create_info.taskShader), alloc);
	value.AddMember("meshShader", uint32_t(create_info.meshShader), alloc);
	value.AddMember("multiviewMeshShader", uint32_t(create_info.multiviewMeshShader), alloc);
	value.AddMember("primitiveFragmentShadingRateMeshShader", uint32_t(create_info.primitiveFragmentShadingRateMeshShader), alloc);
	value.AddMember("meshShaderQueries", uint32_t(create_info.meshShaderQueries), alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceMeshShaderFeaturesNV &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("taskShader", uint32_t(create_info.taskShader), alloc);
	value.AddMember("meshShader", uint32_t(create_info.meshShader), alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceDescriptorBufferFeaturesEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("descriptorBuffer", uint32_t(create_info.descriptorBuffer), alloc);
	value.AddMember("descriptorBufferCaptureReplay", uint32_t(create_info.descriptorBufferCaptureReplay), alloc);
	value.AddMember("descriptorBufferImageLayoutIgnored", uint32_t(create_info.descriptorBufferImageLayoutIgnored), alloc);
	value.AddMember("descriptorBufferPushDescriptors", uint32_t(create_info.descriptorBufferPushDescriptors), alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceShaderObjectFeaturesEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("shaderObject", create_info.shaderObject, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("primitivesGeneratedQuery", create_info.primitivesGeneratedQuery, alloc);
	value.AddMember("primitivesGeneratedQueryWithRasterizerDiscard", create_info.primitivesGeneratedQueryWithRasterizerDiscard, alloc);
	value.AddMember("primitivesGeneratedQueryWithNonZeroStreams", create_info.primitivesGeneratedQueryWithNonZeroStreams, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool json_value(const VkPhysicalDeviceImage2DViewOf3DFeaturesEXT &create_info, Allocator &alloc, Value *out_value)
{
	Value value(kObjectType);
	value.AddMember("sType", create_info.sType, alloc);
	value.AddMember("image2DViewOf3D", create_info.image2DViewOf3D, alloc);
	value.AddMember("sampler2DViewOf3D", create_info.sampler2DViewOf3D, alloc);
	*out_value = value;
	return true;
}

template <typename Allocator>
static bool pnext_chain_pdf2_json_value(const void *pNext, Allocator &alloc, Value *out_value)
{
	Value nexts(kArrayType);

	while ((pNext = pnext_chain_pdf2_skip_ignored_entries(pNext)) != nullptr)
	{
		auto *pin = static_cast<const VkBaseInStructure *>(pNext);
		Value next;
		switch (pin->sType)
		{
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
			if (!json_value(*static_cast<const VkPhysicalDeviceRobustness2FeaturesEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
			if (!json_value(*static_cast<const VkPhysicalDeviceImageRobustnessFeaturesEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
			if (!json_value(*static_cast<const VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
			if (!json_value(*static_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
			if (!json_value(*static_cast<const VkPhysicalDeviceMeshShaderFeaturesEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
			if (!json_value(*static_cast<const VkPhysicalDeviceMeshShaderFeaturesNV *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
			if (!json_value(*static_cast<const VkPhysicalDeviceDescriptorBufferFeaturesEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
			if (!json_value(*static_cast<const VkPhysicalDeviceShaderObjectFeaturesEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
			if (!json_value(*static_cast<const VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT *>(pNext), alloc, &next))
				return false;
			break;

		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
			if (!json_value(*static_cast<const VkPhysicalDeviceImage2DViewOf3DFeaturesEXT *>(pNext), alloc, &next))
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

template <typename T, typename Allocator>
static bool pnext_chain_pdf2_add_json_value(Value &base, const T &t, Allocator &alloc)
{
	if (t.pNext)
	{
		Value nexts;
		if (!pnext_chain_pdf2_json_value(t.pNext, alloc, &nexts))
			return false;
		base.AddMember("pNext", nexts, alloc);
	}
	return true;
}

template <typename AllocType>
static bool serialize_physical_device_features_inline(Value &value, const VkPhysicalDeviceFeatures2 &features, AllocType &alloc)
{
	value.AddMember("robustBufferAccess", features.features.robustBufferAccess, alloc);
	if (!pnext_chain_pdf2_add_json_value(value, features, alloc))
		return false;
	return true;
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
		if (!serialize_physical_device_features_inline(pdf_info, *physical_device_features, alloc))
			return false;

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
	if (!application_feature_links)
		return false;

	PayloadWriteFlags payload_flags = 0;
	if (checksum)
		payload_flags |= PAYLOAD_WRITE_COMPUTE_CHECKSUM_BIT;

	Hash link_hash = get_application_link_hash(tag, hash);
	register_on_use(RESOURCE_APPLICATION_BLOB_LINK, link_hash);
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

bool StateRecorder::Impl::serialize_render_pass2(Hash hash, const VkRenderPassCreateInfo2 &create_info, vector<uint8_t> &blob) const
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
	doc.AddMember("renderPasses2", serialized_render_passes, alloc);

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

	StateRecorder::SubpassMeta meta = {};
	if (!get_subpass_meta_for_pipeline(create_info, api_object_cast<Hash>(create_info.renderPass), &meta))
		return false;

	if (!json_value(create_info, meta, alloc, &value))
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

bool StateRecorder::Impl::serialize_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR &create_info,
                                                        std::vector<uint8_t> &blob) const
{
	Document doc;
	doc.SetObject();
	auto &alloc = doc.GetAllocator();

	Value value;
	if (!json_value(create_info, alloc, &value))
		return false;

	Value serialized_raytracing_pipelines(kObjectType);
	serialized_raytracing_pipelines.AddMember(uint64_string(hash, alloc), value, alloc);

	doc.AddMember("version", FOSSILIZE_FORMAT_VERSION, alloc);
	doc.AddMember("raytracingPipelines", serialized_raytracing_pipelines, alloc);

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
		if (!serialize_physical_device_features_inline(pdf_info, *impl->physical_device_features, alloc))
			return false;

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
	Value render_passes2(kObjectType);
	for (auto &pass : impl->render_passes)
	{
		switch (static_cast<VkBaseInStructure *>(pass.second)->sType)
		{
		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO:
			if (!json_value(*static_cast<VkRenderPassCreateInfo *>(pass.second), alloc, &value))
				return false;
			render_passes.AddMember(uint64_string(pass.first, alloc), value, alloc);
			break;

		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2:
			if (!json_value(*static_cast<VkRenderPassCreateInfo2 *>(pass.second), alloc, &value))
				return false;
			render_passes2.AddMember(uint64_string(pass.first, alloc), value, alloc);
			break;

		default:
			return false;
		}
	}
	doc.AddMember("renderPasses", render_passes, alloc);
	doc.AddMember("renderPasses2", render_passes2, alloc);

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
		SubpassMeta subpass_meta = {};
		if (!impl->get_subpass_meta_for_pipeline(*pipe.second, api_object_cast<Hash>(pipe.second->renderPass),
		                                         &subpass_meta))
			return false;
		if (!json_value(*pipe.second, subpass_meta, alloc, &value))
			return false;
		graphics_pipelines.AddMember(uint64_string(pipe.first, alloc), value, alloc);
	}
	doc.AddMember("graphicsPipelines", graphics_pipelines, alloc);

	Value raytracing_pipelines(kObjectType);
	for (auto &pipe : impl->raytracing_pipelines)
	{
		if (!json_value(*pipe.second, alloc, &value))
			return false;
		raytracing_pipelines.AddMember(uint64_string(pipe.first, alloc), value, alloc);
	}
	doc.AddMember("raytracingPipelines", raytracing_pipelines, alloc);

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

void StateRecorder::set_module_identifier_database_interface(DatabaseInterface *iface)
{
	impl->module_identifier_database_iface = iface;
}

void StateRecorder::set_on_use_database_interface(DatabaseInterface *iface)
{
	impl->on_use_database_iface = iface;
}

void StateRecorder::init_recording_thread(DatabaseInterface *iface)
{
	impl->database_iface = iface;
	impl->record_data = {};
	impl->should_record_identifier_only =
			impl->module_identifier_database_iface && impl->on_use_database_iface;

	auto level = get_thread_log_level();
	auto cb = Internal::get_thread_log_callback();
	auto userdata = Internal::get_thread_log_userdata();
	impl->worker_thread = std::thread([=]() {
		set_thread_log_level(level);
		set_thread_log_callback(cb, userdata);
		impl->record_task(this, true);
	});
}

void StateRecorder::init_recording_synchronized(DatabaseInterface *iface)
{
	impl->database_iface = iface;
	impl->record_data = {};
	impl->should_record_identifier_only =
			impl->module_identifier_database_iface && impl->on_use_database_iface;
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

static bool pnext_chain_stype_is_hash_invariant(VkStructureType sType)
{
	switch (sType)
	{
	case VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO:
	case VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT:
		// This is purely used for copying purposes.
		// For hashing purposes, this must be ignored.
		return true;

	default:
		break;
	}

	return false;
}

static const void *pnext_chain_skip_ignored_entries(const void *pNext)
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);
		bool ignored;

		switch (base->sType)
		{
		case VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO_EXT:
		case VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT:
		case VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT:
		case VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT:
		case VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_FEEDBACK_CREATE_INFO_EXT:
		case VK_STRUCTURE_TYPE_RENDER_PASS_SUBPASS_FEEDBACK_CREATE_INFO_EXT:
		case VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR:
			// We need to ignore any pNext struct which represents output information from a pipeline object, or
			// irrelevant information which only tooling would care about.
			ignored = true;
			break;

		default:
			ignored = false;
			break;
		}

		if (ignored)
			pNext = base->pNext;
		else
			break;
	}

	return pNext;
}

static const void *pnext_chain_pdf2_skip_ignored_entries(const void *pNext)
{
	while (pNext)
	{
		auto *base = static_cast<const VkBaseInStructure *>(pNext);
		bool ignored;

		switch (base->sType)
		{
		// Robustness tends to affect shader compilation.
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT:
		// Affects compilation on NV.
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV:
		// Affects compilation on RADV.
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
		// Workaround: Affects compilation on NV and is causing some awkward cache corruption on some drivers.
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV:
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
		// RADV: Might want to turn off FMASK at some point based on this.
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
		// RADV uses these :(
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT:
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT:
			ignored = false;
			break;

		default:
			ignored = true;
			break;
		}

		if (ignored)
			pNext = base->pNext;
		else
			break;
	}

	return pNext;
}

#ifndef FOSSILIZE_API_DEFAULT_LOG_LEVEL
#define FOSSILIZE_API_DEFAULT_LOG_LEVEL LOG_DEFAULT
#endif

static thread_local LogLevel thread_log_level = FOSSILIZE_API_DEFAULT_LOG_LEVEL;
static thread_local LogCallback thread_log_callback;
static thread_local void *thread_log_userdata;
void set_thread_log_level(LogLevel level)
{
	thread_log_level = level;
}

void set_thread_log_callback(LogCallback cb, void *userdata)
{
	thread_log_callback = cb;
	thread_log_userdata = userdata;
}

namespace Internal
{
bool log_thread_callback(LogLevel level, const char *fmt, ...)
{
	if (!thread_log_callback)
		return false;

	va_list va;
	va_start(va, fmt);
	char buffer[8 * 1024];
	vsnprintf(buffer, sizeof(buffer), fmt, va);
	// Guard against ancient MSVC bugs.
	buffer[sizeof(buffer) - 1] = '\0';
	va_end(va);
	thread_log_callback(level, buffer, thread_log_userdata);
	return true;
}

LogCallback get_thread_log_callback()
{
	return thread_log_callback;
}

void *get_thread_log_userdata()
{
	return thread_log_userdata;
}
}

LogLevel get_thread_log_level()
{
	return thread_log_level;
}
}
