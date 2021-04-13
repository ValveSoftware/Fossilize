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
struct DisasmReplayer : StateCreatorInterface
{
	DisasmReplayer(const VulkanDevice *device_)
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
		}
	}

	bool enqueue_create_sampler(Hash hash, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		if (device)
		{
			LOGI("Creating sampler %0" PRIX64 "\n", hash);
			if (vkCreateSampler(device->get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
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
		if (device)
		{
			if (device->has_pipeline_stats())
				const_cast<VkComputePipelineCreateInfo *>(create_info)->flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;

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
		if (device)
		{
			if (device->has_pipeline_stats())
				const_cast<VkGraphicsPipelineCreateInfo *>(create_info)->flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;

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

	const VulkanDevice *device;

	vector<const VkSamplerCreateInfo *> sampler_infos;
	vector<const VkDescriptorSetLayoutCreateInfo *> set_layout_infos;
	vector<const VkPipelineLayoutCreateInfo *> pipeline_layout_infos;
	vector<const VkShaderModuleCreateInfo *> shader_module_infos;
	vector<const void *> render_pass_infos;
	vector<const VkGraphicsPipelineCreateInfo *> graphics_infos;
	vector<const VkComputePipelineCreateInfo *> compute_infos;

	vector<Hash> graphics_hashes;
	vector<Hash> compute_hashes;
	vector<Hash> module_hashes;
	unordered_map<VkShaderModule, unsigned> module_to_index;

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
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	if (!tools.Disassemble(create_info->pCode, create_info->codeSize / sizeof(uint32_t), &str))
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
		switch (stage)
		{
		case VK_SHADER_STAGE_VERTEX_BIT:
			spvc_compiler_set_entry_point(comp, entry, SpvExecutionModelVertex);
			break;

		case VK_SHADER_STAGE_FRAGMENT_BIT:
			spvc_compiler_set_entry_point(comp, entry, SpvExecutionModelFragment);
			break;

		case VK_SHADER_STAGE_GEOMETRY_BIT:
			spvc_compiler_set_entry_point(comp, entry, SpvExecutionModelGeometry);
			break;

		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
			spvc_compiler_set_entry_point(comp, entry, SpvExecutionModelTessellationControl);
			break;

		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
			spvc_compiler_set_entry_point(comp, entry, SpvExecutionModelTessellationEvaluation);
			break;

		case VK_SHADER_STAGE_COMPUTE_BIT:
			spvc_compiler_set_entry_point(comp, entry, SpvExecutionModelGLCompute);
			break;

		default:
			return "// Failed";
		}
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

		if (vkGetPipelineExecutableInternalRepresentationsKHR(device.get_device(), &executable, &count, nullptr) != VK_SUCCESS)
			return "";

		vector<VkPipelineExecutableInternalRepresentationKHR> representations(count);
		for (auto &rep : representations)
			rep.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;

		if (vkGetPipelineExecutableInternalRepresentationsKHR(device.get_device(), &executable, &count, representations.data()) != VK_SUCCESS)
			return "";

		for (auto &rep : representations)
			rep.pData = malloc(rep.dataSize);

		if (vkGetPipelineExecutableInternalRepresentationsKHR(device.get_device(), &executable, &count, representations.data()) != VK_SUCCESS)
			return "";

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
	default:
		return "????";
	}
}

int main(int argc, char *argv[])
{
	string json_path;
	string output;
	VulkanDevice::Options opts;
	DisasmMethod method = DisasmMethod::Asm;
	bool module_only = false;

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

	VulkanDevice device;
	if (method == DisasmMethod::ISA)
	{
		opts.want_amd_shader_info = true;
		opts.want_pipeline_stats = true;

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

	DisasmReplayer replayer(device.get_device() ? &device : nullptr);
	StateReplayer state_replayer;
	auto resolver = unique_ptr<DatabaseInterface>(create_database(json_path.c_str(), DatabaseMode::ReadOnly));
	if (!resolver->prepare())
	{
		LOGE("Failed to open database: %s\n", json_path.c_str());
		return EXIT_FAILURE;
	}

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO, // This will create the device, etc.
		RESOURCE_SHADER_MODULE, // Kick off shader modules first since it can be done in a thread while we deal with trivial objects.
		RESOURCE_SAMPLER, // Trivial, run in main thread.
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Trivial, run in main thread
		RESOURCE_PIPELINE_LAYOUT, // Trivial, run in main thread
		RESOURCE_RENDER_PASS, // Trivial, run in main thread
		RESOURCE_GRAPHICS_PIPELINE, // Multi-threaded
		RESOURCE_COMPUTE_PIPELINE, // Multi-threaded
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
	};

	vector<uint8_t> state_json;

	for (auto &tag : playback_order)
	{
		if (module_only && tag != RESOURCE_SHADER_MODULE)
			continue;

		LOGI("Replaying tag: %s\n", tag_names[tag]);
		size_t hash_count = 0;
		if (!resolver->get_hash_list_for_resource_tag(tag, &hash_count, nullptr))
		{
			LOGE("Failed to get hashes.\n");
			return EXIT_FAILURE;
		}

		vector<Hash> hashes(hash_count);

		if (!resolver->get_hash_list_for_resource_tag(tag, &hash_count, hashes.data()))
		{
			LOGE("Failed to get shader module hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto hash : hashes)
		{
			size_t state_json_size;
			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			state_json.resize(state_json_size);

			if (!resolver->read_entry(tag, hash, &state_json_size, state_json.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			if (!state_replayer.parse(replayer, resolver.get(), state_json.data(), state_json.size()))
				LOGE("Failed to parse blob (tag: %d, hash: 0x%016" PRIx64 ").\n", tag, hash);
		}
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

			LOGI("Dumping disassembly to: %s\n", path.c_str());
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

				LOGI("Dumping disassembly to: %s\n", path.c_str());
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

			LOGI("Dumping disassembly to: %s\n", path.c_str());
			if (!write_string_to_file(path.c_str(), disassembled.c_str()))
			{
				LOGE("Failed to write disassembly to file: %s\n", output.c_str());
				return EXIT_FAILURE;
			}
		}

		LOGI("Shader modules used: %u, shader modules in database: %u\n",
		     unsigned(unique_shader_modules.size()), unsigned(replayer.shader_module_infos.size()));
	}
	return EXIT_SUCCESS;
}
