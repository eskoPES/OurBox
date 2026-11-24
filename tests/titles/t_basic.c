#include <windows.h>
#include <string.h>
#include "jbtest.h"
__declspec(dllimport) int titledll_value(void);
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    HMODULE self = GetModuleHandleA(NULL);
    CHECK((void *)self <= (void *)&main && (char *)&main < (char *)self + 0x1000000);
    char *p = malloc(1 << 20); CHECK(p); memset(p, 0xAB, 1 << 20); free(p);
    CHECK(titledll_value() == 42); /* relocated pointer (30) + DllMain ran (12) */
    DONE();
}
