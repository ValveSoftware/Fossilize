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

P(DESCRIPTOR_INDEXING, descriptor_indexing, 1_0, EXT_DESCRIPTOR_INDEXING);
P_CORE(SUBGROUP, subgroup, 1_1);
P(FLOAT_CONTROLS, float_control, 1_0, KHR_SHADER_FLOAT_CONTROLS);
P(DEPTH_STENCIL_RESOLVE, ds_resolve, 1_0, KHR_DEPTH_STENCIL_RESOLVE);
P(MULTIVIEW, multiview, 1_0, KHR_MULTIVIEW);
PE(FRAGMENT_SHADING_RATE, fragment_shading_rate, KHR);
PE(RAY_TRACING_PIPELINE, ray_tracing_pipeline, KHR);
PE(ACCELERATION_STRUCTURE, acceleration_structure, KHR);
PE(SUBGROUP_SIZE_CONTROL, subgroup_size_control, EXT);
PE(INLINE_UNIFORM_BLOCK, inline_uniform_block, EXT);
PE_ALIAS(VERTEX_ATTRIBUTE_DIVISOR, attribute_divisor, KHR, EXT);
PE(PUSH_DESCRIPTOR, push_descriptor, KHR);
PE(SAMPLE_LOCATIONS, sample_locations, EXT);
PE(EXTENDED_DYNAMIC_STATE_3, extended_dynamic_state3, EXT);
PE(MESH_SHADER, mesh_shader, EXT);
PE(SHADER_MODULE_IDENTIFIER, shader_module_identifier, EXT);
PE(BLEND_OPERATION_ADVANCED, blend_operation_advanced, EXT);
PE(MAINTENANCE_7, maintenance7, KHR);
