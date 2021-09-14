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

#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "layer/utils.hpp"
#include <chrono>
#include <memory>
#include <random>
#include <vector>
#include "fossilize_inttypes.h"

using namespace Fossilize;

static void bench_recorder(const char *path, bool compressed, bool checksum)
{
	remove(path);
	auto iface = std::unique_ptr<DatabaseInterface>(create_database(path, DatabaseMode::OverWrite));
	StateRecorder recorder;
	recorder.set_database_enable_checksum(checksum);
	recorder.set_database_enable_compression(compressed);
	recorder.init_recording_thread(iface.get());

	std::mt19937 rnd(1);
	std::uniform_int_distribution<int> dist(1, 500);

	// Create 10000 random SPIR-V modules with reasonable ID distribution.
	std::vector<uint32_t> dummy_spirv(4096);
	for (auto &d : dummy_spirv)
		d = dist(rnd);

	for (unsigned i = 0; i < 10000; i++)
	{
		dummy_spirv[0] = i;

		VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		info.codeSize = dummy_spirv.size() * sizeof(uint32_t);
		info.pCode = dummy_spirv.data();
		if (!recorder.record_shader_module((VkShaderModule)uint64_t(i + 1), info))
			abort();
	}

	for (unsigned i = 0; i < 10000; i++)
	{
		VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.minLod = float(i);
		if (!recorder.record_sampler((VkSampler)uint64_t(i + 1), sampler))
			abort();
	}

	for (unsigned i = 0; i < 10000; i++)
	{
		VkDescriptorSetLayoutCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		info.bindingCount = 16;
		VkDescriptorSetLayoutBinding bindings[16] = {};
		for (unsigned j = 0; j < 16; j++)
		{
			bindings[j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[j].binding = i + j;
			bindings[j].descriptorCount = 3;
			bindings[j].stageFlags = VK_SHADER_STAGE_ALL;
		}
		info.pBindings = bindings;
		if (!recorder.record_descriptor_set_layout((VkDescriptorSetLayout)uint64_t(i + 1), info))
			abort();
	}

	for (unsigned i = 0; i < 9000; i++)
	{
		VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		VkDescriptorSetLayout set_layouts[4] = {
			(VkDescriptorSetLayout)uint64_t(i + 1),
			(VkDescriptorSetLayout)uint64_t(i + 2),
			(VkDescriptorSetLayout)uint64_t(i + 3),
			(VkDescriptorSetLayout)uint64_t(i + 4),
		};
		info.pSetLayouts = set_layouts;
		info.setLayoutCount = 4;
		if (!recorder.record_pipeline_layout((VkPipelineLayout)uint64_t(i + 1), info))
			abort();
	}

	std::uniform_int_distribution<int> format_dist(0, 15);

	for (unsigned i = 0; i < 10000; i++)
	{
		VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		info.attachmentCount = 4;

		static const VkFormat random_formats[16] = {
			VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
			VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
			VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_B8G8R8A8_UNORM,
			VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
		};

		VkAttachmentDescription attachments[4] = {};
		for (auto &att : attachments)
		{
			att.format = random_formats[format_dist(rnd)];
			att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			att.samples = VK_SAMPLE_COUNT_1_BIT;
		}
		info.pAttachments = attachments;
		info.subpassCount = 1;

		VkSubpassDescription subpass = {};
		subpass.colorAttachmentCount = 4;
		VkAttachmentReference colors[4] = {
			{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
			{ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
			{ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
			{ 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
		};
		subpass.pColorAttachments = colors;
		info.pSubpasses = &subpass;

		if (!recorder.record_render_pass((VkRenderPass)uint64_t(i + 1), info))
			abort();
	}

	for (unsigned i = 0; i < 100000; i++)
	{
		VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		info.layout = (VkPipelineLayout)uint64_t((i % 9000) + 1);
		info.renderPass = (VkRenderPass)uint64_t((i % 10000) + 1);
		info.stageCount = 2;
		VkPipelineShaderStageCreateInfo stages[2] = {};
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].pName = "main";
		stages[0].module = (VkShaderModule)uint64_t((i % 10000) + 1);
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].pName = "main";
		stages[1].module = (VkShaderModule)uint64_t(((3 * i) % 10000) + 1);
		info.pStages = stages;

		VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		info.pColorBlendState = &cb;

		VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		info.pVertexInputState = &vi;

		VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		info.pDepthStencilState = &ds;

		VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		info.pDynamicState = &dyn;

		VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		info.pInputAssemblyState = &ia;

		VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		info.pRasterizationState = &rs;

		VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		info.pMultisampleState = &ms;

		VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		info.pViewportState = &vp;

		if (!recorder.record_graphics_pipeline((VkPipeline)uint64_t(i + 1), info, nullptr, 0))
			abort();
	}
}

struct ReplayInterface : StateCreatorInterface
{
	bool enqueue_create_sampler(Hash, const VkSamplerCreateInfo *, VkSampler *) override
	{
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash, const VkDescriptorSetLayoutCreateInfo *, VkDescriptorSetLayout *) override
	{
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash, const VkPipelineLayoutCreateInfo *, VkPipelineLayout *) override
	{
		return true;
	}

	bool enqueue_create_shader_module(Hash, const VkShaderModuleCreateInfo *, VkShaderModule *) override
	{
		return true;
	}

	bool enqueue_create_render_pass(Hash, const VkRenderPassCreateInfo *, VkRenderPass *) override
	{
		return true;
	}

	bool enqueue_create_render_pass2(Hash, const VkRenderPassCreateInfo2 *, VkRenderPass *) override
	{
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash, const VkComputePipelineCreateInfo *, VkPipeline *) override
	{
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash, const VkGraphicsPipelineCreateInfo *, VkPipeline *) override
	{
		return true;
	}

	bool enqueue_create_raytracing_pipeline(Hash, const VkRayTracingPipelineCreateInfoKHR *, VkPipeline *) override
	{
		return true;
	}
};

static bool dummy_replay_archive(const char *path)
{
	auto iface = std::unique_ptr<DatabaseInterface>(create_database(path, DatabaseMode::ReadOnly));
	if (!iface->prepare())
		return false;
	StateReplayer state_replayer;
	ReplayInterface replayer;

	std::vector<Hash> resource_hashes;
	std::vector<uint8_t> state_json;

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

	for (auto &tag : playback_order)
	{
		size_t resource_hash_count = 0;
		if (!iface->get_hash_list_for_resource_tag(tag, &resource_hash_count, nullptr))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return false;
		}

		resource_hashes.resize(resource_hash_count);

		if (!iface->get_hash_list_for_resource_tag(tag, &resource_hash_count, resource_hashes.data()))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return false;
		}

		for (auto &hash : resource_hashes)
		{
			size_t state_json_size = 0;
			if (!iface->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return false;
			}

			state_json.resize(state_json_size);

			if (!iface->read_entry(tag, hash, &state_json_size, state_json.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			if (!state_replayer.parse(replayer, nullptr, state_json.data(), state_json.size()))
				LOGE("Failed to parse blob (tag: %d, hash: 0x%" PRIx64 ").\n", tag, hash);
		}
	}

	return true;
}

int main()
{
	for (unsigned i = 0; i < 2; i++)
	{
		const char *path_compressed = i ? ".test.compressed.zip" : ".test.compressed.foz";
		const char *path_uncompressed = i ? ".test.uncompressed.zip" : ".test.uncompressed.foz";

		if (i)
			LOGI("=== Testing ZIP (miniz) ===\n");
		else
			LOGI("=== Testing Fossilize DB ===\n");

		const auto run = [&](bool compressed, bool checksum) {
			const char *path = compressed ? path_compressed : path_uncompressed;
			auto begin_time = std::chrono::steady_clock::now();
			bench_recorder(path, compressed, checksum);
			auto end_time = std::chrono::steady_clock::now();
			auto len = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - begin_time).count();

			if (compressed && checksum)
				LOGI("[WRITE] Compressed & checksum: %.3f ms\n", len * 1e-6);
			else if (compressed)
				LOGI("[WRITE] Compressed: %.3f ms\n", len * 1e-6);
			else if (checksum)
				LOGI("[WRITE] Uncompressed & checksum: %.3f ms\n", len * 1e-6);
			else
				LOGI("[WRITE] Uncompressed: %.3f ms\n", len * 1e-6);

			begin_time = std::chrono::steady_clock::now();
			if (!dummy_replay_archive(path))
				LOGE("Failed to replay archive.\n");
			remove(path);
			end_time = std::chrono::steady_clock::now();
			len = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - begin_time).count();
			LOGI("[READ]: %.3f ms\n", len * 1e-6);
		};

		run(false, false);
		run(false, true);
		run(true, false);
		run(true, true);
		LOGI("===================\n\n");
	}
}
