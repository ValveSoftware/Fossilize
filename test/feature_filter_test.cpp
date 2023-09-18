#include "volk.h"
#include "fossilize_feature_filter.hpp"
#include <stdlib.h>
#include <stdio.h>
using namespace Fossilize;

#if 0
OpCapability Shader
OpCapability MeshShadingEXT
OpExtension "SPV_EXT_mesh_shader"
OpMemoryModel Logical GLSL450

OpEntryPoint GLCompute %cs_local_1_1_1 "main_local_1_1_1"
OpEntryPoint GLCompute %cs_local_256_1_1 "main_local_256_1_1"
OpEntryPoint GLCompute %cs_local_16_16_1 "main_local_16_16_1"
OpEntryPoint GLCompute %cs_local_id_1_1_1 "main_local_id_1_1_1"
OpEntryPoint GLCompute %cs_local_id_256_1_1 "main_local_id_256_1_1"
OpEntryPoint GLCompute %cs_local_id_16_16_1 "main_local_id_16_16_1"
OpEntryPoint GLCompute %cs_spec_1_2_3 "main_spec_1_2_3"

OpEntryPoint MeshEXT %ms_local_1_1_1 "main_local_1_1_1"
OpEntryPoint MeshEXT %ms_local_256_1_1 "main_local_256_1_1"
OpEntryPoint MeshEXT %ms_local_16_16_1 "main_local_16_16_1"
OpEntryPoint MeshEXT %ms_local_id_1_1_1 "main_local_id_1_1_1"
OpEntryPoint MeshEXT %ms_local_id_256_1_1 "main_local_id_256_1_1"
OpEntryPoint MeshEXT %ms_local_id_16_16_1 "main_local_id_16_16_1"
OpEntryPoint MeshEXT %ms_spec_1_2_3 "main_spec_1_2_3"

OpEntryPoint TaskEXT %ts_local_1_1_1 "main_local_1_1_1"
OpEntryPoint TaskEXT %ts_local_256_1_1 "main_local_256_1_1"
OpEntryPoint TaskEXT %ts_local_16_16_1 "main_local_16_16_1"
OpEntryPoint TaskEXT %ts_local_id_1_1_1 "main_local_id_1_1_1"
OpEntryPoint TaskEXT %ts_local_id_256_1_1 "main_local_id_256_1_1"
OpEntryPoint TaskEXT %ts_local_id_16_16_1 "main_local_id_16_16_1"
OpEntryPoint TaskEXT %ts_spec_1_2_3 "main_spec_1_2_3"

OpExecutionMode %cs_local_1_1_1 LocalSize 1 1 1
OpExecutionMode %cs_local_256_1_1 LocalSize 256 1 1
OpExecutionMode %cs_local_16_16_1 LocalSize 16 16 1
OpExecutionModeId %cs_local_id_1_1_1 LocalSizeId %uint_1 %uint_1 %uint_1
OpExecutionModeId %cs_local_id_256_1_1 LocalSizeId %uint_256 %uint_1 %uint_1
OpExecutionModeId %cs_local_id_16_16_1 LocalSizeId %uint_16 %uint_16 %uint_1
OpExecutionModeId %cs_spec_1_2_3 LocalSizeId %spec_uint_1 %spec_uint_2 %spec_uint_3

OpExecutionMode %ms_local_1_1_1 LocalSize 1 1 1
OpExecutionMode %ms_local_256_1_1 LocalSize 256 1 1
OpExecutionMode %ms_local_16_16_1 LocalSize 16 16 1
OpExecutionModeId %ms_local_id_1_1_1 LocalSizeId %uint_1 %uint_1 %uint_1
OpExecutionModeId %ms_local_id_256_1_1 LocalSizeId %uint_256 %uint_1 %uint_1
OpExecutionModeId %ms_local_id_16_16_1 LocalSizeId %uint_16 %uint_16 %uint_1
OpExecutionModeId %ms_spec_1_2_3 LocalSizeId %spec_uint_1 %spec_uint_2 %spec_uint_3

OpExecutionMode %ts_local_1_1_1 LocalSize 1 1 1
OpExecutionMode %ts_local_256_1_1 LocalSize 256 1 1
OpExecutionMode %ts_local_16_16_1 LocalSize 16 16 1
OpExecutionModeId %ts_local_id_1_1_1 LocalSizeId %uint_1 %uint_1 %uint_1
OpExecutionModeId %ts_local_id_256_1_1 LocalSizeId %uint_256 %uint_1 %uint_1
OpExecutionModeId %ts_local_id_16_16_1 LocalSizeId %uint_16 %uint_16 %uint_1
OpExecutionModeId %ts_spec_1_2_3 LocalSizeId %spec_uint_1 %spec_uint_2 %spec_uint_3

OpExecutionMode %ms_local_1_1_1 OutputVertices 1
OpExecutionMode %ms_local_256_1_1 OutputVertices 1
OpExecutionMode %ms_local_16_16_1 OutputVertices 1
OpExecutionMode %ms_local_id_1_1_1 OutputVertices 1
OpExecutionMode %ms_local_id_256_1_1 OutputVertices 1
OpExecutionMode %ms_local_id_16_16_1 OutputVertices 1
OpExecutionMode %ms_spec_1_2_3 OutputVertices 1

