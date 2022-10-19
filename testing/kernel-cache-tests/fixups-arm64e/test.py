#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *


def check(kernel_cache):
    enableLogging = False
    kernel_cache.buildKernelCollection("arm64e", "/fixups-arm64e/main.kc", "/fixups-arm64e/main.kernel", "/fixups-arm64e/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/fixups-arm64e/main.kc", ["-layout", "-arch", "arm64e"])

    # Get the base address of the cache, so that we can compute fixups as offsets from that
    cacheBaseVMAddr = findCacheBaseVMAddr(kernel_cache)

    kernel_cache.analyze("/fixups-arm64e/main.kc", ["-symbols", "-arch", "arm64e"])
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

    # print("barPtrOffset: " + barPtrOffset)

    # Check the fixups
    kernel_cache.analyze("/fixups-arm64e/main.kc", ["-fixups", "-arch", "arm64e"])
    assert len(kernel_cache.dictionary()["fixups"]) >= 11
    # main.kernel: S s = { &func, &func, &g, &func, &g };
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 0)] == "kc(0) + " + funcVMAddr + " auth(IA !addr 0)"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 8)] == "kc(0) + " + funcVMAddr + " auth(IA !addr 0)"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 16)] == "kc(0) + " + gVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 16384)] == "kc(0) + " + funcVMAddr + " auth(IA !addr 0)"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(sOffset, 16384 + 8)] == "kc(0) + " + gVMAddr
    # main.kernel: PackedS ps = { 0, &func, &func, 0, &g, 0, &g };
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 4)] == "kc(0) + " + funcVMAddr + " auth(IA !addr 0)"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 12)] == "kc(0) + " + funcVMAddr + " auth(IA !addr 0)"
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 24)] == "kc(0) + " + gVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 36)] == "kc(0) + " + gVMAddr
    # bar.kext: __typeof(&bar) barPtr = &bar;
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(barPtrOffset, 0)] == "kc(0) + " + barVMAddr + " auth(IA !addr 42271)"
    # foo.kext: int* gPtr = &g;
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(gPtrOffset, 0)] == "kc(0) + " + g2VMAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][2]["fixups"] == "none"

# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -std=c++11 -Wl,-static -mkernel -Wl,-fixup_chains -Wl,-kernel -nostdlib -Wl,-add_split_seg_info -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -o main.kernel  -Wl,-image_base,0xfffffff007004000 -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

