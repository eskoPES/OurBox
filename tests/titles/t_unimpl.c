/* Calls a GDK function JackBox does not implement. Expected: the loader
 * reports xgameruntime.dll!XUserAddAsync at load, traps the call and exits
 * with JB_EXIT_UNIMPLEMENTED (0x4A420002). */
#include <windows.h>
#include <stdio.h>
__declspec(dllimport) HRESULT XUserAddAsync(int options, void *async);
int main(void)
{
    printf("calling XUserAddAsync\n"); fflush(stdout);
    HRESULT hr = XUserAddAsync(0, NULL);
    printf("returned 0x%08lx\n", (unsigned long)hr); /* reached only with JACKBOX_UNIMPL=continue */
    return hr == (HRESULT)0x80004001 ? 77 : 1;
}
