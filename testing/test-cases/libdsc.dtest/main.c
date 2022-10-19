
// BUILD:  $CC main.c      -o $BUILD_DIR/libdsc-test.exe -ldsc

// RUN: ./libdsc-test.exe

#include <stdlib.h>
#include <string.h> 
#include <mach-o/dyld_priv.h>
#include <dlfcn.h>
#include <mach-o/dsc_iterator.h>

#include "test_support.h"

// This program links libdsc.a and verifies that dyld_shared_cache_iterate() works

int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    size_t cacheLen;
    const void* cacheStart = _dyld_get_shared_cache_range(&cacheLen);

    if ( cacheStart != NULL ) {
        dyld_shared_cache_iterate(cacheStart, cacheLen, ^(const dyld_shared_cache_dylib_info* dylibInfo, const dyld_shared_cache_segment_info* segInfo) {
            if ( false ) {
                LOG("%p %s", dylibInfo->machHeader, dylibInfo->path);
                LOG("    dylib.version=%d", dylibInfo->version);
                LOG("    dylib.isAlias=%d", dylibInfo->isAlias);
                LOG("    dylib.inode=%lld",   dylibInfo->inode);
                LOG("    dylib.modTime=%lld", dylibInfo->modTime);
                LOG("    segment.name=         %s", segInfo->name);
                LOG("    segment.fileOffset=   0x%08llX", segInfo->fileOffset);
                LOG("    segment.fileSize=     0x%08llX", segInfo->fileSize);
                LOG("    segment.address=      0x%08llX", segInfo->address);
                LOG("    segment.addressOffset=0x%08llX", segInfo->addressOffset);
            }
        });
    }

    PASS("Success");
}
