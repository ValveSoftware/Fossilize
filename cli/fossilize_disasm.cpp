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

#include <string>
#include <unordered_map>
#include <stdlib.h>

#include "spirv-tools/libspirv.hpp"
#include "spirv_glsl.hpp"

template <typename T>
static inline T fake_handle(uint64_t v)
{
	return (T)v;
}

using namespace std;
using namespace Fossilize;
struct DisasmReplayer : StateCreatorInterface
{
	DisasmReplayer(const VulkanDevice *device)
		: device(device)
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
		}
	}

	bool set_num_samplers(unsigned count) override
	{
		samplers.resize(count);
		sampler_infos.resize(count);
		return true;
	}

	bool set_num_descriptor_set_layouts(unsigned count) override
	{
		layouts.resize(count);
		set_layout_infos.resize(count);
		return true;
	}

	bool set_num_pipeline_layouts(unsigned count) override
	{
		pipeline_layouts.resize(count);
		pipeline_layout_infos.resize(count);
		return true;
	}

	bool set_num_shader_modules(unsigned count) override
	{
		shader_modules.resize(count);
		shader_module_infos.resize(count);
		return true;
	}

	bool set_num_render_passes(unsigned count) override
	{
		render_passes.resize(count);
		render_pass_infos.resize(count);
		return true;
	}

	bool set_num_compute_pipelines(unsigned count) override
	{
		compute_pipelines.resize(count);
		compute_infos.resize(count);
		return true;
	}

	bool set_num_graphics_pipelines(unsigned count) override
	{
		graphics_pipelines.resize(count);
		graphics_infos.resize(count);
		return true;
	}

	bool enqueue_create_sampler(Hash, unsigned index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		if (device)
		{
			LOGI("Creating sampler #%u\n", index);
			if (vkCreateSampler(device->get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*sampler = fake_handle<VkSampler>(index + 1);

		samplers[index] = *sampler;
		sampler_to_index[*sampler] = index;
		sampler_infos[index] = create_info;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash, unsigned index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		if (device)
		{
			LOGI("Creating descriptor set layout #%u\n", index);
			if (vkCreateDescriptorSetLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*layout =  fake_handle<VkDescriptorSetLayout>(index + 1);

		layouts[index] = *layout;
		set_to_index[*layout] = index;
		set_layout_infos[index] = create_info;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash, unsigned index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		if (device)
		{
			LOGI("Creating pipeline layout #%u\n", index);
			if (vkCreatePipelineLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*layout = fake_handle<VkPipelineLayout>(index + 1);

		pipeline_layouts[index] = *layout;
		layout_to_index[*layout] = index;
		pipeline_layout_infos[index] = create_info;
		return true;
	}

	bool enqueue_create_shader_module(Hash, unsigned index, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		if (device)
		{
			LOGI("Creating shader module #%u\n", index);
			if (vkCreateShaderModule(device->get_device(), create_info, nullptr, module) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*module = fake_handle<VkShaderModule>(index + 1);

		shader_modules[index] = *module;
		module_to_index[*module] = index;
		shader_module_infos[index] = create_info;
		return true;
	}

	bool enqueue_create_render_pass(Hash, unsigned index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		if (device)
		{
			LOGI("Creating render pass #%u\n", index);
			if (vkCreateRenderPass(device->get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*render_pass = fake_handle<VkRenderPass>(index + 1);

		render_passes[index] = *render_pass;
		render_pass_to_index[*render_pass] = index;
		render_pass_infos[index] = create_info;
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash, unsigned index, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if (device)
		{
			LOGI("Creating compute pipeline #%u\n", index);
			if (vkCreateComputePipelines(device->get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) !=
			    VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*pipeline = fake_handle<VkPipeline>(index + 1);

		compute_pipelines[index] = *pipeline;
		compute_to_index[*pipeline] = index;
		compute_infos[index] = create_info;
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash, unsigned index, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if (device)
		{
			LOGI("Creating graphics pipeline #%u\n", index);
			if (vkCreateGraphicsPipelines(device->get_device(), pipeline_cache, 1, create_info, nullptr, pipeline) !=
			    VK_SUCCESS)
			{
				LOGE(" ... Failed!\n");
				return false;
			}
			LOGI(" ... Succeeded!\n");
		}
		else
			*pipeline = fake_handle<VkPipeline>(index + 1);

		graphics_pipelines[index] = *pipeline;
		graphics_to_index[*pipeline] = index;
		graphics_infos[index] = create_info;
		return true;
	}

	const VulkanDevice *device;

	vector<const VkSamplerCreateInfo *> sampler_infos;
	vector<const VkDescriptorSetLayoutCreateInfo *> set_layout_infos;
	vector<const VkPipelineLayoutCreateInfo *> pipeline_layout_infos;
	vector<const VkShaderModuleCreateInfo *> shader_module_infos;
	vector<const VkRenderPassCreateInfo *> render_pass_infos;
	vector<const VkGraphicsPipelineCreateInfo *> graphics_infos;
	vector<const VkComputePipelineCreateInfo *> compute_infos;

	unordered_map<VkSampler, unsigned> sampler_to_index;
	unordered_map<VkDescriptorSetLayout, unsigned> set_to_index;
	unordered_map<VkPipelineLayout, unsigned> layout_to_index;
	unordered_map<VkShaderModule, unsigned> module_to_index;
	unordered_map<VkRenderPass, unsigned> render_pass_to_index;
	unordered_map<VkPipeline, unsigned> compute_to_index;
	unordered_map<VkPipeline, unsigned> graphics_to_index;

	vector<VkSampler> samplers;
	vector<VkDescriptorSetLayout> layouts;
	vector<VkPipelineLayout> pipeline_layouts;
	vector<VkShaderModule> shader_modules;
	vector<VkRenderPass> render_passes;
	vector<VkPipeline> compute_pipelines;
	vector<VkPipeline> graphics_pipelines;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
};

enum class DisasmMethod
{
	Asm,
	GLSL,
	AMD
};

static DisasmMethod method_from_string(const char *method)
{
	if (strcmp(method, "asm") == 0)
		return DisasmMethod::Asm;
	else if (strcmp(method, "glsl") == 0)
		return DisasmMethod::GLSL;
	else if (strcmp(method, "amd") == 0)
		return DisasmMethod::AMD;
	else
	{
		LOGE("Invalid disasm method: %s\n", method);
		exit(EXIT_FAILURE);
	}
}

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

static string disassemble_spirv_asm(const VkShaderModuleCreateInfo *create_info)
{
	string str;
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_0);
	if (!tools.Disassemble(create_info->pCode, create_info->codeSize / sizeof(uint32_t), &str))
		return "";
	return str;
}

static string disassemble_spirv_glsl(const VkShaderModuleCreateInfo *create_info, const char *entry, VkShaderStageFlagBits stage)
{
	spirv_cross::CompilerGLSL comp(create_info->pCode, create_info->codeSize / sizeof(uint32_t));
	spirv_cross::CompilerGLSL::Options opts;
	opts.version = 450;
	opts.es = false;
	opts.vulkan_semantics = true;
	comp.set_common_options(opts);
	comp.build_dummy_sampler_for_combined_images();

	switch (stage)
	{
	case VK_SHADER_STAGE_VERTEX_BIT:
		comp.set_entry_point(entry, spv::ExecutionModelVertex);
		break;

	case VK_SHADER_STAGE_FRAGMENT_BIT:
		comp.set_entry_point(entry, spv::ExecutionModelFragment);
		break;

	case VK_SHADER_STAGE_GEOMETRY_BIT:
		comp.set_entry_point(entry, spv::ExecutionModelGeometry);
		break;

	case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
		comp.set_entry_point(entry, spv::ExecutionModelTessellationControl);
		break;

	case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
		comp.set_entry_point(entry, spv::ExecutionModelTessellationEvaluation);
		break;

	case VK_SHADER_STAGE_COMPUTE_BIT:
		comp.set_entry_point(entry, spv::ExecutionModelGLCompute);
		break;

	default:
		return "";
	}

	return comp.compile();
}

static string disassemble_spirv_amd(const VulkanDevice &device, VkPipeline pipeline, VkShaderStageFlagBits stage)
{
	if (!vkGetShaderInfoAMD)
	{
		LOGE("Does not have vkGetShaderInfoAMD.\n");
		// FIXME: Check extension properly, lazy for now :)
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

	case DisasmMethod::AMD:
		return disassemble_spirv_amd(device, pipeline, stage);

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
	     "\t[--graphics-pipeline <index>]\n"
	     "\t[--compute-pipeline <index>]\n"
	     "\t[--stage vert/frag/comp/geom/tesc/tese]\n"
	     "\t[--output <path>]\n"
	     "\t[--target asm/glsl/amd]\n"
	     "state.json\n");
}

int main(int argc, char *argv[])
{
	string json_path;
	string output;
	VulkanDevice::Options opts;
	DisasmMethod method = DisasmMethod::Asm;
	VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL;

	int graphics_index = -1;
	int compute_index = -1;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { json_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--graphics-pipeline", [&](CLIParser &parser) { graphics_index = parser.next_uint(); });
	cbs.add("--compute-pipeline", [&](CLIParser &parser) { compute_index = parser.next_uint(); });
	cbs.add("--stage", [&](CLIParser &parser) { stage = stage_from_string(parser.next_string()); });
	cbs.add("--output", [&](CLIParser &parser) { output = parser.next_string(); });
	cbs.add("--target", [&](CLIParser &parser) {
		method = method_from_string(parser.next_string());
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

	if (compute_index >= 0)
		stage = VK_SHADER_STAGE_COMPUTE_BIT;

	if (stage == VK_SHADER_STAGE_ALL)
	{
		LOGE("Must choose --stage!\n");
		return EXIT_FAILURE;
	}

	if (((graphics_index >= 0 && compute_index >= 0)) || ((graphics_index < 0) && (compute_index < 0)))
	{
		LOGE("Use either --disasm-graphics-pipeline or --disasm-compute-pipeline.\n");
		return EXIT_FAILURE;
	}

	VulkanDevice device;
	if (method == DisasmMethod::AMD)
	{
		if (!device.init_device(opts))
		{
			LOGE("Failed to create device.\n");
			return EXIT_FAILURE;
		}
	}

	DisasmReplayer replayer(device.get_device() ? &device : nullptr);
	StateReplayer state_replayer;

	try
	{
		auto state_json = load_json_from_file(json_path.c_str());
		if (state_json.empty())
		{
			LOGE("Failed to load state JSON from disk.\n");
			return EXIT_FAILURE;
		}
		state_replayer.parse(replayer, state_json.data(), state_json.size());
	}
	catch (const exception &e)
	{
		LOGE("Caught exception: %s\n", e.what());
		return EXIT_FAILURE;
	}

	string disassembled;
	if (compute_index >= 0)
	{
		if (size_t(compute_index) > replayer.compute_infos.size())
		{
			LOGE("Used compute index: %d, but there's only %u compute pipelines in the dump.\n",
			     compute_index, unsigned(replayer.compute_infos.size()));
			return EXIT_FAILURE;
		}

		auto *info = replayer.compute_infos[compute_index];
		auto *module_info = replayer.shader_module_infos[replayer.module_to_index[info->stage.module]];
		disassembled = disassemble_spirv(device, replayer.compute_pipelines[compute_index], method, stage, module_info, info->stage.pName);
	}
	else if (graphics_index >= 0)
	{
		if (size_t(graphics_index) > replayer.graphics_infos.size())
		{
			LOGE("Used graphics index: %d, but there's only %u graphics pipelines in the dump.\n",
			     graphics_index, unsigned(replayer.graphics_infos.size()));
			return EXIT_FAILURE;
		}

		auto *info = replayer.graphics_infos[graphics_index];

		VkShaderModule module = VK_NULL_HANDLE;
		const char *entry = nullptr;

		for (uint32_t i = 0; i < info->stageCount; i++)
		{
			if (info->pStages[i].stage == static_cast<uint32_t>(stage))
			{
				module = info->pStages[i].module;
				entry = info->pStages[i].pName;
			}
		}

		if (!module)
		{
			LOGE("Cannot find module for --stage ...\n");
			return EXIT_FAILURE;
		}

		auto *module_info = replayer.shader_module_infos[replayer.module_to_index[module]];
		disassembled = disassemble_spirv(device, replayer.graphics_pipelines[graphics_index], method, stage, module_info, entry);
	}

	if (output.empty())
	{
		printf("%s\n", disassembled.c_str());
	}
	else
	{
		if (!write_string_to_file(output.c_str(), disassembled.c_str()))
		{
			LOGE("Failed to write disassembly to file: %s\n", output.c_str());
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}
