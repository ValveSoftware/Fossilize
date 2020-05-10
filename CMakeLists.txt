cmake_minimum_required(VERSION 3.5)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
project(Fossilize LANGUAGES CXX C)

set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_THREAD_PREFER_PTHREAD ON)

include(CMakeDependentOption)
include(GNUInstallDirs)
include(CTest)

find_package(Threads REQUIRED)

if (NOT DEFINED CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 14)
  set(CMAKE_CXX_STANDARD 14)
endif()
if (NOT DEFINED CMAKE_C_STANDARD OR CMAKE_C_STANDARD LESS 99)
  set(CMAKE_C_STANDARD 99)
endif()

if (NOT DEFINED CMAKE_CXX_VISIBILITY_PRESET)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
endif()

if (NOT DEFINED CMAKE_C_VISIBILITY_PRESET)
  set(CMAKE_C_VISIBILITY_PRESET hidden)
endif()

cmake_dependent_option(FOSSILIZE_TESTS
  "Enable Fossilize tests" ON
  "CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME;BUILD_TESTING" OFF)

option(FOSSILIZE_ENABLE_EXCEPTIONS "Compile Fossilize with exception support." OFF)
option(FOSSILIZE_MSVC_ANALYZE "Enable /analyze" OFF)
# Not needed for position-independent-code, as its always turned on
option(FOSSILIZE_VULKAN_LAYER "Build Vulkan layer." ON) 
option(FOSSILIZE_CLI "Enable Fossilize CLI support." ON)

option(FOSSILIZE_SANITIZE_ADDRESS "Sanitize address" OFF)
option(FOSSILIZE_SANITIZE_THREADS "Sanitize threads" OFF)

# option() cannot technically be used in newer CMake versions for non-boolean values
# warnings will be thrown and this can be annoying otherwise.
set(FOSSILIZE_VULKAN_INCLUDE_PATH "${PROJECT_SOURCE_DIR}/khronos/vulkan"
  CACHE PATH "include path for vulkan headers")
set(FOSSILIZE_RAPIDJSON_INCLUDE_PATH "${PROJECT_SOURCE_DIR}/rapidjson/include"
  CACHE PATH "include path for rapidjson headers")

# TODO: Gate all of this behind a better generator expression
if (CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND FOSSILIZE_CXX_FLAGS
      -Wall -Wextra -Wunused-result -Wno-missing-field-initializers -Wno-empty-body -Wshadow
      $<$<NOT:$<BOOL:${FOSSILIZE_ENABLE_EXCEPTIONS}>>:-fno-exceptions>)
elseif (MSVC)
    # XXX: This can be done, and safely, but will require more work and a more recent version of CMake
    # Disabling exceptions in MSVC without a ton of scary warnings is quite finicky.
    # Don't bother. We can verify no exceptions are present with -fno-exceptions on GCC/Clang.
    list(APPEND FOSSILIZE_CXX_FLAGS
      $<$<BOOL:${FOSSILIZE_MSVC_ANALYZE}>:/analyze>
      /wd4267
      /wd4244
      /wd4309
      /wd4005
      /MP)
endif()

# TODO: This does not currently support the MSVC support for AddressSanitizer
if (FOSSILIZE_SANITIZE_ADDRESS)
    list(APPEND FOSSILIZE_CXX_FLAGS -fsanitize=address)
    list(APPEND FOSSILIZE_LINK_FLAGS -fsanitize=address)
endif()

if (FOSSILIZE_SANITIZE_THREADS)
    list(APPEND FOSSILIZE_CXX_FLAGS -fsanitize=thread)
    list(APPEND FOSSILIZE_LINK_FLAGS -fsanitize=thread)
endif()

# Temporary variables to mmake some generator expressions more readable


if (NOT TARGET miniz)
  add_library(miniz STATIC miniz/miniz.h miniz/miniz.c)
  set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(miniz
    PUBLIC
      $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/miniz>)
  string(CONCAT cxx-is-clang $<OR:
    $<CXX_COMPILER_ID:AppleClang>,
    $<CXX_COMPILER_ID:Clang>
  >)
  set(cxx-is-gcc $<CXX_COMPILER_ID:GNU>)
  string(CONCAT is-darwin-or-android $<OR:
    $<PLATFORM_ID:Darwin>,
    $<PLATFORM_ID:Android>
  >)

  string(CONCAT largefile $<AND:
    $<OR:${cxx-is-clang},${cxx-is-gcc}>,
    $<NOT:${is-darwin-or-android}>
  >)
  target_compile_definitions(miniz PRIVATE $<${largefile}:_LARGEFILE64_SOURCE>)
endif()

add_library(fossilize STATIC
        fossilize.hpp fossilize.cpp
        fossilize_errors.hpp
        fossilize_application_filter.hpp fossilize_application_filter.cpp
        fossilize_types.hpp
        varint.cpp varint.hpp
        fossilize_db.cpp fossilize_db.hpp
        fossilize_inttypes.h
        util/intrusive_list.hpp util/object_pool.hpp util/object_cache.hpp
        path.hpp path.cpp)
set_target_properties(fossilize PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(fossilize
  PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
    $<$<CXX_COMPILER_ID:MSVC>:_SCL_SECURE_NO_WARNINGS>
    $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>
    $<$<AND:$<PLATFORM_ID:Windows>,$<CXX_COMPILER_ID:GNU>>:__USE_MINGW_ANSI_STDIO=1>)
target_sources(fossilize
  PRIVATE
    $<$<NOT:$<PLATFORM_ID:Android>>:fossilize_external_replayer.cpp>
    $<$<NOT:$<PLATFORM_ID:Android>>:fossilize_external_replayer.hpp>
    $<$<NOT:$<PLATFORM_ID:Windows>>:fossilize_external_replayer_linux.hpp>
    $<$<PLATFORM_ID:Windows>:fossilize_external_replayer_windows.hpp>
    $<$<PLATFORM_ID:Darwin>:platform/gcc_clang_spinlock.hpp>
    $<$<PLATFORM_ID:Linux>:platform/futex_wrapper_linux.hpp>)
target_include_directories(fossilize
  PUBLIC
    $<BUILD_INTERFACE:$<$<PLATFORM_ID:Windows>:${PROJECT_SOURCE_DIR}/cli/dirent/include>>
    $<BUILD_INTERFACE:${FOSSILIZE_RAPIDJSON_INCLUDE_PATH}>
    $<BUILD_INTERFACE:${FOSSILIZE_VULKAN_INCLUDE_PATH}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(fossilize PRIVATE ${FOSSILIZE_CXX_FLAGS})
target_link_libraries(fossilize
  PRIVATE
    Threads::Threads
    $<$<PLATFORM_ID:Android>:android>
    $<$<PLATFORM_ID:Android>:log>
    $<$<PLATFORM_ID:Linux>:rt>
    miniz)

if (CMAKE_VERSION VERSION_LESS 3.13)
  string(REPLACE ";" " " FOSSILIZE_LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
  set_property(TARGET fossilize APPEND_STRING PROPERTY LINK_FLAGS " ${FOSSILIZE_LINK_FLAGS}")
else()
  target_link_options(fossilize PRIVATE ${FOSSILIZE_LINK_FLAGS})
endif()

if (FOSSILIZE_VULKAN_LAYER)
    add_subdirectory(layer)
endif()

if (FOSSILIZE_CLI)
    add_subdirectory(cli)
endif()

if (FOSSILIZE_TESTS)
    add_subdirectory(test)
endif()
