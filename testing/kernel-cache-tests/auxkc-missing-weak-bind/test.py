#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# Check that weak binds can be missing, so long as we check for the magic symbol

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/auxkc-missing-weak-bind/main.kc", "/auxkc-missing-weak-bind/main.kernel", "/auxkc-missing-weak-bind/extensions", [], [])
    kernel_cache.analyze("/auxkc-missing-weak-bind/main.kc", ["-symbols", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 1
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    kextUnresolvedVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_gOSKextUnresolved")

    # Now build an aux cache using the baseline kernel collection
    kernel_cache.buildAuxKernelCollection("arm64", "/auxkc-missing-weak-bind/aux.kc", "/auxkc-missing-weak-bind/main.kc", "", "/auxkc-missing-weak-bind/extensions", ["com.apple.foo", "com.apple.bar"], [])

    kernel_cache.analyze("/auxkc-missing-weak-bind/aux.kc", ["-layout", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    barGOTVMAddr = findSectionVMAddr(kernel_cache, 0, "__DATA_CONST", "__got")
    fooGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__got")

    # Check the fixups
    kernel_cache.analyze("/auxkc-missing-weak-bind/aux.kc", ["-fixups", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["fixups"]) >= 8
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(barGOTVMAddr, 0)] == "kc(0) + " + kextUnresolvedVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(barGOTVMAddr, 8)] == "kc(0) + " + kextUnresolvedVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 0)] == "kc(0) + " + kextUnresolvedVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 8)] == "kc(0) + " + kextUnresolvedVMAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"


# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r-- main.c -o main.kernel
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info bar.c -o extensions/bar.kext/bar -Wl,-fixup_chains
# [~]> rm -r extensions/*.kext/*.ld

