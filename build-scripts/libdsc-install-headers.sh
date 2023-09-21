#!/bin/sh

if [ -n "${SYSTEM_PREFIX}" ]; then # ExclaveKit
    # do nothing
    exit 0
fi

PRIVATE_HEADERS_FOLDER_PATH=/usr/local/include/mach-o
mkdir -p ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}

cp ${SRCROOT}/other-tools/dsc_extractor.h ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}
cp ${SRCROOT}/other-tools/dsc_iterator.h ${DSTROOT}${PRIVATE_HEADERS_FOLDER_PATH}

#echo "Installed libdsc headers"
