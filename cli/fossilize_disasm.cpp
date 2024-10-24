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

#include "volk.h"
#include "device.hpp"
#include "fossilize.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "file.hpp"
#include "fossilize_db.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <stdlib.h>
#include <string.h>
#include "fossilize_inttypes.h"

#include "spirv-tools/libspirv.hpp"
#include "spirv_cross_c.h"

template <typename T>
static inline T fake_handle(uint64_t v)
{
	return (T)v;
}

using namespace std;
using namespace Fossilize;

struct FilterReplayer : StateCreatorInterface
{
	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *, VkSampler *sampler) override
	{
		*sampler = fake_handle<VkSampler>(hash);
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *, VkDescriptorSetLayout *layout) override
	{
		*layout = fake_handle<VkDescriptorSetLayout>(hash);
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *, VkPipelineLayout *layout) override
	{
		*layout = fake_handle<VkPipelineLayout>(hash);
		return true;
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return true;
	}

	bool enqueue_create_render_pass2(Hash hash, const VkRenderPassCreateInfo2 *, VkRenderPass *render_pass) override
	{
		*render_pass = fake_handle<VkRenderPass>(hash);
		return true;
	}

	bool enqueue_create_shader_module(Hash, const VkShaderModuleCreateInfo *, VkShaderModule*) override { return false; }

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);

		// We are active if we either explicitly add the pipeline, or we explicitly add one of the module dependencies.
		bool active;
		if (filter_graphics.count(hash) != 0)
		{
			active = true;
		}
		else
		{
			active = false;
			for (uint32_t i = 0; !active && i < create_info->stageCount; i++)
				active = filter_modules.count((Hash)create_info->pStages[i].module) != 0;
		}

		if (!active)
			return true;

		// If the pipeline is to be emitted, promote all dependencies to be active as well.
		if (create_info->basePipelineHandle != VK_NULL_HANDLE)
			filter_graphics.insert((Hash)create_info->basePipelineHandle);
		for (uint32_t i = 0; i < create_info->stageCount; i++)
			filter_modules_promoted.insert((Hash)create_info->pStages[i].module);
		filter_graphics.insert(hash);

		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);

		// We are active if we either explicitly add the pipeline, or we explicitly add one of the module dependencies.
		bool active;
		if (filter_compute.count(hash) != 0)
			active = true;
		else
			active = filter_modules.count((Hash)create_info->stage.module) != 0;

		if (!active)
			return true;

		// If the pipeline is to be emitted, promote all dependencies to be active as well.
		if (create_info->basePipelineHandle != VK_NULL_HANDLE)
			filter_compute.insert((Hash)create_info->basePipelineHandle);
		filter_modules_promoted.insert((Hash)create_info->stage.module);
		filter_compute.insert(hash);

		return true;
	}

	bool enqueue_create_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info, VkPipeline *pipeline) override
	{
		*pipeline = fake_handle<VkPipeline>(hash);

		// We are active if we either explicitly add the pipeline, or we explicitly add one of the module dependencies.
		bool active;
		if (filter_raytracing.count(hash) != 0)
		{
			active = true;
		}
		else
		{
			active = false;
			for (uint32_t i = 0; !active && i < create_info->stageCount; i++)
				active = filter_modules.count((Hash)create_info->pStages[i].module) != 0;
			for (uint32_t i = 0; !active && create_info->pLibraryInfo && i < create_info->pLibraryInfo->libraryCount; i++)
				active = filter_raytracing.count((Hash)create_info->pLibraryInfo->pLibraries[i]) != 0;
		}

		if (!active)
			return true;

		// If the pipeline is to be emitted, promote all dependencies to be active as well.
		if (create_info->basePipelineHandle != VK_NULL_HANDLE)
			filter_raytracing.insert((Hash)create_info->basePipelineHandle);
		for (uint32_t i = 0; i < create_info->stageCount; i++)
			filter_modules_promoted.insert((Hash)create_info->pStages[i].module);
		if (create_info->pLibraryInfo)
			for (uint32_t i = 0; i < create_info->pLibraryInfo->libraryCount; i++)
				filter_raytracing.insert((Hash)create_info->pLibraryInfo->pLibraries[i]);
		filter_raytracing.insert(hash);

		return true;
	}

	void set_application_info(Hash, const VkApplicationInfo *info, const VkPhysicalDeviceFeatures2 *features2) override
	{
		app = info;
		pdf2 = features2;
	}

	unordered_set<Hash> filter_graphics;
	unordered_set<Hash> filter_compute;
	unordered_set<Hash> filter_raytracing;
	unordered_set<Hash> filter_modules;
	unordered_set<Hash> filter_modules_promoted;

	const VkApplicationInfo *app = nullptr;
	const VkPhysicalDeviceFeatures2 *pdf2 = nullptr;
};

