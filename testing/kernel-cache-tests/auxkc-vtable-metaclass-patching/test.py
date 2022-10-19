#!/usr/bin/python3

import os
import KernelCollection
from FixupHelpers import *

# OSMetaClass in the kernel has a vtable which has to be patched in to Foo::MetaClass

def check(kernel_cache):
    enableLogging = False
    kernel_cache.buildKernelCollection("x86_64", "/auxkc-vtable-metaclass-patching/main.kc", "/auxkc-vtable-metaclass-patching/main.kernel", None, [], [])
    kernel_cache.analyze("/auxkc-vtable-metaclass-patching/main.kc", ["-layout", "-arch", "x86_64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 1
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.kernel"

    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/auxkc-vtable-metaclass-patching/main.kc", ["-symbols", "-arch", "x86_64"])
    
    # From OSMetaClass, we want to know where the vtable is, and the foo() and fooUsed0() slots in that vtable
    basePlaceholderVMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN15OSMetaClassBase11placeholderEv")
    baseUsed4VMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN15OSMetaClassBase18metaclassBaseUsed4Ev")
    baseUsed5VMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN15OSMetaClassBase18metaclassBaseUsed5Ev")
    baseUsed6VMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN15OSMetaClassBase18metaclassBaseUsed6Ev")
    baseUsed7VMAddr = findGlobalSymbolVMAddr(kernel_cache, 0, "__ZN15OSMetaClassBase18metaclassBaseUsed7Ev")
    if enableLogging:
        print("basePlaceholderVMAddr: " + basePlaceholderVMAddr)
        print("baseUsed4VMAddr: " + baseUsed4VMAddr)
        print("baseUsed5VMAddr: " + baseUsed5VMAddr)
        print("baseUsed6VMAddr: " + baseUsed6VMAddr)
        print("baseUsed7VMAddr: " + baseUsed7VMAddr)


    # Check the fixups
    kernel_cache.analyze("/auxkc-vtable-metaclass-patching/main.kc", ["-fixups", "-arch", "x86_64"])
    
    # In vtable for OSMetaClass, we match the entry for OSMetaClass::placeholder() by looking for its value on the RHS of the fixup
    basePlaceholderFixupAddr = findFixupVMAddr(kernel_cache, "kc(0) + " + basePlaceholderVMAddr + " : pointer64")
    if enableLogging:
        print("basePlaceholderFixupAddr: " + basePlaceholderFixupAddr)
    # Then the following fixup should be to OSMetaClass::metaclassBaseUsed4()
    nextFixupAddr = offsetVMAddr(basePlaceholderFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixupAddr] == "kc(0) + " + baseUsed4VMAddr + " : pointer64"
    # Then OSMetaClass::metaclassBaseUsed5()
    nextFixupAddr = offsetVMAddr(nextFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixupAddr] == "kc(0) + " + baseUsed5VMAddr + " : pointer64"
    # Then OSMetaClass::metaclassBaseUsed6()
    nextFixupAddr = offsetVMAddr(nextFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixupAddr] == "kc(0) + " + baseUsed6VMAddr + " : pointer64"
    # Then OSMetaClass::metaclassBaseUsed7()
    nextFixupAddr = offsetVMAddr(nextFixupAddr, 8)
    assert kernel_cache.dictionary()["fixups"][nextFixupAddr] == "kc(0) + " + baseUsed7VMAddr + " : pointer64"


    # -----------------------------------------------------------
    # Now build an aux cache using the baseline kernel collection
    kernel_cache.buildAuxKernelCollection("x86_64", "/auxkc-vtable-metaclass-patching/aux.kc", "/auxkc-vtable-metaclass-patching/main.kc", "", "/auxkc-vtable-metaclass-patching/extensions", ["com.apple.foo"], [])
    kernel_cache.analyze("/auxkc-vtable-metaclass-patching/aux.kc", ["-layout", "-arch", "x86_64"])

    assert len(kernel_cache.dictionary()["dylibs"]) == 1
    assert kernel_cache.dictionary()["dylibs"][0]["name"] == "com.apple.foo"

    # Get the addresses for the symbols we are looking at.  This will make it easier to work out the fixup slots
    kernel_cache.analyze("/auxkc-vtable-metaclass-patching/aux.kc", ["-symbols", "-arch", "x86_64"])
    
    # From foo.kext, find the vtable and its override of placeholder()
    # Foo::placeholder()
    fooPlaceholderVMAddr = findLocalSymbolVMAddr(kernel_cache, 0, "__ZN3Foo11placeholderEv")
    if enableLogging:
        print("fooPlaceholderVMAddr: " + fooPlaceholderVMAddr)


    # Check the fixups
    kernel_cache.analyze("/auxkc-vtable-metaclass-patching/aux.kc", ["-fixups", "-arch", "x86_64"])

    # Now in foo.kext, match the entry for its Foo::placeholder() symbol
    fooPlaceholderFixupAddr = findAuxFixupVMAddr(kernel_cache, 0, "kc(3) + " + fooPlaceholderVMAddr)
    if enableLogging:
        print("fooPlaceholderFixupAddr: " + fooPlaceholderFixupAddr)

    # And if the patching was correct, then following entry should be to OSMetaClass::metaclassBaseUsed4()
    nextFixupAddr = offsetVMAddr(fooPlaceholderFixupAddr, 8)
    offsetInBaseUsed4 = offsetVMAddr(baseUsed4VMAddr, -16384)
    if enableLogging:
        print("offsetInBaseUsed4: " + offsetInBaseUsed4)
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][nextFixupAddr] == "kc(0) + " + offsetInBaseUsed4

    # Then OSMetaClass::metaclassBaseUsed5()
    nextFixupAddr = offsetVMAddr(nextFixupAddr, 8)
    offsetInBaseUsed5 = offsetVMAddr(baseUsed5VMAddr, -16384)
    if enableLogging:
        print("offsetInBaseUsed5: " + offsetInBaseUsed5)
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][nextFixupAddr] == "kc(0) + " + offsetInBaseUsed5

    # Then OSMetaClass::metaclassBaseUsed6()
    nextFixupAddr = offsetVMAddr(nextFixupAddr, 8)
    offsetInBaseUsed6 = offsetVMAddr(baseUsed6VMAddr, -16384)
    if enableLogging:
        print("offsetInBaseUsed6: " + offsetInBaseUsed6)
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][nextFixupAddr] == "kc(0) + " + offsetInBaseUsed6

    # Then OSMetaClass::metaclassBaseUsed7()
    nextFixupAddr = offsetVMAddr(nextFixupAddr, 8)
    offsetInBaseUsed7 = offsetVMAddr(baseUsed7VMAddr, -16384)
    if enableLogging:
        print("offsetInBaseUsed7: " + offsetInBaseUsed7)
    assert kernel_cache.dictionary()["dylibs"][0]["fixups"][nextFixupAddr] == "kc(0) + " + offsetInBaseUsed7


# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-static -mkernel -nostdlib -Wl,-e,__start -Wl,-pie main.cpp -Wl,-pagezero_size,0x0 -o main.kernel -Wl,-image_base,0x10000 -Wl,-segaddr,__HIB,0x4000 -Wl,-add_split_seg_info -Wl,-install_name,/usr/lib/swift/split.seg.v2.hack -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers -Wl,-sectcreate,__LINKINFO,__symbolsets,SymbolSets.plist -Wl,-segprot,__LINKINFO,r--,r-- -DMETACLASS_BASE_USED=1
# [~]> xcrun -sdk macosx.internal cc -arch x86_64 -Wl,-kext -mkernel -nostdlib -Wl,-add_split_seg_info -Wl,-no_data_const foo.cpp -o extensions/foo.kext/foo -iwithsysroot /System/Library/Frameworks/Kernel.framework/Headers
# [~]> rm -r extensions/*.kext/*.ld

