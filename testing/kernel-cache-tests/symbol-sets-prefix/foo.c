

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.foo, "1.0.0", startKext, endKext)

extern int symbol_from_xnu0();
extern int symbol_from_xnu1();
extern int symbol_from_xnu2();
extern int symbol_from_xnu3();

int foo() {
	return symbol_from_xnu0() + symbol_from_xnu1() + symbol_from_xnu2() + symbol_from_xnu3();
}
