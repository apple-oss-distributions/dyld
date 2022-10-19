#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# Last data const should be folded in under data const and allowed to have fixups

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/last-data-const/main.kc", "/last-data-const/main.kernel", "/last-data-const/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/last-data-const/main.kc", ["-layout", "-arch", "arm64"])

    cacheDataConstVMAddr = findCacheSegmentVMAddr(kernel_cache, "__DATA_CONST")
    cacheDataConstVMSize = findCacheSegmentVMSize(kernel_cache, "__DATA_CONST")
    assert cacheDataConstVMSize == "0x8000"

    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    kernelLastDataConstVMAddr = findSegmentVMAddr(kernel_cache, 0, "__LASTDATA_CONST")
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"

    cacheBaseVMAddr = findCacheBaseVMAddr(kernel_cache)
    barGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__got")

    # Check we have the correct addresses of the symbols being bound to
    kernel_cache.analyze("/last-data-const/main.kc", ["-symbols", "-arch", "arm64"])
    # main.kernel
    # int g = &x;
    kernel_g_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_g")
    kernel_x_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_x")
    foo_foo_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_foo")

    kernel_cache.analyze("/last-data-const/main.kc", ["-fixups", "-arch", "arm64"])
    # main.kernel
    # int g = &x;
    assert kernel_cache.dictionary()["fixups"][fixupOffset(kernel_g_vmAddr, cacheBaseVMAddr)] == "kc(0) + " + kernel_x_vmAddr
    # foo.kext
    # foo()
    assert kernel_cache.dictionary()["fixups"][fixupOffset(barGOTVMAddr, cacheBaseVMAddr)] == "kc(0) + " + foo_foo_vmAddr

# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie -Wl,-rename_section,__DATA,__data,__LASTDATA_CONST,__data -Wl,-segprot,__LASTDATA_CONST,r--,r-- main.c -o main.kernel
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