OpExecutionMode %ms_local_1_1_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_local_256_1_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_local_16_16_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_local_id_1_1_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_local_id_256_1_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_local_id_16_16_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_spec_1_2_3 OutputPrimitivesEXT 1

OpExecutionMode %ms_local_1_1_1 OutputTrianglesEXT
OpExecutionMode %ms_local_256_1_1 OutputTrianglesEXT
OpExecutionMode %ms_local_16_16_1 OutputTrianglesEXT
OpExecutionMode %ms_local_id_1_1_1 OutputTrianglesEXT
OpExecutionMode %ms_local_id_256_1_1 OutputTrianglesEXT
OpExecutionMode %ms_local_id_16_16_1 OutputTrianglesEXT
OpExecutionMode %ms_spec_1_2_3 OutputTrianglesEXT

OpDecorate %spec_uint_1 SpecId 0
OpDecorate %spec_uint_2 SpecId 1
OpDecorate %spec_uint_3 SpecId 2

%void = OpTypeVoid
%3 = OpTypeFunction %void
%uint = OpTypeInt 32 0
%spec_uint_1 = OpSpecConstant %uint 1
%spec_uint_2 = OpSpecConstant %uint 2
%spec_uint_3 = OpSpecConstant %uint 3
%uint_1 = OpConstant %uint 1
%uint_16 = OpConstant %uint 16
%uint_256 = OpConstant %uint 256

%cs_local_1_1_1 = OpFunction %void None %3
%cs_local_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%cs_local_256_1_1 = OpFunction %void None %3
%cs_local_256_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%cs_local_16_16_1 = OpFunction %void None %3
%cs_local_16_16_1_label = OpLabel
OpReturn
OpFunctionEnd
%cs_local_id_1_1_1 = OpFunction %void None %3
%cs_local_id_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%cs_local_id_256_1_1 = OpFunction %void None %3
%cs_local_id_256_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%cs_local_id_16_16_1 = OpFunction %void None %3
%cs_local_id_16_16_1_label = OpLabel
OpReturn
OpFunctionEnd
%cs_spec_1_2_3 = OpFunction %void None %3
%cs_spec_1_2_3_label = OpLabel
OpReturn
OpFunctionEnd

%ms_local_1_1_1 = OpFunction %void None %3
%ms_local_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ms_local_256_1_1 = OpFunction %void None %3
%ms_local_256_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ms_local_16_16_1 = OpFunction %void None %3
%ms_local_16_16_1_label = OpLabel
OpReturn
OpFunctionEnd
%ms_local_id_1_1_1 = OpFunction %void None %3
%ms_local_id_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ms_local_id_256_1_1 = OpFunction %void None %3
%ms_local_id_256_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ms_local_id_16_16_1 = OpFunction %void None %3
%ms_local_id_16_16_1_label = OpLabel
OpReturn
OpFunctionEnd
%ms_spec_1_2_3 = OpFunction %void None %3
%ms_spec_1_2_3_label = OpLabel
OpReturn
OpFunctionEnd

%ts_local_1_1_1 = OpFunction %void None %3
%ts_local_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ts_local_256_1_1 = OpFunction %void None %3
%ts_local_256_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ts_local_16_16_1 = OpFunction %void None %3
%ts_local_16_16_1_label = OpLabel
OpReturn
OpFunctionEnd
%ts_local_id_1_1_1 = OpFunction %void None %3
%ts_local_id_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ts_local_id_256_1_1 = OpFunction %void None %3
%ts_local_id_256_1_1_label = OpLabel
OpReturn
OpFunctionEnd
%ts_local_id_16_16_1 = OpFunction %void None %3
%ts_local_id_16_16_1_label = OpLabel
OpReturn
OpFunctionEnd
%ts_spec_1_2_3 = OpFunction %void None %3
%ts_spec_1_2_3_label = OpLabel
OpReturn
OpFunctionEnd
#endif