struct DisasmReplayer : StateCreatorInterface
{
	DisasmReplayer(VulkanDevice *device_)
		: device(device_)
	{
		if (device)
		{
			VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
			vkCreatePipelineCache(device->get_device(), &info, nullptr, &pipeline_cache);
		}
	}

	~DisasmReplayer()
	{
		if (device)
		{
			if (pipeline_cache)
				vkDestroyPipelineCache(device->get_device(), pipeline_cache, nullptr);
			for (auto &sampler : samplers)
				if (sampler)
					vkDestroySampler(device->get_device(), sampler, nullptr);
			for (auto &layout : layouts)
				if (layout)
					vkDestroyDescriptorSetLayout(device->get_device(), layout, nullptr);
			for (auto &pipeline_layout : pipeline_layouts)
				if (pipeline_layout)
					vkDestroyPipelineLayout(device->get_device(), pipeline_layout, nullptr);
			for (auto &shader_module : shader_modules)
				if (shader_module)
					vkDestroyShaderModule(device->get_device(), shader_module, nullptr);
			for (auto &render_pass : render_passes)
				if (render_pass)
					vkDestroyRenderPass(device->get_device(), render_pass, nullptr);
			for (auto &pipeline : compute_pipelines)
				if (pipeline)
					vkDestroyPipeline(device->get_device(), pipeline, nullptr);
			for (auto &pipeline : graphics_pipelines)
				if (pipeline)
					vkDestroyPipeline(device->get_device(), pipeline, nullptr);
			for (auto &pipeline : raytracing_pipelines)
				if (pipeline)
					vkDestroyPipeline(device->get_device(), pipeline, nullptr);
		}
	}

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		if (device)
		{
			LOGI("Creating sampler %0" PRIX64 "\n", hash);
			if (device->create_sampler_with_ycbcr_remap(create_info, sampler) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*sampler = fake_handle<VkSampler>(hash);

		samplers.push_back(*sampler);
		sampler_infos.push_back(create_info);
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		if (device)
		{
			LOGI("Creating descriptor set layout 0%" PRIX64 "\n", hash);
			if (vkCreateDescriptorSetLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*layout = fake_handle<VkDescriptorSetLayout>(hash);

		layouts.push_back(*layout);
		set_layout_infos.push_back(create_info);
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		if (device)
		{
			LOGI("Creating pipeline layout 0%" PRIX64 "\n", hash);
			if (vkCreatePipelineLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*layout = fake_handle<VkPipelineLayout>(hash);

		pipeline_layouts.push_back(*layout);
		pipeline_layout_infos.push_back(create_info);
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		if (!shader_module_is_active(hash))
		{
			*module = fake_handle<VkShaderModule>(hash);
			return true;
		}

		if (device)
		{
			LOGI("Creating shader module 0%" PRIX64 "\n", hash);
			if (vkCreateShaderModule(device->get_device(), create_info, nullptr, module) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*module = fake_handle<VkShaderModule>(hash);

		module_to_index[*module] = shader_modules.size();
		shader_modules.push_back(*module);
		shader_module_infos.push_back(create_info);
		module_hashes.push_back(hash);
		return true;
	}

	bool enqueue_create_render_pass(Hash hash, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		if (device)
		{
			LOGI("Creating render pass %0" PRIX64 "\n", hash);
			if (vkCreateRenderPass(device->get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*render_pass = fake_handle<VkRenderPass>(hash);

		render_passes.push_back(*render_pass);
		render_pass_infos.push_back(create_info);
		return true;
	}

	bool enqueue_create_render_pass2(Hash hash, const VkRenderPassCreateInfo2 *create_info, VkRenderPass *render_pass) override
	{
		if (device)
		{
			LOGI("Creating render pass (version 2) %0" PRIX64 "\n", hash);
			if (vkCreateRenderPass2KHR(device->get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*render_pass = fake_handle<VkRenderPass>(hash);

		render_passes.push_back(*render_pass);
		render_pass_infos.push_back(create_info);
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if (!compute_pipeline_is_active(hash))
		{
			*pipeline = fake_handle<VkPipeline>(hash);
			return true;
		}

		if (device)
		{
			if (device->has_pipeline_stats())
				const_cast<VkComputePipelineCreateInfo *>(create_info)->flags |=
						VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;

			LOGI("Creating compute pipeline %0" PRIX64 "\n", hash);
			if (vkCreateComputePipelines(device->get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) !=
			    VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*pipeline = fake_handle<VkPipeline>(hash);

		compute_pipelines.push_back(*pipeline);
		compute_infos.push_back(create_info);
		compute_hashes.push_back(hash);
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if (!graphics_pipeline_is_active(hash))
		{
			*pipeline = fake_handle<VkPipeline>(hash);
			return true;
		}

		if (device)
		{
			if (device->has_pipeline_stats())
				const_cast<VkGraphicsPipelineCreateInfo *>(create_info)->flags |=
						VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;

			LOGI("Creating graphics pipeline %0" PRIX64 "\n", hash);
			if (vkCreateGraphicsPipelines(device->get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) !=
			    VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*pipeline = fake_handle<VkPipeline>(hash);

		graphics_pipelines.push_back(*pipeline);
		graphics_infos.push_back(create_info);
		graphics_hashes.push_back(hash);
		return true;
	}

	bool enqueue_create_raytracing_pipeline(Hash hash, const VkRayTracingPipelineCreateInfoKHR *create_info, VkPipeline *pipeline) override
	{
		if (!raytracing_pipeline_is_active(hash))
		{
			*pipeline = fake_handle<VkPipeline>(hash);
			return true;
		}

		if (device)
		{
			if (device->has_pipeline_stats())
			{
				const_cast<VkRayTracingPipelineCreateInfoKHR *>(create_info)->flags |=
						VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
			}

			LOGI("Creating raytracing pipeline %0" PRIX64 "\n", hash);
			if (vkCreateRayTracingPipelinesKHR(device->get_device(), VK_NULL_HANDLE, pipeline_cache,
			                                   1, create_info, nullptr, pipeline) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*pipeline = fake_handle<VkPipeline>(hash);

		raytracing_pipelines.push_back(*pipeline);
		raytracing_infos.push_back(create_info);
		raytracing_hashes.push_back(hash);
		return true;
	}

	bool shader_module_is_active(Hash hash) const
	{
		return !filter_is_active() || filter_modules.count(hash) != 0;
	}

	bool graphics_pipeline_is_active(Hash hash) const
	{
		return !filter_is_active() || filter_graphics.count(hash) != 0;
	}

	bool compute_pipeline_is_active(Hash hash) const
	{
		return !filter_is_active() || filter_compute.count(hash) != 0;
	}

	bool raytracing_pipeline_is_active(Hash hash) const
	{
		return !filter_is_active() || filter_raytracing.count(hash) != 0;
	}

	bool filter_is_active() const
	{
		return !filter_graphics.empty() || !filter_compute.empty() ||
		       !filter_raytracing.empty() || !filter_modules.empty();
	}

	VulkanDevice *device;

	vector<const VkSamplerCreateInfo *> sampler_infos;
	vector<const VkDescriptorSetLayoutCreateInfo *> set_layout_infos;
	vector<const VkPipelineLayoutCreateInfo *> pipeline_layout_infos;
	vector<const VkShaderModuleCreateInfo *> shader_module_infos;
	vector<const void *> render_pass_infos;
	vector<const VkGraphicsPipelineCreateInfo *> graphics_infos;
	vector<const VkComputePipelineCreateInfo *> compute_infos;
	vector<const VkRayTracingPipelineCreateInfoKHR *> raytracing_infos;

	vector<Hash> graphics_hashes;
	vector<Hash> compute_hashes;
	vector<Hash> raytracing_hashes;
	vector<Hash> module_hashes;
	unordered_map<VkShaderModule, unsigned> module_to_index;

	vector<VkSampler> samplers;
	vector<VkDescriptorSetLayout> layouts;
	vector<VkPipelineLayout> pipeline_layouts;
	vector<VkShaderModule> shader_modules;
	vector<VkRenderPass> render_passes;
	vector<VkPipeline> compute_pipelines;
	vector<VkPipeline> graphics_pipelines;
	vector<VkPipeline> raytracing_pipelines;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

	unordered_set<Hash> filter_graphics;
	unordered_set<Hash> filter_compute;
	unordered_set<Hash> filter_raytracing;
	unordered_set<Hash> filter_modules;
};

enum class DisasmMethod
{
	Asm,
	GLSL,
	ISA
};

static DisasmMethod method_from_string(const char *method)
{
	if (strcmp(method, "asm") == 0)
		return DisasmMethod::Asm;
	else if (strcmp(method, "glsl") == 0)
		return DisasmMethod::GLSL;
	else if (strcmp(method, "amd") == 0) // Compat
		return DisasmMethod::ISA;
	else if (strcmp(method, "isa") == 0) // Compat
		return DisasmMethod::ISA;
	else
	{
		LOGE("Invalid disasm method: %s\n", method);
		exit(EXIT_FAILURE);
	}
}

#if 0
static VkShaderStageFlagBits stage_from_string(const char *stage)
{
	if (strcmp(stage, "vert") == 0)
		return VK_SHADER_STAGE_VERTEX_BIT;
	else if (strcmp(stage, "frag") == 0)
		return VK_SHADER_STAGE_FRAGMENT_BIT;
	else if (strcmp(stage, "tesc") == 0)
		return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
	else if (strcmp(stage, "tese") == 0)
		return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	else if (strcmp(stage, "comp") == 0)
		return VK_SHADER_STAGE_COMPUTE_BIT;
	else
	{
		LOGE("Invalid stage: %s\n", stage);
		exit(EXIT_FAILURE);
	}
}
#endif

static string disassemble_spirv_asm(const VkShaderModuleCreateInfo *create_info)
{
	string str;
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
	const uint32_t options = spvtools::SpirvTools::kDefaultDisassembleOption &
							 ~SPV_BINARY_TO_TEXT_OPTION_NO_HEADER;
	if (!tools.Disassemble(create_info->pCode, create_info->codeSize / sizeof(uint32_t), &str, options))
		return "";
	return str;
}

static string disassemble_spirv_glsl(const VkShaderModuleCreateInfo *create_info, const char *entry, VkShaderStageFlagBits stage)
{
	spvc_context ctx;
	if (spvc_context_create(&ctx) != SPVC_SUCCESS)
		return "// Failed";

	spvc_parsed_ir ir;
	if (spvc_context_parse_spirv(ctx, create_info->pCode, create_info->codeSize / sizeof(uint32_t), &ir) != SPVC_SUCCESS)
	{
		spvc_context_destroy(ctx);
		return "// Failed";
	}

	spvc_compiler comp;
	if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS)
	{
		spvc_context_destroy(ctx);
		return "// Failed";
	}

	spvc_compiler_options opts;
	if (spvc_compiler_create_compiler_options(comp, &opts) != SPVC_SUCCESS)
	{
		spvc_context_destroy(ctx);
		return "// Failed";
	}

	spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_GLSL_VERSION, 460);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
	spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_TRUE);
	spvc_compiler_install_compiler_options(comp, opts);

	if (entry)
	{
		SpvExecutionModel model = SpvExecutionModelMax;
		switch (stage)
		{
		case VK_SHADER_STAGE_VERTEX_BIT:
			model = SpvExecutionModelVertex;
			break;

		case VK_SHADER_STAGE_FRAGMENT_BIT:
			model = SpvExecutionModelFragment;
			break;

		case VK_SHADER_STAGE_GEOMETRY_BIT:
			model = SpvExecutionModelGeometry;
			break;

		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
			model = SpvExecutionModelTessellationControl;
			break;

		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			model = SpvExecutionModelTessellationEvaluation;
			break;

		case VK_SHADER_STAGE_COMPUTE_BIT:
			model = SpvExecutionModelGLCompute;
			break;

		case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
			model = SpvExecutionModelRayGenerationKHR;
			break;

		case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
			model = SpvExecutionModelIntersectionKHR;
			break;

		case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
			model = SpvExecutionModelAnyHitKHR;
			break;

		case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
			model = SpvExecutionModelClosestHitKHR;
			break;

		case VK_SHADER_STAGE_MISS_BIT_KHR:
			model = SpvExecutionModelMissKHR;
			break;

		case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
			model = SpvExecutionModelCallableKHR;
			break;

		case VK_SHADER_STAGE_TASK_BIT_EXT:
			model = SpvExecutionModelTaskEXT;
			break;

		case VK_SHADER_STAGE_MESH_BIT_EXT:
			model = SpvExecutionModelMeshEXT;
			break;

		default:
			return "// Failed";
		}
		spvc_compiler_set_entry_point(comp, entry, model);
	}

	const char *output;
	if (spvc_compiler_compile(comp, &output) != SPVC_SUCCESS)
	{
		spvc_context_destroy(ctx);
		return "// Failed";
	}

	std::string ret(output);
	spvc_context_destroy(ctx);
	return ret;
}

static string disassemble_spirv_amd(const VulkanDevice &device, VkPipeline pipeline, VkShaderStageFlagBits stage)
{
	if (!device.has_amd_shader_info())
	{
		LOGE("Does not have vkGetShaderInfoAMD.\n");
		return "";
	}

	size_t size = 0;
	if (vkGetShaderInfoAMD(device.get_device(), pipeline, stage, VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, &size, nullptr) != VK_SUCCESS)
	{
		LOGE("Failed vkGetShaderInfoAMD.\n");
		return "";
	}

	vector<char> ret(size);

	if (vkGetShaderInfoAMD(device.get_device(), pipeline, stage, VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, &size, ret.data()) != VK_SUCCESS)
	{
		LOGE("Failed vkGetShaderInfoAMD.\n");
		return "";
	}

	return string(begin(ret), end(ret));
}

static string disassemble_spirv_isa(const VulkanDevice &device, VkPipeline pipeline, VkShaderStageFlagBits stage)
{
	if (device.has_pipeline_stats())
	{
		uint32_t count = 0;
		VkPipelineInfoKHR pipeline_info = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR };
		pipeline_info.pipeline = pipeline;

		if (vkGetPipelineExecutablePropertiesKHR(device.get_device(), &pipeline_info, &count, nullptr) != VK_SUCCESS)
			return "";

		vector<VkPipelineExecutablePropertiesKHR> executables(count);
		for (auto &exec : executables)
			exec.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;

		if (vkGetPipelineExecutablePropertiesKHR(device.get_device(), &pipeline_info, &count, executables.data()) != VK_SUCCESS)
			return "";

		uint32_t index = 0;
		for (; index < count; index++)
		{
			if ((executables[index].stages & stage) != 0)
				break;
		}

		if (index >= count)
			return "// Could not find stage in compiled pipeline.";

		VkPipelineExecutableInfoKHR executable = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
		executable.pipeline = pipeline;
		executable.executableIndex = index;

		vector<VkPipelineExecutableInternalRepresentationKHR> representations;
		if (vkGetPipelineExecutableInternalRepresentationsKHR(device.get_device(), &executable, &count, nullptr) == VK_SUCCESS)
		{
			representations.resize(count);
			for (auto &rep : representations)
				rep.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;

			bool success = vkGetPipelineExecutableInternalRepresentationsKHR(device.get_device(), &executable, &count, representations.data()) == VK_SUCCESS;
			if (success)
			{
				for (auto &rep : representations)
					rep.pData = malloc(rep.dataSize);

				success = vkGetPipelineExecutableInternalRepresentationsKHR(device.get_device(), &executable, &count, representations.data()) == VK_SUCCESS;
				if (!success)
				{
					for (auto &rep : representations)
						free(rep.pData);
				}
			}

			if (!success)
				representations.clear();
		}

		vector<VkPipelineExecutableStatisticKHR> statistics;
		if (vkGetPipelineExecutableStatisticsKHR(device.get_device(), &executable, &count, nullptr) == VK_SUCCESS)
		{
			statistics.resize(count);
			for (auto &stat : statistics)
				stat.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;

			const bool success = vkGetPipelineExecutableStatisticsKHR(device.get_device(), &executable, &count, statistics.data()) == VK_SUCCESS;
			if (!success)
				statistics.clear();
		}

		string result;
		for (auto &rep : representations)
		{
			if (rep.isText)
			{
				result += "Representation: ";
				result += rep.name;
				result += " (";
				result += rep.description;
				result += ")\n\n";
				result += static_cast<const char *>(rep.pData);
				result += "\n\n";
			}

			free(rep.pData);
		}

		for (auto &stat : statistics)
		{
			char hex_value[17];
			result += stat.name;
			result += " (";
			result += stat.description;
			result += "): ";

			switch (stat.format)
			{
			case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
				result += to_string(stat.value.b32);
				break;
			case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
				result += to_string(stat.value.i64);
				result += " / 0x";
				snprintf(hex_value, sizeof(hex_value), "%016llx", static_cast<unsigned long long>(stat.value.i64));
				result += hex_value;
				break;
			case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
				result += to_string(stat.value.u64);
				result += " / 0x";
				snprintf(hex_value, sizeof(hex_value), "%016llx", static_cast<unsigned long long>(stat.value.u64));
				result += hex_value;
				break;
			case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
				result += to_string(stat.value.f64);
				break;
			default:
				result += "[Unknown VkPipelineExecutableStatisticFormatKHR]";
			}
			result += "\n";
		}

		return result;
	}
	else if (device.has_amd_shader_info())
		return disassemble_spirv_amd(device, pipeline, stage);
	else
		return "";
}

static string disassemble_spirv(const VulkanDevice &device, VkPipeline pipeline,
                                DisasmMethod method, VkShaderStageFlagBits stage,
                                const VkShaderModuleCreateInfo *module_create_info, const char *entry_point)
{
	switch (method)
	{
	case DisasmMethod::Asm:
		return disassemble_spirv_asm(module_create_info);

	case DisasmMethod::GLSL:
		return disassemble_spirv_glsl(module_create_info, entry_point, stage);

	case DisasmMethod::ISA:
		return disassemble_spirv_isa(device, pipeline, stage);

	default:
		return "";
	}
}

static void print_help()
{
	LOGI("fossilize-disasm\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--output <path>]\n"
	     "\t[--target asm/glsl/isa]\n"
	     "\t[--module-only]\n"
	     "\t[--filter-graphics hash]\n"
	     "\t[--filter-compute hash]\n"
	     "\t[--filter-raytracing hash]\n"
	     "\t[--filter-module hash]\n"
		 "\t[--disasm-match <pattern>]\n"
	     "state.json\n");
}

static string uint64_string(uint64_t value)
{
	char str[17]; // 16 digits + null
	sprintf(str, "%016" PRIx64, value);
	return string(str);
}

static string stage_to_string(VkShaderStageFlagBits stage)
{
	switch (stage)
	{
	case VK_SHADER_STAGE_VERTEX_BIT:
		return "vert";
	case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
		return "tesc";
	case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
		return "tese";
	case VK_SHADER_STAGE_GEOMETRY_BIT:
		return "geom";
	case VK_SHADER_STAGE_FRAGMENT_BIT:
		return "frag";
	case VK_SHADER_STAGE_COMPUTE_BIT:
		return "comp";
	case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
		return "rgen";
	case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
		return "rint";
	case VK_SHADER_STAGE_MISS_BIT_KHR:
		return "rmiss";
	case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
		return "rahit";
	case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
		return "rchit";
	case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
		return "rcall";
	case VK_SHADER_STAGE_TASK_BIT_EXT:
		return "task";
	case VK_SHADER_STAGE_MESH_BIT_EXT:
		return "mesh";
	default:
		return "????";
	}
}

static void replay_hash(ResourceTag tag, Hash hash, DatabaseInterface &db_iface,
                        StateReplayer &replayer, StateCreatorInterface &iface,
                        vector<uint8_t> &state_json)
{
	size_t state_json_size;
	if (!db_iface.read_entry(tag, hash, &state_json_size, nullptr, 0))
	{
		LOGE("Failed to load blob from cache.\n");
		exit(EXIT_FAILURE);
	}

	state_json.resize(state_json_size);

	if (!db_iface.read_entry(tag, hash, &state_json_size, state_json.data(), 0))
	{
		LOGE("Failed to load blob from cache.\n");
		exit(EXIT_FAILURE);
	}

	if (!replayer.parse(iface, &db_iface, state_json.data(), state_json.size()))
		LOGE("Failed to parse blob (tag: %d, hash: 0x%016" PRIx64 ").\n", tag, hash);
}

static void replay_all_hashes(ResourceTag tag, DatabaseInterface &db_iface,
                              StateReplayer &replayer, StateCreatorInterface &iface,
                              vector<uint8_t> &state_json,
                              const unordered_set<Hash> *filter)
{
	if (filter)
	{
		for (auto hash : *filter)
			replay_hash(tag, hash, db_iface, replayer, iface, state_json);
	}
	else
	{
		size_t size;
		if (!db_iface.get_hash_list_for_resource_tag(tag, &size, nullptr))
			exit(EXIT_FAILURE);

		vector<Hash> hashes(size);
		if (!db_iface.get_hash_list_for_resource_tag(tag, &size, hashes.data()))
			exit(EXIT_FAILURE);

		for (auto hash : hashes)
			replay_hash(tag, hash, db_iface, replayer, iface, state_json);
	}
}

int main(int argc, char *argv[])
{
	string json_path;
	string output;
	string disasm_match;
	VulkanDevice::Options opts;
	DisasmMethod method = DisasmMethod::Asm;
	bool module_only = false;

	std::unordered_set<Hash> filter_graphics;
	std::unordered_set<Hash> filter_compute;
	std::unordered_set<Hash> filter_raytracing;
	std::unordered_set<Hash> filter_modules;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { json_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--output", [&](CLIParser &parser) { output = parser.next_string(); });
	cbs.add("--target", [&](CLIParser &parser) {
		method = method_from_string(parser.next_string());
	});
	cbs.add("--module-only", [&](CLIParser &) { module_only = true; });
	cbs.add("--filter-graphics", [&](CLIParser &parser) {
		filter_graphics.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--filter-compute", [&](CLIParser &parser) {
		filter_compute.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--filter-raytracing", [&](CLIParser &parser) {
		filter_raytracing.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--filter-module", [&](CLIParser &parser) {
		filter_modules.insert(strtoull(parser.next_string(), nullptr, 16));
	});
	cbs.add("--disasm-match", [&](CLIParser &parser) {
		disasm_match = parser.next_string();
	});
	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (json_path.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	auto resolver = unique_ptr<DatabaseInterface>(create_database(json_path.c_str(), DatabaseMode::ReadOnly));
	if (!resolver->prepare())
	{
		LOGE("Failed to open database: %s\n", json_path.c_str());
		return EXIT_FAILURE;
	}

	FilterReplayer filter_replayer;
	vector<uint8_t> state_json;

	VulkanDevice device;
	if (method == DisasmMethod::ISA)
	{
		StateReplayer application_info_replayer;
		replay_all_hashes(RESOURCE_APPLICATION_INFO, *resolver, application_info_replayer, filter_replayer, state_json, nullptr);

		opts.want_amd_shader_info = true;
		opts.want_pipeline_stats = true;
		opts.application_info = filter_replayer.app;
		opts.features = filter_replayer.pdf2;

		if (module_only)
		{
			LOGE("Cannot do module-only disassembly with ISA target.\n");
			return EXIT_FAILURE;
		}

		if (!device.init_device(opts))
		{
			LOGE("Failed to create device.\n");
			return EXIT_FAILURE;
		}

		if (!device.has_amd_shader_info() && !device.has_pipeline_stats())
		{
			LOGE("Neither AMD_shader_info or executable properties extension are available.\n");
			return EXIT_FAILURE;
		}
	}

	bool use_filter = !filter_graphics.empty() || !filter_compute.empty() ||
	                  !filter_raytracing.empty() || !filter_modules.empty();

	if (use_filter && !module_only)
	{
		StateReplayer state_replayer;

		state_replayer.set_resolve_derivative_pipeline_handles(false);
		state_replayer.set_resolve_shader_module_handles(false);

		filter_replayer.filter_graphics = std::move(filter_graphics);
		filter_replayer.filter_compute = std::move(filter_compute);
		filter_replayer.filter_raytracing = std::move(filter_raytracing);
		filter_replayer.filter_modules = std::move(filter_modules);

		// Don't know which pipelines depend on a module in question, so need to replay all pipelines and promote on demand.
		bool replay_all = !filter_replayer.filter_modules.empty();

		static const ResourceTag early_playback_order[] = {
			RESOURCE_DESCRIPTOR_SET_LAYOUT, // Implicitly pulls in samplers.
			RESOURCE_PIPELINE_LAYOUT,
			RESOURCE_RENDER_PASS,
		};

		for (auto tag : early_playback_order)
			replay_all_hashes(tag, *resolver, state_replayer, filter_replayer, state_json, nullptr);

		if (replay_all)
		{
			static const ResourceTag playback_order[] = {
				RESOURCE_GRAPHICS_PIPELINE,
				RESOURCE_COMPUTE_PIPELINE,
				RESOURCE_RAYTRACING_PIPELINE,
			};

			for (auto tag : playback_order)
				replay_all_hashes(tag, *resolver, state_replayer, filter_replayer, state_json, nullptr);
		}
		else
		{
			// Need copies since we might modify the hashmap inside the replay callback.
			auto replays_graphics = filter_replayer.filter_graphics;
			auto replays_compute = filter_replayer.filter_compute;
			auto replays_raytracing = filter_replayer.filter_raytracing;
			replay_all_hashes(RESOURCE_GRAPHICS_PIPELINE, *resolver, state_replayer,
			                  filter_replayer, state_json, &replays_graphics);
			replay_all_hashes(RESOURCE_COMPUTE_PIPELINE, *resolver, state_replayer,
			                  filter_replayer, state_json, &replays_compute);
			replay_all_hashes(RESOURCE_RAYTRACING_PIPELINE, *resolver, state_replayer,
			                  filter_replayer, state_json, &replays_raytracing);
		}

		filter_graphics = std::move(filter_replayer.filter_graphics);
		filter_compute = std::move(filter_replayer.filter_compute);
		filter_raytracing = std::move(filter_replayer.filter_raytracing);
		filter_modules = std::move(filter_replayer.filter_modules);
		for (auto hash : filter_replayer.filter_modules_promoted)
			filter_modules.insert(hash);
	}

	StateReplayer state_replayer;
	DisasmReplayer replayer(device.get_device() ? &device : nullptr);
	replayer.filter_graphics = std::move(filter_graphics);
	replayer.filter_compute = std::move(filter_compute);
	replayer.filter_raytracing = std::move(filter_raytracing);
	replayer.filter_modules = std::move(filter_modules);

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO, // This will create the device, etc.
		RESOURCE_SHADER_MODULE, // Kick off shader modules first since it can be done in a thread while we deal with trivial objects.
		RESOURCE_SAMPLER, // Trivial, run in main thread.
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Trivial, run in main thread
		RESOURCE_PIPELINE_LAYOUT, // Trivial, run in main thread
		RESOURCE_RENDER_PASS, // Trivial, run in main thread
		RESOURCE_GRAPHICS_PIPELINE, // Multi-threaded
		RESOURCE_COMPUTE_PIPELINE, // Multi-threaded
		RESOURCE_RAYTRACING_PIPELINE, // Multi-threaded
	};

	static const char *tag_names[] = {
		"AppInfo",
		"Sampler",
		"Descriptor Set Layout",
		"Pipeline Layout",
		"Shader Module",
		"Render Pass",
		"Graphics Pipeline",
		"Compute Pipeline",
		"Info Links",
		"Raytracing Pipeline",
	};

	for (auto &tag : playback_order)
	{
		if (module_only && tag != RESOURCE_SHADER_MODULE)
			continue;

		LOGI("Replaying tag: %s\n", tag_names[tag]);
		const unordered_set<Hash> *filter = nullptr;

		if (use_filter)
		{
			switch (tag)
			{
			case RESOURCE_SHADER_MODULE: filter = &replayer.filter_modules; break;
			case RESOURCE_GRAPHICS_PIPELINE: filter = &replayer.filter_graphics; break;
			case RESOURCE_COMPUTE_PIPELINE: filter = &replayer.filter_compute; break;
			case RESOURCE_RAYTRACING_PIPELINE: filter = &replayer.filter_raytracing; break;
			default: break;
			}
		}

		replay_all_hashes(tag, *resolver, state_replayer, replayer, state_json, filter);
		LOGI("Replayed tag: %s\n", tag_names[tag]);
	}

	unordered_set<VkShaderModule> unique_shader_modules;

	if (module_only)
	{
		string disassembled;
		size_t module_count = replayer.shader_module_infos.size();
		for (size_t i = 0; i < module_count; i++)
		{
			auto *module_info = replayer.shader_module_infos[i];
			disassembled = disassemble_spirv(device, VK_NULL_HANDLE, method, VK_SHADER_STAGE_ALL,
			                                 module_info, nullptr);

			auto module_hash = replayer.module_hashes[i];
			string path = output + "/" + uint64_string(module_hash);

			if (disasm_match.empty())
				LOGI("Dumping disassembly to: %s\n", path.c_str());
			else if (disassembled.find(disasm_match) != std::string::npos)
				LOGI("Found matching string, dumping disassembly to: %s\n", path.c_str());

			if (!write_string_to_file(path.c_str(), disassembled.c_str()))
			{
				LOGE("Failed to write disassembly to file: %s\n", output.c_str());
				return EXIT_FAILURE;
			}
		}
	}
	else
	{
		string disassembled;
		size_t graphics_pipeline_count = replayer.graphics_infos.size();
		for (size_t i = 0; i < graphics_pipeline_count; i++)
		{
			auto *info = replayer.graphics_infos[i];
			for (uint32_t j = 0; j < info->stageCount; j++)
			{
				VkShaderModule module = info->pStages[j].module;
				unique_shader_modules.insert(module);
				unsigned index = replayer.module_to_index[module];
				auto *module_info = replayer.shader_module_infos[index];
				disassembled = disassemble_spirv(device, replayer.graphics_pipelines[i], method, info->pStages[j].stage,
				                                 module_info, info->pStages[j].pName);

				Hash module_hash = replayer.module_hashes[index];

				string path = output + "/" + uint64_string(module_hash) + "." +
				              info->pStages[j].pName + "." +
				              uint64_string(replayer.graphics_hashes[i]) +
				              "." + stage_to_string(info->pStages[j].stage);

				if (disasm_match.empty())
					LOGI("Dumping disassembly to: %s\n", path.c_str());
				else if (disassembled.find(disasm_match) != std::string::npos)
					LOGI("Found matching string, dumping disassembly to: %s\n", path.c_str());

				if (!write_string_to_file(path.c_str(), disassembled.c_str()))
				{
					LOGE("Failed to write disassembly to file: %s\n", output.c_str());
					return EXIT_FAILURE;
				}
			}
		}

		size_t compute_pipeline_count = replayer.compute_infos.size();
		for (size_t i = 0; i < compute_pipeline_count; i++)
		{
			auto *info = replayer.compute_infos[i];
			VkShaderModule module = info->stage.module;
			unique_shader_modules.insert(module);

			unsigned index = replayer.module_to_index[module];
			auto *module_info = replayer.shader_module_infos[index];
			disassembled = disassemble_spirv(device, replayer.compute_pipelines[i], method, info->stage.stage,
			                                 module_info, info->stage.pName);

			Hash module_hash = replayer.module_hashes[index];

			string path = output + "/" + uint64_string(module_hash) + "." +
			              info->stage.pName + "." +
			              uint64_string(replayer.compute_hashes[i]) +
			              "." + stage_to_string(info->stage.stage);

			if (disasm_match.empty())
				LOGI("Dumping disassembly to: %s\n", path.c_str());
			else if (disassembled.find(disasm_match) != std::string::npos)
				LOGI("Found matching string, dumping disassembly to: %s\n", path.c_str());

			if (!write_string_to_file(path.c_str(), disassembled.c_str()))
			{
				LOGE("Failed to write disassembly to file: %s\n", output.c_str());
				return EXIT_FAILURE;
			}
		}

		size_t raytracing_pipeline_count = replayer.raytracing_infos.size();
		for (size_t i = 0; i < raytracing_pipeline_count; i++)
		{
			auto *info = replayer.raytracing_infos[i];
			for (uint32_t j = 0; j < info->stageCount; j++)
			{
				VkShaderModule module = info->pStages[j].module;
				unique_shader_modules.insert(module);
				unsigned index = replayer.module_to_index[module];
				auto *module_info = replayer.shader_module_infos[index];
				disassembled = disassemble_spirv(device, replayer.raytracing_pipelines[i], method, info->pStages[j].stage,
												 module_info, info->pStages[j].pName);

				Hash module_hash = replayer.module_hashes[index];

				string path = output + "/" + uint64_string(module_hash) + "." +
						info->pStages[j].pName + "." +
						uint64_string(replayer.raytracing_hashes[i]) +
						"." + stage_to_string(info->pStages[j].stage);

				if (disasm_match.empty())
					LOGI("Dumping disassembly to: %s\n", path.c_str());
				else if (disassembled.find(disasm_match) != std::string::npos)
					LOGI("Found matching string, dumping disassembly to: %s\n", path.c_str());

				if (!write_string_to_file(path.c_str(), disassembled.c_str()))
				{
					LOGE("Failed to write disassembly to file: %s\n", output.c_str());
					return EXIT_FAILURE;
				}
			}
		}

		LOGI("Shader modules used: %u, shader modules in database: %u\n",
		     unsigned(unique_shader_modules.size()), unsigned(replayer.shader_module_infos.size()));
	}
	return EXIT_SUCCESS;
}
