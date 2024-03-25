if [ "${EXCLAVEKIT}" == 1 ]
then
    ls -1 ${SDKROOT}/System/ExclaveKit/usr/local/lib/dyld/liblibc* > ${DERIVED_SOURCES_DIR}/archives.txt
    if [ -f "${SDKROOT}/AppleInternal/DirtyDataFiles/dyld.dirty" ]
    then
        cp "${SDKROOT}/AppleInternal/DirtyDataFiles/dyld.dirty" "${DERIVED_SOURCES_DIR}/dyld.dirty"
    else
        cp "${SRCROOT}/dyld/dyld.dirty" "${DERIVED_SOURCES_DIR}/dyld.dirty"
    fi
else
    # link with all .a files in /usr/local/lib/dyld
    ls -1 ${SDKROOT}/usr/local/lib/dyld/*.a | grep -v libcompiler_rt | grep -v libunwind | grep -v libc++abi > ${DERIVED_SOURCES_DIR}/archives.txt

    # link with crash report archive if it exists
    if [ -f ${SDKROOT}/usr/local/lib/libCrashReporterClient.a ]
    then
      echo \"${SDKROOT}/usr/local/lib/libCrashReporterClient.a\" >> ${DERIVED_SOURCES_DIR}/archives.txt
    fi

    # link with crypto archive if it exists
    if [ -f ${SDKROOT}/usr/local/lib/libcorecrypto_static.a ]
    then
      echo \"${SDKROOT}/usr/local/lib/libcorecrypto_static.a\" >> ${DERIVED_SOURCES_DIR}/archives.txt
    fi

    # always use a .dirty file.  If none in SDK, use our own
    if [ -f "${SDKROOT}/AppleInternal/DirtyDataFiles/dyld.dirty" ]
    then
        cp "${SDKROOT}/AppleInternal/DirtyDataFiles/dyld.dirty" "${DERIVED_SOURCES_DIR}/dyld.dirty"
    else
        cp "${SRCROOT}/dyld/dyld.dirty" "${DERIVED_SOURCES_DIR}/dyld.dirty"
    fi
fi

