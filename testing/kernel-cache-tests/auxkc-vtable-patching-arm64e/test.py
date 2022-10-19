#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This verifies that a kext can bind to another kext
# foo.kext exports foo and bar.kext uses it
# We put foo.kext in the base KC so that the patch slot in bar.kext has to know to use the correct fixup level in the fixup chain

def check(kernel_cache):
    enableLogging = False
    kernel_cache.buildKernelCollection("arm64e", "/auxkc-vtable-patching-arm64e/main.kc", "/auxkc-vtable-patching-arm64e/main.kernel", "/auxkc-vtable-patching-arm64e/extensions", ["com.apple.foo"], [])
    kernel_cache.analyze("/auxkc-vtable-patching-arm64e/main.kc", ["-layout", "-arch", "arm64e"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"

    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/auxkc-vtable-patching-arm64e/main.kc", ["-symbols", "-arch", "arm64e"])
    
    # From foo, we want to know where the vtable is, and the foo() and fooUsed0() slots in that vtable
    # Foo::foo()
    fooFooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "__ZN3Foo3fooEv")
    if enableLogging:
        print("fooFooVMAddr: " + fooFooVMAddr)

    # Foo::fooUsed0()
    fooFooUsed0VMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "__ZN3Foo8fooUsed0Ev")
    if enableLogging:
        print("fooFooUsed0VMAddr: " + fooFooUsed0VMAddr)


    # Check the fixups
    kernel_cache.analyze("/auxkc-vtable-patching-arm64e/main.kc", ["-fixups", "-arch", "arm64e"])
    
    # In vtable for Foo, we match the entry for Foo::foo() by looking for its value on the RHS of the fixup
    fooFooFixupAddr = findFixupVMAddr(kernel_cache, "kc(0) + " + fooFooVMAddr + " auth(IA addr 49764)")
    if enableLogging:
        print("fooFooFixupAddr: " + fooFooFixupAddr)

    # Then the following fixup should be to Foo::fooUsed0()
    nextFixupAddr = offsetVMAddr(fooFooFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixupAddr] == "kc(0) + " + fooFooUsed0VMAddr + " auth(IA addr 61962)"


    # -----------------------------------------------------------
    # Now build an aux cache using the baseline kernel collection
    kernel_cache.buildAuxKernelCollection("arm64e", "/auxkc-vtable-patching-arm64e/aux.kc", "/auxkc-vtable-patching-arm64e/main.kc", "", "/auxkc-vtable-patching-arm64e/extensions", ["com.apple.bar"], [])
    kernel_cache.analyze("/auxkc-vtable-patching-arm64e/aux.kc", ["-layout", "-arch", "arm64e"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 1
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.bar"


    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/auxkc-vtable-patching-arm64e/aux.kc", ["-symbols", "-arch", "arm64e"])
    
    # From bar, find the vtable and its override of foo()
    # Bar::foo()
    barFooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN3Bar3fooEv")
    if enableLogging:
        print("barFooVMAddr: " + barFooVMAddr)

    # Check the fixups
    kernel_cache.analyze("/auxkc-vtable-patching-arm64e/aux.kc", ["-fixups", "-arch", "arm64e"])

    # Now in bar, again match the entry for its Bar::foo() symbol
    barFooFixupAddr = findFixupVMAddr(kernel_cache, "kc(3) + " + barFooVMAddr + " auth(IA addr 49764)")
    if enableLogging:
        print("barFooFixupAddr: " + barFooFixupAddr)

    # And if the patching was correct, then following entry should be to Foo::fooUsed0()
    nextFixupAddr = offsetVMAddr(barFooFixupAddr, 8)
    # Adjust the VMAddr by the size  of the baseKC
    offset = offsetVMAddr(fooFooUsed0VMAddr, -65536)
    if enableLogging:
        print("offset: " + offset)
    assert kernel_cache.dictionary()["fixups"][nextFixupAddr] == "kc(0) + " + offset + " auth(IA addr 61962)"


# [~]> xcrun -sdk macosx.internal cc -arch arm64e -Wl,-static -mkernel -nostdlib -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-image_base,0x10000 -Wl,-segaddr,__HIB,0x4000 -Wl,-add_split_seg_info -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r-- -Wl,-fixup_chains -Wl,-kernel -target arm64e-apple-macosx12.0.0 -Wl,-version_load_command
# [~]> xcrun -sdk macosx.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.cpp -o extensions/foo.kext/foo -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -DFOO_USED=1
# [~]> xcrun -sdk macosx.internal cc -arch arm64e -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-data_const bar.cpp -o extensions/bar.kext/bar -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers
# [~]> rm -r extensions/*.kext/*.ld