static const uint32_t spirv_blob[] =
{
	0x07230203, 0x00010600, 0x00070000, 0x00000034, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
	0x000014a3, 0x0006000a, 0x5f565053, 0x5f545845, 0x6873656d, 0x6168735f, 0x00726564, 0x0003000e,
	0x00000000, 0x00000001, 0x0008000f, 0x00000005, 0x00000001, 0x6e69616d, 0x636f6c5f, 0x315f6c61,
	0x315f315f, 0x00000000, 0x0008000f, 0x00000005, 0x00000002, 0x6e69616d, 0x636f6c5f, 0x325f6c61,
	0x315f3635, 0x0000315f, 0x0008000f, 0x00000005, 0x00000003, 0x6e69616d, 0x636f6c5f, 0x315f6c61,
	0x36315f36, 0x0000315f, 0x0008000f, 0x00000005, 0x00000004, 0x6e69616d, 0x636f6c5f, 0x695f6c61,
	0x5f315f64, 0x00315f31, 0x0009000f, 0x00000005, 0x00000005, 0x6e69616d, 0x636f6c5f, 0x695f6c61,
	0x35325f64, 0x5f315f36, 0x00000031, 0x0009000f, 0x00000005, 0x00000006, 0x6e69616d, 0x636f6c5f,
	0x695f6c61, 0x36315f64, 0x5f36315f, 0x00000031, 0x0007000f, 0x00000005, 0x00000007, 0x6e69616d,
	0x6570735f, 0x5f315f63, 0x00335f32, 0x0008000f, 0x000014f5, 0x00000008, 0x6e69616d, 0x636f6c5f,
	0x315f6c61, 0x315f315f, 0x00000000, 0x0008000f, 0x000014f5, 0x00000009, 0x6e69616d, 0x636f6c5f,
	0x325f6c61, 0x315f3635, 0x0000315f, 0x0008000f, 0x000014f5, 0x0000000a, 0x6e69616d, 0x636f6c5f,
	0x315f6c61, 0x36315f36, 0x0000315f, 0x0008000f, 0x000014f5, 0x0000000b, 0x6e69616d, 0x636f6c5f,
	0x695f6c61, 0x5f315f64, 0x00315f31, 0x0009000f, 0x000014f5, 0x0000000c, 0x6e69616d, 0x636f6c5f,
	0x695f6c61, 0x35325f64, 0x5f315f36, 0x00000031, 0x0009000f, 0x000014f5, 0x0000000d, 0x6e69616d,
	0x636f6c5f, 0x695f6c61, 0x36315f64, 0x5f36315f, 0x00000031, 0x0007000f, 0x000014f5, 0x0000000e,
	0x6e69616d, 0x6570735f, 0x5f315f63, 0x00335f32, 0x0008000f, 0x000014f4, 0x0000000f, 0x6e69616d,
	0x636f6c5f, 0x315f6c61, 0x315f315f, 0x00000000, 0x0008000f, 0x000014f4, 0x00000010, 0x6e69616d,
	0x636f6c5f, 0x325f6c61, 0x315f3635, 0x0000315f, 0x0008000f, 0x000014f4, 0x00000011, 0x6e69616d,
	0x636f6c5f, 0x315f6c61, 0x36315f36, 0x0000315f, 0x0008000f, 0x000014f4, 0x00000012, 0x6e69616d,
	0x636f6c5f, 0x695f6c61, 0x5f315f64, 0x00315f31, 0x0009000f, 0x000014f4, 0x00000013, 0x6e69616d,
	0x636f6c5f, 0x695f6c61, 0x35325f64, 0x5f315f36, 0x00000031, 0x0009000f, 0x000014f4, 0x00000014,
	0x6e69616d, 0x636f6c5f, 0x695f6c61, 0x36315f64, 0x5f36315f, 0x00000031, 0x0007000f, 0x000014f4,
	0x00000015, 0x6e69616d, 0x6570735f, 0x5f315f63, 0x00335f32, 0x00060010, 0x00000001, 0x00000011,
	0x00000001, 0x00000001, 0x00000001, 0x00060010, 0x00000002, 0x00000011, 0x00000100, 0x00000001,
	0x00000001, 0x00060010, 0x00000003, 0x00000011, 0x00000010, 0x00000010, 0x00000001, 0x0006014b,
	0x00000004, 0x00000026, 0x00000016, 0x00000016, 0x00000016, 0x0006014b, 0x00000005, 0x00000026,
	0x00000017, 0x00000016, 0x00000016, 0x0006014b, 0x00000006, 0x00000026, 0x00000018, 0x00000018,
	0x00000016, 0x0006014b, 0x00000007, 0x00000026, 0x00000019, 0x0000001a, 0x0000001b, 0x00060010,
	0x00000008, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00060010, 0x00000009, 0x00000011,
	0x00000100, 0x00000001, 0x00000001, 0x00060010, 0x0000000a, 0x00000011, 0x00000010, 0x00000010,
	0x00000001, 0x0006014b, 0x0000000b, 0x00000026, 0x00000016, 0x00000016, 0x00000016, 0x0006014b,
	0x0000000c, 0x00000026, 0x00000017, 0x00000016, 0x00000016, 0x0006014b, 0x0000000d, 0x00000026,
	0x00000018, 0x00000018, 0x00000016, 0x0006014b, 0x0000000e, 0x00000026, 0x00000019, 0x0000001a,
	0x0000001b, 0x00060010, 0x0000000f, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00060010,
	0x00000010, 0x00000011, 0x00000100, 0x00000001, 0x00000001, 0x00060010, 0x00000011, 0x00000011,
	0x00000010, 0x00000010, 0x00000001, 0x0006014b, 0x00000012, 0x00000026, 0x00000016, 0x00000016,
	0x00000016, 0x0006014b, 0x00000013, 0x00000026, 0x00000017, 0x00000016, 0x00000016, 0x0006014b,
	0x00000014, 0x00000026, 0x00000018, 0x00000018, 0x00000016, 0x0006014b, 0x00000015, 0x00000026,
	0x00000019, 0x0000001a, 0x0000001b, 0x00040010, 0x00000008, 0x0000001a, 0x00000001, 0x00040010,
	0x00000009, 0x0000001a, 0x00000001, 0x00040010, 0x0000000a, 0x0000001a, 0x00000001, 0x00040010,
	0x0000000b, 0x0000001a, 0x00000001, 0x00040010, 0x0000000c, 0x0000001a, 0x00000001, 0x00040010,
	0x0000000d, 0x0000001a, 0x00000001, 0x00040010, 0x0000000e, 0x0000001a, 0x00000001, 0x00040010,
	0x00000008, 0x00001496, 0x00000001, 0x00040010, 0x00000009, 0x00001496, 0x00000001, 0x00040010,
	0x0000000a, 0x00001496, 0x00000001, 0x00040010, 0x0000000b, 0x00001496, 0x00000001, 0x00040010,
	0x0000000c, 0x00001496, 0x00000001, 0x00040010, 0x0000000d, 0x00001496, 0x00000001, 0x00040010,
	0x0000000e, 0x00001496, 0x00000001, 0x00030010, 0x00000008, 0x000014b2, 0x00030010, 0x00000009,
	0x000014b2, 0x00030010, 0x0000000a, 0x000014b2, 0x00030010, 0x0000000b, 0x000014b2, 0x00030010,
	0x0000000c, 0x000014b2, 0x00030010, 0x0000000d, 0x000014b2, 0x00030010, 0x0000000e, 0x000014b2,
	0x00040047, 0x00000019, 0x00000001, 0x00000000, 0x00040047, 0x0000001a, 0x00000001, 0x00000001,
	0x00040047, 0x0000001b, 0x00000001, 0x00000002, 0x00020013, 0x0000001c, 0x00030021, 0x0000001d,
	0x0000001c, 0x00040015, 0x0000001e, 0x00000020, 0x00000000, 0x00040032, 0x0000001e, 0x00000019,
	0x00000001, 0x00040032, 0x0000001e, 0x0000001a, 0x00000002, 0x00040032, 0x0000001e, 0x0000001b,
	0x00000003, 0x0004002b, 0x0000001e, 0x00000016, 0x00000001, 0x0004002b, 0x0000001e, 0x00000018,
	0x00000010, 0x0004002b, 0x0000001e, 0x00000017, 0x00000100, 0x00050036, 0x0000001c, 0x00000001,
	0x00000000, 0x0000001d, 0x000200f8, 0x0000001f, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c,
	0x00000002, 0x00000000, 0x0000001d, 0x000200f8, 0x00000020, 0x000100fd, 0x00010038, 0x00050036,
	0x0000001c, 0x00000003, 0x00000000, 0x0000001d, 0x000200f8, 0x00000021, 0x000100fd, 0x00010038,
	0x00050036, 0x0000001c, 0x00000004, 0x00000000, 0x0000001d, 0x000200f8, 0x00000022, 0x000100fd,
	0x00010038, 0x00050036, 0x0000001c, 0x00000005, 0x00000000, 0x0000001d, 0x000200f8, 0x00000023,
	0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x00000006, 0x00000000, 0x0000001d, 0x000200f8,
	0x00000024, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x00000007, 0x00000000, 0x0000001d,
	0x000200f8, 0x00000025, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x00000008, 0x00000000,
	0x0000001d, 0x000200f8, 0x00000026, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x00000009,
	0x00000000, 0x0000001d, 0x000200f8, 0x00000027, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c,
	0x0000000a, 0x00000000, 0x0000001d, 0x000200f8, 0x00000028, 0x000100fd, 0x00010038, 0x00050036,
	0x0000001c, 0x0000000b, 0x00000000, 0x0000001d, 0x000200f8, 0x00000029, 0x000100fd, 0x00010038,
	0x00050036, 0x0000001c, 0x0000000c, 0x00000000, 0x0000001d, 0x000200f8, 0x0000002a, 0x000100fd,
	0x00010038, 0x00050036, 0x0000001c, 0x0000000d, 0x00000000, 0x0000001d, 0x000200f8, 0x0000002b,
	0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x0000000e, 0x00000000, 0x0000001d, 0x000200f8,
	0x0000002c, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x0000000f, 0x00000000, 0x0000001d,
	0x000200f8, 0x0000002d, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x00000010, 0x00000000,
	0x0000001d, 0x000200f8, 0x0000002e, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c, 0x00000011,
	0x00000000, 0x0000001d, 0x000200f8, 0x0000002f, 0x000100fd, 0x00010038, 0x00050036, 0x0000001c,
	0x00000012, 0x00000000, 0x0000001d, 0x000200f8, 0x00000030, 0x000100fd, 0x00010038, 0x00050036,
	0x0000001c, 0x00000013, 0x00000000, 0x0000001d, 0x000200f8, 0x00000031, 0x000100fd, 0x00010038,
	0x00050036, 0x0000001c, 0x00000014, 0x00000000, 0x0000001d, 0x000200f8, 0x00000032, 0x000100fd,
	0x00010038, 0x00050036, 0x0000001c, 0x00000015, 0x00000000, 0x0000001d, 0x000200f8, 0x00000033,
	0x000100fd, 0x00010038,
};

