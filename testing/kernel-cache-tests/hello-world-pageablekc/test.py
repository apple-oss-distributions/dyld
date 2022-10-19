#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# Verify that pageableKC has nothing packed on the same page

def check(kernel_cache):
    # First build a kernel collection
    kernel_cache.buildKernelCollection("arm64", "/hello-world-pageablekc/main.kc", "/hello-world-pageablekc/main.kernel", "/hello-world-pageablekc/extensions", [], [])
    kernel_cache.analyze("/hello-world-pageablekc/main.kc", ["-layout", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 1
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"

    # Now build an pageable cache using the baseline kernel collection
    kernel_cache.buildPageableKernelCollection("arm64", "/hello-world-pageablekc/pageable.kc", "/hello-world-pageablekc/main.kc", "/hello-world-pageablekc/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/hello-world-pageablekc/pageable.kc", ["-layout", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["cache-segments"]) == 6
    assert kernel_cache.dictionary()["cache-segments"][0]["name"] == "__TEXT"
    assert kernel_cache.dictionary()["cache-segments"][0]["vmAddr"] == "0x0"
    assert kernel_cache.dictionary()["cache-segments"][1]["name"] == "__PRELINK_TEXT"
    assert kernel_cache.dictionary()["cache-segments"][1]["vmAddr"] == "0x4000"
    assert kernel_cache.dictionary()["cache-segments"][2]["name"] == "__TEXT_EXEC"
    assert kernel_cache.dictionary()["cache-segments"][2]["vmAddr"] == "0xC000"
    assert kernel_cache.dictionary()["cache-segments"][3]["name"] == "__PRELINK_INFO"
    assert kernel_cache.dictionary()["cache-segments"][3]["vmAddr"] == "0x14000"
    assert kernel_cache.dictionary()["cache-segments"][4]["name"] == "__DATA"
    assert kernel_cache.dictionary()["cache-segments"][4]["vmAddr"] == "0x18000"
    assert kernel_cache.dictionary()["cache-segments"][5]["name"] == "__LINKEDIT"
    assert kernel_cache.dictionary()["cache-segments"][5]["vmAddr"] == "0x20000"

    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.bar"
    assert len(kernel_cache.dictionary()["dylibs"][0]["segments"]) == 4
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][0]["name"] == "__TEXT"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][0]["vmAddr"] == "0x4000"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][1]["name"] == "__TEXT_EXEC"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][1]["vmAddr"] == "0xC000"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][2]["name"] == "__DATA"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][2]["vmAddr"] == "0x18000"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][3]["name"] == "__LINKEDIT"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][3]["vmAddr"] == "0x20000"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    assert len(kernel_cache.dictionary()["dylibs"][1]["segments"]) == 4
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][0]["name"] == "__TEXT"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][0]["vmAddr"] == "0x8000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["name"] == "__TEXT_EXEC"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["vmAddr"] == "0x10000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][2]["name"] == "__DATA"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][2]["vmAddr"] == "0x1C000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][3]["name"] == "__LINKEDIT"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][3]["vmAddr"] == "0x20000"

    # Note down the base addresses and GOT section for later
    barBaseAddress = findBaseVMAddr(kernel_cache, 0)
    fooBaseAddress = findBaseVMAddr(kernel_cache, 1)
    barGOTVMAddr = findSectionVMAddr(kernel_cache, 0, "__DATA", "__got")

    # Get the symbols, to be used to find the fixups later
    kernel_cache.analyze("/hello-world-pageablekc/pageable.kc", ["-symbols", "-arch", "arm64"])
    bar_p_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_p")
    bar_x_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "_x")
    fooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_foo")
    foo_p_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_p")
    foo_x_vmAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "_x")

    # Check the fixups
    kernel_cache.analyze("/hello-world-pageablekc/pageable.kc", ["-fixups", "-arch", "arm64"])
    assert kernel_cache.dictionary()["fixups"] == ""
    assert len(kernel_cache.dictionary()["dylibs"]) == 2

    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.bar"
    assert len(kernel_cache.dictionary()["dylibs"][0]["fixups"]) >= 2
    # extern int foo()
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][fixupOffset(barGOTVMAddr, barBaseAddress)] == "kc(1) + " + fooVMAddr
    # int* p = &x;
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][fixupOffset(bar_p_vmAddr, barBaseAddress)] == "kc(1) + " + bar_x_vmAddr

    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"
    assert len(kernel_cache.dictionary()["dylibs"][1]["fixups"]) >= 1
    # int* p = &x;
    assert kernel_cache.dictionary()["dylibs"][1]["fixups"][fixupOffset(foo_p_vmAddr, fooBaseAddress)] == "kc(1) + " + foo_x_vmAddr


# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie main.c -o main.kernel
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

