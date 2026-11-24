/* XTaskQueue: clean-room implementation on the Win32 thread pool. */
#include "xgr.h"

typedef struct Entry {
    struct Entry *next;
    XTaskQueueCallback *cb;
    void *ctx;
    struct XTaskQueueObject *q;
    XTaskQueuePort port;
    PTP_TIMER timer;
} Entry;

typedef struct {
    XTaskQueueDispatchMode mode;
    SRWLOCK lock;
    CONDITION_VARIABLE cv;     /* manual dispatch waiters */
    Entry *head, *tail;
    bool draining;             /* serialized mode: a drainer is scheduled */
    LONG pending;              /* queued + delayed + running */
} Port;

struct XTaskQueueObject {
    LONG refs;
    Port ports[2];
    volatile LONG terminated;
    bool term_fired;
    XTaskQueueTerminatedCallback *term_cb;
    void *term_ctx;
    SRWLOCK term_lock;
    CONDITION_VARIABLE term_cv;
};

static void queue_addref(XTaskQueueHandle q) { InterlockedIncrement(&q->refs); }
static void queue_release(XTaskQueueHandle q)
{
    if (InterlockedDecrement(&q->refs) == 0) {
        XGR_LOG("queue %p destroyed", (void *)q);
        HeapFree(GetProcessHeap(), 0, q);
    }
}

static void maybe_finish_termination(XTaskQueueHandle q)
{
    if (!q->terminated) return;
    AcquireSRWLockExclusive(&q->term_lock);
    bool fire = !q->term_fired && q->ports[0].pending == 0 && q->ports[1].pending == 0;
    if (fire) q->term_fired = true;
    ReleaseSRWLockExclusive(&q->term_lock);
    if (fire) {
        if (q->term_cb) q->term_cb(q->term_ctx);
        AcquireSRWLockExclusive(&q->term_lock);
        WakeAllConditionVariable(&q->term_cv);
        ReleaseSRWLockExclusive(&q->term_lock);
    }
}

static void run_entry(Entry *e)
{
    XTaskQueueHandle q = e->q;
    e->cb(e->ctx, q->terminated != 0);
    InterlockedDecrement(&q->ports[e->port].pending);
    HeapFree(GetProcessHeap(), 0, e);
    maybe_finish_termination(q);
    queue_release(q);
}

static void CALLBACK tp_run(PTP_CALLBACK_INSTANCE inst, void *p) { (void)inst; run_entry(p); }

static void CALLBACK tp_drain(PTP_CALLBACK_INSTANCE inst, void *p)
{
    (void)inst;
    Port *port = p;
    for (;;) {
        AcquireSRWLockExclusive(&port->lock);
        Entry *e = port->head;
        if (e) { port->head = e->next; if (!port->head) port->tail = NULL; }
        else port->draining = false;
        ReleaseSRWLockExclusive(&port->lock);
        if (!e) return;
        run_entry(e);
    }
}

static void deliver(Entry *e)
{
    Port *port = &e->q->ports[e->port];
    switch (port->mode) {
    case XTaskQueueDispatchMode_Immediate:
        run_entry(e);
        break;
    case XTaskQueueDispatchMode_ThreadPool:
        if (!TrySubmitThreadpoolCallback(tp_run, e, NULL)) run_entry(e);
        break;
    case XTaskQueueDispatchMode_SerializedThreadPool:
    case XTaskQueueDispatchMode_Manual: {
        bool kick = false;
        AcquireSRWLockExclusive(&port->lock);
        e->next = NULL;
        if (port->tail) port->tail->next = e; else port->head = e;
        port->tail = e;
        if (port->mode == XTaskQueueDispatchMode_SerializedThreadPool && !port->draining) kick = port->draining = true;
        ReleaseSRWLockExclusive(&port->lock);
        if (port->mode == XTaskQueueDispatchMode_Manual) WakeAllConditionVariable(&port->cv);
        else if (kick && !TrySubmitThreadpoolCallback(tp_drain, port, NULL)) tp_drain(NULL, port);
        break;
    }
    }
}

static HRESULT resolve(XTaskQueueHandle *q)
{
    if (*q) return S_OK;
    return XTaskQueueGetCurrentProcessTaskQueue(q) ? S_FALSE : E_INVALIDARG; /* S_FALSE: we own a ref */
}

XGR_API HRESULT XTaskQueueCreate(XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion, XTaskQueueHandle *out)
{
    if (!out) return E_POINTER;
    if ((unsigned)work > 3 || (unsigned)completion > 3) return E_INVALIDARG;
    XTaskQueueHandle q = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *q);
    if (!q) return E_OUTOFMEMORY;
    q->refs = 1;
    q->ports[0].mode = work;
    q->ports[1].mode = completion;
    for (int i = 0; i < 2; i++) { InitializeSRWLock(&q->ports[i].lock); InitializeConditionVariable(&q->ports[i].cv); }
    InitializeSRWLock(&q->term_lock); InitializeConditionVariable(&q->term_cv);
    *out = q;
    XGR_LOG("XTaskQueueCreate(%d,%d) -> %p", work, completion, (void *)q);
    return S_OK;
}

XGR_API HRESULT XTaskQueueDuplicateHandle(XTaskQueueHandle q, XTaskQueueHandle *out)
{
    if (!q || !out) return E_INVALIDARG;
    queue_addref(q); *out = q; return S_OK;
}

