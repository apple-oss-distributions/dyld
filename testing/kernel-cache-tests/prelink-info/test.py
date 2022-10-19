#!/usr/bin/python3

import os
import KernelCollection

# The verifies that the executable size in the prelink info is the VM size from the kexts

def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64", "/prelink-info/main.kc", "/prelink-info/main.kernel", "/prelink-info/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/prelink-info/main.kc", ["-layout", "-arch", "arm64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"

    # Check the prelink info
    kernel_cache.analyze("/prelink-info/main.kc", ["-prelink-info", "-arch", "arm64"])
    assert len(kernel_cache.dictionary()) == 2
    assert kernel_cache.dictionary()[0]["bundle-id"] == "com.apple.bar"
    assert kernel_cache.dictionary()[0]["executable-size"] == "0x2F5"
    assert kernel_cache.dictionary()[1]["bundle-id"] == "com.apple.foo"
    assert kernel_cache.dictionary()[1]["executable-size"] == "0x2F4"


# [~]> xcrun -sdk macosx.internal cc -arch arm64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -Wl,-e,__start -Wl,-pagezero_size,0x0 -Wl,-pie main.c -o main.kernel
# [~]> xcrun -sdk macosx.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk macosx.internal cc -arch arm64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const bar.c -o extensions/bar.kext/bar

