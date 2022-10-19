#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This verifies that a kext can bind to another kext
# foo.kext exports foo and bar.kext uses it

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/kext-bind-to-kext/main.kc", "/kext-bind-to-kext/main.kernel", "/kext-bind-to-kext/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/kext-bind-to-kext/main.kc", ["-layout", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"

    # Note down the base addresses and GOT section for later
    cacheBaseAddress = findCacheBaseVMAddr(kernel_cache)
    barGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA", "__got")

    kernel_cache.analyze("/kext-bind-to-kext/main.kc", ["-symbols", "-arch", "arm64"])
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    foo_foo_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_foo")

    # Check the fixups
    kernel_cache.analyze("/kext-bind-to-kext/main.kc", ["-fixups", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["fixups"]) >= 1
    assert kernel_cache.dictionary()["fixups"][fixupOffset(barGOTVMAddr, cacheBaseAddress)] == "kc(0) + " + foo_foo_vmAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][2]["fixups"] == "none"


# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie main.c -o main.kernel
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

