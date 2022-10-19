

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.foo, "1.0.0", (void*)startKext, (void*)endKext)

#include "foo.h"

OSDefineMetaClassAndStructors( Foo, OSObject )

// Index 0 has been replaced with a method
OSMetaClassDefineReservedUsed(Foo, 0)
OSMetaClassDefineReservedUnused( Foo, 1 )
OSMetaClassDefineReservedUnused( Foo, 2 )
OSMetaClassDefineReservedUnused( Foo, 3 )

int Foo::foo() {
	return 0;
}

int Foo::fooUsed0() {
	return 0;
}

int foo() {
	Foo* foo = new Foo();
	return foo->foo() + foo->fooUsed0();
}
