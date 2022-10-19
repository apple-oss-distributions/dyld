

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.foo1, "1.0.0", (void*)startKext, (void*)endKext)

#include "foo1.h"

using namespace X;

OSDefineMetaClassAndStructors( Foo1, KernelClass )

// Index 0 has been replaced with a method
OSMetaClassDefineReservedUsed(Foo1, 0)
// Index 1 has been replaced with a method
OSMetaClassDefineReservedUsed( Foo1, 1 )

OSMetaClassDefineReservedUnused( Foo1, 2 )
OSMetaClassDefineReservedUnused( Foo1, 3 )

int hack __asm("___cxa_pure_virtual");

int Foo1::foo() {
	return 0;
}

int Foo1::foo1Used0() {
	return 0;
}

int Foo1::foo1Used1() {
	return 0;
}
