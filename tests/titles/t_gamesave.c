/* OurBox save-game test: sign in the offline user, write/read/delete blobs,
 * enumerate containers, quota, and persistence across provider reopen. */
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "jbtest.h"
typedef struct XAsyncBlock { void *queue; void *context; void *callback; unsigned char internal[32]; } XAsyncBlock;
typedef void *XUserHandle, *XGameSaveProviderHandle, *XGameSaveContainerHandle, *XGameSaveUpdateHandle;
typedef struct { const char *name; const char *displayName; uint32_t blobCount; uint64_t totalSize; time_t lastModifiedTime; bool needsSync; } XGameSaveContainerInfo;
typedef struct { const char *name; uint32_t size; } XGameSaveBlobInfo;
typedef struct { XGameSaveBlobInfo info; uint8_t *data; } XGameSaveBlob;
__declspec(dllimport) HRESULT XGameRuntimeInitialize(void);
__declspec(dllimport) HRESULT XAsyncGetStatus(XAsyncBlock *, bool);
__declspec(dllimport) HRESULT XUserAddAsync(uint32_t, XAsyncBlock *);
__declspec(dllimport) HRESULT XUserAddResult(XAsyncBlock *, XUserHandle *);
__declspec(dllimport) HRESULT XGameSaveInitializeProviderAsync(XUserHandle, const char *, bool, XAsyncBlock *);
__declspec(dllimport) HRESULT XGameSaveInitializeProviderResult(XAsyncBlock *, XGameSaveProviderHandle *);
__declspec(dllimport) HRESULT XGameSaveInitializeProvider(XUserHandle, const char *, bool, XGameSaveProviderHandle *);
__declspec(dllimport) void XGameSaveCloseProvider(XGameSaveProviderHandle);
__declspec(dllimport) HRESULT XGameSaveGetRemainingQuota(XGameSaveProviderHandle, int64_t *);
__declspec(dllimport) HRESULT XGameSaveCreateContainer(XGameSaveProviderHandle, const char *, XGameSaveContainerHandle *);
__declspec(dllimport) void XGameSaveCloseContainer(XGameSaveContainerHandle);
__declspec(dllimport) HRESULT XGameSaveDeleteContainer(XGameSaveProviderHandle, const char *);
__declspec(dllimport) HRESULT XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle, void *, bool (CALLBACK *)(const XGameSaveContainerInfo *, void *));
__declspec(dllimport) HRESULT XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle, void *, bool (CALLBACK *)(const XGameSaveBlobInfo *, void *));
__declspec(dllimport) HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle, const char **, uint32_t *, size_t, XGameSaveBlob *);
__declspec(dllimport) HRESULT XGameSaveCreateUpdate(XGameSaveContainerHandle, const char *, XGameSaveUpdateHandle *);
__declspec(dllimport) void XGameSaveCloseUpdate(XGameSaveUpdateHandle);
__declspec(dllimport) HRESULT XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle, const char *, const uint8_t *, size_t);
__declspec(dllimport) HRESULT XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle, const char *);
__declspec(dllimport) HRESULT XGameSaveSubmitUpdate(XGameSaveUpdateHandle);

static int g_containers; static char g_disp[64];
static bool CALLBACK on_container(const XGameSaveContainerInfo *i, void *c)
{ (void)c; if (!strcmp(i->name, "slot/1:\"odd\"")) { g_containers++; snprintf(g_disp, sizeof g_disp, "%s", i->displayName); } return true; }
static int g_blobs;
static bool CALLBACK on_blob(const XGameSaveBlobInfo *i, void *c) { (void)i; (void)c; g_blobs++; return true; }

