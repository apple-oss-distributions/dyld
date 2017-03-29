
// BUILD:  $CC main.c -o $BUILD_DIR/dlopen-realpath.exe
// BUILD:  cd $BUILD_DIR && ln -s ./IOKit.framework/IOKit IOKit && ln -s /System/Library/Frameworks/IOKit.framework IOKit.framework

// RUN:  ./dlopen-realpath.exe

#include <stdio.h>
#include <dlfcn.h>


static void tryImage(const char* path)
{
    printf("[BEGIN] dlopen-realpath %s\n", path);
	void* handle = dlopen(path, RTLD_LAZY);
	if ( handle == NULL ) {
        printf("dlerror(): %s\n", dlerror());
        printf("[FAIL] dlopen-realpath %s\n", path);
		return;
	}

	int result = dlclose(handle);
	if ( result != 0 ) {
        printf("dlclose() returned %c\n", result);
        printf("[FAIL] dlopen-realpath %s\n", path);
		return;
	}

    printf("[PASS] dlopen-realpath %s\n", path);
}



int main()
{
	tryImage("./IOKit.framework/IOKit");
	tryImage("./././IOKit/../IOKit.framework/IOKit");
        tryImage("./IOKit");

	return 0;
}

