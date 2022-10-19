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

if [ "${DRIVERKIT}" = 1 ]; then
    # dyld.h and dyld_priv.h are not for use by actual drivers, so they are both in the Runtime directory
    mkdir -p ${DSTROOT}/${PUBLIC_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/dyld.h ${DSTROOT}/${PUBLIC_HEADERS_FOLDER_PATH}/dyld.h

    # only some of dlfcn.h symbols are for use by drivers
    mkdir -p ${DSTROOT}${DRIVERKITROOT}/usr/include/
    dlfcnUnifdef -D ${DSTROOT}${DRIVERKITROOT}/usr/include/dlfcn.h
    mkdir -p ${DSTROOT}${DRIVERKITROOT}/Runtime/usr/include/
    dlfcnUnifdef -U ${DSTROOT}${DRIVERKITROOT}/Runtime/usr/include/dlfcn.h
elif [ -n "${SYSTEM_PREFIX}" ]; then
    mkdir -p ${DSTROOT}${SYSTEM_PREFIX}/usr/include/mach-o
    cp ${SRCROOT}/include/mach-o/dyld.h ${DSTROOT}${SYSTEM_PREFIX}/usr/include/mach-o/dyld.h
    cp ${SRCROOT}/include/mach-o/dyld_priv.h ${DSTROOT}${SYSTEM_PREFIX}/usr/include/mach-o/dyld_priv.h
    cp ${SRCROOT}/include/dlfcn.h  ${DSTROOT}${SYSTEM_PREFIX}/usr/include/dlfcn.h
else
    # xcode only lets you install public headers to one directory
    dlfcnUnifdef -U ${DSTROOT}/usr/include/dlfcn.h
    cp ${SRCROOT}/include/dlfcn_private.h  ${DSTROOT}/usr/local/include
    #  manual install of modulemap
    cp ${SRCROOT}/include/mach-o/dyld.modulemap  ${DSTROOT}${PUBLIC_HEADERS_FOLDER_PATH}
    cp ${SRCROOT}/include/mach-o/dyld.private.modulemap  ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}
fi

#echo "Installed headers"