int main(void)
{
    CHECK(XGameRuntimeInitialize() == S_OK);
    XAsyncBlock ab; memset(&ab, 0, sizeof ab);
    XUserHandle user = NULL;
    CHECK(XUserAddAsync(1, &ab) == S_OK && XAsyncGetStatus(&ab, true) == S_OK && XUserAddResult(&ab, &user) == S_OK);

    memset(&ab, 0, sizeof ab);
    XGameSaveProviderHandle p = NULL;
    CHECK(XGameSaveInitializeProviderAsync(user, "00000000-0000-0000-0000-00004a424f58", false, &ab) == S_OK);
    CHECK(XAsyncGetStatus(&ab, true) == S_OK && XGameSaveInitializeProviderResult(&ab, &p) == S_OK && p);
    XGameSaveDeleteContainer(p, "slot/1:\"odd\""); /* clean slate if a previous run left it */
    int64_t q0 = 0; CHECK(XGameSaveGetRemainingQuota(p, &q0) == S_OK && q0 > 0);

    /* write two blobs (one 1 MB), with a name that is not a valid file name */
    static uint8_t big[1 << 20]; for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 31);
    XGameSaveContainerHandle c = NULL; XGameSaveUpdateHandle u = NULL;
    CHECK(XGameSaveCreateContainer(p, "slot/1:\"odd\"", &c) == S_OK);
    CHECK(XGameSaveCreateUpdate(c, "Chapter 3 - Docks", &u) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(u, "player", (const uint8_t *)"hp=100;x=5", 10) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(u, "world*?", big, sizeof big) == S_OK);
    CHECK(XGameSaveSubmitUpdate(u) == S_OK);
    XGameSaveCloseUpdate(u);
    int64_t q1 = 0; XGameSaveGetRemainingQuota(p, &q1);
    CHECK(q0 - q1 == (int64_t)sizeof big + 10);
    XGameSaveCloseContainer(c); XGameSaveCloseProvider(p);

    /* reopen: data persisted */
    CHECK(XGameSaveInitializeProvider(user, "00000000-0000-0000-0000-00004a424f58", false, &p) == S_OK);
    CHECK(XGameSaveEnumerateContainerInfo(p, NULL, on_container) == S_OK && g_containers == 1 && !strcmp(g_disp, "Chapter 3 - Docks"));
    CHECK(XGameSaveCreateContainer(p, "slot/1:\"odd\"", &c) == S_OK);
    CHECK(XGameSaveEnumerateBlobInfo(c, NULL, on_blob) == S_OK && g_blobs == 2);
    size_t bufsz = 2 * sizeof(XGameSaveBlob) + sizeof big + 64;
    XGameSaveBlob *blobs = malloc(bufsz);
    const char *names[] = { "player", "world*?" }; uint32_t n = 2;
    CHECK(XGameSaveReadBlobData(c, names, &n, 16, blobs) == (HRESULT)0x8007007A); /* too small */
    CHECK(XGameSaveReadBlobData(c, names, &n, bufsz, blobs) == S_OK && n == 2);
    CHECK(blobs[0].info.size == 10 && !memcmp(blobs[0].data, "hp=100;x=5", 10));
    CHECK(blobs[1].info.size == sizeof big && !memcmp(blobs[1].data, big, sizeof big));

    /* delete one blob, overwrite the other */
    CHECK(XGameSaveCreateUpdate(c, NULL, &u) == S_OK);
    CHECK(XGameSaveSubmitBlobDelete(u, "world*?") == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(u, "player", (const uint8_t *)"hp=42", 5) == S_OK);
    CHECK(XGameSaveSubmitUpdate(u) == S_OK); XGameSaveCloseUpdate(u);
    n = 0; CHECK(XGameSaveReadBlobData(c, NULL, &n, bufsz, blobs) == S_OK && n == 1 && blobs[0].info.size == 5 && !memcmp(blobs[0].data, "hp=42", 5));

    /* an update that is closed without submitting changes nothing */
    CHECK(XGameSaveCreateUpdate(c, NULL, &u) == S_OK);
    CHECK(XGameSaveSubmitBlobWrite(u, "player", (const uint8_t *)"LOST", 4) == S_OK);
    XGameSaveCloseUpdate(u);
    n = 0; CHECK(XGameSaveReadBlobData(c, NULL, &n, bufsz, blobs) == S_OK && n == 1 && !memcmp(blobs[0].data, "hp=42", 5));

    XGameSaveCloseContainer(c);
    CHECK(XGameSaveDeleteContainer(p, "slot/1:\"odd\"") == S_OK);
    CHECK(XGameSaveDeleteContainer(p, "slot/1:\"odd\"") < 0);
    XGameSaveCloseProvider(p);
    free(blobs);
    DONE();
}
