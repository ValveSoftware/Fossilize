#!/bin/bash

mode=$1
if [ -z $1 ]; then
	mode=Release
fi

njobs=`nproc`

# Android
if [ -z $ANDROID_SDK ]; then
	ANDROID_SDK="$HOME/Android/Sdk"
fi

# Android armeabi-v7a
mkdir -p build-android-armeabi-v7a
cd build-android-armeabi-v7a

ANDROID_ABI=armeabi-v7a
"$ANDROID_SDK"/cmake/*/bin/cmake \
	.. \
	-DANDROID_STL=c++_static \
	-DANDROID_TOOLCHAIN=clang \
	-DCMAKE_TOOLCHAIN_FILE="$ANDROID_SDK/ndk-bundle/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=$ANDROID_ABI \
	-DANDROID_CPP_FEATURES=exceptions \
	-DANDROID_ARM_MODE=arm \
	-DCMAKE_BUILD_TYPE=$mode

make -j$njobs
cd ..

# Android arm64-v8a
mkdir -p build-android-arm64-v8a
cd build-android-arm64-v8a

ANDROID_ABI=arm64-v8a
"$ANDROID_SDK"/cmake/*/bin/cmake \
	.. \
	-DANDROID_STL=c++_static \
	-DANDROID_TOOLCHAIN=clang \
	-DCMAKE_TOOLCHAIN_FILE="$ANDROID_SDK/ndk-bundle/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=$ANDROID_ABI \
	-DANDROID_CPP_FEATURES=exceptions \
	-DANDROID_ARM_MODE=arm \
	-DCMAKE_BUILD_TYPE=$mode

make -j$njobs
cd ..

