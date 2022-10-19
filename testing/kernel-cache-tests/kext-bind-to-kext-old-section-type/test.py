#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This tests that the very old bar.kext can be parsed.  It's __got section has a S_NON_LAZY_SYMBOL_POINTERS type,
# but newer linkers changed to just S_REGULAR.

def check(kernel_cache):
    kernel_cache.buildKernelCollection("x86_64", "/kext-bind-to-kext-old-section-type/main.kc", "/kext-bind-to-kext-old-section-type/main.kernel", "/kext-bind-to-kext-old-section-type/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/kext-bind-to-kext-old-section-type/main.kc", ["-layout", "-arch", "x86_64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"

    # The fixup is at the start of __DATA for bar
    dataSegmentVMAddr = findSegmentVMAddr(kernel_cache, 1, "__DATA")
    cacheBaseVMAddr = findCacheBaseVMAddr(kernel_cache)
    fooFixupOffset = fixupOffset(dataSegmentVMAddr, cacheBaseVMAddr)

    kernel_cache.analyze("/kext-bind-to-kext-old-section-type/main.kc", ["-symbols", "-arch", "x86_64"])
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    fooSymbolVMAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_foo")

    # Check the fixups
    kernel_cache.analyze("/kext-bind-to-kext-old-section-type/main.kc", ["-fixups", "-arch", "x86_64"])
    assert len(kernel_cache.dictionary()["fixups"]) == 5
    assert kernel_cache.dictionary()["fixups"][fooFixupOffset] == "kc(0) + " + fooSymbolVMAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][2]["fixups"] == "none"


# Note, bar.kext has to be linked with a very old linker from 10.7 to get the __got section with S_NON_LAZY_SYMBOL_POINTERS.
# You can emulate this with a new linker by having sectionFlags() return S_NON_LAZY_SYMBOL_POINTERS for ld::Section::typeNonLazyPointer
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-static -Wl,-kernel -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__HIB,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie main.c -o main.kernel -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack -Wl,-segprot,__HIB,r-x,r-x -Wl,-image_base,0x8000 -Wl,-segaddr,__HIB,0x4000
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const bar.c -o extensions/bar.kext/bar

