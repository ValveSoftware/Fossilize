#!/bin/bash

mode=$1
if [ -z $1 ]; then
	mode=Release
fi

njobs=`nproc`

# Android
if [ -z $ANDROID_SDK ]; then
	ANDROID_SDK="$HOME/Android"
fi

# Android arm64-v8a
mkdir -p build-android-arm64-v8a
cd build-android-arm64-v8a

if [ -z $ANDROID_NDK ]; then
	echo "Must specify path to NDK in ANDROID_NDK".
	exit 1
fi

unset ANDROID_HOME
echo "Using ANDROID_SDK=$ANDROID_SDK"
echo "Using ANDROID_NDK=$ANDROID_NDK"

ANDROID_ABI=arm64-v8a
cmake .. \
	-DANDROID_STL=c++_static \
	-DANDROID_TOOLCHAIN=clang \
	-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
	-DANDROID_ABI=$ANDROID_ABI \
	-DANDROID_CPP_FEATURES=exceptions \
	-DANDROID_ARM_MODE=arm \
	-DCMAKE_BUILD_TYPE=$mode \
	-DANDROID_PLATFORM=30 \
	-DFOSSILIZE_LAYER_APK=ON -G Ninja

ninja
cd ..

