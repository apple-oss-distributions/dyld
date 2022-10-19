#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This verifies that a kext can bind to another kext
# foo.kext exports foo and bar.kext uses it
# We put foo.kext in the base KC so that the patch slot in bar.kext has to know to use the correct fixup level in the fixup chain

def check(kernel_cache):
    enableLogging = False
    kernel_cache.buildKernelCollection("x86_64", "/auxkc-vtable-patching/main.kc", "/auxkc-vtable-patching/main.kernel", "/auxkc-vtable-patching/extensions", ["com.apple.foo"], [])
    kernel_cache.analyze("/auxkc-vtable-patching/main.kc", ["-layout", "-arch", "x86_64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"

    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/auxkc-vtable-patching/main.kc", ["-symbols", "-arch", "x86_64"])
    
    # From foo, we want to know where the vtable is, and the foo() and fooUsed0() slots in that vtable
    # Foo::foo()
    fooClassFooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "__ZN3Foo3fooEv")
    if enableLogging:
        print("fooClassFooVMAddr: " + fooClassFooVMAddr)
    # Foo::fooUsed0()
    fooClassFooUsed0VMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "__ZN3Foo8fooUsed0Ev")
    if enableLogging:
        print("fooClassFooUsed0VMAddr: " + fooClassFooUsed0VMAddr)


    # Check the fixups
    kernel_cache.analyze("/auxkc-vtable-patching/main.kc", ["-fixups", "-arch", "x86_64"])
    
    # In vtable for Foo, we match the entry for Foo::foo() by looking for its value on the RHS of the fixup
    kernelFooFixupAddr = findFixupVMAddr(kernel_cache, "kc(0) + " + fooClassFooVMAddr)
    if enableLogging:
        print("kernelFooFixupAddr: " + kernelFooFixupAddr)
    # Then the following fixup should be to Foo::fooUsed0()
    kernelFooNextFixupAddr = offsetVMAddr(kernelFooFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][kernelFooNextFixupAddr] == "kc(0) + " + fooClassFooUsed0VMAddr


    # -----------------------------------------------------------
    # Now build an aux cache using the baseline kernel collection
    kernel_cache.buildAuxKernelCollection("x86_64", "/auxkc-vtable-patching/aux.kc", "/auxkc-vtable-patching/main.kc", "", "/auxkc-vtable-patching/extensions", ["com.apple.bar"], [])
    kernel_cache.analyze("/auxkc-vtable-patching/aux.kc", ["-layout", "-arch", "x86_64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 1
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.bar"


    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/auxkc-vtable-patching/aux.kc", ["-symbols", "-arch", "x86_64"])
    
    # From bar, find the vtable and its override of foo()
    # Bar::foo()
    barFooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN3Bar3fooEv")
    if enableLogging:
        print("barFooVMAddr: " + barFooVMAddr)


    # Check the fixups
    kernel_cache.analyze("/auxkc-vtable-patching/aux.kc", ["-fixups", "-arch", "x86_64"])

    # Now in bar, again match the entry for its Bar::foo() symbol
    fixupAddr = findAuxFixupVMAddr(kernel_cache, 0, "kc(3) + " + barFooVMAddr)
    if enableLogging:
        print("fixupAddr: " + fixupAddr)

    # And if the patching was correct, then following entry should be to Foo::fooUsed0()
    nextFixupAddr = offsetVMAddr(fixupAddr, 8)
    offsetOfFooUsed0 = offsetVMAddr(fooClassFooUsed0VMAddr, -16384)
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][nextFixupAddr] == "kc(0) + " + offsetOfFooUsed0


# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-static -mkernel -nostdlib -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-image_base,0x10000 -Wl,-segaddr,__HIB,0x4000 -Wl,-add_split_seg_info -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r--
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.cpp -o extensions/foo.kext/foo -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -DFOO_USED=1
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-data_const bar.cpp -o extensions/bar.kext/bar -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers
# [~]> rm -r extensions/*.kext/*.ld

