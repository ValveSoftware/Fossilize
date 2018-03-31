#include "vulkan_pipeline_cache.hpp"

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

	bool enqueue_create_sampler(Hash hash, unsigned index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		Hash recorded_hash = Hashing::compute_hash_sampler(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		unsigned sampler_index = recorder.register_sampler(hash, *create_info);
		*sampler = fake_handle<VkSampler>(sampler_index + 1000);
		recorder.set_sampler_handle(sampler_index, *sampler);
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash hash, unsigned index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout)
	{
		Hash recorded_hash = Hashing::compute_hash_descriptor_set_layout(recorder, *create_info);
		if (recorded_hash != hash)
			return false;

		*layout = VK_NULL_HANDLE;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash hash, unsigned index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) {}
	bool enqueue_create_shader_module(Hash hash, unsigned index, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) {}
	bool enqueue_create_render_pass(Hash hash, unsigned index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) {}
	bool enqueue_create_compute_pipeline(Hash hash, unsigned index, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) {}
	bool enqueue_create_graphics_pipeline(Hash hash, unsigned index, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) {}
};


int main()
{
	StateRecorder recorder;
	StateReplayer replayer;
	ReplayInterface iface;

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

	VkDescriptorSetLayoutBinding bindings[3] = {};
	static const VkSampler immutable_samplers[] = {
			fake_handle<VkSampler>(101),
			fake_handle<VkSampler>(100),
	};
	{
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
		recorder.register_descriptor_set_layout(Hashing::compute_hash_descriptor_set_layout(recorder, layout), layout);
	}

	auto res = recorder.serialize();
	fprintf(stderr, "Serialized: %s\n", res.c_str());

	replayer.parse(iface, res.c_str(), res.size());
}