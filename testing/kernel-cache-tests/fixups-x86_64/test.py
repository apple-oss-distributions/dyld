#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *


def check(kernel_cache):
    kernel_cache.buildKernelCollection("x86_64", "/fixups-x86_64/main.kc", "/fixups-x86_64/main.kernel", "/fixups-x86_64/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/fixups-x86_64/main.kc", ["-layout", "-arch", "x86_64"])

    # Get the base address of the cache, so that we can compute fixups as offsets from that
    cacheBaseVMAddr = findCacheBaseVMAddr(kernel_cache)

    kernel_cache.analyze("/fixups-x86_64/main.kc", ["-symbols", "-arch", "x86_64"])
    # Get the addresses of &func, &g, &bar for use later
    funcVMAddr = findLocalSymbolVMAddr(kernel_cache, 0, "__ZL4funcv")
    gVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_g")
    barVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_bar")
    g2VMAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_g")

    # Get the addresses, and offsets, to all the values we are fixing up
    sVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_s")
    sOffset = fixupOffset(sVMAddr, cacheBaseVMAddr)
    psVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_ps")
    psOffset = fixupOffset(psVMAddr, cacheBaseVMAddr)
    barPtrVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_barPtr")
    barPtrOffset = fixupOffset(barPtrVMAddr, cacheBaseVMAddr)
    gPtrVMAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_gPtr")
    gPtrOffset = fixupOffset(gPtrVMAddr, cacheBaseVMAddr)

    # Check the fixups
    kernel_cache.analyze("/fixups-x86_64/main.kc", ["-fixups", "-arch", "x86_64"])
    assert len(kernel_cache.dictionary()["fixups"]) >= 12
    # main.kernel: S s = { &func, &func, &g, &func, &g };
    # _s is at 0xFFFFFF8000208000 which is offset 0x108000 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 0)] == "kc(0) + " + funcVMAddr + " : pointer64"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 8)] == "kc(0) + " + funcVMAddr + " : pointer64"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 16)] == "kc(0) + " + gVMAddr + " : pointer64"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 16384)] == "kc(0) + " + funcVMAddr + " : pointer64"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 16384 + 8)] == "kc(0) + " + gVMAddr + " : pointer64"
    # main.kernel: PackedS ps = { 0, &func, &func, 0, &g, 0, &g };
    # _ps is at 0xFFFFFF8000210000 which is offset 0x110000 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 4)] == "kc(0) + " + funcVMAddr + " : pointer64"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 12)] == "kc(0) + " + funcVMAddr + " : pointer64"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 24)] == "kc(0) + " + gVMAddr + " : pointer64"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 33)] == "kc(0) + " + gVMAddr + " : pointer64"
    # bar.kext: __typeof(&bar) barPtr = &bar;
    # _barPtr is at 0xFFFFFF8000210030 which is offset 0x110030 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(barPtrOffset, 0)] == "kc(0) + " + barVMAddr
    # foo.kext: int* gPtr = &g;
    # _gPtr is at 0xFFFFFF8000210040 which is offset 0x110040 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(gPtrOffset, 0)] == "kc(0) + " + g2VMAddr
    # main.kernel: movl _foo, %esp
    # The _foo reloc is at 0xFFFFFF8000100002 which is offset 0x2 from __HIB
    assert kernel_cache.dictionary()["fixups"]["0x2"] == "kc(0) + 0x100000 : pointer32"
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][2]["fixups"] == "none"

# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-static -mkernel -nostdlib -Wl,-kernel -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-image_base,0xffffff8000200000 -Wl,-segaddr,__HIB,0xffffff8000100000 -Wl,-add_split_seg_info -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info bar.c -o extensions/bar.kext/bar

