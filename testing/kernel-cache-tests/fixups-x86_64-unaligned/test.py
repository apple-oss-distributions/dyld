#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# Test unaligned fixups in x86_64 kexts

def check(kernel_cache):
    kernel_cache.buildKernelCollection("x86_64", "/fixups-x86_64-unaligned/main.kc", "/fixups-x86_64-unaligned/main.kernel", "/fixups-x86_64-unaligned/extensions", ["com.apple.foo"], [])
    kernel_cache.analyze("/fixups-x86_64-unaligned/main.kc", ["-layout", "-arch", "x86_64"])

    # Get the base address of the cache, so that we can compute fixups as offsets from that
    cacheBaseVMAddr = findCacheBaseVMAddr(kernel_cache)

    kernel_cache.analyze("/fixups-x86_64-unaligned/main.kc", ["-symbols", "-arch", "x86_64"])
    assert len(kernel_cache.dictionary()["dylibs"]) == 2

    # Get the addresses of the targets of the fixups
    funcVMAddr = findLocalSymbolVMAddr(kernel_cache, 1, "_func")
    gVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_g")

    # Get the addresses, and offsets, to all the values we are fixing up
    psVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_ps")
    psOffset = fixupOffset(psVMAddr, cacheBaseVMAddr)
    psArrayVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_ps_array")
    psArrayOffset = fixupOffset(psArrayVMAddr, cacheBaseVMAddr)

    # Check the fixups
    kernel_cache.analyze("/fixups-x86_64-unaligned/main.kc", ["-fixups", "-arch", "x86_64"])
    assert len(kernel_cache.dictionary()["fixups"]) >= 20
    # foo.kext: PackedS ps = { 0, &func, &func, 0, &g, 0, &g };
    # _ps is at 0x20C000 which is offset 0x10C000 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 4)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 12)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 24)] == "kc(0) + " + gVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psOffset, 33)] == "kc(0) + " + gVMAddr
    # foo.kext: PackedS ps_array = { { 0, &func, &func, 0, &g, 0, &g }, ... }
    # _ps_array[0] is at 0x20D000 which is offset 0x10D000 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 12)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 24)] == "kc(0) + " + gVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 33)] == "kc(0) + " + gVMAddr
    # _ps_array[1] is at 0x20E000 which is offset 0x10D000 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 12)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 24)] == "kc(0) + " + gVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 33)] == "kc(0) + " + gVMAddr
    # _ps_array[2] is at 0x20F000 which is offset 0x10D000 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 4)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 12)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 24)] == "kc(0) + " + gVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 33)] == "kc(0) + " + gVMAddr
    # _ps_array[3] is at 0x210000 which is offset 0x10D000 from __HIB
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 4096 + 4)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 4096 + 12)] == "kc(0) + " + funcVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 4096 + 24)] == "kc(0) + " + gVMAddr
    assert kernel_cache.dictionary()["fixups"][offsetVMAddr(psArrayOffset, 4096 + 4096 + 4096 + 33)] == "kc(0) + " + gVMAddr

# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-static -mkernel -nostdlib -Wl,-kernel -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-image_base,0x200000 -Wl,-segaddr,__HIB,0x100000 -Wl,-add_split_seg_info -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info foo.c -o extensions/foo.kext/foo

