

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

__attribute__((weak))
extern int weakValue;

extern int gOSKextUnresolved;

int bar() {
	// Missing weak import test
	if ( &weakValue != &gOSKextUnresolved )
		return 0;
	return weakValue;
}