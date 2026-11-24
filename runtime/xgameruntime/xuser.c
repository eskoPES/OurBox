/* XUser: offline local user. JackBox has no Xbox network services; it
 * provides one signed-in local player so titles can get past user sign-in.
 * Gamertag from JACKBOX_GAMERTAG (default "JackBoxPlayer"). */
#include "xgr.h"
#include <string.h>

XGR_API HRESULT XAsyncBegin(XAsyncBlock *, void *, const void *, const char *, XAsyncProvider *);
XGR_API HRESULT XAsyncSchedule(XAsyncBlock *, uint32_t);
XGR_API void XAsyncComplete(XAsyncBlock *, HRESULT, size_t);
XGR_API HRESULT XAsyncGetResult(XAsyncBlock *, const void *, size_t, void *, size_t *);

typedef struct XUser { volatile LONG refs; uint64_t xuid; uint64_t local_id; char gamertag[64]; } XUser;
typedef XUser *XUserHandle;
typedef struct { uint64_t value; } XUserLocalId;
enum { XUserState_SignedIn = 0, XUserState_SigningOut = 1, XUserState_SignedOut = 2 }; /* (guess) values */

static XUser g_user; /* single local player, never freed */
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_user(PINIT_ONCE o, void *p, void **c)
{
    (void)o; (void)p; (void)c;
    g_user.refs = 1;
    g_user.xuid = 0x0009FFFF4A424F58ull; /* offline, recognisably not a real XUID */
    g_user.local_id = 1;
    if (!GetEnvironmentVariableA("JACKBOX_GAMERTAG", g_user.gamertag, sizeof g_user.gamertag))
        strcpy(g_user.gamertag, "JackBoxPlayer");
    return TRUE;
}

static HRESULT CALLBACK add_provider(XAsyncOp op, const XAsyncProviderData *d)
{
    switch (op) {
    case XAsyncOp_Begin: return XAsyncSchedule(d->async, 0);
    case XAsyncOp_DoWork:
        InitOnceExecuteOnce(&g_once, init_user, NULL, NULL);
        XAsyncComplete(d->async, S_OK, sizeof(XUserHandle));
        return S_OK;
    case XAsyncOp_GetResult:
        InterlockedIncrement(&g_user.refs);
        *(XUserHandle *)d->buffer = &g_user;
        return S_OK;
    default: return S_OK;
    }
}


XGR_API HRESULT XUserAddAsync(uint32_t options, XAsyncBlock *async)
{
    (void)options;
    XGR_LOG("XUserAddAsync(options=0x%x): offline local user", options);
    return XAsyncBegin(async, NULL, (const void *)XUserAddAsync, "XUserAddAsync", add_provider);
}

XGR_API HRESULT XUserAddResult(XAsyncBlock *async, XUserHandle *user)
{
    if (!user) return E_POINTER;
    return XAsyncGetResult(async, (const void *)XUserAddAsync, sizeof *user, user, NULL);
}

XGR_API HRESULT XUserDuplicateHandle(XUserHandle h, XUserHandle *out)
{
    if (!h || !out) return E_INVALIDARG;
    InterlockedIncrement(&h->refs); *out = h; return S_OK;
}

XGR_API void XUserCloseHandle(XUserHandle h) { if (h) InterlockedDecrement(&h->refs); }

XGR_API HRESULT XUserGetId(XUserHandle h, uint64_t *id)
{ if (!h || !id) return E_INVALIDARG; *id = h->xuid; return S_OK; }

XGR_API HRESULT XUserGetLocalId(XUserHandle h, XUserLocalId *id)
{ if (!h || !id) return E_INVALIDARG; id->value = h->local_id; return S_OK; }

XGR_API HRESULT XUserGetIsGuest(XUserHandle h, bool *guest)
{ if (!h || !guest) return E_INVALIDARG; *guest = false; return S_OK; }

XGR_API HRESULT XUserGetState(XUserHandle h, int32_t *state)
{ if (!h || !state) return E_INVALIDARG; *state = XUserState_SignedIn; return S_OK; }

XGR_API int32_t XUserCompare(XUserHandle a, XUserHandle b)
{ uint64_t x = a ? a->local_id : 0, y = b ? b->local_id : 0; return x < y ? -1 : x > y; }

XGR_API HRESULT XUserGetGamertag(XUserHandle h, int32_t component, size_t size, char *out, size_t *used)
{
    (void)component; /* every component maps to the same local name */
    if (!h) return E_INVALIDARG;
    size_t n = strlen(h->gamertag) + 1;
    if (used) *used = n;
    if (!out || size < n) return E_NOT_SUFFICIENT_BUFFER;
    memcpy(out, h->gamertag, n);
    return S_OK;
}