XGR_API void XTaskQueueCloseHandle(XTaskQueueHandle q) { if (q) queue_release(q); }

XGR_API HRESULT XTaskQueueSubmitCallback(XTaskQueueHandle q, XTaskQueuePort port, void *ctx, XTaskQueueCallback *cb)
{
    return XTaskQueueSubmitDelayedCallback(q, port, 0, ctx, cb);
}

static void CALLBACK tp_timer(PTP_CALLBACK_INSTANCE inst, void *p, PTP_TIMER t)
{
    (void)inst; (void)t;
    Entry *e = p;
    PTP_TIMER timer = e->timer;
    deliver(e);
    CloseThreadpoolTimer(timer);
}

XGR_API HRESULT XTaskQueueSubmitDelayedCallback(XTaskQueueHandle q, XTaskQueuePort port, uint32_t delayMs, void *ctx, XTaskQueueCallback *cb)
{
    if (!cb || (unsigned)port > 1) return E_INVALIDARG;
    HRESULT hr = resolve(&q);
    if (FAILED(hr)) return hr;
    bool owned = hr == S_FALSE;
    if (q->terminated) { if (owned) queue_release(q); return E_ABORT; }
    Entry *e = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *e);
    if (!e) { if (owned) queue_release(q); return E_OUTOFMEMORY; }
    e->cb = cb; e->ctx = ctx; e->q = q; e->port = port;
    if (!owned) queue_addref(q); /* entry holds a ref */
    InterlockedIncrement(&q->ports[port].pending);
    if (delayMs == 0) { deliver(e); return S_OK; }
    e->timer = CreateThreadpoolTimer(tp_timer, e, NULL);
    if (!e->timer) { deliver(e); return S_OK; }
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)delayMs * 10000;
    FILETIME ft = { due.LowPart, (DWORD)due.HighPart };
    SetThreadpoolTimer(e->timer, &ft, 0, 0);
    return S_OK;
}

XGR_API bool XTaskQueueDispatch(XTaskQueueHandle q, XTaskQueuePort port, uint32_t timeoutMs)
{
    if ((unsigned)port > 1) return false;
    bool owned = resolve(&q) == S_FALSE;
    if (!q) return false;
    Port *p = &q->ports[port];
    Entry *e = NULL;
    AcquireSRWLockExclusive(&p->lock);
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (!(e = p->head)) {
        if (timeoutMs == 0) break;
        DWORD wait = timeoutMs == INFINITE ? INFINITE : (DWORD)(deadline > GetTickCount64() ? deadline - GetTickCount64() : 0);
        if (!wait || !SleepConditionVariableSRW(&p->cv, &p->lock, wait, 0)) { e = p->head; break; }
    }
    if (e) { p->head = e->next; if (!p->head) p->tail = NULL; }
    ReleaseSRWLockExclusive(&p->lock);
    if (e) run_entry(e);
    if (owned) queue_release(q);
    return e != NULL;
}

XGR_API bool XTaskQueueIsEmpty(XTaskQueueHandle q, XTaskQueuePort port)
{
    bool owned = resolve(&q) == S_FALSE;
    if (!q || (unsigned)port > 1) return true;
    bool empty = q->ports[port].pending == 0;
    if (owned) queue_release(q);
    return empty;
}

XGR_API HRESULT XTaskQueueTerminate(XTaskQueueHandle q, bool wait, void *ctx, XTaskQueueTerminatedCallback *cb)
{
    if (!q) return E_INVALIDARG;
    AcquireSRWLockExclusive(&q->term_lock);
    if (q->terminated) { ReleaseSRWLockExclusive(&q->term_lock); return E_ABORT; }
    q->term_cb = cb; q->term_ctx = ctx;
    InterlockedExchange(&q->terminated, 1);
    ReleaseSRWLockExclusive(&q->term_lock);
    queue_addref(q);
    for (int i = 0; i < 2; i++) WakeAllConditionVariable(&q->ports[i].cv);
    maybe_finish_termination(q);
    if (wait) {
        AcquireSRWLockExclusive(&q->term_lock);
        while (!q->term_fired) SleepConditionVariableSRW(&q->term_cv, &q->term_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&q->term_lock);
    }
    queue_release(q);
    return S_OK;
}

/* Process-wide default queue (thread pool for both ports). */
static SRWLOCK g_proc_lock = SRWLOCK_INIT;
static XTaskQueueHandle g_proc_queue;
static bool g_proc_queue_set; /* explicitly set, possibly to NULL */

XGR_API bool XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle *out)
{
    if (!out) return false;
    AcquireSRWLockExclusive(&g_proc_lock);
    if (!g_proc_queue && !g_proc_queue_set)
        XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool, XTaskQueueDispatchMode_ThreadPool, &g_proc_queue);
    XTaskQueueHandle q = g_proc_queue;
    if (q) queue_addref(q);
    ReleaseSRWLockExclusive(&g_proc_lock);
    *out = q;
    return q != NULL;
}

XGR_API void XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle q)
{
    if (q) queue_addref(q);
    AcquireSRWLockExclusive(&g_proc_lock);
    XTaskQueueHandle old = g_proc_queue;
    g_proc_queue = q; g_proc_queue_set = true;
    ReleaseSRWLockExclusive(&g_proc_lock);
    if (old) queue_release(old);
}
