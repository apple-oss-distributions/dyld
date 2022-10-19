

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.bar, "1.0.0", startKext, endKext)

int g = 0;

int bar() {
	return g;
}

__typeof(&bar) barPtr = &bar;

int baz() {
	return barPtr();
}
