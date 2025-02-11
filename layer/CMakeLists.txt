add_library(VkLayer_fossilize SHARED
	device.cpp
	device.hpp
	instance.cpp
	instance.hpp
	dispatch.cpp
	dispatch_helper.hpp
	dispatch_helper.cpp)

target_include_directories(VkLayer_fossilize PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(VkLayer_fossilize PRIVATE ${FOSSILIZE_CXX_FLAGS})
set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS}")
install(TARGETS VkLayer_fossilize
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

option(FOSSILIZE_LAYER_CAPTURE_SIGSEGV "Support capturing segfaults." ON)
if (FOSSILIZE_LAYER_CAPTURE_SIGSEGV)
	target_compile_definitions(VkLayer_fossilize PRIVATE FOSSILIZE_LAYER_CAPTURE_SIGSEGV)
endif()

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/layer)

# First is a JSON file which can be used directly from the build folder.
if (WIN32)
	set(FOSSILIZE_LAYER_PATH ".\\\\VkLayer_fossilize.dll")
elseif (APPLE)
	set(FOSSILIZE_LAYER_PATH "./libVkLayer_fossilize.dylib")
else()
	set(FOSSILIZE_LAYER_PATH "./libVkLayer_fossilize.so")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_fossilize.json.in ${CMAKE_BINARY_DIR}/layer/VkLayer_fossilize.json @ONLY)

if (WIN32)
	set_target_properties(VkLayer_fossilize PROPERTIES PREFIX "" LIBRARY_OUTPUT_NAME VkLayer_fossilize)
	if (CMAKE_COMPILER_IS_GNUCXX)
		if (${CMAKE_BUILD_TYPE} MATCHES "Release")
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined -static -s")
		else()
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined -static")
		endif()
	endif()
	# Normally, we would have to install the layer into the registry, but that is way too annoying for something like this.
	# Just place the layer along the dll on Windows if we're actually bothering to install.
	configure_file(${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_fossilize.json.in ${CMAKE_BINARY_DIR}/VkLayer_fossilize.json @ONLY)
	install(FILES ${CMAKE_BINARY_DIR}/VkLayer_fossilize.json DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
	if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
		target_compile_options(VkLayer_fossilize PRIVATE -fvisibility=hidden)
	endif()

	if (NOT ANDROID)
		if (APPLE)
			# Second one is for installation, where we want to rely on LD_LIBRARY_PATH
			set(FOSSILIZE_LAYER_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libVkLayer_fossilize.dylib")
		else()
			# Second one is for installation, where we want to rely on LD_LIBRARY_PATH
			set(FOSSILIZE_LAYER_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libVkLayer_fossilize.so")
		endif()
		configure_file(${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_fossilize.json.in ${CMAKE_BINARY_DIR}/VkLayer_fossilize.json @ONLY)
		install(FILES ${CMAKE_BINARY_DIR}/VkLayer_fossilize.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d)
	endif()
	if (${CMAKE_BUILD_TYPE} MATCHES "Release")
		if (APPLE)
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -s")
		else()
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined -s")
		endif()
	else()
		if (NOT APPLE)
			set_target_properties(VkLayer_fossilize PROPERTIES LINK_FLAGS "${FOSSILIZE_LINK_FLAGS} -Wl,--no-undefined")
		endif()
	endif()
endif()

set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_fossilize PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/layer")
target_link_libraries(VkLayer_fossilize fossilize)

if (ANDROID)
	target_link_libraries(VkLayer_fossilize log)
	option(FOSSILIZE_LAYER_APK "Enable Fossilize layer APK build." OFF)
	if (FOSSILIZE_LAYER_APK)
		set(ANDROID_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
		set(ANDROID_BUILD_DIR "${CMAKE_BINARY_DIR}/android")
		set(ANDROID_GRADLE "${ANDROID_BUILD_DIR}/gradlew")
		if (WIN32)
			set(ANDROID_GRADLE "${ANDROID_GRADLE}.bat")
		endif()

		option(ANDROID_SDK "Path to the Android SDK" "")
		if (NOT ANDROID_SDK)
			if (DEFINED ENV{ANDROID_SDK})
				set(ANDROID_SDK "$ENV{ANDROID_SDK}")
			else()
				message(FATAL_ERROR "ANDROID_SDK must be set either as a CMake option or an environment variable.")
			endif()
		endif()
		get_filename_component(ANDROID_SDK_PATH "${ANDROID_SDK}" ABSOLUTE)

		file(COPY ${ANDROID_PROJECT_DIR} DESTINATION ${CMAKE_BINARY_DIR})

		add_custom_target(copy_layer_so ALL
			COMMAND ${CMAKE_COMMAND} -E copy
			${CMAKE_BINARY_DIR}/layer/libVkLayer_fossilize.so
			${ANDROID_BUILD_DIR}/app/src/main/jniLibs/arm64-v8a/libVkLayer_fossilize.so
			DEPENDS VkLayer_fossilize
		)

		add_custom_target(build_apk ALL
			COMMAND ${CMAKE_COMMAND} -E env "ANDROID_HOME=${ANDROID_SDK_PATH}" ${ANDROID_GRADLE} assembleDebug
			WORKING_DIRECTORY ${ANDROID_BUILD_DIR}
			DEPENDS copy_layer_so
		)

		add_custom_target(copy_apk ALL
			COMMAND ${CMAKE_COMMAND} -E copy
			${ANDROID_BUILD_DIR}/app/build/outputs/apk/debug/app-debug.apk
			${CMAKE_BINARY_DIR}/fossilize.apk
			DEPENDS build_apk
		)
	endif()
endif()