#if 0
OpCapability Shader
OpMemoryModel Logical GLSL450

OpEntryPoint GLCompute %cs_local_1_1_1 "main_local_1_1_1"
OpEntryPoint GLCompute %cs_local_id_1_1_1 "main_local_id_1_1_1"
OpEntryPoint GLCompute %cs_spec_1_2_3 "main_spec_1_2_3"

OpExecutionMode %cs_local_1_1_1 LocalSize 1 1 1
OpExecutionModeId %cs_local_id_1_1_1 LocalSizeId %uint_1 %uint_1 %uint_1
OpExecutionModeId %cs_spec_1_2_3 LocalSizeId %spec_uint_1 %spec_uint_2 %spec_uint_3

OpDecorate %spec_uint_1 SpecId 0
OpDecorate %spec_uint_2 SpecId 1
OpDecorate %spec_uint_3 SpecId 2
OpDecorate %wg BuiltIn WorkgroupSize

%void = OpTypeVoid
%3 = OpTypeFunction %void
%uint = OpTypeInt 32 0
%uvec3 = OpTypeVector %uint 3
%global_spec_uint_1 = OpConstant %uint 1
%global_spec_uint_2 = OpConstant %uint 2
%global_spec_uint_3 = OpConstant %uint 3
%spec_uint_1 = OpSpecConstant %uint 100
%spec_uint_2 = OpSpecConstant %uint 100
%spec_uint_3 = OpSpecConstant %uint 100
%uint_1 = OpConstant %uint 1

