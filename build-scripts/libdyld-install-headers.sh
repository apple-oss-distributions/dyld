#!/bin/sh

set -x

dlfcnUnifdef()
{
    unifdef "$1"UNIFDEF_DRIVERKIT -o "$2" ${SRCROOT}/include/dlfcn.h
    EXIT_CODE=$? # man: The unifdef utility exits 0 if the output is an exact copy of the input, 1 if not, and 2 if in trouble.
    if [ \( $EXIT_CODE -eq 0 \) -o \( $EXIT_CODE -eq 2 \) ]; then
        echo "unifdef failed and returned $EXIT_CODE"
        exit 1
    fi
}

PLATFORM_SDK="macosx.internal"

# Check that a header can be parsed as C, not C++
checkHeader()
{
    for ARCH in $ARCHS
    do
        xcrun -sdk ${PLATFORM_SDK} clang -x c "$1" -fsyntax-only -Wno-visibility -arch ${ARCH} -I${DSTROOT}${SYSTEM_PREFIX}${PUBLIC_HEADERS_FOLDER_PATH}  -I${DSTROOT}${SYSTEM_PREFIX}${PRIVATE_HEADERS_FOLDER_PATH} -I${DSTROOT}${SYSTEM_PREFIX}/usr/include/ -I${DSTROOT}${SYSTEM_PREFIX}/usr/local/include/
    done
}

if [ "${DRIVERKIT}" == 1 ]; then
    # dyld.h and dyld_priv.h are not for use by actual drivers, so they are both in the Runtime directory
    mkdir -p ${DSTROOT}/${PUBLIC_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/dyld.h ${DSTROOT}/${PUBLIC_HEADERS_FOLDER_PATH}/dyld.h

    # only some of dlfcn.h symbols are for use by drivers
    mkdir -p ${DSTROOT}${DRIVERKITROOT}/usr/include/
    dlfcnUnifdef -D ${DSTROOT}${DRIVERKITROOT}/usr/include/dlfcn.h
    mkdir -p ${DSTROOT}${DRIVERKITROOT}/Runtime/usr/include/
    dlfcnUnifdef -U ${DSTROOT}${DRIVERKITROOT}/Runtime/usr/include/dlfcn.h
elif [ -n "${SYSTEM_PREFIX}" ]; then # ExclaveKit
    mkdir -p ${DSTROOT}${SYSTEM_PREFIX}${PUBLIC_HEADERS_FOLDER_PATH}
    mkdir -p ${DSTROOT}${SYSTEM_PREFIX}${PRIVATE_HEADERS_FOLDER_PATH}

    cp ${SRCROOT}/include/mach-o/dyld.h ${DSTROOT}${SYSTEM_PREFIX}${PUBLIC_HEADERS_FOLDER_PATH}/dyld.h
    cp ${SRCROOT}/include/mach-o/dyld_priv.h ${DSTROOT}${SYSTEM_PREFIX}${PRIVATE_HEADERS_FOLDER_PATH}/dyld_priv.h

    cp ${SRCROOT}/include/dlfcn.h  ${DSTROOT}${SYSTEM_PREFIX}/usr/include/dlfcn.h

    cp ${SRCROOT}/include/mach-o/dyld_exclavekit.modulemap  ${DSTROOT}${SYSTEM_PREFIX}${PUBLIC_HEADERS_FOLDER_PATH}/dyld.modulemap
    cp ${SRCROOT}/include/mach-o/dyld_exclavekit.private.modulemap  ${DSTROOT}${SYSTEM_PREFIX}${PRIVATE_HEADERS_FOLDER_PATH}/dyld.private.modulemap

    checkHeader ${DSTROOT}${SYSTEM_PREFIX}${PRIVATE_HEADERS_FOLDER_PATH}/dyld_priv.h
else
    mkdir -p ${DSTROOT}${PUBLIC_HEADERS_FOLDER_PATH}
    mkdir -p ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}

    # public
    cp ${SRCROOT}/include/mach-o/dyld.h ${DSTROOT}${PUBLIC_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/dyld_images.h ${DSTROOT}${PUBLIC_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/fixup-chains.h ${DSTROOT}${PUBLIC_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/utils.h ${DSTROOT}${PUBLIC_HEADERS_FOLDER_PATH}

    #private
    cp ${SRCROOT}/include/mach-o/dyld-interposing.h ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/dyld_introspection.h ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/dyld_process_info.h ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/utils_priv.h ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}

    cp ${SRCROOT}/cache-builder/dyld_cache_format.h ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/objc-shared-cache.h ${DSTROOT}/usr/local/include

    # dlfcn
    dlfcnUnifdef -U ${DSTROOT}/usr/include/dlfcn.h
    cp ${SRCROOT}/include/dlfcn_private.h  ${DSTROOT}/usr/local/include

    #  manual install of modulemap
    cp ${SRCROOT}/include/mach-o/dyld.modulemap  ${DSTROOT}${PUBLIC_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/dyld.private.modulemap  ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}

    # dyld_priv.h is generated in libdyld-generation-headers
    checkHeader ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}/dyld_priv.h
fi

#echo "Installed headers"
