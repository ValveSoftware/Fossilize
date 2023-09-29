cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)
project(Fossilize LANGUAGES CXX C)

option(FOSSILIZE_MSVC_ANALYZE "Enable /analyze" OFF)
option(FOSSILIZE_ENABLE_EXCEPTIONS "Compile Fossilize with exception support." OFF)

if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fvisibility=hidden")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
    if (NOT FOSSILIZE_ENABLE_EXCEPTIONS)
        set(FOSSILIZE_EXCEPTION_FLAGS "-fno-exceptions")
    else()
        set(FOSSILIZE_EXCEPTION_FLAGS "")
    endif()
    set(FOSSILIZE_CXX_FLAGS -Wall -Wextra -Wunused-result -Wno-missing-field-initializers -Wno-empty-body -Wshadow ${FOSSILIZE_EXCEPTION_FLAGS})
elseif (MSVC)
    if (FOSSILIZE_MSVC_ANALYZE)
        set(FOSSILIZE_ANALYZE_FLAGS "/analyze")
    else()
        set(FOSSILIZE_ANALYZE_FLAGS "")
    endif()
    # Disabling exceptions in MSVC without a ton of scary warnings is quite finicky.
    # Don't bother. We can verify no exceptions are present with -fno-exceptions on GCC/Clang.
    set(FOSSILIZE_CXX_FLAGS ${FOSSILIZE_ANALYZE_FLAGS} /D_CRT_SECURE_NO_WARNINGS /D_SCL_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005 /MP /DNOMINMAX)
endif()

option(FOSSILIZE_SANITIZE_ADDRESS "Sanitize address" OFF)
set(FOSSILIZE_LINK_FLAGS)
if (FOSSILIZE_SANITIZE_ADDRESS)
    set(FOSSILIZE_CXX_FLAGS ${FOSSILIZE_CXX_FLAGS} -fsanitize=address)
    set(FOSSILIZE_LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -fsanitize=address")
endif()

option(FOSSILIZE_SANITIZE_THREADS "Sanitize threads" OFF)
if (FOSSILIZE_SANITIZE_THREADS)
    set(FOSSILIZE_CXX_FLAGS ${FOSSILIZE_CXX_FLAGS} -fsanitize=thread)
    set(FOSSILIZE_LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -fsanitize=thread")
endif()

if (CMAKE_COMPILER_IS_GNUCXX AND WIN32)
    # MinGW build.
    set(CMAKE_EXE_LINKER_FLAGS "${FOSSILIZE_LINK_FLAGS} -static -static-libstdc++ -static-libgcc -fstack-protector")
endif()

include(GNUInstallDirs)

add_library(miniz STATIC miniz/miniz.h miniz/miniz.c)
set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/miniz)
if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
    target_compile_options(miniz PRIVATE -fvisibility=hidden)
    if (NOT APPLE AND NOT ANDROID)
        target_compile_definitions(miniz PRIVATE _LARGEFILE64_SOURCE)
    endif()
endif()

add_library(fossilize STATIC
        fossilize.hpp fossilize.cpp
        fossilize_errors.hpp
        fossilize_application_filter.hpp fossilize_application_filter.cpp
        fossilize_types.hpp fossilize_hasher.hpp
        varint.cpp varint.hpp
        fossilize_db.cpp fossilize_db.hpp
        fossilize_inttypes.h
        util/intrusive_list.hpp util/object_pool.hpp util/object_cache.hpp
        path.hpp path.cpp)
set_target_properties(fossilize PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (WIN32 AND CMAKE_COMPILER_IS_GNUCXX)
	target_compile_definitions(fossilize PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()

if (NOT ANDROID)
    target_sources(fossilize PRIVATE fossilize_external_replayer.cpp fossilize_external_replayer.hpp)
endif()

if (WIN32)
    target_sources(fossilize PRIVATE fossilize_external_replayer_windows.hpp)
else()
    target_sources(fossilize PRIVATE fossilize_external_replayer_linux.hpp)
    if (APPLE)
        target_sources(fossilize PRIVATE platform/gcc_clang_spinlock.hpp)
    else()
        target_sources(fossilize PRIVATE platform/futex_wrapper_linux.hpp)
    endif()
endif()

target_include_directories(fossilize PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(fossilize PRIVATE ${FOSSILIZE_CXX_FLAGS})
if (NOT WIN32)
    target_link_libraries(fossilize -pthread)
    target_compile_options(fossilize PUBLIC -pthread)
    if (NOT APPLE)
        if (ANDROID)
            target_link_libraries(fossilize android log)
        else()
            target_link_libraries(fossilize rt)
        endif()
    endif()
endif()
target_link_libraries(fossilize miniz)

if (WIN32)
    target_include_directories(fossilize PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cli/dirent/include)
endif()

option(FOSSILIZE_VULKAN_LAYER "Build Vulkan layer." ON)
if (FOSSILIZE_VULKAN_LAYER)
    set_target_properties(fossilize PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

option(FOSSILIZE_RAPIDJSON_INCLUDE_PATH "Optional custom include path for rapidjson.")
if (FOSSILIZE_RAPIDJSON_INCLUDE_PATH)
    target_include_directories(fossilize PUBLIC ${FOSSILIZE_RAPIDJSON_INCLUDE_PATH})
else()
    target_include_directories(fossilize PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rapidjson/include)
endif()

option(FOSSILIZE_VULKAN_INCLUDE_PATH "Optional custom include path for Vulkan headers.")
if (FOSSILIZE_VULKAN_INCLUDE_PATH)
    target_include_directories(fossilize PUBLIC ${FOSSILIZE_VULKAN_INCLUDE_PATH})
else()
    target_include_directories(fossilize PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/khronos)
endif()

if (FOSSILIZE_VULKAN_LAYER)
    add_subdirectory(layer)
endif()

option(FOSSILIZE_CLI "Enable Fossilize CLI support." ON)
if (FOSSILIZE_CLI)
    add_subdirectory(cli)
endif()

option(FOSSILIZE_TESTS "Enable Fossilize tests." ON)
if (FOSSILIZE_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

