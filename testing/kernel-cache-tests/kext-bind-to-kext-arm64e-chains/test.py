#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This tests that kexts can bind to each other using DYLD_CHAINED_PTR_ARM64E_KERNEL

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64e", "/kext-bind-to-kext-arm64e-chains/main.kc", "/kext-bind-to-kext-arm64e-chains/main.kernel", "/kext-bind-to-kext-arm64e-chains/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/kext-bind-to-kext-arm64e-chains/main.kc", ["-layout", "-arch", "arm64e"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"

    # Note down the base addresses and GOT section for later
    cacheBaseAddress = findCacheBaseVMAddr(kernel_cache)
    barAuthGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__auth_got")
    barGOTVMAddr = findSectionVMAddr(kernel_cache, 1, "__DATA_CONST", "__got")

    # Symbols
    kernel_cache.analyze("/kext-bind-to-kext-arm64e-chains/main.kc", ["-symbols", "-arch", "arm64e"])
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    main_func_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_func")
    main_funcPtr_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_funcPtr")
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    bar_fooPtr_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_fooPtr")
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    foo_foo_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_foo")
    foo_f_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "_f")

    # Check the fixups
    kernel_cache.analyze("/kext-bind-to-kext-arm64e-chains/main.kc", ["-fixups", "-arch", "arm64e"])
    assert len(kernel_cache.dictionary()["fixups"]) >= 4
    # __DATA_CONST
    # bar.kext: extern int foo();
    # bar.kext: extern int f;
    assert kernel_cache.dictionary()["fixups"][fixupOffset(barAuthGOTVMAddr, cacheBaseAddress)] == "kc(0) + " + foo_foo_vmAddr + " auth(IA addr 0)"
    assert kernel_cache.dictionary()["fixups"][fixupOffset(barGOTVMAddr, cacheBaseAddress)] == "kc(0) + " + foo_f_vmAddr
    # __DATA
    # main.kernel: __typeof(&func) funcPtr = &func;
    assert kernel_cache.dictionary()["fixups"][fixupOffset(main_funcPtr_vmAddr, cacheBaseAddress)] == "kc(0) + " + main_func_vmAddr + " auth(IA !addr 0)"
    # bar.kext: __typeof(&foo) fooPtr = &foo;
    assert kernel_cache.dictionary()["fixups"][fixupOffset(bar_fooPtr_vmAddr, cacheBaseAddress)] == "kc(0) + " + foo_foo_vmAddr + " auth(IA !addr 42271)"
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"] == "none"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][2]["fixups"] == "none"


# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -Wl,-static -mkernel -Wl,-fixup_chains -Wl,-kernel -nostdlib -Wl,-add_split_seg_info -Wl,-e,__start -Wl,-pie main.c -Wl,-pagezero_size,0x0 -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -o main.kernel  -Wl,-image_base,0xfffffff007004000 -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

