#!/usr/bin/python3

import os
import KernelCollection

# Test that we can have an x86_64 entrypoint in a non-executable segment


def check(kernel_cache):
    kernel_cache.buildKernelCollection("x86_64", "/entrypoint-x86_64/main.kc", "/entrypoint-x86_64/main.kernel", None, [], [])

    # Check the layout
    kernel_cache.analyze("/entrypoint-x86_64/main.kc", ["-layout", "-arch", "x86_64"])

# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-static -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-e,__start -Wl,-pie main.c -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack -Wl,-segprot,__HIB,rw-,rw- -Wl,-image_base,0x8000 -Wl,-segaddr,__HIB,0x4000

