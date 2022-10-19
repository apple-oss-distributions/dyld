
int x = 0;
int* g = &x;

int doFixups()
{
	return *g;
}

__attribute__((section(("__TEXT_BOOT_EXEC, __text"))))
int _start() {
	return doFixups();
}