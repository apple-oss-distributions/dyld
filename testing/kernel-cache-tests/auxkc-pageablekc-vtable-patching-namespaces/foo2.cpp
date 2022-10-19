

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.foo2, "1.0.0", (void*)startKext, (void*)endKext)

#include "foo2.h"

using namespace X;

OSDefineMetaClassAndStructors( Foo2, Foo1 )

int Foo2::foo() {
	return 0;
}

int Foo2::foo1Used0() {
	return 0;
}
