#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# Check that weak binds can be missing, so long as we check for the magic symbol

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/kext-missing-weak-bind/main.kc", "/kext-missing-weak-bind/main.kernel", "/kext-missing-weak-bind/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/kext-missing-weak-bind/main.kc", ["-layout", "-arch", "arm64"])
    dataConstVMAddr = findCacheSegmentVMAddr(kernel_cache, "__DATA_CONST")

    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"

    # Symbols
    kernel_cache.analyze("/kext-missing-weak-bind/main.kc", ["-symbols", "-arch", "arm64"])
    # kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    kextUnresolvedVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_gOSKextUnresolved")

    # Check the fixups
    kernel_cache.analyze("/kext-missing-weak-bind/main.kc", ["-fixups", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["fixups"]) == 8
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(dataConstVMAddr, 0)] == "kc(0) + " + kextUnresolvedVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(dataConstVMAddr, 8)] == "kc(0) + " + kextUnresolvedVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(dataConstVMAddr, 0)] == "kc(0) + " + kextUnresolvedVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(dataConstVMAddr, 8)] == "kc(0) + " + kextUnresolvedVMAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][2]["fixups"] == "none"


# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r-- main.c -o main.kernel
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info bar.c -o extensions/bar.kext/bar -Wl,-fixup_chains
# [~]> rm -r extensions/*.kext/*.ld

