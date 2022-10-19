#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This verifies that a kext can bind to another kext
# foo.kext exports foo and bar.kext uses it
# bar.kext then exports foo and baz.kext binds to it

# Note this is the same as the kext-vtable-patching test, just with arm64e so ptrauth on the fixups

def check(kernel_cache):
    enableLogging = False
    kernel_cache.buildKernelCollection("arm64e", "/kext-vtable-patching-arm64e/main.kc", "/kext-vtable-patching-arm64e/main.kernel", "/kext-vtable-patching-arm64e/extensions", ["com.apple.foo", "com.apple.bar", "com.apple.baz"], [])
    kernel_cache.analyze("/kext-vtable-patching-arm64e/main.kc", ["-layout", "-arch", "arm64e"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 4
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.bar"
    assert kernel_cache.dictionary()["dylibs"][2]["name"] == "com.apple.baz"
    assert kernel_cache.dictionary()["dylibs"][3]["name"] == "com.apple.foo"

    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/kext-vtable-patching-arm64e/main.kc", ["-symbols", "-arch", "arm64e"])
    
    # From bar, find the vtable and its override of foo()
    # Bar::foo()
    barClassBar_fooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "__ZN3Bar3fooEv")
    if enableLogging:
        print("barClassBar_fooVMAddr: " + barClassBar_fooVMAddr)
    
    # From baz, find the vtable and its override of foo()
    # Baz::foo()
    bazClassBaz_fooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 2, "__ZN3Baz3fooEv")
    if enableLogging:
        print("bazClassBaz_fooVMAddr: " + bazClassBaz_fooVMAddr)
    
    # From foo, we want to know where the vtable is, and the foo() and fooUsed0() slots in that vtable
    # Foo::foo()
    fooClassFoo_fooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 3, "__ZN3Foo3fooEv")
    if enableLogging:
        print("fooClassFoo_fooVMAddr: " + fooClassFoo_fooVMAddr)
    # Foo::fooUsed0()
    fooClassFoo_fooUsed0VMAddr = findGlobalSymbolVMAddr(kernel_cache, 3, "__ZN3Foo8fooUsed0Ev")
    if enableLogging:
        print("fooClassFoo_fooUsed0VMAddr: " + fooClassFoo_fooUsed0VMAddr)


    # Check the fixups
    kernel_cache.analyze("/kext-vtable-patching-arm64e/main.kc", ["-fixups", "-arch", "arm64e"])
    
    # In vtable for Foo, we match the entry for Foo::foo() by looking for its value on the RHS of the fixup
    fooFixupAddr = findFixupVMAddr(kernel_cache, "kc(0) + " + fooClassFoo_fooVMAddr + " auth(IA addr 49764)")
    if enableLogging:
        print("fooFixupAddr: " + fooFixupAddr)

    # Then the following fixup should be to Foo::fooUsed0()
    nextFixup = offsetVMAddr(fooFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixup] == "kc(0) + " + fooClassFoo_fooUsed0VMAddr + " auth(IA addr 61962)"

    # Now in bar, again match the entry for its Bar::foo() symbol
    barFixupAddr = findFixupVMAddr(kernel_cache, "kc(0) + " + barClassBar_fooVMAddr + " auth(IA addr 49764)")
    if enableLogging:
        print("barFixupAddr: " + barFixupAddr)

    # And if the patching was correct, then following entry should be to Foo::fooUsed0()
    nextFixup = offsetVMAddr(barFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixup] == "kc(0) + " + fooClassFoo_fooUsed0VMAddr + " auth(IA addr 61962)"

    # Now in baz, again match the entry for its Baz::foo() symbol
    bazFixupAddr = findFixupVMAddr(kernel_cache, "kc(0) + " + bazClassBaz_fooVMAddr + " auth(IA addr 49764)")
    if enableLogging:
        print("barFixupAddr: " + barFixupAddr)

    # And if the patching was correct, then following entry should be to Foo::fooUsed0()
    nextFixup = offsetVMAddr(bazFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixup] == "kc(0) + " + fooClassFoo_fooUsed0VMAddr + " auth(IA addr 61962)"


# [~]> xcrun -sdk macosx.internal cc -arch arm64e -Wl,-static -mkernel -nostdlib -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-image_base,0x10000 -Wl,-add_split_seg_info -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r-- -Wl,-fixup_chains -Wl,-kernel  -target arm64e-apple-macosx12.0.0 -Wl,-version_load_command
# [~]> xcrun -sdk macosx.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.cpp -o extensions/foo.kext/foo -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -DFOO_USED=1
# [~]> xcrun -sdk macosx.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const bar.cpp -o extensions/bar.kext/bar -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers
# [~]> xcrun -sdk macosx.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const baz.cpp -o extensions/baz.kext/baz -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers
# [~]> rm -r extensions/*.kext/*.ld

