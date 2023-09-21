#!/bin/sh

if [ -n "${SYSTEM_PREFIX}" ]; then
    # do nothing
    exit 0
fi

KTRACE_CODES_DIR=${DSTROOT}/usr/local/share/misc
mkdir -p ${KTRACE_CODES_DIR}
cp ${SRCROOT}/doc/tracing/dyld.codes ${KTRACE_CODES_DIR}

#echo "Installed ktrace codes"