%wg = OpSpecConstantComposite %uvec3 %global_spec_uint_1 %global_spec_uint_2 %global_spec_uint_3

%cs_local_1_1_1 = OpFunction %void None %3
%cs_local_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd

%cs_local_id_1_1_1 = OpFunction %void None %3
%cs_local_id_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd

%cs_spec_1_2_3 = OpFunction %void None %3
%cs_spec_1_2_3_label = OpLabel
OpReturn
OpFunctionEnd
#endif

static const uint32_t spirv_blob_deprecated_wg_size[] =
{
	0x07230203, 0x00010600, 0x00070000, 0x00000013, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
	0x00000000, 0x00000001, 0x0008000f, 0x00000005, 0x00000001, 0x6e69616d, 0x636f6c5f, 0x315f6c61,
	0x315f315f, 0x00000000, 0x0008000f, 0x00000005, 0x00000002, 0x6e69616d, 0x636f6c5f, 0x695f6c61,
	0x5f315f64, 0x00315f31, 0x0007000f, 0x00000005, 0x00000003, 0x6e69616d, 0x6570735f, 0x5f315f63,
	0x00335f32, 0x00060010, 0x00000001, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x0006014b,
	0x00000002, 0x00000026, 0x00000004, 0x00000004, 0x00000004, 0x0006014b, 0x00000003, 0x00000026,
	0x00000005, 0x00000006, 0x00000007, 0x00040047, 0x00000005, 0x00000001, 0x00000000, 0x00040047,
	0x00000006, 0x00000001, 0x00000001, 0x00040047, 0x00000007, 0x00000001, 0x00000002, 0x00040047,
	0x00000008, 0x0000000b, 0x00000019, 0x00020013, 0x00000009, 0x00030021, 0x0000000a, 0x00000009,
	0x00040015, 0x0000000b, 0x00000020, 0x00000000, 0x00040017, 0x0000000c, 0x0000000b, 0x00000003,
	0x0004002b, 0x0000000b, 0x0000000d, 0x00000001, 0x0004002b, 0x0000000b, 0x0000000e, 0x00000002,
	0x0004002b, 0x0000000b, 0x0000000f, 0x00000003, 0x00040032, 0x0000000b, 0x00000005, 0x00000064,
	0x00040032, 0x0000000b, 0x00000006, 0x00000064, 0x00040032, 0x0000000b, 0x00000007, 0x00000064,
	0x0004002b, 0x0000000b, 0x00000004, 0x00000001, 0x00060033, 0x0000000c, 0x00000008, 0x0000000d,
	0x0000000e, 0x0000000f, 0x00050036, 0x00000009, 0x00000001, 0x00000000, 0x0000000a, 0x000200f8,
	0x00000010, 0x000100fd, 0x00010038, 0x00050036, 0x00000009, 0x00000002, 0x00000000, 0x0000000a,
	0x000200f8, 0x00000011, 0x000100fd, 0x00010038, 0x00050036, 0x00000009, 0x00000003, 0x00000000,
	0x0000000a, 0x000200f8, 0x00000012, 0x000100fd, 0x00010038,
};

#if 0
OpCapability Shader
OpMemoryModel Logical GLSL450

OpEntryPoint GLCompute %cs_local_1_1_1 "main_local_1_1_1"
OpEntryPoint GLCompute %cs_local_id_1_1_1 "main_local_id_1_1_1"
OpEntryPoint GLCompute %cs_spec_1_2_3 "main_spec_1_2_3"

OpExecutionMode %cs_local_1_1_1 LocalSize 1 1 1
OpExecutionModeId %cs_local_id_1_1_1 LocalSizeId %uint_1 %uint_1 %uint_1
OpExecutionModeId %cs_spec_1_2_3 LocalSizeId %spec_uint_1 %spec_uint_2 %spec_uint_3

OpDecorate %global_spec_uint_1 SpecId 0
OpDecorate %global_spec_uint_2 SpecId 1
OpDecorate %global_spec_uint_3 SpecId 2
OpDecorate %spec_uint_1 SpecId 3
OpDecorate %spec_uint_2 SpecId 4
OpDecorate %spec_uint_3 SpecId 5
OpDecorate %wg BuiltIn WorkgroupSize

%void = OpTypeVoid
%3 = OpTypeFunction %void
%uint = OpTypeInt 32 0
%uvec3 = OpTypeVector %uint 3
%global_spec_uint_1 = OpSpecConstant %uint 1
%global_spec_uint_2 = OpSpecConstant %uint 2
%global_spec_uint_3 = OpSpecConstant %uint 3
%spec_uint_1 = OpSpecConstant %uint 100
%spec_uint_2 = OpSpecConstant %uint 100
%spec_uint_3 = OpSpecConstant %uint 100
%uint_1 = OpConstant %uint 1

%wg = OpSpecConstantComposite %uvec3 %global_spec_uint_1 %global_spec_uint_2 %global_spec_uint_3

