#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "jbtest.h"
typedef struct XTaskQueueObject *XTaskQueueHandle;
typedef struct XAsyncBlock XAsyncBlock;
typedef void (CALLBACK XAsyncCompletionRoutine)(XAsyncBlock *);
struct XAsyncBlock { XTaskQueueHandle queue; void *context; XAsyncCompletionRoutine *callback; unsigned char internal[32]; };
typedef struct { XAsyncBlock *async; size_t bufferSize; void *buffer; void *context; } XAsyncProviderData;
typedef HRESULT (CALLBACK XAsyncProvider)(int op, const XAsyncProviderData *);
__declspec(dllimport) HRESULT XGameRuntimeInitialize(void);
__declspec(dllimport) HRESULT XGameGetXboxTitleId(uint32_t *);
__declspec(dllimport) int32_t XSystemGetDeviceType(void);
__declspec(dllimport) HRESULT XSystemGetConsoleId(size_t, char *, size_t *);
__declspec(dllimport) HRESULT XTaskQueueCreate(int, int, XTaskQueueHandle *);
__declspec(dllimport) void XTaskQueueCloseHandle(XTaskQueueHandle);
__declspec(dllimport) HRESULT XTaskQueueSubmitCallback(XTaskQueueHandle, int, void *, void (CALLBACK *)(void *, bool));
__declspec(dllimport) HRESULT XTaskQueueSubmitDelayedCallback(XTaskQueueHandle, int, uint32_t, void *, void (CALLBACK *)(void *, bool));
__declspec(dllimport) bool XTaskQueueDispatch(XTaskQueueHandle, int, uint32_t);
__declspec(dllimport) HRESULT XTaskQueueTerminate(XTaskQueueHandle, bool, void *, void (CALLBACK *)(void *));
__declspec(dllimport) HRESULT XAsyncBegin(XAsyncBlock *, void *, const void *, const char *, XAsyncProvider *);
__declspec(dllimport) HRESULT XAsyncSchedule(XAsyncBlock *, uint32_t);
__declspec(dllimport) void XAsyncComplete(XAsyncBlock *, HRESULT, size_t);
__declspec(dllimport) HRESULT XAsyncGetStatus(XAsyncBlock *, bool);
__declspec(dllimport) HRESULT XAsyncGetResultSize(XAsyncBlock *, size_t *);
__declspec(dllimport) HRESULT XAsyncGetResult(XAsyncBlock *, const void *, size_t, void *, size_t *);
__declspec(dllimport) void XAsyncCancel(XAsyncBlock *);
__declspec(dllimport) HRESULT XAsyncRun(XAsyncBlock *, HRESULT (CALLBACK *)(XAsyncBlock *));
typedef struct XUser *XUserHandle;
__declspec(dllimport) HRESULT XUserAddAsync(uint32_t, XAsyncBlock *);
__declspec(dllimport) HRESULT XUserAddResult(XAsyncBlock *, XUserHandle *);
__declspec(dllimport) HRESULT XUserGetId(XUserHandle, uint64_t *);
__declspec(dllimport) HRESULT XUserGetGamertag(XUserHandle, int32_t, size_t, char *, size_t *);
__declspec(dllimport) void XUserCloseHandle(XUserHandle);

enum { Manual, ThreadPool, Serialized, Immediate };
enum { Work, Completion };

static volatile LONG g_count;
static DWORD g_main_tid;
static volatile LONG g_order[8], g_order_n;
static void CALLBACK count_cb(void *ctx, bool canceled) { (void)canceled; InterlockedAdd(&g_count, (LONG)(intptr_t)ctx); }
static void CALLBACK order_cb(void *ctx, bool canceled) { (void)canceled; g_order[InterlockedIncrement(&g_order_n) - 1] = (LONG)(intptr_t)ctx; Sleep(1); }
static volatile LONG g_canceled_seen;
static void CALLBACK cancel_probe(void *ctx, bool canceled) { (void)ctx; if (canceled) InterlockedIncrement(&g_canceled_seen); }
static volatile LONG g_terminated;
static void CALLBACK on_term(void *ctx) { (void)ctx; InterlockedExchange(&g_terminated, 1); }

/* An async provider that computes a 64-bit result on the work port. */
static int g_cleanups;
static HRESULT CALLBACK square_provider(int op, const XAsyncProviderData *d)
{
    switch (op) {
    case 0: return XAsyncSchedule(d->async, 0);                          /* Begin */
    case 1: XAsyncComplete(d->async, S_OK, sizeof(uint64_t)); return S_OK; /* DoWork */
    case 2: *(uint64_t *)d->buffer = (uint64_t)(uintptr_t)d->context * (uint64_t)(uintptr_t)d->context; return S_OK;
    case 4: g_cleanups++; return S_OK;
    default: return S_OK;
    }
}
static volatile LONG g_completion_tid;
static void CALLBACK on_complete(XAsyncBlock *b) { (void)b; InterlockedExchange(&g_completion_tid, (LONG)GetCurrentThreadId()); }
static HRESULT CALLBACK run_work(XAsyncBlock *b) { (void)b; return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); }
static HRESULT CALLBACK never_provider(int op, const XAsyncProviderData *d) { (void)d; (void)op; return S_OK; }

