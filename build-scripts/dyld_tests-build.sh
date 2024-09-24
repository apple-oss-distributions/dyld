#!/bin/sh

set -e

source $SRCROOT/build-scripts/include.sh

# Exit on failure

OBJROOT_DRIVERKIT="${TARGET_TEMP_DIR}/driverkit"

SYMROOT=${BUILD_DIR}/${CONFIGURATION}${EFFECTIVE_PLATFORM_NAME}/dyld_tests
SYMROOT_DRIVERKIT=${BUILD_DIR}/${CONFIGURATION}${EFFECTIVE_PLATFORM_NAME}/dyld_tests-driverkit
OBJROOT=${PROJECT_TEMP_DIR}/${CONFIGURATION}${EFFECTIVE_PLATFORM_NAME}
SDKROOT=${SDKROOT:-$(xcrun -sdk macosx.internal --show-sdk-path)}

case "$PLATFORM_NAME" in
    "bridgeos")
        DK_SDK="driverkit.macosx.internal";;
    *)
        DK_SDK="driverkit.${PLATFORM_NAME}.internal";;
esac
DK_SDKROOT=$(xcrun -sdk ${DK_SDK} --show-sdk-path)
DK_SYSTEM_HEADER_SEARCH_PATHS="${DK_SDKROOT}/System/DriverKit/Runtime/usr/include/"


DERIVED_FILES_DIR=${DERIVED_FILES_DIR}
LDFLAGS="-L$BUILT_PRODUCTS_DIR"

xcodebuild install -target libdyld_driverkit OBJROOT="${OBJROOT_DRIVERKIT}" SYMROOT="${SYMROOT_DRIVERKIT}" DSTROOT="$BUILT_PRODUCTS_DIR-driverkit" -sdk $DK_SDK
DK_LDFLAGS="-L$BUILT_PRODUCTS_DIR-driverkit/System/DriverKit/usr/lib/system/"

#LLBUILD=$(xcrun --sdk $SDKROOT --find llbuild 2> /dev/null)
NINJA=${LLBUILD:-`xcrun  --sdk $SDKROOT --find ninja  2> /dev/null`}
BUILD_TARGET=${ONLY_BUILD_TEST:-all}

if [ ! -z "$LLBUILD" ]; then
  NINJA="$LLBUILD ninja build"
fi

if [ -z "$DEPLOYMENT_TARGET_SETTING_NAME" ]; then
    echo "Error $$DEPLOYMENT_TARGET_SETTING_NAME must be set"
    exit 1
fi
OSVERSION=${!DEPLOYMENT_TARGET_SETTING_NAME}

if [ -z "$PLATFORM_NAME" ]; then
    echo "Error $$PLATFORM_NAME must be set"
    exit 1
fi

if [ -z "$SRCROOT" ]; then
    echo "Error $$SRCROOT must be set"
    exit 1
fi

if [ -z "$ARCHS" ]; then
    PLATFORM_NAME=${PLATFORM_NAME:macosx}
    case "$PLATFORM_NAME" in
       "watchos")   ARCHS="armv7k arm64_32"
       ;;
       "appletvos") ARCHS="arm64"
       ;;
       *)    ARCHS=${ARCHS_STANDARD}
       ;;
    esac
fi

if [ -z "$ARCHS" ]; then
    ARCHS="x86_64"
fi

/bin/mkdir -p ${DERIVED_FILES_DIR}
TMPFILE=$(mktemp ${DERIVED_FILES_DIR}/config.ninja.XXXXXX)

echo "OBJROOT = $OBJROOT" >> $TMPFILE
echo "OSPLATFORM = $PLATFORM_NAME" >> $TMPFILE
echo "OSVERSION = $OSVERSION" >> $TMPFILE
echo "SDKROOT = $SDKROOT" >> $TMPFILE
echo "DK_SDKROOT = $DK_SDKROOT" >> $TMPFILE
echo "DK_SYSTEM_HEADER_SEARCH_PATHS = $DK_SYSTEM_HEADER_SEARCH_PATHS" >> $TMPFILE
echo "SRCROOT = $SRCROOT" >> $TMPFILE
echo "SYMROOT = $SYMROOT" >> $TMPFILE
echo "BUILT_PRODUCTS_DIR = $BUILT_PRODUCTS_DIR" >> $TMPFILE
echo "INSTALL_GROUP = $INSTALL_GROUP" >> $TMPFILE
echo "INSTALL_MODE_FLAG = $INSTALL_MODE_FLAG" >> $TMPFILE
echo "INSTALL_OWNER = $INSTALL_OWNER" >> $TMPFILE
echo "INSTALL_DIR = $INSTALL_DIR" >> $TMPFILE
echo "USER_HEADER_SEARCH_PATHS = $USER_HEADER_SEARCH_PATHS" >> $TMPFILE
echo "SYSTEM_HEADER_SEARCH_PATHS = $SYSTEM_HEADER_SEARCH_PATHS" >> $TMPFILE
echo "ARCHS = $ARCHS" >> $TMPFILE
echo "DERIVED_FILES_DIR = $DERIVED_FILES_DIR" >> $TMPFILE
echo "LDFLAGS = $LDFLAGS" >> $TMPFILE
echo "DK_LDFLAGS = $DK_LDFLAGS" >> $TMPFILE

/usr/bin/rsync -vc $TMPFILE ${DERIVED_FILES_DIR}/config.ninja
/bin/rm -f $TMPFILE

${SRCROOT}/testing/build_ninja.py ${DERIVED_FILES_DIR}/config.ninja || exit_if_error $? "Generating build.ninja failed"
${NINJA} -k 0 -C ${DERIVED_FILES_DIR} ${BUILD_TARGET} || exit_if_error $? "Ninja build failed"
