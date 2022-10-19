#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# kxld has an implicit alias for a metaclass vtable entry.  Test that we also rewrite that alias

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/vtable-patching-metaclass-alias/main.kc", "/vtable-patching-metaclass-alias/main.kernel", "/vtable-patching-metaclass-alias/extensions", ["com.apple.foo"], [])
    kernel_cache.analyze("/vtable-patching-metaclass-alias/main.kc", ["-layout", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"

    gotVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__got")

    # Find the address of the symbols to bind to
    kernel_cache.analyze("/vtable-patching-metaclass-alias/main.kc", ["-symbols", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    metaclassSymbolVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN15OSMetaClassBase8DispatchE5IORPC")

    # Check the fixups
    kernel_cache.analyze("/vtable-patching-metaclass-alias/main.kc", ["-fixups", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["fixups"]) == 4
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(gotVMAddr, 0)] == "kc(0) + " + metaclassSymbolVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(gotVMAddr, 8)] == "kc(0) + " + metaclassSymbolVMAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"

# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie main.c -o main.kernel -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r--
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo
# [~]> rm -r extensions/foo.kext/*.ld

