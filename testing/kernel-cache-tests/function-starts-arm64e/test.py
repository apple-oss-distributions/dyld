#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *


def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64e", "/function-starts-arm64e/main.kc", "/function-starts-arm64e/main.kernel", "/function-starts-arm64e/extensions", ["com.apple.foo", "com.apple.bar"], [])

    # Check the symbols
    kernel_cache.analyze("/function-starts-arm64e/main.kc", ["-symbols", "-arch", "arm64e"])
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    # main.kernel: func()
    # main.kernel: _start()
    startVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__start")
    funcVMAddr = findLocalSymbolVMAddr(kernel_cache, 0, "__ZL4funcv")

    # Check the function starts
    kernel_cache.analyze("/function-starts-arm64e/main.kc", ["-function-starts", "-arch", "arm64e"])
    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    # main.kernel: func()
    # main.kernel: _start()
    assert len(kernel_cache.dictionary()["dylibs"][0]["function-starts"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["function-starts"][0] == funcVMAddr
    assert kernel_cache.dictionary()["dylibs"][0]["function-starts"][1] == startVMAddr
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][1]["function-starts"] == ""
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert kernel_cache.dictionary()["dylibs"][2]["function-starts"] == ""

# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -std=c++11 -Wl,-static -mkernel -Wl,-fixup_chains -Wl,-kernel -nostdlib -Wl,-add_split_seg_info -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -o main.kernel -Wl,-function_starts -Wl,-image_base,0xfffffff007004000 -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk iphoneos.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

