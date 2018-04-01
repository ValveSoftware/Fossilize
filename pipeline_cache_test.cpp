#include "vulkan_pipeline_cache.hpp"
#include <stdexcept>

using namespace VPC;

template <typename T>
constexpr T fake_handle(uint64_t value)
{
	static_assert(sizeof(T) == sizeof(uint64_t), "Handle size is not 64-bit.");
	return reinterpret_cast<T>(value);
}

struct ReplayInterface : StateCreatorInterface
{
	StateRecorder recorder;

	bool enqueue_create_sampler(Hash hash, unsigned, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		Hash recorded_hash = Hashing::compute_hash_sampler(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned sampler_index = recorder.register_sampler(hash, *create_info);
		*sampler = fake_handle<VkSampler>(sampler_index + 1000);
		recorder.set_sampler_handle(sampler_index, *sampler);
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, unsigned, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout)
	{
		Hash recorded_hash = Hashing::compute_hash_descriptor_set_layout(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned set_index = recorder.register_descriptor_set_layout(hash, *create_info);
		*layout = fake_handle<VkDescriptorSetLayout>(set_index + 10000);
		recorder.set_descriptor_set_layout_handle(set_index, *layout);
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, unsigned, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout)
	{
		Hash recorded_hash = Hashing::compute_hash_pipeline_layout(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned layout_index = recorder.register_pipeline_layout(hash, *create_info);
		*layout = fake_handle<VkPipelineLayout>(layout_index + 10000);
		recorder.set_pipeline_layout_handle(layout_index, *layout);
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, unsigned, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module)
	{
		Hash recorded_hash = Hashing::compute_hash_shader_module(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned module_index = recorder.register_shader_module(hash, *create_info);
		*module = fake_handle<VkShaderModule>(module_index + 20000);
		recorder.set_shader_module_handle(module_index, *module);
		return true;
	}

	bool enqueue_create_render_pass(Hash hash, unsigned, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass)
	{
		Hash recorded_hash = Hashing::compute_hash_render_pass(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned pass_index = recorder.register_render_pass(hash, *create_info);
		*render_pass = fake_handle<VkRenderPass>(pass_index + 40000);
		recorder.set_render_pass_handle(pass_index, *render_pass);
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, unsigned, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline)
	{
		Hash recorded_hash = Hashing::compute_hash_compute_pipeline(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned pipe_index = recorder.register_compute_pipeline(hash, *create_info);
		*pipeline = fake_handle<VkPipeline>(pipe_index + 40000);
		recorder.set_compute_pipeline_handle(pipe_index, *pipeline);
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, unsigned, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline)
	{
		Hash recorded_hash = Hashing::compute_hash_graphics_pipeline(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned pipe_index = recorder.register_graphics_pipeline(hash, *create_info);
		*pipeline = fake_handle<VkPipeline>(pipe_index + 600000);
		recorder.set_graphics_pipeline_handle(pipe_index, *pipeline);
		return true;
	}
};

static void record_samplers(StateRecorder &recorder)
{
	VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	sampler.unnormalizedCoordinates = VK_TRUE;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
	sampler.anisotropyEnable = VK_FALSE;
	sampler.maxAnisotropy = 30.0f;
	sampler.compareOp = VK_COMPARE_OP_EQUAL;
	sampler.compareEnable = VK_TRUE;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler.mipLodBias = 90.0f;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minLod = 10.0f;
	sampler.maxLod = 20.0f;
	unsigned index = recorder.register_sampler(Hashing::compute_hash_sampler(recorder, sampler), sampler);
	recorder.set_sampler_handle(index, fake_handle<VkSampler>(100));
	sampler.minLod = 11.0f;
	index = recorder.register_sampler(Hashing::compute_hash_sampler(recorder, sampler), sampler);
	recorder.set_sampler_handle(index, fake_handle<VkSampler>(101));
}

static void record_set_layouts(StateRecorder &recorder)
{
	VkDescriptorSetLayoutBinding bindings[3] = {};
	static const VkSampler immutable_samplers[] = {
		fake_handle<VkSampler>(101),
		fake_handle<VkSampler>(100),
	};

	VkDescriptorSetLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layout.bindingCount = 3;
	layout.pBindings = bindings;
	bindings[0].binding = 8;
	bindings[0].descriptorCount = 2;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[0].pImmutableSamplers = immutable_samplers;

	bindings[1].binding = 9;
	bindings[1].descriptorCount = 5;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

	bindings[2].binding = 2;
	bindings[2].descriptorCount = 3;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	unsigned index = recorder.register_descriptor_set_layout(Hashing::compute_hash_descriptor_set_layout(recorder, layout), layout);
	recorder.set_descriptor_set_layout_handle(index, fake_handle<VkDescriptorSetLayout>(1000));

	layout.bindingCount = 2;
	layout.pBindings = bindings + 1;
	index = recorder.register_descriptor_set_layout(Hashing::compute_hash_descriptor_set_layout(recorder, layout), layout);
	recorder.set_descriptor_set_layout_handle(index, fake_handle<VkDescriptorSetLayout>(1001));
}

static void record_pipeline_layouts(StateRecorder &recorder)
{
	static const VkDescriptorSetLayout set_layouts0[2] = {
		fake_handle<VkDescriptorSetLayout>(1000),
		fake_handle<VkDescriptorSetLayout>(1001),
	};
	static const VkDescriptorSetLayout set_layouts1[2] = {
		fake_handle<VkDescriptorSetLayout>(1001),
		fake_handle<VkDescriptorSetLayout>(1000),
	};

	VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout.pSetLayouts = set_layouts0;
	layout.setLayoutCount = 2;

	static const VkPushConstantRange ranges[2] = {
		{ VK_SHADER_STAGE_VERTEX_BIT, 0, 16 },
		{ VK_SHADER_STAGE_FRAGMENT_BIT, 16, 32 },
	};
	layout.pushConstantRangeCount = 2;
	layout.pPushConstantRanges = ranges;
	unsigned index = recorder.register_pipeline_layout(Hashing::compute_hash_pipeline_layout(recorder, layout), layout);
	recorder.set_pipeline_layout_handle(index, fake_handle<VkPipelineLayout>(10000));

	VkPipelineLayoutCreateInfo layout2 = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	index = recorder.register_pipeline_layout(Hashing::compute_hash_pipeline_layout(recorder, layout2), layout2);
	recorder.set_pipeline_layout_handle(index, fake_handle<VkPipelineLayout>(10001));

	VkPipelineLayoutCreateInfo layout3 = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layout3.setLayoutCount = 2;
	layout3.pSetLayouts = set_layouts1;
	index = recorder.register_pipeline_layout(Hashing::compute_hash_pipeline_layout(recorder, layout3), layout3);
	recorder.set_pipeline_layout_handle(index, fake_handle<VkPipelineLayout>(10002));
}

static void record_shader_modules(StateRecorder &recorder)
{
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	static const uint32_t code[] = { 0xdeadbeef, 0xcafebabe };
	info.pCode = code;
	info.codeSize = sizeof(code);

	unsigned index = recorder.register_shader_module(Hashing::compute_hash_shader_module(recorder, info), info);
	recorder.set_shader_module_handle(index, fake_handle<VkShaderModule>(5000));

	static const uint32_t code2[] = { 0xabba1337, 0xbabba100, 0xdeadbeef, 0xcafebabe };
	info.pCode = code2;
	info.codeSize = sizeof(code2);
	index = recorder.register_shader_module(Hashing::compute_hash_shader_module(recorder, info), info);
	recorder.set_shader_module_handle(index, fake_handle<VkShaderModule>(5001));
}

static void record_render_passes(StateRecorder &recorder)
{
	VkRenderPassCreateInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	VkSubpassDependency deps[2] = {};
	VkSubpassDescription subpasses[2] = {};
	VkAttachmentDescription att[2] = {};

	deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
	deps[0].dstAccessMask = 49;
	deps[0].srcAccessMask = 34;
	deps[0].dstStageMask = 199;
	deps[0].srcStageMask = 10;
	deps[0].srcSubpass = 9;
	deps[0].dstSubpass = 19;
	deps[1].dependencyFlags = 19;
	deps[1].dstAccessMask = 490;
	deps[1].srcAccessMask = 340;
	deps[1].dstStageMask = 1990;
	deps[1].srcStageMask = 100;
	deps[1].srcSubpass = 90;
	deps[1].dstSubpass = 190;

	att[0].format = VK_FORMAT_R16G16_SFLOAT;
	att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].samples = VK_SAMPLE_COUNT_16_BIT;

	static const uint32_t preserves[4] = { 9, 4, 2, 3 };
	static const VkAttachmentReference inputs[2] = { { 3, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { 9, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
	static const VkAttachmentReference colors[2] = { { 8, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
	static const VkAttachmentReference resolves[2] = { { 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
	static const VkAttachmentReference ds = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
	subpasses[0].preserveAttachmentCount = 4;
	subpasses[0].pPreserveAttachments = preserves;
	subpasses[0].inputAttachmentCount = 2;
	subpasses[0].pInputAttachments = inputs;
	subpasses[0].colorAttachmentCount = 2;
	subpasses[0].pColorAttachments = colors;
	subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
	subpasses[0].pDepthStencilAttachment = &ds;
	subpasses[0].pResolveAttachments = resolves;

	subpasses[1].inputAttachmentCount = 1;
	subpasses[1].pInputAttachments = inputs;
	subpasses[1].colorAttachmentCount = 2;
	subpasses[1].pColorAttachments = colors;
	subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

	pass.attachmentCount = 2;
	pass.pAttachments = att;
	pass.subpassCount = 1;
	pass.pSubpasses = subpasses;
	pass.dependencyCount = 0;
	pass.pDependencies = deps;
	unsigned index = recorder.register_render_pass(Hashing::compute_hash_render_pass(recorder, pass), pass);
	recorder.set_render_pass_handle(index, fake_handle<VkRenderPass>(30000));

	pass.dependencyCount = 0;
	index = recorder.register_render_pass(Hashing::compute_hash_render_pass(recorder, pass), pass);
	recorder.set_render_pass_handle(index, fake_handle<VkRenderPass>(30001));
}

static void record_compute_pipelines(StateRecorder &recorder)
{
	VkComputePipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipe.stage.module = fake_handle<VkShaderModule>(5000);
	pipe.stage.pName = "main";

	VkSpecializationInfo spec = {};
	spec.dataSize = 16;
	static const float data[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
	spec.pData = data;
	spec.mapEntryCount = 2;
	static const VkSpecializationMapEntry entries[2] = {
		{ 0, 4, 8 },
		{ 4, 4, 16 },
	};
	spec.pMapEntries = entries;
	pipe.stage.pSpecializationInfo = &spec;
	pipe.layout = fake_handle<VkPipelineLayout>(10001);

	unsigned index = recorder.register_compute_pipeline(Hashing::compute_hash_compute_pipeline(recorder, pipe), pipe);
	recorder.set_compute_pipeline_handle(index, fake_handle<VkPipeline>(80000));

	pipe.basePipelineHandle = fake_handle<VkPipeline>(80000);
	pipe.basePipelineIndex = 10;
	pipe.stage.pSpecializationInfo = nullptr;
	index = recorder.register_compute_pipeline(Hashing::compute_hash_compute_pipeline(recorder, pipe), pipe);
	recorder.set_compute_pipeline_handle(index, fake_handle<VkPipeline>(80001));
}

static void record_graphics_pipelines(StateRecorder &recorder)
{
	VkSpecializationInfo spec = {};
	spec.dataSize = 16;
	static const float data[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
	spec.pData = data;
	spec.mapEntryCount = 2;
	static const VkSpecializationMapEntry entries[2] = {
		{ 0, 4, 8 },
		{ 4, 4, 16 },
	};
	spec.pMapEntries = entries;

	VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pipe.layout = fake_handle<VkPipelineLayout>(10002);
	pipe.subpass = 1;
	pipe.renderPass = fake_handle<VkRenderPass>(30001);
	pipe.stageCount = 2;
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].pName = "vert";
	stages[0].module = fake_handle<VkShaderModule>(5000);
	stages[0].pSpecializationInfo = &spec;
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].pName = "frag";
	stages[1].module = fake_handle<VkShaderModule>(5001);
	stages[1].pSpecializationInfo = &spec;
	pipe.pStages = stages;

	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineTessellationStateCreateInfo tess = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };

	static const VkVertexInputAttributeDescription attrs[2] = {
		{ 2, 1, VK_FORMAT_R16G16_SFLOAT, 5 },
		{ 9, 1, VK_FORMAT_R8_UINT, 5 },
	};
	static const VkVertexInputBindingDescription binds[2] = {
		{ 8, 1, VK_VERTEX_INPUT_RATE_INSTANCE },
		{ 9, 6, VK_VERTEX_INPUT_RATE_VERTEX },
	};
	vi.vertexBindingDescriptionCount = 2;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexBindingDescriptions = binds;
	vi.pVertexAttributeDescriptions = attrs;

	ms.rasterizationSamples = VK_SAMPLE_COUNT_16_BIT;
	ms.sampleShadingEnable = VK_TRUE;
	ms.minSampleShading = 0.5f;
	ms.alphaToCoverageEnable = VK_TRUE;
	ms.alphaToOneEnable = VK_TRUE;
	static const uint32_t mask = 0xf;
	ms.pSampleMask = &mask;

	static const VkDynamicState dyn_states[3] = {
			VK_DYNAMIC_STATE_BLEND_CONSTANTS,
			VK_DYNAMIC_STATE_DEPTH_BIAS,
			VK_DYNAMIC_STATE_LINE_WIDTH
	};
	dyn.dynamicStateCount = 3;
	dyn.pDynamicStates = dyn_states;

	static const VkViewport vps[2] = {
		{ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f },
		{ 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f },
	};
	static const VkRect2D sci[2] = {
		{ { 3, 4 }, { 8, 9 }},
		{ { 13, 14 }, { 18, 19 }},
	};
	vp.viewportCount = 2;
	vp.scissorCount = 2;
	vp.pViewports = vps;
	vp.pScissors = sci;

	static const VkPipelineColorBlendAttachmentState blend_attachments[2] = {
			{ VK_TRUE,
					VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_OP_ADD,
					VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_OP_SUBTRACT,
					0xf },
			{ VK_TRUE,
					VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_OP_ADD,
					VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_OP_SUBTRACT,
					0x3 },
	};
	blend.logicOpEnable = VK_TRUE;
	blend.logicOp = VK_LOGIC_OP_AND_INVERTED;
	blend.blendConstants[0] = 9.0f;
	blend.blendConstants[1] = 19.0f;
	blend.blendConstants[2] = 29.0f;
	blend.blendConstants[3] = 39.0f;
	blend.attachmentCount = 2;
	blend.pAttachments = blend_attachments;

	tess.patchControlPoints = 9;

	ds.front.compareOp = VK_COMPARE_OP_GREATER;
	ds.front.writeMask = 9;
	ds.front.reference = 10;
	ds.front.failOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	ds.front.depthFailOp = VK_STENCIL_OP_INVERT;
	ds.front.compareMask = 19;
	ds.front.passOp = VK_STENCIL_OP_REPLACE;
	ds.back.compareOp = VK_COMPARE_OP_LESS;
	ds.back.writeMask = 79;
	ds.back.reference = 80;
	ds.back.failOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
	ds.back.depthFailOp = VK_STENCIL_OP_ZERO;
	ds.back.compareMask = 29;
	ds.back.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	ds.stencilTestEnable = VK_TRUE;
	ds.minDepthBounds = 0.1f;
	ds.maxDepthBounds = 0.2f;
	ds.depthCompareOp = VK_COMPARE_OP_EQUAL;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthTestEnable = VK_TRUE;
	ds.depthBoundsTestEnable = VK_TRUE;

	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.polygonMode = VK_POLYGON_MODE_LINE;
	rs.depthClampEnable = VK_TRUE;
	rs.depthBiasEnable = VK_TRUE;
	rs.depthBiasSlopeFactor = 0.3f;
	rs.depthBiasConstantFactor = 0.8f;
	rs.depthBiasClamp = 0.5f;
	rs.rasterizerDiscardEnable = VK_TRUE;
	rs.lineWidth = 0.1f;
	rs.cullMode = VK_CULL_MODE_FRONT_AND_BACK;

	ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
	ia.primitiveRestartEnable = VK_TRUE;

	pipe.pVertexInputState = &vi;
	pipe.pMultisampleState = &ms;
	pipe.pDynamicState = &dyn;
	pipe.pViewportState = &vp;
	pipe.pColorBlendState = &blend;
	pipe.pTessellationState = &tess;
	pipe.pDepthStencilState = &ds;
	pipe.pRasterizationState = &rs;
	pipe.pInputAssemblyState = &ia;

	unsigned index = recorder.register_graphics_pipeline(Hashing::compute_hash_graphics_pipeline(recorder, pipe), pipe);
	recorder.set_graphics_pipeline_handle(index, fake_handle<VkPipeline>(100000));

	vp.viewportCount = 0;
	vp.scissorCount = 0;
	pipe.basePipelineHandle = fake_handle<VkPipeline>(100000);
	pipe.basePipelineIndex = 200;
	index = recorder.register_graphics_pipeline(Hashing::compute_hash_graphics_pipeline(recorder, pipe), pipe);
	recorder.set_graphics_pipeline_handle(index, fake_handle<VkPipeline>(100001));
}

int main()
{
	try
	{
		StateRecorder recorder;
		StateReplayer replayer;
		ReplayInterface iface;

		record_samplers(recorder);
		record_set_layouts(recorder);
		record_pipeline_layouts(recorder);
		record_shader_modules(recorder);
		record_render_passes(recorder);
		record_compute_pipelines(recorder);
		record_graphics_pipelines(recorder);

		auto res = recorder.serialize();
		fprintf(stderr, "Serialized: %s\n", res.c_str());

		replayer.parse(iface, res.c_str(), res.size());
		return EXIT_SUCCESS;
	}
	catch (const std::exception &e)
	{
		fprintf(stderr, "Error: %s\n", e.what());
		return EXIT_FAILURE;
	}
}