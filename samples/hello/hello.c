/* Minimal GDK-style console program used to test the JackBox loader path.
 * Milestone M1: this should run under Wine with JackBox's xgameruntime.dll. */
#include <stdint.h>
typedef int32_t HRESULT;
__declspec(dllimport) HRESULT XGameRuntimeInitialize(void);
__declspec(dllimport) HRESULT XGameGetXboxTitleId(uint32_t *);
__declspec(dllimport) int32_t XSystemGetDeviceType(void);
__declspec(dllimport) HRESULT XUserAddAsync(int32_t, void *); /* not implemented yet: shows in coverage */
__declspec(dllimport) void __stdcall ExitProcess(uint32_t);

void mainCRTStartup(void)
{
    uint32_t tid = 0;
    if (XGameRuntimeInitialize() < 0) ExitProcess(1);
    if (XGameGetXboxTitleId(&tid) < 0) ExitProcess(2);
    if (XSystemGetDeviceType() != 6) ExitProcess(3); /* expect Series S */
    static volatile int never = 0;
    if (never) XUserAddAsync(0, 0);
    ExitProcess(tid == 0x4A424F58 ? 0 : 4);
}
