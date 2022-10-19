
#include "../kernel-test-runner.h"

#include <mach/kmod.h>

int startKext() {
    return 0;
}
int endKext() {
    return 0;
}

KMOD_EXPLICIT_DECL(com.apple.export, "1.0.0", startKext, endKext)

int kernelExport() {
	return 1;
}

int kernelExportDirect() {
    return 42;
}
