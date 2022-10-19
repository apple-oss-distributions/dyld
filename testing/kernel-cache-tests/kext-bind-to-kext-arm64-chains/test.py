#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This tests that kexts can bind to each other using DYLD_CHAINED_PTR_64_OFFSET

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/kext-bind-to-kext-arm64-chains/main.kc", "/kext-bind-to-kext-arm64-chains/main.kernel", "/kext-bind-to-kext-arm64-chains/extensions", ["com.apple.foo", "com.apple.bar"], [])

    # layout
    kernel_cache.analyze("/kext-bind-to-kext-arm64-chains/main.kc", ["-layout", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"

    # Note down the base addresses and GOT section for later
    cacheBaseAddress = findCacheBaseVMAddr(kernel_cache)
    barGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__got")

    # Symbols
    kernel_cache.analyze("/kext-bind-to-kext-arm64-chains/main.kc", ["-symbols", "-arch", "arm64"])
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    main_func_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_func")
    main_funcPtr_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_funcPtr")
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    foo_foo_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_foo")

    # Check the fixups
    kernel_cache.analyze("/kext-bind-to-kext-arm64-chains/main.kc", ["-fixups", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()["fixups"]) >= 2
    # bar.kext: extern int foo();
    assert kernel_cache.dictionary()["fixups"][fixupOffset(barGOTVMAddr, cacheBaseAddress)] == "kc(0) + " + foo_foo_vmAddr
    # main.kernel: __typeof(&func) funcPtr = &func;
    assert kernel_cache.dictionary()["fixups"][fixupOffset(main_funcPtr_vmAddr, cacheBaseAddress)] == "kc(0) + " + main_func_vmAddr
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][2]["fixups"] == "none"


# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -Wl,-fixup_chains -Wl,-kernel -nostdlib -Wl,-add_split_seg_info -Wl,-e,__start -Wl,-pie main.c -Wl,-pagezero_size,0x0 -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -o main.kernel  -Wl,-image_base,0xfffffff007004000 -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

