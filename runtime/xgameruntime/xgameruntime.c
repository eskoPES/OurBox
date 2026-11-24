/*
 * JackBox clean-room xgameruntime.dll
 *
 * Written from the public GDK API reference on learn.microsoft.com, not from
 * Microsoft headers or binaries. Built freestanding (no CRT, no Windows SDK)
 * so it compiles anywhere clang + lld-link exist.
 *
 * Keep the exported names in sync with XGAMERUNTIME_IMPLEMENTED in
 * crates/hle/src/lib.rs.
 */
#include <stddef.h>
#include <stdint.h>

typedef int32_t HRESULT;
#define S_OK            ((HRESULT)0)
#define E_NOTIMPL       ((HRESULT)0x80004001)
#define E_POINTER       ((HRESULT)0x80004003)
#define E_INVALIDARG    ((HRESULT)0x80070057)
#define E_NOT_SUFFICIENT_BUFFER ((HRESULT)0x8007007A)
/* (guess) GDK returns this when the runtime was not initialized. */
#define E_GAMERUNTIME_NOT_INITIALIZED ((HRESULT)0x89240100)

#define EXPORT __declspec(dllexport)

/* XSystemDeviceType values. (guess) from the public reference; verify. */
enum {
    XSystemDeviceType_Unknown = 0,
    XSystemDeviceType_Pc = 1,
    XSystemDeviceType_XboxOne = 2,
    XSystemDeviceType_XboxOneS = 3,
    XSystemDeviceType_XboxOneX = 4,
    XSystemDeviceType_XboxOneXDevkit = 5,
    XSystemDeviceType_XboxScarlettLockhart = 6, /* Series S */
    XSystemDeviceType_XboxScarlettAnaconda = 7, /* Series X */
    XSystemDeviceType_XboxScarlettDevkit = 8,
};

static int g_initialized;
static uint32_t g_title_id = 0x4A424F58; /* "JBOX"; override with JackBoxSetTitleId */
static int32_t g_device_type = XSystemDeviceType_XboxScarlettLockhart;

static const char k_console_id[] = "JACKBOX-0000-0000-0000-000000000000";

/* ---- JackBox control surface (not part of the GDK) ---------------------- */
EXPORT void JackBoxSetTitleId(uint32_t id) { g_title_id = id; }
EXPORT void JackBoxSetDeviceType(int32_t t) { g_device_type = t; }

/* ---- XGameRuntime ------------------------------------------------------- */
EXPORT HRESULT XGameRuntimeInitialize(void) { g_initialized = 1; return S_OK; }
EXPORT void XGameRuntimeUninitialize(void) { g_initialized = 0; }

EXPORT HRESULT XGameGetXboxTitleId(uint32_t *titleId)
{
    if (!titleId) return E_POINTER;
    if (!g_initialized) return E_GAMERUNTIME_NOT_INITIALIZED;
    *titleId = g_title_id;
    return S_OK;
}

/* ---- XSystem ------------------------------------------------------------ */
EXPORT int32_t XSystemGetDeviceType(void) { return g_device_type; }

EXPORT HRESULT XSystemGetConsoleId(size_t consoleIdSize, char *consoleId, size_t *consoleIdUsed)
{
    const size_t need = sizeof(k_console_id);
    if (consoleIdUsed) *consoleIdUsed = need;
    if (!consoleId) return E_POINTER;
    if (consoleIdSize < need) return E_NOT_SUFFICIENT_BUFFER;
    for (size_t i = 0; i < need; i++) consoleId[i] = k_console_id[i];
    return S_OK;
}

/* ---- XTaskQueue (milestone M2) ------------------------------------------ */
typedef void *XTaskQueueHandle;
EXPORT HRESULT XTaskQueueCreate(int32_t workMode, int32_t completionMode, XTaskQueueHandle *queue)
{
    (void)workMode; (void)completionMode;
    if (!queue) return E_POINTER;
    *queue = NULL;
    return E_NOTIMPL;
}
EXPORT void XTaskQueueCloseHandle(XTaskQueueHandle queue) { (void)queue; }

/* lld-link /noentry means no DllMain is required. */