%cs_local_1_1_1 = OpFunction %void None %3
%cs_local_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd

%cs_local_id_1_1_1 = OpFunction %void None %3
%cs_local_id_1_1_1_label = OpLabel
OpReturn
OpFunctionEnd

%cs_spec_1_2_3 = OpFunction %void None %3
%cs_spec_1_2_3_label = OpLabel
OpReturn
OpFunctionEnd
#endif

static const uint32_t spirv_blob_deprecated_wg_size_spec[] =
{
	0x07230203, 0x00010600, 0x00070000, 0x00000013, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
	0x00000000, 0x00000001, 0x0008000f, 0x00000005, 0x00000001, 0x6e69616d, 0x636f6c5f, 0x315f6c61,
	0x315f315f, 0x00000000, 0x0008000f, 0x00000005, 0x00000002, 0x6e69616d, 0x636f6c5f, 0x695f6c61,
	0x5f315f64, 0x00315f31, 0x0007000f, 0x00000005, 0x00000003, 0x6e69616d, 0x6570735f, 0x5f315f63,
	0x00335f32, 0x00060010, 0x00000001, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x0006014b,
	0x00000002, 0x00000026, 0x00000004, 0x00000004, 0x00000004, 0x0006014b, 0x00000003, 0x00000026,
	0x00000005, 0x00000006, 0x00000007, 0x00040047, 0x00000008, 0x00000001, 0x00000000, 0x00040047,
	0x00000009, 0x00000001, 0x00000001, 0x00040047, 0x0000000a, 0x00000001, 0x00000002, 0x00040047,
	0x00000005, 0x00000001, 0x00000003, 0x00040047, 0x00000006, 0x00000001, 0x00000004, 0x00040047,
	0x00000007, 0x00000001, 0x00000005, 0x00040047, 0x0000000b, 0x0000000b, 0x00000019, 0x00020013,
	0x0000000c, 0x00030021, 0x0000000d, 0x0000000c, 0x00040015, 0x0000000e, 0x00000020, 0x00000000,
	0x00040017, 0x0000000f, 0x0000000e, 0x00000003, 0x00040032, 0x0000000e, 0x00000008, 0x00000001,
	0x00040032, 0x0000000e, 0x00000009, 0x00000002, 0x00040032, 0x0000000e, 0x0000000a, 0x00000003,
	0x00040032, 0x0000000e, 0x00000005, 0x00000064, 0x00040032, 0x0000000e, 0x00000006, 0x00000064,
	0x00040032, 0x0000000e, 0x00000007, 0x00000064, 0x0004002b, 0x0000000e, 0x00000004, 0x00000001,
	0x00060033, 0x0000000f, 0x0000000b, 0x00000008, 0x00000009, 0x0000000a, 0x00050036, 0x0000000c,
	0x00000001, 0x00000000, 0x0000000d, 0x000200f8, 0x00000010, 0x000100fd, 0x00010038, 0x00050036,
	0x0000000c, 0x00000002, 0x00000000, 0x0000000d, 0x000200f8, 0x00000011, 0x000100fd, 0x00010038,
	0x00050036, 0x0000000c, 0x00000003, 0x00000000, 0x0000000d, 0x000200f8, 0x00000012, 0x000100fd,
	0x00010038,
};

#if 0
OpCapability Shader
OpCapability MeshShadingEXT
OpExtension "SPV_EXT_mesh_shader"
OpMemoryModel Logical GLSL450

OpEntryPoint MeshEXT %ms_1_1 "main_1_1"
OpEntryPoint MeshEXT %ms_512_1 "main_512_1"
OpEntryPoint MeshEXT %ms_1_512 "main_1_512"

OpExecutionMode %ms_1_1 OutputVertices 1
OpExecutionMode %ms_512_1 OutputVertices 512
OpExecutionMode %ms_1_512 OutputVertices 1

OpExecutionMode %ms_1_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_512_1 OutputPrimitivesEXT 1
OpExecutionMode %ms_1_512 OutputPrimitivesEXT 512

OpExecutionMode %ms_1_1 OutputTrianglesEXT
OpExecutionMode %ms_512_1 OutputTrianglesEXT
OpExecutionMode %ms_1_512 OutputTrianglesEXT

%void = OpTypeVoid
%3 = OpTypeFunction %void
%uint = OpTypeInt 32 0

%ms_1_1 = OpFunction %void None %3
%ms_1_1_label = OpLabel
OpReturn
OpFunctionEnd

%ms_512_1 = OpFunction %void None %3
%ms_512_1_label = OpLabel
OpReturn
OpFunctionEnd

%ms_1_512 = OpFunction %void None %3
%ms_1_512_label = OpLabel
OpReturn
OpFunctionEnd
#endif

