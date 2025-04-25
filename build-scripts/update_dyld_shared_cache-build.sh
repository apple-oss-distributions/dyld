set -e # exit when any command fails
set -v # verbose

# override min macOS deployment target
MACOSX_DEPLOYMENT_TARGET=13.0

if [ "${RC_PURPLE}" = "" ]
then
    # macOS platform
    OBJROOT_BDR="${TARGET_TEMP_DIR}/Objects_shared"
    TARGETS=""
    TARGETS+=" -target dyld_shared_cache_builder"
    TARGETS+=" -target update_dyld_shared_cache_tool"
    TARGETS+=" -target libslc_builder.dylib"
    TARGETS+=" -target dyld_symbols_cache"
    TARGETS+=" -target dsc_extractor"
    TARGETS+=" -target libKernelCollectionBuilder"
    xcodebuild ${ACTION} $TARGETS OBJROOT="${OBJROOT_BDR}" SDKROOT="${SDKROOT}" MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} SRCROOT="${SRCROOT}" DSTROOT="${DSTROOT}" SYMROOT="${SYMROOT}" RC_ProjectSourceVersion="${RC_ProjectSourceVersion}"  DISABLE_SDK_METADATA_PARSING=YES  RC_PLATFORM_INSTALL_PATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform RC_ARCHS="arm64 arm64e x86_64"

    # move build results to host locations
    if [ "${ACTION}" == "install" ]
    then
        # install the kernel linker twice, once rpath based in toolchain and once in /usr/lib/
        ditto "${DSTROOT}/usr/lib/libKernelCollectionBuilder.dylib" "${DSTROOT}/${DT_TOOLCHAIN_DIR}/usr/lib/"
        install_name_tool -id "@rpath/libKernelCollectionBuilder.dylib" "${DSTROOT}/${DT_TOOLCHAIN_DIR}/usr/lib/libKernelCollectionBuilder.dylib"

        # HACK: somehow the toolchain TBD is missing even though we copy the dylib.  Make another TBD
        xcodebuild installapi -target libKernelCollectionBuilder OBJROOT="${OBJROOT_BDR}" SDKROOT="${SDKROOT}" MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} SRCROOT="${SRCROOT}" DSTROOT="${DSTROOT}" SYMROOT="${SYMROOT}" RC_ProjectSourceVersion="${RC_ProjectSourceVersion}"  DISABLE_SDK_METADATA_PARSING=YES  KERNEL_LINKER_INSTALL_PATH="${DT_TOOLCHAIN_DIR}/usr/lib/" KERNEL_LINKER_INSTALL_NAME="@rpath/libKernelCollectionBuilder.dylib" RC_ARCHS="arm64 arm64e x86_64"
    fi

    # copy performance files from SDK to platform
    if [ -r "${SDKROOT}/AppleInternal/DirtyDataFiles/dirty-data-segments-order.txt" ]; then
        mkdir -p "${DSTROOT}/usr/local/bin"
        cp "${SDKROOT}/AppleInternal/DirtyDataFiles/dirty-data-segments-order.txt"  "${DSTROOT}/usr/local/bin"
    fi
    if [ -r "${SDKROOT}/AppleInternal/OrderFiles/dylib-order.txt" ]; then
        mkdir -p "${DSTROOT}/usr/local/bin"
        cp "${SDKROOT}/AppleInternal/OrderFiles/dylib-order.txt"  "${DSTROOT}/usr/local/bin"
    fi

else
    # for iOS/tvOS/watchOS/bridgeOS platform, build "host" tools
    TC=$(basename $TOOLCHAIN_DIR)
    CANON_TOOLCHAIN_DIR="/Applications/Xcode.app/Contents/Developer/Toolchains/${TC}"

    OBJROOT_BDR="${TARGET_TEMP_DIR}/Objects_shared"

    TARGETS=""
    TARGETS+=" -target dyld_shared_cache_builder"
    TARGETS+=" -target libslc_builder.dylib"
    TARGETS+=" -target dyld_symbols_cache"
    TARGETS+=" -target dyld_shared_cache_util"
    TARGETS+=" -target libdsc"
    TARGETS+=" -target dsc_extractor"
    TARGETS+=" -target libKernelCollectionBuilder"

    # no simulator for bridgeOS
    if [ "${RC_BRIDGE}" != "YES" ]
    then
        TARGETS+=" -target update_dyld_sim_shared_cache"
    fi

    xcodebuild ${ACTION} $TARGETS OBJROOT="${OBJROOT_BDR}" SDKROOT="${SDKROOT}" MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET} SRCROOT="${SRCROOT}" DSTROOT="${DSTROOT}" SYMROOT="${SYMROOT}" RC_ProjectSourceVersion="${RC_ProjectSourceVersion}"  DISABLE_SDK_METADATA_PARSING=YES KERNEL_LINKER_INSTALL_PATH="${CANON_TOOLCHAIN_DIR}/usr/lib/" KERNEL_LINKER_INSTALL_NAME="@rpath/libKernelCollectionBuilder.dylib" RC_ARCHS="arm64 arm64e x86_64"

    # move roots to platform dir
    if [ -e ${DSTROOT}/usr/local/include ]
    then
        mkdir -p "${DSTROOT}/${RC_PLATFORM_INSTALL_PATH}/"
        cp -R "${DSTROOT}/usr" "${DSTROOT}/${RC_PLATFORM_INSTALL_PATH}/"
        rm -r "${DSTROOT}/usr"
    fi

    # copy performance files from SDK to platform
    if [ -r "${ARM_SDK}/AppleInternal/DirtyDataFiles/dirty-data-segments-order.txt" ];
    then
        mkdir -p "${DSTROOT}/${RC_PLATFORM_INSTALL_PATH}/usr/local/bin"
        cp "${ARM_SDK}/AppleInternal/DirtyDataFiles/dirty-data-segments-order.txt"  "${DSTROOT}/${RC_PLATFORM_INSTALL_PATH}/usr/local/bin"
    fi
    if [ -r "${ARM_SDK}/AppleInternal/OrderFiles/dylib-order.txt" ];
    then
        mkdir -p "${DSTROOT}/${RC_PLATFORM_INSTALL_PATH}/usr/local/bin"
        cp "${ARM_SDK}/AppleInternal/OrderFiles/dylib-order.txt"  "${DSTROOT}/${RC_PLATFORM_INSTALL_PATH}/usr/local/bin"
    fi

fi

