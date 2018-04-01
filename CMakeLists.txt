cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD 99)
project(Vulkan-Pipeline-Cache LANGUAGES CXX C)

if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
	set(VPC_CXX_FLAGS -Wall -Wextra -pedantic -Wno-missing-field-initializers -Wno-empty-body)
elseif (MSVC)
	set(VPC_CXX_FLAGS /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005 /MP /DNOMINMAX)
endif()

option(VPC_SANITIZE_ADDRESS "Sanitize address" OFF)
set(VPC_LINK_FLAGS)
if (VPC_SANITIZE_ADDRESS)
    set(VPC_CXX_FLAGS ${VPC_CXX_FLAGS} -fsanitize=address)
    set(VPC_LINK_FLAGS ${VPC_LINK_FLAGS} -fsanitize=address)
endif()

option(VPC_SANITIZE_THREADS "Sanitize threads" OFF)
if (VPC_SANITIZE_THREADS)
    set(VPC_CXX_FLAGS ${VPC_CXX_FLAGS} -fsanitize=thread)
    set(VPC_LINK_FLAGS ${VPC_LINK_FLAGS} -fsanitize=thread)
endif()

add_library(vulkan-pipeline-cache STATIC
	vulkan_pipeline_cache.hpp
	vulkan_pipeline_cache.cpp)

target_include_directories(vulkan-pipeline-cache PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/khronos)

target_compile_options(vulkan-pipeline-cache PUBLIC ${VPC_CXX_FLAGS})
option(VPC_RAPIDJSON_INCLUDE_PATH "Optional custom include path for rapidjson.")
if (VPC_RAPIDJSON_INCLUDE_PATH)
	target_include_directories(vulkan-pipeline-cache PUBLIC ${VPC_RAPIDJSON_INCLUDE_PATH})
else()
    target_include_directories(vulkan-pipeline-cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rapidjson/include)
endif()

add_executable(pipeline-cache-test pipeline_cache_test.cpp)
target_link_libraries(pipeline-cache-test vulkan-pipeline-cache)
target_compile_options(pipeline-cache-test PRIVATE ${VPC_CXX_FLAGS})

enable_testing()
add_test(NAME pipeline-cache-system-test COMMAND pipeline-cache-test)
