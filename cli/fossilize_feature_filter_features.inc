/* Copyright (c) 2021 Hans-Kristian Arntzen
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

F(16BIT_STORAGE, storage_16bit, 1_0, KHR_16BIT_STORAGE);
F(MULTIVIEW, multiview, 1_0, KHR_MULTIVIEW);
F(VARIABLE_POINTERS, variable_pointers, 1_0, KHR_VARIABLE_POINTERS);
F(SAMPLER_YCBCR_CONVERSION, ycbcr_conversion, 1_0, KHR_SAMPLER_YCBCR_CONVERSION);
F(SHADER_DRAW_PARAMETERS, draw_parameters, 1_1, KHR_SHADER_DRAW_PARAMETERS);
F(8BIT_STORAGE, storage_8bit, 1_0, KHR_8BIT_STORAGE);
F(SHADER_ATOMIC_INT64, atomic_int64, 1_0, KHR_SHADER_ATOMIC_INT64);
F(SHADER_FLOAT16_INT8, float16_int8, 1_0, KHR_SHADER_FLOAT16_INT8);
F(DESCRIPTOR_INDEXING, descriptor_indexing, 1_0, EXT_DESCRIPTOR_INDEXING);
F(VULKAN_MEMORY_MODEL, memory_model, 1_0, KHR_VULKAN_MEMORY_MODEL);
F(UNIFORM_BUFFER_STANDARD_LAYOUT, ubo_standard_layout, 1_0, KHR_UNIFORM_BUFFER_STANDARD_LAYOUT);
F(SHADER_SUBGROUP_EXTENDED_TYPES, subgroup_extended_types, 1_1, KHR_SHADER_SUBGROUP_EXTENDED_TYPES);
F(SEPARATE_DEPTH_STENCIL_LAYOUTS, separate_ds_layout, 1_0, KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS);
F(BUFFER_DEVICE_ADDRESS, buffer_device_address, 1_0, KHR_BUFFER_DEVICE_ADDRESS);
FE(SHADER_CLOCK, shader_clock, KHR);
FE(FRAGMENT_SHADING_RATE, fragment_shading_rate, KHR);
FE(RAY_TRACING_PIPELINE, ray_tracing_pipeline, KHR);
FE(ACCELERATION_STRUCTURE, acceleration_structure, KHR);
FE(RAY_QUERY, ray_query, KHR);
FE(WORKGROUP_MEMORY_EXPLICIT_LAYOUT, workgroup_memory_explicit_layout, KHR);
FE(SHADER_INTEGER_DOT_PRODUCT, shader_integer_dot_product, KHR);
FE(SYNCHRONIZATION_2, synchronization2, KHR);
FE(DYNAMIC_RENDERING, dynamic_rendering, KHR);
FE(ZERO_INITIALIZE_WORKGROUP_MEMORY, zero_initialize_workgroup_memory, KHR);
FE(TRANSFORM_FEEDBACK, transform_feedback, EXT);
FE(DEPTH_CLIP_ENABLE, depth_clip, EXT);
FE(INLINE_UNIFORM_BLOCK, inline_uniform_block, EXT);
FE(BLEND_OPERATION_ADVANCED, blend_operation_advanced, EXT);
FE_ALIAS(VERTEX_ATTRIBUTE_DIVISOR, attribute_divisor, KHR, EXT);
FE(SHADER_DEMOTE_TO_HELPER_INVOCATION, demote_to_helper, EXT);
FE(FRAGMENT_SHADER_INTERLOCK, shader_interlock, EXT);
FE(FRAGMENT_DENSITY_MAP, fragment_density, EXT);
FE(BUFFER_DEVICE_ADDRESS, buffer_device_address_ext, EXT);
FE_ALIAS(LINE_RASTERIZATION, line_rasterization, KHR, EXT);
FE(SUBGROUP_SIZE_CONTROL, subgroup_size_control, EXT);
FE(EXTENDED_DYNAMIC_STATE, extended_dynamic_state, EXT);
FE(EXTENDED_DYNAMIC_STATE_2, extended_dynamic_state2, EXT);
FE(EXTENDED_DYNAMIC_STATE_3, extended_dynamic_state3, EXT);
FE(COLOR_WRITE_ENABLE, color_write_enable, EXT);
FE(VERTEX_INPUT_DYNAMIC_STATE, vertex_input_dynamic_state, EXT);
FE(SHADER_IMAGE_ATOMIC_INT64, shader_image_atomic_int64, EXT);
FE(SHADER_ATOMIC_FLOAT, shader_atomic_float, EXT);
FE(SHADER_ATOMIC_FLOAT_2, shader_atomic_float2, EXT);
FE(PROVOKING_VERTEX, provoking_vertex, EXT);
FE(CUSTOM_BORDER_COLOR, custom_border_color, EXT);
FE_ALIAS(COMPUTE_SHADER_DERIVATIVES, compute_shader_derivatives, KHR, NV);
FE(FRAGMENT_SHADER_BARYCENTRIC, barycentric, KHR);
FE(SHADER_IMAGE_FOOTPRINT, image_footprint_nv, NV);
FE(SHADING_RATE_IMAGE, shading_rate_nv, NV);
FE(COOPERATIVE_MATRIX, cooperative_matrix_nv, NV);
FE(SHADER_SM_BUILTINS, sm_builtins_nv, NV);
FE(SHADER_INTEGER_FUNCTIONS_2, integer_functions2_intel, INTEL);
FE_ALIAS(MUTABLE_DESCRIPTOR_TYPE, mutable_descriptor_type, EXT, VALVE);
FE(ROBUSTNESS_2, robustness2, EXT);
FE(IMAGE_ROBUSTNESS, image_robustness, EXT);
FE(GRAPHICS_PIPELINE_LIBRARY, graphics_pipeline_library, EXT);
FE(FRAGMENT_SHADING_RATE_ENUMS, fragment_shading_rate_enums, NV);
FE(DEPTH_CLIP_CONTROL, depth_clip_control, EXT);
FE(MESH_SHADER, mesh_shader_nv, NV);
FE(MESH_SHADER, mesh_shader, EXT);
FE(DESCRIPTOR_BUFFER, descriptor_buffer, EXT);
FE(NON_SEAMLESS_CUBE_MAP, non_seamless_cube_map, EXT);
FE(ATTACHMENT_FEEDBACK_LOOP_LAYOUT, attachment_feedback_loop_layout, EXT);
FE(MAINTENANCE_4, maintenance4, KHR);
FE(MAINTENANCE_5, maintenance5, KHR);
FE(MAINTENANCE_7, maintenance7, KHR);
FE(SHADER_MODULE_IDENTIFIER, shader_module_identifier, EXT);
FE(RAY_TRACING_MAINTENANCE_1, ray_tracing_maintenance1, KHR);
FE(RAY_TRACING_MOTION_BLUR, ray_tracing_motion_blur_nv, NV);
FE(IMAGE_PROCESSING, image_processing_qcom, QCOM);
FE(IMAGE_PROCESSING_2, image_processing2_qcom, QCOM);
FE(SHADER_CORE_BUILTINS, shader_core_builtins_arm, ARM);
FE(RAY_TRACING_POSITION_FETCH, ray_tracing_position_fetch, KHR);
FE(SHADER_TILE_IMAGE, shader_tile_image, EXT);
FE(COOPERATIVE_MATRIX, cooperative_matrix, KHR);
FE(SHADER_SUBGROUP_ROTATE, shader_subgroup_rotate, KHR);
FE(SHADER_EXPECT_ASSUME, expect_assume, KHR);
FE(SHADER_FLOAT_CONTROLS_2, shader_float_controls2, KHR);
FE(SHADER_QUAD_CONTROL, shader_quad_control, KHR);
FE(RAW_ACCESS_CHAINS, raw_access_chains_nv, NV);
FE(SHADER_REPLICATED_COMPOSITES, shader_replicated_composites, EXT);
FE(PIPELINE_ROBUSTNESS, pipeline_robustness, EXT);
FE(CONDITIONAL_RENDERING, conditional_rendering, EXT);
FE(DYNAMIC_RENDERING_LOCAL_READ, dynamic_rendering_local_read, KHR);
FE_ALIAS(RASTERIZATION_ORDER_ATTACHMENT_ACCESS, rasterization_order_attachment_access, EXT, ARM);
FE(DEVICE_GENERATED_COMMANDS, device_generated_commands_nv, NV);
FE(DEVICE_GENERATED_COMMANDS_COMPUTE, device_generated_commands_compute_nv, NV);
FE(OPTICAL_FLOW, optical_flow_nv, NV);
FE(LEGACY_DITHERING, legacy_dithering, EXT);
FE(PIPELINE_PROTECTED_ACCESS, pipeline_protected_access, EXT);
FE(PER_STAGE_DESCRIPTOR_SET, per_stage_descriptor_set_nv, NV);
FE(ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE, attachment_feedback_loop_dynamic_state, EXT);
FE(SUBPASS_MERGE_FEEDBACK, subpass_merge_feedback, EXT);
FE(BORDER_COLOR_SWIZZLE, border_color_swizzle, EXT);
FE(MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED, multisampled_render_to_single_sampled, EXT);
FE(DEPTH_BIAS_CONTROL, depth_bias_control, EXT);
FE(DEPTH_CLAMP_CONTROL, depth_clamp_control, EXT);
FE(DEVICE_GENERATED_COMMANDS, device_generated_commands, EXT);
FE(SHADER_OBJECT, shader_object, EXT);
FE(PRIMITIVES_GENERATED_QUERY, primitives_generated_query, EXT);
FE(IMAGE_2D_VIEW_OF_3D, image_2d_view_of_3d, EXT);
