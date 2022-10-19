

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
#include <memory.h>

void* operator new(size_t size) { return (void*)1; }
void operator delete(void*) { }

OSDefineMetaClassAndStructors( Foo, OSObject )

OSMetaClassDefineReservedUnused( Foo, 0 )
OSMetaClassDefineReservedUnused( Foo, 1 )
OSMetaClassDefineReservedUnused( Foo, 2 )
OSMetaClassDefineReservedUnused( Foo, 3 )
