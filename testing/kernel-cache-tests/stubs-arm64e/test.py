#!/usr/bin/python3

import os
import KernelCollection


def check(kernel_cache):
    kernel_cache.buildKernelCollection("arm64e", "/stubs-arm64e/main.kc", "/stubs-arm64e/main.kernel", "/stubs-arm64e/extensions", ["com.apple.foo", "com.apple.bar"], [])
    kernel_cache.analyze("/stubs-arm64e/main.kc", ["-layout", "-arch", "arm64e"])

    assert len(kernel_cache.dictionary()["cache-segments"]) == 7
    assert kernel_cache.dictionary()["cache-segments"][0]["name"] == "__TEXT"
    assert kernel_cache.dictionary()["cache-segments"][0]["vmAddr"] == "0xFFFFFFF007004000"
    assert kernel_cache.dictionary()["cache-segments"][1]["name"] == "__PRELINK_TEXT"
    assert kernel_cache.dictionary()["cache-segments"][1]["vmAddr"] == "0xFFFFFFF007008000"
    assert kernel_cache.dictionary()["cache-segments"][2]["name"] == "__DATA_CONST"
    assert kernel_cache.dictionary()["cache-segments"][2]["vmAddr"] == "0xFFFFFFF007010000"
    assert kernel_cache.dictionary()["cache-segments"][3]["name"] == "__TEXT_EXEC"
    assert kernel_cache.dictionary()["cache-segments"][3]["vmAddr"] == "0xFFFFFFF007014000"
    assert kernel_cache.dictionary()["cache-segments"][4]["name"] == "__PRELINK_INFO"
    assert kernel_cache.dictionary()["cache-segments"][4]["vmAddr"] == "0xFFFFFFF007020000"
    assert kernel_cache.dictionary()["cache-segments"][5]["name"] == "__DATA"
    assert kernel_cache.dictionary()["cache-segments"][5]["vmAddr"] == "0xFFFFFFF007024000"
    assert kernel_cache.dictionary()["cache-segments"][6]["name"] == "__LINKEDIT"
    assert kernel_cache.dictionary()["cache-segments"][6]["vmAddr"] == "0xFFFFFFF007028000"

    assert len(kernel_cache.dictionary()["dylibs"]) == 3
    # main.kernel
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert len(kernel_cache.dictionary()["dylibs"][0]["segments"]) == 4
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][0]["name"] == "__TEXT"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][0]["vmAddr"] == "0xFFFFFFF007014000"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][1]["name"] == "__TEXT_EXEC"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][1]["vmAddr"] == "0xFFFFFFF007018000"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][2]["name"] == "__LINKINFO"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][2]["vmAddr"] == "0xFFFFFFF007028000"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][3]["name"] == "__LINKEDIT"
    assert kernel_cache.dictionary()["dylibs"][0]["segments"][3]["vmAddr"] == "0xFFFFFFF00702C000"
    # bar.kext
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert len(kernel_cache.dictionary()["dylibs"][1]["segments"]) == 5
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][0]["name"] == "__TEXT"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][0]["vmAddr"] == "0xFFFFFFF007008000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["name"] == "__TEXT_EXEC"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["vmAddr"] == "0xFFFFFFF00701C000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["vmEnd"] == "0xFFFFFFF00701C044"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["vmSize"] == "0x44"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][0]["name"] == "__text"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][0]["vmAddr"] == "0xFFFFFFF00701C000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][0]["vmEnd"] == "0xFFFFFFF00701C044"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][0]["vmSize"] == "0x44"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][1]["name"] == "__auth_stubs"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][1]["vmAddr"] == "0xFFFFFFF00701C044"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][1]["vmEnd"] == "0xFFFFFFF00701C044"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][1]["sections"][1]["vmSize"] == "0x0"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][2]["name"] == "__DATA"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][2]["vmAddr"] == "0xFFFFFFF007024000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][3]["name"] == "__DATA_CONST"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][3]["vmAddr"] == "0xFFFFFFF007010000"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][4]["name"] == "__LINKEDIT"
    assert kernel_cache.dictionary()["dylibs"][1]["segments"][4]["vmAddr"] == "0xFFFFFFF00702C000"
    # foo.kext
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.foo"
    assert len(kernel_cache.dictionary()["dylibs"][2]["segments"]) == 5
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][0]["name"] == "__TEXT"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][0]["vmAddr"] == "0xFFFFFFF00700C000"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["name"] == "__TEXT_EXEC"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["vmAddr"] == "0xFFFFFFF00701C050"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["vmEnd"] == "0xFFFFFFF00701C094"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["vmSize"] == "0x44"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][0]["name"] == "__text"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][0]["vmAddr"] == "0xFFFFFFF00701C050"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][0]["vmEnd"] == "0xFFFFFFF00701C094"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][0]["vmSize"] == "0x44"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][1]["name"] == "__auth_stubs"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][1]["vmAddr"] == "0xFFFFFFF00701C094"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][1]["vmEnd"] == "0xFFFFFFF00701C094"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][1]["sections"][1]["vmSize"] == "0x0"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][2]["name"] == "__DATA"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][2]["vmAddr"] == "0xFFFFFFF0070240C4"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][3]["name"] == "__DATA_CONST"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][3]["vmAddr"] == "0xFFFFFFF007010010"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][4]["name"] == "__LINKEDIT"
    assert kernel_cache.dictionary()["dylibs"][2]["segments"][4]["vmAddr"] == "0xFFFFFFF00702C000"

# [~]> xcrun -sdk macosx.internal cc -arch arm64e -target macosx12.0 -std=c++11 -Wl,-static -mkernel -Wl,-fixup_chains -Wl,-kernel -nostdlib -Wl,-add_split_seg_info -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -Wl,-rename_section,__TEXT,__text,__TEXT_EXEC,__text -o main.kernel  -Wl,-image_base,0xfffffff007004000 -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r-- -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack
# [~]> xcrun -sdk macosx.internal cc -arch arm64e -target macosx12.0 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains foo.c -o extensions/foo.kext/foo
# [~]> xcrun -sdk macosx.internal cc -arch arm64e -target macosx12.0 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-fixup_chains bar.c -o extensions/bar.kext/bar
# [~]> rm -r extensions/*.kext/*.ld

