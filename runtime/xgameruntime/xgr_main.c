/* XGameRuntime core and XSystem. */
#include "xgr.h"
#include <stdlib.h>
#include <string.h>

int xgr_verbose;
static volatile LONG g_init_count;
static uint32_t g_title_id;
static bool g_title_id_known;

/* XSystemDeviceType values. (guess) from the public reference; verify. */
enum { DevUnknown = 0, DevPc = 1, DevXboxOne = 2, DevXboxOneS = 3, DevXboxOneX = 4, DevXboxOneXDevkit = 5,
       DevScarlettLockhart = 6 /* Series S */, DevScarlettAnaconda = 7 /* Series X */, DevScarlettDevkit = 8 };

/* Title ID: JACKBOX_TITLE_ID env (hex) or <TitleId> in MicrosoftGame.config
 * next to the title (the public GDK title configuration file). */
static void load_title_id(void)
{
    char buf[64];
    if (GetEnvironmentVariableA("JACKBOX_TITLE_ID", buf, sizeof buf)) {
        g_title_id = (uint32_t)strtoul(buf, NULL, 16); g_title_id_known = true; return;
    }
    char dir[MAX_PATH], path[MAX_PATH + 32];
    if (!GetEnvironmentVariableA("JACKBOX_TITLE_DIR", dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s\\MicrosoftGame.config", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char xml[16384]; size_t n = fread(xml, 1, sizeof xml - 1, f); fclose(f); xml[n] = 0;
    char *t = strstr(xml, "<TitleId>");
    if (t) { g_title_id = (uint32_t)strtoul(t + 9, NULL, 16); g_title_id_known = true; }
    XGR_LOG("MicrosoftGame.config TitleId=%08X", g_title_id);
}

uint32_t xgr_title_id(void) { return g_title_id_known ? g_title_id : 0; }

XGR_API HRESULT XGameRuntimeInitialize(void)
{
    if (InterlockedIncrement(&g_init_count) == 1) {
        char v[8]; xgr_verbose = GetEnvironmentVariableA("JACKBOX_LOG", v, sizeof v) && v[0] != '0';
        load_title_id();
        XGR_LOG("initialized");
    }
    return S_OK;
}

XGR_API void XGameRuntimeUninitialize(void)
{
    if (g_init_count > 0) InterlockedDecrement(&g_init_count);
}

XGR_API HRESULT XGameGetXboxTitleId(uint32_t *titleId)
{
    if (!titleId) return E_POINTER;
    if (g_init_count <= 0) return E_GAMERUNTIME_NOT_INITIALIZED;
    if (!g_title_id_known) return E_FAIL; /* (guess) error when no TitleId is configured */
    *titleId = g_title_id;
    return S_OK;
}

XGR_API int32_t XSystemGetDeviceType(void)
{
    char v[16];
    if (GetEnvironmentVariableA("JACKBOX_DEVICE", v, sizeof v)) {
        if (!_stricmp(v, "seriesx")) return DevScarlettAnaconda;
        if (!_stricmp(v, "pc")) return DevPc;
    }
    return DevScarlettLockhart;
}

/* Console ID: stable per host, derived from the Windows machine GUID-free
 * volume serial so no personal identifier is exposed. Format (guess). */
XGR_API HRESULT XSystemGetConsoleId(size_t size, char *out, size_t *used)
{
    DWORD serial = 0;
    GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0);
    char id[40];
    int n = snprintf(id, sizeof id, "JB%08lX.0000.0000.0000.000000000000", (unsigned long)serial) + 1;
    if (used) *used = (size_t)n;
    if (!out) return E_POINTER;
    if (size < (size_t)n) return E_NOT_SUFFICIENT_BUFFER;
    memcpy(out, id, (size_t)n);
    return S_OK;
}