int main(void)
{
    g_main_tid = GetCurrentThreadId();
    uint32_t tid = 0;
    CHECK(XGameGetXboxTitleId(&tid) < 0); /* before init: must fail */
    CHECK(XGameRuntimeInitialize() == S_OK);
    CHECK(XGameGetXboxTitleId(&tid) == S_OK && tid == 0x4A424F58);
    CHECK(XSystemGetDeviceType() == 6);
    char cid[64]; size_t used = 0;
    CHECK(XSystemGetConsoleId(4, cid, &used) == (HRESULT)0x8007007A && used > 4);
    CHECK(XSystemGetConsoleId(sizeof cid, cid, &used) == S_OK && strlen(cid) + 1 == used);

    /* Thread-pool queue. */
    XTaskQueueHandle q;
    CHECK(XTaskQueueCreate(ThreadPool, ThreadPool, &q) == S_OK);
    for (int i = 0; i < 100; i++) CHECK(XTaskQueueSubmitCallback(q, Work, (void *)1, count_cb) == S_OK);
    CHECK(XTaskQueueSubmitDelayedCallback(q, Work, 30, (void *)1000, count_cb) == S_OK);
    for (int i = 0; i < 400 && g_count != 1100; i++) Sleep(5);
    CHECK(g_count == 1100);
    CHECK(XTaskQueueTerminate(q, true, NULL, on_term) == S_OK && g_terminated);
    CHECK(XTaskQueueSubmitCallback(q, Work, NULL, count_cb) == E_ABORT);
    XTaskQueueCloseHandle(q);

    /* Serialized queue keeps FIFO order. */
    CHECK(XTaskQueueCreate(Serialized, Immediate, &q) == S_OK);
    for (int i = 0; i < 8; i++) XTaskQueueSubmitCallback(q, Work, (void *)(intptr_t)i, order_cb);
    for (int i = 0; i < 400 && g_order_n < 8; i++) Sleep(5);
    int in_order = g_order_n == 8; for (int i = 0; i < 8; i++) in_order &= g_order[i] == i;
    CHECK(in_order);
    XTaskQueueTerminate(q, true, NULL, NULL); XTaskQueueCloseHandle(q);

    /* Manual queue: nothing runs until dispatched, on the dispatching thread. */
    CHECK(XTaskQueueCreate(Manual, Manual, &q) == S_OK);
    g_count = 0;
    XTaskQueueSubmitCallback(q, Work, (void *)5, count_cb);
    Sleep(20);
    CHECK(g_count == 0);
    CHECK(XTaskQueueDispatch(q, Work, 0) && g_count == 5);
    CHECK(!XTaskQueueDispatch(q, Work, 10)); /* empty: times out */

    /* XAsync with a result, completion delivered on the manual completion port. */
    XAsyncBlock ab; memset(&ab, 0, sizeof ab);
    ab.queue = q; ab.callback = on_complete;
    CHECK(XAsyncBegin(&ab, (void *)12345, (void *)square_provider, "square", square_provider) == S_OK);
    CHECK(XAsyncGetStatus(&ab, false) == E_PENDING);
    CHECK(XTaskQueueDispatch(q, Work, 1000));
    CHECK(XAsyncGetStatus(&ab, true) == S_OK);
    size_t sz = 0; CHECK(XAsyncGetResultSize(&ab, &sz) == S_OK && sz == 8);
    uint64_t r = 0; CHECK(XAsyncGetResult(&ab, (void *)square_provider, sizeof r, &r, &used) == S_OK && r == 12345ull * 12345ull);
    CHECK(g_cleanups == 1);
    CHECK(g_completion_tid == 0);
    CHECK(XTaskQueueDispatch(q, Completion, 1000) && g_completion_tid == (LONG)g_main_tid);

    /* Cancel a pending op. */
    memset(&ab, 0, sizeof ab); ab.queue = q;
    CHECK(XAsyncBegin(&ab, NULL, NULL, "never", never_provider) == S_OK);
    XAsyncCancel(&ab);
    CHECK(XAsyncGetStatus(&ab, false) == E_ABORT);

    /* Terminate with queued work: callbacks see canceled = true. */
    XTaskQueueSubmitCallback(q, Work, NULL, cancel_probe);
    g_terminated = 0;
    XTaskQueueTerminate(q, false, NULL, on_term);
    while (XTaskQueueDispatch(q, Work, 0)) {}
    CHECK(g_canceled_seen == 1 && g_terminated);
    XTaskQueueCloseHandle(q);

    /* XAsyncRun on the process default queue (thread pool), failing work. */
    memset(&ab, 0, sizeof ab);
    CHECK(XAsyncRun(&ab, run_work) == S_OK);
    CHECK(XAsyncGetStatus(&ab, true) == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));

    /* Offline local user sign-in through the async model. */
    memset(&ab, 0, sizeof ab);
    CHECK(XUserAddAsync(1 /* AddDefaultUserSilently (guess) */, &ab) == S_OK);
    CHECK(XAsyncGetStatus(&ab, true) == S_OK);
    XUserHandle user = NULL;
    CHECK(XUserAddResult(&ab, &user) == S_OK && user);
    uint64_t xuid = 0; CHECK(XUserGetId(user, &xuid) == S_OK && xuid != 0);
    char tag[32]; CHECK(XUserGetGamertag(user, 0, sizeof tag, tag, &used) == S_OK && strcmp(tag, "JackBoxPlayer") == 0);
    XUserCloseHandle(user);
    DONE();
}
