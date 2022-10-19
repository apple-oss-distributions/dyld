

#include "../kmod.h"

__attribute__((visibility(("hidden"))))
int startKext() {
	return 0;
}

__attribute__((visibility(("hidden"))))
int endKext() {
	return 0;
}

KMOD_EXPLICIT_DECL(com.apple.baz, "1.0.0", (void*)startKext, (void*)endKext)

#include "bar.h"

class Baz : public Bar
{
    OSDeclareDefaultStructors( Baz )
    
public:
    virtual int foo();
};

OSDefineMetaClassAndStructors( Baz, Bar )

int Baz::foo() {
	return 1;
}

int baz() {
	Baz* baz = new Baz();
	return baz->foo();
}
