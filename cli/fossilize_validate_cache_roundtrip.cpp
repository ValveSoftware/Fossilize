/* Copyright (c) 2026 Hans-Kristian Arntzen for Valve Corporation
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
#include "cli_parser.hpp"
#include "fossilize.hpp"
#include "fossilize_db.hpp"
#include "logging.hpp"
#include <unistd.h>
#include <limits.h>
#include <memory>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>

using namespace Fossilize;

static void print_help()
{
	LOGI("fossilize-validate-cache-roundtrip [--help] [--fossilize-replay <custom path to replayer>]\n");
}

static constexpr VkApplicationInfo app_info = {
	VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr,
	"fossilize-validate-cache-roundtrip", 1,
	"fossilize-cli", 1,
	VK_API_VERSION_1_1,
};

// Basic idea, with a custom dumb device with minimal features enables, study if the full replayer roundtrips properly.
static VkInstance create_instance()
{
	if (volkInitialize() != VK_SUCCESS)
		return VK_NULL_HANDLE;

	uint32_t instance_version;
	if (vkEnumerateInstanceVersion(&instance_version) != VK_SUCCESS || instance_version < VK_API_VERSION_1_1)
		return VK_NULL_HANDLE;

	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instance_info.pApplicationInfo = &app_info;

	VkInstance instance;
	if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS)
	{
		LOGE("Failed to create instance.\n");
		return VK_NULL_HANDLE;
	}

	volkLoadInstance(instance);

	return instance;
}

static VkDevice create_device(VkInstance instance, const VkPhysicalDeviceFeatures &features)
{
	// Just pick the first GPU. Could be improved if needed.
	uint32_t count = 1;
	VkPhysicalDevice gpu;
	if (vkEnumeratePhysicalDevices(instance, &count, &gpu) < 0)
		return VK_NULL_HANDLE;

	// We don't care about Vulkan 1.0 devices.
	VkPhysicalDeviceProperties props = {};
	vkGetPhysicalDeviceProperties(gpu, &props);
	if (props.apiVersion < VK_API_VERSION_1_1)
		return VK_NULL_HANDLE;

	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	VkDeviceQueueCreateInfo queue_create_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	const float queue_prio = 1.0f;
	queue_create_info.queueCount = 1;
	// Don't care what this queue does.
	queue_create_info.queueFamilyIndex = 0;
	queue_create_info.pQueuePriorities = &queue_prio;
	device_info.pQueueCreateInfos = &queue_create_info;
	device_info.pEnabledFeatures = &features;

	VkDevice device;
	if (vkCreateDevice(gpu, &device_info, nullptr, &device) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	volkLoadDevice(device);

	return device;
}

static VkPipelineCache create_pipeline_cache(VkDevice device)
{
	VkPipelineCacheCreateInfo create_info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	VkPipelineCache cache;
	if (vkCreatePipelineCache(device, &create_info, nullptr, &cache) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return cache;
}

static const uint32_t vert_spirv[] = {
	0x07230203, 0x00010000, 0x000d000b, 0x00000029,
	0x00000000, 0x00020011, 0x00000001, 0x0006000b,
	0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001,
	0x0007000f, 0x00000000, 0x00000004, 0x6e69616d,
	0x00000000, 0x00000008, 0x00000016, 0x00030003,
	0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47,
	0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79,
	0x656e696c, 0x7269645f, 0x69746365, 0x00006576,
	0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45,
	0x64756c63, 0x69645f65, 0x74636572, 0x00657669,
	0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
	0x00060005, 0x00000008, 0x565f6c67, 0x65747265,
	0x646e4978, 0x00007865, 0x00060005, 0x00000014,
	0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
	0x00060006, 0x00000014, 0x00000000, 0x505f6c67,
	0x7469736f, 0x006e6f69, 0x00070006, 0x00000014,
	0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953,
	0x00000000, 0x00070006, 0x00000014, 0x00000002,
	0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e,
	0x00070006, 0x00000014, 0x00000003, 0x435f6c67,
	0x446c6c75, 0x61747369, 0x0065636e, 0x00030005,
	0x00000016, 0x00000000, 0x00040047, 0x00000008,
	0x0000000b, 0x0000002a, 0x00030047, 0x00000014,
	0x00000002, 0x00050048, 0x00000014, 0x00000000,
	0x0000000b, 0x00000000, 0x00050048, 0x00000014,
	0x00000001, 0x0000000b, 0x00000001, 0x00050048,
	0x00000014, 0x00000002, 0x0000000b, 0x00000003,
	0x00050048, 0x00000014, 0x00000003, 0x0000000b,
	0x00000004, 0x00020013, 0x00000002, 0x00030021,
	0x00000003, 0x00000002, 0x00040015, 0x00000006,
	0x00000020, 0x00000001, 0x00040020, 0x00000007,
	0x00000001, 0x00000006, 0x0004003b, 0x00000007,
	0x00000008, 0x00000001, 0x0004002b, 0x00000006,
	0x0000000a, 0x00000000, 0x00020014, 0x0000000b,
	0x00030016, 0x0000000f, 0x00000020, 0x00040017,
	0x00000010, 0x0000000f, 0x00000004, 0x00040015,
	0x00000011, 0x00000020, 0x00000000, 0x0004002b,
	0x00000011, 0x00000012, 0x00000001, 0x0004001c,
	0x00000013, 0x0000000f, 0x00000012, 0x0006001e,
	0x00000014, 0x00000010, 0x0000000f, 0x00000013,
	0x00000013, 0x00040020, 0x00000015, 0x00000003,
	0x00000014, 0x0004003b, 0x00000015, 0x00000016,
	0x00000003, 0x0004002b, 0x0000000f, 0x00000017,
	0xbf800000, 0x0004002b, 0x0000000f, 0x00000018,
	0x00000000, 0x0004002b, 0x0000000f, 0x00000019,
	0x3f800000, 0x0007002c, 0x00000010, 0x0000001a,
	0x00000017, 0x00000017, 0x00000018, 0x00000019,
	0x00040020, 0x0000001b, 0x00000003, 0x00000010,
	0x0004002b, 0x00000006, 0x0000001f, 0x00000001,
	0x0004002b, 0x0000000f, 0x00000023, 0x40400000,
	0x0007002c, 0x00000010, 0x00000024, 0x00000017,
	0x00000023, 0x00000018, 0x00000019, 0x0007002c,
	0x00000010, 0x00000027, 0x00000023, 0x00000019,
	0x00000018, 0x00000019, 0x00050036, 0x00000002,
	0x00000004, 0x00000000, 0x00000003, 0x000200f8,
	0x00000005, 0x0004003d, 0x00000006, 0x00000009,
	0x00000008, 0x000500aa, 0x0000000b, 0x0000000c,
	0x00000009, 0x0000000a, 0x000300f7, 0x0000000e,
	0x00000000, 0x000400fa, 0x0000000c, 0x0000000d,
	0x0000001d, 0x000200f8, 0x0000000d, 0x00050041,
	0x0000001b, 0x0000001c, 0x00000016, 0x0000000a,
	0x0003003e, 0x0000001c, 0x0000001a, 0x000200f9,
	0x0000000e, 0x000200f8, 0x0000001d, 0x0004003d,
	0x00000006, 0x0000001e, 0x00000008, 0x000500aa,
	0x0000000b, 0x00000020, 0x0000001e, 0x0000001f,
	0x000300f7, 0x00000022, 0x00000000, 0x000400fa,
	0x00000020, 0x00000021, 0x00000026, 0x000200f8,
	0x00000021, 0x00050041, 0x0000001b, 0x00000025,
	0x00000016, 0x0000000a, 0x0003003e, 0x00000025,
	0x00000024, 0x000200f9, 0x00000022, 0x000200f8,
	0x00000026, 0x00050041, 0x0000001b, 0x00000028,
	0x00000016, 0x0000000a, 0x0003003e, 0x00000028,
	0x00000027, 0x000200f9, 0x00000022, 0x000200f8,
	0x00000022, 0x000200f9, 0x0000000e, 0x000200f8,
	0x0000000e, 0x000100fd, 0x00010038,
};

static const uint32_t frag_spirv[] = {
	0x07230203, 0x00010000, 0x000d000b, 0x00000012,
	0x00000000, 0x00020011, 0x00000001, 0x0006000b,
	0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001,
	0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
	0x00000000, 0x00000009, 0x00030010, 0x00000004,
	0x00000007, 0x00030003, 0x00000002, 0x000001c2,
	0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45,
	0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f,
	0x69746365, 0x00006576, 0x00080004, 0x475f4c47,
	0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65,
	0x74636572, 0x00657669, 0x00040005, 0x00000004,
	0x6e69616d, 0x00000000, 0x00050005, 0x00000009,
	0x67617246, 0x6f6c6f43, 0x00000072, 0x00050005,
	0x0000000a, 0x69676552, 0x72657473, 0x00000073,
	0x00050006, 0x0000000a, 0x00000000, 0x6f6c6f63,
	0x00000072, 0x00030005, 0x0000000c, 0x00000000,
	0x00040047, 0x00000009, 0x0000001e, 0x00000000,
	0x00030047, 0x0000000a, 0x00000002, 0x00050048,
	0x0000000a, 0x00000000, 0x00000023, 0x00000000,
	0x00020013, 0x00000002, 0x00030021, 0x00000003,
	0x00000002, 0x00030016, 0x00000006, 0x00000020,
	0x00040017, 0x00000007, 0x00000006, 0x00000004,
	0x00040020, 0x00000008, 0x00000003, 0x00000007,
	0x0004003b, 0x00000008, 0x00000009, 0x00000003,
	0x0003001e, 0x0000000a, 0x00000007, 0x00040020,
	0x0000000b, 0x00000009, 0x0000000a, 0x0004003b,
	0x0000000b, 0x0000000c, 0x00000009, 0x00040015,
	0x0000000d, 0x00000020, 0x00000001, 0x0004002b,
	0x0000000d, 0x0000000e, 0x00000000, 0x00040020,
	0x0000000f, 0x00000009, 0x00000007, 0x00050036,
	0x00000002, 0x00000004, 0x00000000, 0x00000003,
	0x000200f8, 0x00000005, 0x00050041, 0x0000000f,
	0x00000010, 0x0000000c, 0x0000000e, 0x0004003d,
	0x00000007, 0x00000011, 0x00000010, 0x0003003e,
	0x00000009, 0x00000011, 0x000100fd, 0x00010038
};

static const uint32_t compute_spirv[] = {
	0x07230203, 0x00010000, 0x000d000b, 0x00000012,
	0x00000000, 0x00020011, 0x00000001, 0x0006000b,
	0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
	0x00000000, 0x0003000e, 0x00000000, 0x00000001,
	0x0005000f, 0x00000005, 0x00000004, 0x6e69616d,
	0x00000000, 0x00060010, 0x00000004, 0x00000011,
	0x00000001, 0x00000001, 0x00000001, 0x00030003,
	0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47,
	0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79,
	0x656e696c, 0x7269645f, 0x69746365, 0x00006576,
	0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45,
	0x64756c63, 0x69645f65, 0x74636572, 0x00657669,
	0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
	0x00030005, 0x00000009, 0x00706d74, 0x00030005,
	0x0000000a, 0x004f4255, 0x00040006, 0x0000000a,
	0x00000000, 0x00000061, 0x00030005, 0x0000000c,
	0x00000000, 0x00030047, 0x0000000a, 0x00000002,
	0x00050048, 0x0000000a, 0x00000000, 0x00000023,
	0x00000000, 0x00020013, 0x00000002, 0x00030021,
	0x00000003, 0x00000002, 0x00030016, 0x00000006,
	0x00000020, 0x00040017, 0x00000007, 0x00000006,
	0x00000004, 0x00040020, 0x00000008, 0x00000007,
	0x00000007, 0x0003001e, 0x0000000a, 0x00000007,
	0x00040020, 0x0000000b, 0x00000009, 0x0000000a,
	0x0004003b, 0x0000000b, 0x0000000c, 0x00000009,
	0x00040015, 0x0000000d, 0x00000020, 0x00000001,
	0x0004002b, 0x0000000d, 0x0000000e, 0x00000000,
	0x00040020, 0x0000000f, 0x00000009, 0x00000007,
	0x00050036, 0x00000002, 0x00000004, 0x00000000,
	0x00000003, 0x000200f8, 0x00000005, 0x0004003b,
	0x00000008, 0x00000009, 0x00000007, 0x00050041,
	0x0000000f, 0x00000010, 0x0000000c, 0x0000000e,
	0x0004003d, 0x00000007, 0x00000011, 0x00000010,
	0x0003003e, 0x00000009, 0x00000011, 0x000100fd,
	0x00010038
};

static bool create_graphics_pipeline(VkDevice device, VkPipelineCache cache, StateRecorder &recorder)
{
	VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPushConstantRange push_range = {};
	push_range.size = 16;
	push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layout_info.pPushConstantRanges = &push_range;
	layout_info.pushConstantRangeCount = 1;

	VkPipelineLayout layout;
	if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS)
		return false;
	if (!recorder.record_pipeline_layout(layout, layout_info))
		return false;

	VkRenderPassCreateInfo render_pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	VkAttachmentDescription att = {};
	render_pass_info.attachmentCount = 1;
	render_pass_info.subpassCount = 1;
	att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.format = VK_FORMAT_R8G8B8A8_UNORM;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	render_pass_info.pAttachments = &att;

	VkSubpassDescription subpass = {};
	VkAttachmentReference ref = {};
	ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ref.attachment = 0;
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &ref;
	render_pass_info.pSubpasses = &subpass;
	VkRenderPass render_pass;
	if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS)
		return false;
	if (!recorder.record_render_pass(render_pass, render_pass_info))
		return false;

	VkShaderModule vert_module, frag_module;
	VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

	module_info.pCode = vert_spirv;
	module_info.codeSize = sizeof(vert_spirv);
	if (vkCreateShaderModule(device, &module_info, nullptr, &vert_module) != VK_SUCCESS)
		return false;
	if (!recorder.record_shader_module(vert_module, module_info))
		return false;

	module_info.pCode = frag_spirv;
	module_info.codeSize = sizeof(frag_spirv);
	if (vkCreateShaderModule(device, &module_info, nullptr, &frag_module) != VK_SUCCESS)
		return false;
	if (!recorder.record_shader_module(frag_module, module_info))
		return false;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].module = vert_module;
	stages[0].pName = "main";
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[1].module = frag_module;
	stages[1].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkPipelineDynamicStateCreateInfo dynamic_state_info = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic_state_info.dynamicStateCount = 2;
	static const VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	dynamic_state_info.pDynamicStates = dynamic_states;

	VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipeline_info.layout = layout;
	pipeline_info.basePipelineIndex = -1;
	pipeline_info.renderPass = render_pass;
	pipeline_info.subpass = 0;
	pipeline_info.pStages = stages;
	pipeline_info.stageCount = 2;
	pipeline_info.pDynamicState = &dynamic_state_info;

	VkPipelineColorBlendStateCreateInfo color_blend_state = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState attachment_state = {};
	attachment_state.colorWriteMask = 0xf;
	color_blend_state.attachmentCount = 1;
	color_blend_state.pAttachments = &attachment_state;
	pipeline_info.pColorBlendState = &color_blend_state;

	VkPipelineRasterizationStateCreateInfo raster_state = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster_state.polygonMode = VK_POLYGON_MODE_FILL;
	raster_state.lineWidth = 1.0f;
	pipeline_info.pRasterizationState = &raster_state;

	VkPipelineMultisampleStateCreateInfo msaa_state = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	msaa_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	pipeline_info.pMultisampleState = &msaa_state;

	VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	pipeline_info.pVertexInputState = &vertex_input;

	VkPipelineViewportStateCreateInfo vp_info = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp_info.viewportCount = 1;
	vp_info.scissorCount = 1;
	pipeline_info.pViewportState = &vp_info;

	VkPipelineInputAssemblyStateCreateInfo ia_info = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia_info.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	pipeline_info.pInputAssemblyState = &ia_info;

	VkPipeline pipeline;
	if (vkCreateGraphicsPipelines(device, cache, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS)
		return false;
	if (!recorder.record_graphics_pipeline(pipeline, pipeline_info, nullptr, 0))
		return false;

	vkDestroyRenderPass(device, render_pass, nullptr);
	vkDestroyPipelineLayout(device, layout, nullptr);
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyShaderModule(device, vert_module, nullptr);
	vkDestroyShaderModule(device, frag_module, nullptr);
	return true;
}

static bool create_compute_pipeline(VkDevice device, VkPipelineCache cache, StateRecorder &recorder)
{
	VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPushConstantRange push_range = {};
	push_range.size = 16;
	push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	layout_info.pPushConstantRanges = &push_range;
	layout_info.pushConstantRangeCount = 1;

	VkPipelineLayout layout;
	if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS)
		return false;
	if (!recorder.record_pipeline_layout(layout, layout_info))
		return false;

	VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	module_info.pCode = compute_spirv;
	module_info.codeSize = sizeof(compute_spirv);
	VkShaderModule shader_module;
	if (vkCreateShaderModule(device, &module_info, nullptr, &shader_module) != VK_SUCCESS)
		return false;
	if (!recorder.record_shader_module(shader_module, module_info))
		return false;

	VkComputePipelineCreateInfo compute_info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	compute_info.basePipelineIndex = -1;
	compute_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	compute_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	compute_info.layout = layout;
	compute_info.stage.module = shader_module;
	compute_info.stage.pName = "main";

	VkPipeline pipeline;
	if (vkCreateComputePipelines(device, cache, 1, &compute_info, nullptr, &pipeline) != VK_SUCCESS)
		return false;
	if (!recorder.record_compute_pipeline(pipeline, compute_info, nullptr, 0))
		return false;
	vkDestroyPipelineLayout(device, layout, nullptr);
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyShaderModule(device, shader_module, nullptr);

	return true;
}

static bool record_foz_and_cache(VkDevice device, VkPipelineCache cache, StateRecorder &recorder)
{
	if (!create_compute_pipeline(device, cache, recorder))
		return false;
	if (!create_graphics_pipeline(device, cache, recorder))
		return false;

	return true;
}

static bool serialize_pipeline_cache(VkDevice device, VkPipelineCache cache, const char *path)
{
	size_t size;
	if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS)
		return false;
	std::unique_ptr<uint8_t[]> data(new uint8_t[size]);
	if (vkGetPipelineCacheData(device, cache, &size, data.get()) != VK_SUCCESS)
		return false;

	FILE *file = fopen(path, "wb");
	if (!file)
		return false;

	if (fwrite(data.get(), 1, size, file) != size)
	{
		fclose(file);
		return false;
	}

	fclose(file);
	return true;
}

static bool check_replayer_roundtrip(const std::string &replayer, const char *foz_path, const char *cache_bin)
{
	int pipe_fd[2];
	if (pipe(pipe_fd) < 0)
		return false;
	pid_t pid = fork();

	if (pid)
	{
		close(pipe_fd[1]);
		FILE *file = fdopen(pipe_fd[0], "r");
		if (!file)
		{
			kill(pid, SIGKILL);
			LOGE("fdopen failed somehow.\n");
			return false;
		}

		unsigned hits = 0;
		unsigned misses = 0;

		char line[1024];
		while (fgets(line, sizeof(line), file))
		{
			const auto parse_number_end_of_line = [](const char *str)
			{
				const char *ptr = str + strlen(str);
				ptr--;
				if (*ptr == '\n')
					ptr--;
				while (isdigit(*ptr))
					ptr--;
				return strtoul(ptr, nullptr, 0);
			};

			// Crude parsing of console output.
			if (strstr(line, "Pipeline cache hits reported"))
				hits = parse_number_end_of_line(line);
			else if (strstr(line, "Pipeline cache misses reported"))
				misses = parse_number_end_of_line(line);
			else if (strncmp(line, "Fossilize ERROR:", strlen("Fossilize ERROR:")) == 0)
				fprintf(stderr, "%s", line);
		}

		fclose(file);

		int wait_status;
		if (waitpid(pid, &wait_status, 0) < 0)
		{
			LOGE("Failed to wait for child process\n");
			return false;
		}

		if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0)
		{
			LOGE("Child process did not exit normally.\n");
			return false;
		}

		if (hits == 0 && misses != 0)
		{
			LOGE("Pipeline cache missed! Roundtrip does not work!\n");
			return false;
		}
		else if (hits != 0 && misses == 0)
		{
			LOGI("Roundtrip success :D\n");
		}
		else if (hits == 0 && misses == 0)
		{
			LOGE("No hits or misses reported. Something went wrong.\n");
			return false;
		}
	}
	else
	{
		close(pipe_fd[0]);
		if (dup2(pipe_fd[1], STDOUT_FILENO) < 0)
			_exit(EXIT_FAILURE);
		if (dup2(pipe_fd[1], STDERR_FILENO) < 0)
			_exit(EXIT_FAILURE);
		close(pipe_fd[1]);

		if (execl(replayer.c_str(), replayer.c_str(),
		          foz_path, "--num-threads", "1", "--on-disk-pipeline-cache",
		          cache_bin, static_cast<char *>(nullptr)) < 0)
		{
			LOGE("Failed to spawn child process \"%s\".\n", replayer.c_str());
			_exit(EXIT_FAILURE);
		}
	}

	return true;
}

int main(int argc, char **argv)
{
	std::string fossilize_replay = "fossilize-replay";
	CLICallbacks cbs;

	cbs.add("--help", [&](CLIParser &parser) { parser.end(); });
	cbs.add("--fossilize-replay", [&](CLIParser &parser) { fossilize_replay = parser.next_string(); });

	CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
	{
		print_help();
		return EXIT_FAILURE;
	}
	else if (parser.is_ended_state())
	{
		print_help();
		return EXIT_SUCCESS;
	}

	VkInstance instance = create_instance();
	VkPhysicalDeviceFeatures features = {};
	VkDevice device = create_device(instance, features);
	VkPipelineCache cache = create_pipeline_cache(device);

	char tmp_foz[PATH_MAX];
	char tmp_bin[PATH_MAX];
	snprintf(tmp_foz, sizeof(tmp_foz), "/tmp/tmp_pipeline_cache.%u.foz", getpid());
	snprintf(tmp_bin, sizeof(tmp_bin), "/tmp/tmp_pipeline_cache.%u.bin", getpid());

	bool ret;
	{
		std::unique_ptr<DatabaseInterface> db(create_database(tmp_foz, DatabaseMode::OverWrite));
		StateRecorder recorder;
		recorder.init_recording_synchronized(db.get());
		recorder.set_database_enable_application_feature_links(false);

		if (!recorder.record_application_info(app_info))
			return EXIT_FAILURE;
		if (!recorder.record_physical_device_features(features))
			return EXIT_FAILURE;

		ret = record_foz_and_cache(device, cache, recorder);
	}

	if (ret)
		ret = serialize_pipeline_cache(device, cache, tmp_bin);

	vkDestroyPipelineCache(device, cache, nullptr);
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);

	if (ret)
		ret = check_replayer_roundtrip(fossilize_replay, tmp_foz, tmp_bin);

	unlink(tmp_foz);
	unlink(tmp_bin);

	if (!ret)
		return EXIT_FAILURE;
}