#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# This verifies that a kext can patch vtables against another kext
# We put foo.kext in the base KC so that the patch slot in bar.kext has to know to use the correct fixup level in the fixup chain

def check(kernel_cache):
    kernel_cache.buildKernelCollection("x86_64", "/pageablekc-vtable-patching/main.kc", "/pageablekc-vtable-patching/main.kernel", "/pageablekc-vtable-patching/extensions", ["com.apple.foo"], [])
    kernel_cache.analyze("/pageablekc-vtable-patching/main.kc", ["-layout", "-arch", "x86_64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 2
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"
    assert kernel_cache.dictionary()["dylibs"][1]["name"] == "com.apple.foo"

    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/pageablekc-vtable-patching/main.kc", ["-symbols", "-arch", "x86_64"])
    
    # From foo, we want to know where the vtable is, and the foo() and fooUsed0() slots in that vtable
    # Foo::foo()
    fooFooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "__ZN3Foo3fooEv")
    # Foo::fooUsed0()
    fooFooUsed0VMAddr = findGlobalSymbolVMAddr(kernel_cache, 1, "__ZN3Foo8fooUsed0Ev")

    # Check the fixups
    kernel_cache.analyze("/pageablekc-vtable-patching/main.kc", ["-fixups", "-arch", "x86_64"])
    
    # In vtable for Foo, we match the entry for Foo::foo() by looking for its value on the RHS of the fixup
    fooFooFixupAddr = findFixupVMAddr(kernel_cache, "kc(0) + " + fooFooVMAddr)
    # Then the following fixup should be to Foo::fooUsed0()
    nextFixupAddr = offsetVMAddr(fooFooFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixupAddr] == "kc(0) + " + fooFooUsed0VMAddr


    # -----------------------------------------------------------
    # Now build an pageable cache using the baseline kernel collection
    kernel_cache.buildPageableKernelCollection("x86_64", "/pageablekc-vtable-patching/pageable.kc", "/pageablekc-vtable-patching/main.kc", "/pageablekc-vtable-patching/extensions", ["com.apple.bar"], [])
    kernel_cache.analyze("/pageablekc-vtable-patching/pageable.kc", ["-layout", "-arch", "x86_64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 1
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.bar"


    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/pageablekc-vtable-patching/pageable.kc", ["-symbols", "-arch", "x86_64"])
    
    # From bar, find the vtable and its override of foo()
    # Bar::foo()
    barFooVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN3Bar3fooEv")


    # Check the fixups
    kernel_cache.analyze("/pageablekc-vtable-patching/pageable.kc", ["-fixups", "-arch", "x86_64"])

    # In bar, again match the entry for its Bar::foo() symbol
    barFooFixupAddr = findPagableFixupVMAddr(kernel_cache, 0, "kc(1) + " + barFooVMAddr)
    # And if the patching was correct, then following entry should be to Foo::fooUsed0()
    nextFixupAddr = offsetVMAddr(barFooFixupAddr, 8)
    offsetOfFooUsed0 = offsetVMAddr(fooFooUsed0VMAddr, -16384)
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][nextFixupAddr] == "kc(0) + " + offsetOfFooUsed0


# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-static -mkernel -nostdlib -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-image_base,0x10000 -Wl,-segaddr,__HIB,0x4000 -Wl,-add_split_seg_info -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r--
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.cpp -o extensions/foo.kext/foo -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -DFOO_USED=1
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-data_const bar.cpp -o extensions/bar.kext/bar -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers
# [~]> rm -r extensions/*.kext/*.ld

