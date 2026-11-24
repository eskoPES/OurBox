/* XAsync: clean-room implementation of the GDK async-call model on XTaskQueue.
 *
 * XAsyncBlock.internal layout (ours, opaque to titles):
 *   [0]  AsyncState*        live provider state, NULL once cleaned up
 *   [8]  HRESULT status     E_PENDING until completed
 *   [16] size_t  result size
 *   [24] const void* identity (for GetResult checks after cleanup)
 */
#include "xgr.h"
#include <string.h>
XGR_API HRESULT XTaskQueueDuplicateHandle(XTaskQueueHandle, XTaskQueueHandle *);

typedef struct {
    XAsyncProvider *provider;
    void *context;
    const void *identity;
    XAsyncWork *run_work;      /* set by XAsyncRun */
    XTaskQueueHandle queue;    /* ref held */
    volatile LONG completed;
    volatile LONG cancel_requested;
    volatile LONG refs;        /* 1 for the operation + 1 per scheduled work item */
    XAsyncBlock *block;
} AsyncState;

#define ST(b)       (*(AsyncState **)&(b)->internal[0])
#define STATUS(b)   (*(volatile LONG *)&(b)->internal[8])
#define RSIZE(b)    (*(size_t *)&(b)->internal[16])
#define IDENT(b)    (*(const void **)&(b)->internal[24])

static SRWLOCK g_wait_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_wait_cv = CONDITION_VARIABLE_INIT;

static HRESULT call_provider(AsyncState *s, XAsyncBlock *b, XAsyncOp op, void *buf, size_t size)
{
    XAsyncProviderData d = { b, size, buf, s->context };
    return s->provider(op, &d);
}

static void state_release(AsyncState *s)
{
    if (InterlockedDecrement(&s->refs) == 0) {
        XTaskQueueCloseHandle(s->queue);
        HeapFree(GetProcessHeap(), 0, s);
    }
}

static void cleanup(XAsyncBlock *b)
{
    AsyncState *s = ST(b);
    if (!s) return;
    ST(b) = NULL;
    call_provider(s, b, XAsyncOp_Cleanup, NULL, 0);
    state_release(s);
}

static HRESULT CALLBACK run_provider(XAsyncOp op, const XAsyncProviderData *d)
{
    AsyncState *s = ST(d->async);
    switch (op) {
    case XAsyncOp_DoWork: return s->run_work(d->async);
    default: return S_OK;
    }
}

static void CALLBACK completion_cb(void *ctx, bool canceled)
{
    (void)canceled;
    XAsyncBlock *b = ctx;
    if (b->callback) b->callback(b);
}

XGR_API HRESULT XAsyncBegin(XAsyncBlock *b, void *context, const void *identity, const char *identityName, XAsyncProvider *provider)
{
    if (!b || !provider) return E_INVALIDARG;
    AsyncState *s = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *s);
    if (!s) return E_OUTOFMEMORY;
    s->provider = provider; s->context = context; s->identity = identity;
    s->refs = 1; s->block = b;
    if (b->queue) {
        XTaskQueueDuplicateHandle(b->queue, &s->queue);
    } else if (!XTaskQueueGetCurrentProcessTaskQueue(&s->queue)) {
        HeapFree(GetProcessHeap(), 0, s);
        return E_INVALIDARG; /* no queue available */
    }
    memset(b->internal, 0, sizeof b->internal);
    ST(b) = s; IDENT(b) = identity;
    InterlockedExchange(&STATUS(b), E_PENDING);
    XGR_LOG("XAsyncBegin %p %s", (void *)b, identityName ? identityName : "");
    HRESULT hr = call_provider(s, b, XAsyncOp_Begin, NULL, 0);
    if (FAILED(hr)) {
        ST(b) = NULL;
        state_release(s);
        InterlockedExchange(&STATUS(b), hr);
    }
    return hr;
}

XGR_API void XAsyncComplete(XAsyncBlock *b, HRESULT result, size_t requiredBufferSize);

static void CALLBACK work_cb(void *ctx, bool canceled)
{
    AsyncState *s = ctx;
    /* Never touch the block once the operation completed: the title may have
     * freed it. */
    if (!s->completed) {
        XAsyncBlock *b = s->block;
        if (canceled || s->cancel_requested) XAsyncComplete(b, E_ABORT, 0);
        else {
            HRESULT hr = call_provider(s, b, XAsyncOp_DoWork, NULL, 0);
            if (hr != E_PENDING) XAsyncComplete(b, hr, 0);
        }
    }
    state_release(s);
}

