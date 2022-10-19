#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# Test that the codeless bar.kext (with bundle id com.apple.bar) doesn't interfere with foo using the symbol set of the same bundle id

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/symbol-sets-and-codeless-kexts/main.kc", "/symbol-sets-and-codeless-kexts/main.kernel", "/symbol-sets-and-codeless-kexts/extensions", ["com.apple.foo", "com.apple.bar", "com.apple.baz"], [])
    kernel_cache.analyze("/symbol-sets-and-codeless-kexts/main.kc", ["-layout", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    
    fooGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__got")

    # Find the address of the symbols to bind to
    kernel_cache.analyze("/symbol-sets-and-codeless-kexts/main.kc", ["-symbols", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    xnu_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_symbol_from_xnu")
    xnu_no_alias_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_symbol_from_xnu_no_alias")

    # Check the fixups
    kernel_cache.analyze("/symbol-sets-and-codeless-kexts/main.kc", ["-fixups", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["fixups"]) == 4
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 0)] == "kc(0) + " + xnu_vmAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(fooGOTVMAddr, 8)] == "kc(0) + " + xnu_no_alias_vmAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"

# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie main.c -o main.kernel -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r--
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo
# [~]> rm -r extensions/foo.kext/*.ld

