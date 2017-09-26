
// BUILD:  $CXX main.cxx  -o $BUILD_DIR/operator-new.exe

// RUN:  ./operator-new.exe

#include <stdio.h>
#include <new>



//
// This test case verifies that calling operator new[] in libstdc++.dylib
// will turn around and call operator new in this main exectuable
//

static void* ptr;

void* operator new(size_t s) throw (std::bad_alloc)
{
  ptr = malloc(s);
  return ptr;
}

int main()
{
	printf("[BEGIN] operator-new\n");

    char* stuff = new char[24];
    if ( (void*)stuff == ptr )
        printf("[PASS] operator-new\n");
    else
        printf("[FAIL] operator-new\n");

	return 0;
}


