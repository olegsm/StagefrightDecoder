#!/bin/sh

BUILD_TARGET_ABI=$1
BUILD_TOOLS_PATH=$2

BUILD_HOST_OS=`uname -s | tr '[:upper:]' '[:lower:]'`
BUILD_HOST_MACHINE=`uname -m | tr '[:upper:]' '[:lower:]'`
BUILD_HOST_PLATFORM=`getconf LONG_BIT`

MAKEFLAGS="V=1 -j8"

ROOT_DIR=`pwd`
TOOLS_DIR=$ROOT_DIR/tools
CC_VER=4.8
LIB_NAME="libMediaCodecStagefright"

checkfail()
{
    if [ ! $? -eq 0 ];then
        echo "$1"
        exit 1
    fi
}

unpack_android_framework()
{
    ANDROID_FRAMEWORK_DIR=$TOOLS_DIR/android_framework

    if [ ! -d $ANDROID_FRAMEWORK_DIR ]; then
        echo "Unpack android framework..."
        tar -xzvf android_framework.tar.gz -C $TOOLS_DIR
    fi

    export ANDROID_SYS_HEADERS_GINGERBREAD=${ANDROID_FRAMEWORK_DIR}/android-headers-gingerbread
    export ANDROID_SYS_HEADERS_HC=${ANDROID_FRAMEWORK_DIR}/android-headers-hc
    export ANDROID_SYS_HEADERS_ICS=${ANDROID_FRAMEWORK_DIR}/android-headers-ics
    export ANDROID_SYS_HEADERS_JBMR2=${ANDROID_FRAMEWORK_DIR}/android-headers-jbmr2
    export ANDROID_SYS_HEADERS_KK=${ANDROID_FRAMEWORK_DIR}/android-headers-kk
    export ANDROID_SYS_HEADERS_LL=${ANDROID_FRAMEWORK_DIR}/android-headers-ll

    export ANDROID_LIBS=${ANDROID_FRAMEWORK_DIR}/android-libs-device/${BUILD_TARGET_ABI}

    echo "********* Frameworks ***********"
    echo "ANDROID_SYS_HEADERS_GINGERBREAD   : $ANDROID_SYS_HEADERS_GINGERBREAD"
    echo "ANDROID_SYS_HEADERS_HC            : $ANDROID_SYS_HEADERS_HC"
    echo "ANDROID_SYS_HEADERS_ICS           : $ANDROID_SYS_HEADERS_ICS"
    echo "ANDROID_SYS_HEADERS_JBMR2         : $ANDROID_SYS_HEADERS_JBMR2"
    echo "ANDROID_SYS_HEADERS_KK            : $ANDROID_SYS_HEADERS_KK"
    echo "ANDROID_SYS_HEADERS_LL            : $ANDROID_SYS_HEADERS_LL"
    echo "ANDROID_LIBS                      : $ANDROID_LIBS"
}

setup()
{
    echo "Setup toolchains under $BUILD_TARGET..."
    NDK_ROOT=$1

    export TOOLCHAIN_DIR=$TOOLS_DIR/'android-toolchain-gcc'$CC_VER
    mkdir -p $TOOLCHAIN_DIR

    # Setup ABI variables
    if [ ${BUILD_TARGET_ABI} = "x86" ] ; then
        TARGET_TUPLE="i686-linux-android"
        TARGET_TOOLCHAIN="x86"
        PLATFORM_SHORT_ARCH="x86"
    elif [ ${BUILD_TARGET_ABI} = "mips" ] ; then
        TARGET_TUPLE="mipsel-linux-android"
        TARGET_TOOLCHAIN=$TARGET_TUPLE
        PLATFORM_SHORT_ARCH="mips"
    elif [ ${BUILD_TARGET_ABI} = "armeabi" ] ; then
        TARGET_TUPLE="arm-linux-androideabi"
        TARGET_TOOLCHAIN=$TARGET_TUPLE
        PLATFORM_SHORT_ARCH="arm"
    else
        echo "Unknown ABI. Die, die, die!"
        exit 2
    fi

    export BUILD_TARGET_API="android-14"

    if [ ! -d $TOOLCHAIN_DIR/$TARGET_TOOLCHAIN ]; then
        echo "Creating toolchains dir under $TARGET_TUPLE / $TARGET_TOOLCHAIN"
         $NDK_ROOT/build/tools/make-standalone-toolchain.sh --platform=$BUILD_TARGET_API --install-dir=$TOOLCHAIN_DIR/$TARGET_TOOLCHAIN --toolchain=$TARGET_TOOLCHAIN-$CC_VER --system=$BUILD_HOST_OS-$BUILD_HOST_MACHINE

         if [ ! -d $TOOLCHAIN_DIR/$TARGET_TOOLCHAIN ]; then
             $NDK_ROOT/build/tools/make-standalone-toolchain.sh --platform=$BUILD_TARGET_API --install-dir=$TOOLCHAIN_DIR/$TARGET_TOOLCHAIN --toolchain=$TARGET_TOOLCHAIN-$CC_VER --system=$BUILD_HOST_OS-x86 || exit 1
         fi
    fi

    export TARGET_TUPLE=$TARGET_TUPLE
    export TARGET_TOOLCHAIN=$TARGET_TOOLCHAIN
    export PLATFORM_SHORT_ARCH=$PLATFORM_SHORT_ARCH

    export PATH=$PATH:$TOOLCHAIN_DIR/$TARGET_TOOLCHAIN/bin
    export SYSROOT=$TOOLCHAIN_DIR/$TARGET_TOOLCHAIN/sysroot

    export CC=$TARGET_TUPLE-gcc
    export CXX=$TARGET_TUPLE-g++
    export AR=$TARGET_TUPLE-ar
    export RANLIB=$TARGET_TUPLE-ranlib
    export NM=$TARGET_TUPLE-nm
    export LD=$TARGET_TUPLE-ld
    export STRIP=$TARGET_TUPLE-strip
    export OBJCOPY=$TARGET_TUPLE-objcopy

    echo "********* Toolchains ***********"
    echo "PATH      : $PATH"
    echo "SYSROOT   : $SYSROOT"
    echo "CC        : $CC"
    echo "CXX       : $CXX"
    echo "AR        : $AR"
    echo "RANLIB    : $RANLIB"
    echo "NM        : $NM"
    echo "LD        : $LD"
    echo "STRIP     : $STRIP"

    unpack_android_framework
}

cleanup()
{
    if [ -d $TOOLCHAIN_DIR ]; then
        echo "Removing toolchains dir..."
        rm -rf $TOOLCHAIN_DIR
        unset $TOOLCHAIN_DIR
    fi
}

main()
{
    echo "Usage: ./build.sh [toolchains_path]"
    setup $BUILD_TOOLS_PATH

    echo "Building library..."
    LIB_TARGETS="ICS JBMR2 KK"

    cd $ROOT_DIR/$LIB_NAME && $NDK_ROOT/ndk-build -C $ROOT_DIR/$LIB_NAME \
        ANDROID_SYS_HEADERS=${ANDROID_FRAMEWORK_DIR} \
        LIB_TARGETS="$LIB_TARGETS" \
        APP_BUILD_SCRIPT=jni/Android.mk \
        APP_PLATFORM=${BUILD_TARGET_API} \
        APP_ABI=${ANDROID_ABI} \
        NDK_PROJECT_PATH=jni CXXSTL=${CC_VER} V=1
    checkfail "ndk-build failed"
    return 0;
}

main

