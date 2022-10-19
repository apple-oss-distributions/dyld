

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
    return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
    return 0;
}

KMOD_EXPLICIT_DECL(com.apple.bar2, "1.0.0", (void*)startKext, (void*)endKext)

#include "bar1.h"

using namespace X;

class Bar2 : public Bar1
{
    OSDeclareDefaultStructors( Bar2 )
    
public:
    virtual int foo() override;
};

OSDefineMetaClassAndStructors( Bar2, Bar1 )

int Bar2::foo() {
	return 1;
}
