#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# Symbol sets can have a prefix with an implicit * wildcard on the end which re-exports anything from xnu with that name

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/symbol-sets-prefix/main.kc", "/symbol-sets-prefix/main.kernel", "/symbol-sets-prefix/extensions", ["com.apple.foo"], [])
    kernel_cache.analyze("/symbol-sets-prefix/main.kc", ["-layout", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    
    fooGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__got")

    # Check the symbols
    kernel_cache.analyze("/symbol-sets-prefix/main.kc", ["-symbols", "-arch", "arm64"])
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    xnu0_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_symbol_from_xnu0")
    xnu1_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_symbol_from_xnu1")
    xnu2_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_symbol_from_xnu2")
    xnu3_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_symbol_from_xnu3")

    # Check the fixups
    kernel_cache.analyze("/symbol-sets-prefix/main.kc", ["-fixups", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["fixups"]) == 6
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 0)] == "kc(0) + " + xnu0_vmAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 8)] == "kc(0) + " + xnu1_vmAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 16)] == "kc(0) + " + xnu2_vmAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 24)] == "kc(0) + " + xnu3_vmAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"

# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie main.c -o main.kernel -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r--
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo
# [~]> rm -r extensions/foo.kext/*.ld

