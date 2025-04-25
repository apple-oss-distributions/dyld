#!/bin/sh

if [ "${DRIVERKIT}" == 1 ]; then
    # do nothing
    exit 0
elif [ -n "${SYSTEM_PREFIX}" ]; then # ExclaveKit
    # do nothing
    exit 0
fi

MAN_ONE_DIR=${DSTROOT}/usr/share/man/man1
mkdir -p ${MAN_ONE_DIR}

MAN_THREE_DIR=${DSTROOT}/usr/share/man/man3
mkdir -p ${MAN_THREE_DIR}

cp ${SRCROOT}/doc/man/man1/dyld.1 ${MAN_ONE_DIR}
cp ${SRCROOT}/doc/man/man3/dladdr.3 ${MAN_THREE_DIR}
cp ${SRCROOT}/doc/man/man3/dlclose.3 ${MAN_THREE_DIR}
cp ${SRCROOT}/doc/man/man3/dlerror.3 ${MAN_THREE_DIR}
cp ${SRCROOT}/doc/man/man3/dlsym.3 ${MAN_THREE_DIR}
cp ${SRCROOT}/doc/man/man3/dlopen.3 ${MAN_THREE_DIR}
cp ${SRCROOT}/doc/man/man3/dlopen_preflight.3 ${MAN_THREE_DIR}
cp ${SRCROOT}/doc/man/man3/dyld.3 ${MAN_THREE_DIR}

#echo "Installed docs"