XGR_API HRESULT XAsyncSchedule(XAsyncBlock *b, uint32_t delayMs)
{
    AsyncState *s = b ? ST(b) : NULL;
    if (!s || s->completed) return E_INVALIDARG;
    InterlockedIncrement(&s->refs);
    HRESULT hr = XTaskQueueSubmitDelayedCallback(s->queue, XTaskQueuePort_Work, delayMs, s, work_cb);
    if (FAILED(hr)) state_release(s);
    return hr;
}

XGR_API void XAsyncComplete(XAsyncBlock *b, HRESULT result, size_t requiredBufferSize)
{
    AsyncState *s = ST(b);
    if (!s || InterlockedExchange(&s->completed, 1)) return; /* first completion wins */
    if (result == E_PENDING) result = E_UNEXPECTED;
    /* Capture what we need before publishing: once the status is visible the
     * title may free or reuse the block. */
    XAsyncCompletionRoutine *cb = b->callback;
    XTaskQueueHandle q = s->queue;
    XTaskQueueHandle qref = NULL;
    if (cb) XTaskQueueDuplicateHandle(q, &qref);
    RSIZE(b) = SUCCEEDED(result) ? requiredBufferSize : 0;
    if (FAILED(result) || requiredBufferSize == 0) cleanup(b);
    AcquireSRWLockExclusive(&g_wait_lock);
    InterlockedExchange(&STATUS(b), result);
    WakeAllConditionVariable(&g_wait_cv);
    ReleaseSRWLockExclusive(&g_wait_lock);
    if (cb) {
        if (FAILED(XTaskQueueSubmitCallback(qref, XTaskQueuePort_Completion, b, completion_cb))) cb(b);
        XTaskQueueCloseHandle(qref);
    }
}

XGR_API HRESULT XAsyncGetStatus(XAsyncBlock *b, bool wait)
{
    if (!b) return E_INVALIDARG;
    if (wait) {
        AcquireSRWLockExclusive(&g_wait_lock);
        while (STATUS(b) == E_PENDING) SleepConditionVariableSRW(&g_wait_cv, &g_wait_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&g_wait_lock);
    }
    return STATUS(b);
}

XGR_API HRESULT XAsyncGetResultSize(XAsyncBlock *b, size_t *size)
{
    if (!b || !size) return E_INVALIDARG;
    HRESULT hr = STATUS(b);
    if (FAILED(hr) || hr == E_PENDING) return hr;
    *size = RSIZE(b);
    return S_OK;
}

XGR_API HRESULT XAsyncGetResult(XAsyncBlock *b, const void *identity, size_t bufferSize, void *buffer, size_t *bufferUsed)
{
    if (!b) return E_INVALIDARG;
    HRESULT hr = STATUS(b);
    if (hr == E_PENDING || FAILED(hr)) return hr;
    if (identity && IDENT(b) && identity != IDENT(b)) return E_INVALIDARG;
    size_t need = RSIZE(b);
    if (bufferUsed) *bufferUsed = 0;
    if (need == 0) return hr;
    AsyncState *s = ST(b);
    if (!s) return E_ILLEGAL_METHOD_CALL; /* result already retrieved */
    if (!buffer || bufferSize < need) return E_NOT_SUFFICIENT_BUFFER;
    HRESULT gr = call_provider(s, b, XAsyncOp_GetResult, buffer, need);
    if (bufferUsed && SUCCEEDED(gr)) *bufferUsed = need;
    cleanup(b);
    return SUCCEEDED(gr) ? hr : gr;
}

XGR_API void XAsyncCancel(XAsyncBlock *b)
{
    AsyncState *s = b ? ST(b) : NULL;
    if (!s || s->completed) return;
    InterlockedExchange(&s->cancel_requested, 1);
    call_provider(s, b, XAsyncOp_Cancel, NULL, 0);
    /* Providers that do not complete on Cancel are completed with E_ABORT. */
    XAsyncComplete(b, E_ABORT, 0);
}

XGR_API HRESULT XAsyncRun(XAsyncBlock *b, XAsyncWork *work)
{
    if (!b || !work) return E_INVALIDARG;
    HRESULT hr = XAsyncBegin(b, NULL, (const void *)XAsyncRun, "XAsyncRun", run_provider);
    if (FAILED(hr)) return hr;
    ST(b)->run_work = work;
    hr = XAsyncSchedule(b, 0);
    if (FAILED(hr)) XAsyncComplete(b, hr, 0);
    return hr;
}
