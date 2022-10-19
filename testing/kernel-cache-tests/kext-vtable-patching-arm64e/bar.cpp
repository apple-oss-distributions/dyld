

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.bar, "1.0.0", (void*)startKext, (void*)endKext)

#include "bar.h"

OSDefineMetaClassAndStructors( Bar, Foo )

int Bar::foo() {
	return 1;
}

int bar() {
	Bar* bar = new Bar();
	return bar->foo();
}