static const uint32_t spirv_blob_mesh_limits[] =
{
	0x07230203, 0x00010600, 0x00070000, 0x0000000a, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
	0x000014a3, 0x0006000a, 0x5f565053, 0x5f545845, 0x6873656d, 0x6168735f, 0x00726564, 0x0003000e,
	0x00000000, 0x00000001, 0x0006000f, 0x000014f5, 0x00000001, 0x6e69616d, 0x315f315f, 0x00000000,
	0x0006000f, 0x000014f5, 0x00000002, 0x6e69616d, 0x3231355f, 0x0000315f, 0x0006000f, 0x000014f5,
	0x00000003, 0x6e69616d, 0x355f315f, 0x00003231, 0x00040010, 0x00000001, 0x0000001a, 0x00000001,
	0x00040010, 0x00000002, 0x0000001a, 0x00000200, 0x00040010, 0x00000003, 0x0000001a, 0x00000001,
	0x00040010, 0x00000001, 0x00001496, 0x00000001, 0x00040010, 0x00000002, 0x00001496, 0x00000001,
	0x00040010, 0x00000003, 0x00001496, 0x00000200, 0x00030010, 0x00000001, 0x000014b2, 0x00030010,
	0x00000002, 0x000014b2, 0x00030010, 0x00000003, 0x000014b2, 0x00020013, 0x00000004, 0x00030021,
	0x00000005, 0x00000004, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00050036, 0x00000004,
	0x00000001, 0x00000000, 0x00000005, 0x000200f8, 0x00000007, 0x000100fd, 0x00010038, 0x00050036,
	0x00000004, 0x00000002, 0x00000000, 0x00000005, 0x000200f8, 0x00000008, 0x000100fd, 0x00010038,
	0x00050036, 0x00000004, 0x00000003, 0x00000000, 0x00000005, 0x000200f8, 0x00000009, 0x000100fd,
	0x00010038,
};

struct Test
{
	bool expected;
	VkShaderStageFlagBits stage;
	const char *entry;
	uint32_t wg_size[3];
	VkPipelineShaderStageCreateFlags flags;
	uint32_t required_size;
};

static bool run_test(const FeatureFilter &filter, const Test &test, VkShaderModule module)
{
	VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	VkPipelineShaderStageRequiredSubgroupSizeCreateInfo req =
			{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };

	req.requiredSubgroupSize = test.required_size;

	stage.module = module;
	stage.stage = test.stage;
	stage.pName = test.entry;
	stage.flags = test.flags;
	if (req.requiredSubgroupSize)
		stage.pNext = &req;

	static const VkSpecializationMapEntry entries[3] = {
		{ 0, 0 * sizeof(uint32_t), sizeof(uint32_t) },
		{ 1, 1 * sizeof(uint32_t), sizeof(uint32_t) },
		{ 2, 2 * sizeof(uint32_t), sizeof(uint32_t) },
	};

	VkSpecializationInfo spec_info;
	spec_info.pData = test.wg_size;
	spec_info.dataSize = sizeof(test.wg_size);
	spec_info.mapEntryCount = 3;
	spec_info.pMapEntries = entries;

	if (test.wg_size[0] || test.wg_size[1] || test.wg_size[2])
		stage.pSpecializationInfo = &spec_info;

	if (test.stage == VK_SHADER_STAGE_COMPUTE_BIT)
	{
		VkComputePipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pipe.stage = stage;
		if (filter.compute_pipeline_is_supported(&pipe) != test.expected)
			return false;
	}
	else
	{
		VkGraphicsPipelineCreateInfo pipe = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pipe.stageCount = 1;
		pipe.renderPass = (VkRenderPass)1;
		pipe.pStages = &stage;
		if (filter.graphics_pipeline_is_supported(&pipe) != test.expected)
			return false;
	}

	return true;
}

int main()
{
	const char *ext = VK_EXT_MESH_SHADER_EXTENSION_NAME;

	VkPhysicalDeviceFeatures2 pdf2 =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	VkPhysicalDeviceProperties2 props2 =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	VkPhysicalDeviceMeshShaderPropertiesEXT mesh_props =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT };
	VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
	VkPhysicalDeviceSubgroupSizeControlProperties size_control_props =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES };
	VkPhysicalDeviceSubgroupSizeControlFeatures size_control_features =
			{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES };

	pdf2.pNext = &mesh_features;
	props2.pNext = &mesh_props;
	mesh_props.pNext = &size_control_props;
	mesh_features.pNext = &size_control_features;

	size_control_props.maxSubgroupSize = 32;
	size_control_props.minSubgroupSize = 8;
	size_control_props.requiredSubgroupSizeStages = VK_SHADER_STAGE_COMPUTE_BIT |
	                                                VK_SHADER_STAGE_TASK_BIT_EXT |
	                                                VK_SHADER_STAGE_MESH_BIT_EXT;
	size_control_features.computeFullSubgroups = VK_TRUE;
	size_control_features.subgroupSizeControl = VK_TRUE;
	mesh_features.meshShader = VK_TRUE;
	mesh_features.taskShader = VK_TRUE;

	props2.properties.limits.maxComputeWorkGroupInvocations = 256;
	props2.properties.limits.maxComputeWorkGroupSize[0] = 256;
	props2.properties.limits.maxComputeWorkGroupSize[1] = 64;
	props2.properties.limits.maxComputeWorkGroupSize[2] = 16;

	mesh_props.maxTaskWorkGroupInvocations = 128;
	mesh_props.maxTaskWorkGroupSize[0] = 8;
	mesh_props.maxTaskWorkGroupSize[1] = 8;
	mesh_props.maxTaskWorkGroupSize[2] = 8;

	mesh_props.maxMeshWorkGroupInvocations = 128;
	mesh_props.maxMeshWorkGroupSize[0] = 64;
	mesh_props.maxMeshWorkGroupSize[1] = 32;
	mesh_props.maxMeshWorkGroupSize[2] = 16;

	mesh_props.maxMeshOutputVertices = 256;
	mesh_props.maxMeshOutputPrimitives = 256;

	FeatureFilter filter;
	if (!filter.init(VK_API_VERSION_1_3, &ext, 1, &pdf2, &props2))
		return EXIT_FAILURE;

	static const Test tests[] = {
		// Sanity checks.
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_1_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_256_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_16_16_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_1_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_256_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_16_16_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3" },

		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_1_1_1" },
		{ false, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_256_1_1" },
		{ false, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_16_16_1" },
		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_id_1_1_1" },
		{ false, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_id_256_1_1" },
		{ false, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_id_16_16_1" },
		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_spec_1_2_3" },

		{ true, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_1_1_1" },
		{ false, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_256_1_1" },
		{ false, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_16_16_1" },
		{ true, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_id_1_1_1" },
		{ false, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_id_256_1_1" },
		{ false, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_id_16_16_1" },
		{ true, VK_SHADER_STAGE_TASK_BIT_EXT, "main_spec_1_2_3" },

		// Try overriding nothing, should still work.
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_1_1_1", { 512, 512, 512 } },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_1_1_1", { 512, 512, 512 } },
		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_1_1_1", { 512, 512, 512 } },
		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_local_id_1_1_1", { 512, 512, 512 } },
		{ true, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_1_1_1", { 512, 512, 512 } },
		{ true, VK_SHADER_STAGE_TASK_BIT_EXT, "main_local_id_1_1_1", { 512, 512, 512 } },

		// Test that spec constant override is honored.
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 256, 1, 1 }, },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 257, 1, 1 }, },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 1, 64, 1 }, },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 1, 65, 1 }, },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 1, 1, 16 }, },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 1, 1, 17 }, },

		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_spec_1_2_3", { 64, 1, 1 }, },
		{ false, VK_SHADER_STAGE_MESH_BIT_EXT, "main_spec_1_2_3", { 65, 1, 1 }, },
		{ true, VK_SHADER_STAGE_TASK_BIT_EXT, "main_spec_1_2_3", { 1, 8, 1 }, },
		{ false, VK_SHADER_STAGE_TASK_BIT_EXT, "main_spec_1_2_3", { 1, 9, 1 }, },

		// Test FULL_SUBGROUPS validation
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 8, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 16, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 32, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 64, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 8, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT, 16 },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 16, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT, 16 },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 32, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT, 16 },

		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_spec_1_2_3", { 64, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT },
		{ false, VK_SHADER_STAGE_TASK_BIT_EXT, "main_spec_1_2_3", { 8, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT },
		{ true, VK_SHADER_STAGE_TASK_BIT_EXT, "main_spec_1_2_3", { 8, 1, 1 }, VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT, 8 },
	};

	// Test that BuiltIn always overrides.
	static const Test deprecated_wg_tests[] =
	{
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_1_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_1_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_1_1_1", { 512, 1, 1 } },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_1_1_1", { 1, 512, 1 } },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 1, 1, 512 } },
	};

	// Test that BuiltIn always overrides.
	static const Test deprecated_wg_spec_tests[] =
	{
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_1_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_1_1_1" },
		{ true, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_1_1" },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_1_1_1", { 512, 1, 1 } },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_local_id_1_1_1", { 1, 512, 1 } },
		{ false, VK_SHADER_STAGE_COMPUTE_BIT, "main_spec_1_2_3", { 1, 1, 512 } },
	};

	static const Test mesh_limit_tests[] =
	{
		{ true, VK_SHADER_STAGE_MESH_BIT_EXT, "main_1_1" },
		{ false, VK_SHADER_STAGE_MESH_BIT_EXT, "main_512_1" },
		{ false, VK_SHADER_STAGE_MESH_BIT_EXT, "main_1_512" },
	};

	VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

	module_info.pCode = spirv_blob;
	module_info.codeSize = sizeof(spirv_blob);
	if (!filter.shader_module_is_supported(&module_info))
		return EXIT_FAILURE;

	if (!filter.register_shader_module_info((VkShaderModule)1, &module_info))
		return EXIT_FAILURE;

	module_info.pCode = spirv_blob_deprecated_wg_size;
	module_info.codeSize = sizeof(spirv_blob_deprecated_wg_size);
	if (!filter.register_shader_module_info((VkShaderModule)2, &module_info))
		return EXIT_FAILURE;

	module_info.pCode = spirv_blob_deprecated_wg_size_spec;
	module_info.codeSize = sizeof(spirv_blob_deprecated_wg_size_spec);
	if (!filter.register_shader_module_info((VkShaderModule)3, &module_info))
		return EXIT_FAILURE;

	module_info.pCode = spirv_blob_mesh_limits;
	module_info.codeSize = sizeof(spirv_blob_mesh_limits);
	if (!filter.register_shader_module_info((VkShaderModule)4, &module_info))
		return EXIT_FAILURE;

	for (auto &test : tests)
		if (!run_test(filter, test, (VkShaderModule)1))
			return EXIT_FAILURE;

	for (auto &test : deprecated_wg_tests)
		if (!run_test(filter, test, (VkShaderModule)2))
			return EXIT_FAILURE;

	for (auto &test : deprecated_wg_spec_tests)
		if (!run_test(filter, test, (VkShaderModule)3))
			return EXIT_FAILURE;

	for (auto &test : mesh_limit_tests)
		if (!run_test(filter, test, (VkShaderModule)4))
			return EXIT_FAILURE;
}