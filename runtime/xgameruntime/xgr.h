/* Shared declarations for the JackBox clean-room xgameruntime.dll.
 * Types follow the public GDK API reference (learn.microsoft.com). Layouts
 * marked (guess) are reconstructed from documentation and must be verified
 * against an authorized binary. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define E_GAMERUNTIME_NOT_INITIALIZED ((HRESULT)0x89240100) /* (guess) */
#ifndef E_NOT_SUFFICIENT_BUFFER
#define E_NOT_SUFFICIENT_BUFFER ((HRESULT)0x8007007A)
#endif

#define XGR_API __declspec(dllexport)
#ifndef E_ILLEGAL_METHOD_CALL
#define E_ILLEGAL_METHOD_CALL ((HRESULT)0x8000000E)
#endif

typedef struct XTaskQueueObject *XTaskQueueHandle;
typedef enum { XTaskQueueDispatchMode_Manual, XTaskQueueDispatchMode_ThreadPool,
               XTaskQueueDispatchMode_SerializedThreadPool, XTaskQueueDispatchMode_Immediate } XTaskQueueDispatchMode;
typedef enum { XTaskQueuePort_Work, XTaskQueuePort_Completion } XTaskQueuePort;
typedef void (CALLBACK XTaskQueueCallback)(void *context, bool canceled);
typedef void (CALLBACK XTaskQueueTerminatedCallback)(void *context);

typedef struct XAsyncBlock XAsyncBlock;
typedef void (CALLBACK XAsyncCompletionRoutine)(XAsyncBlock *asyncBlock);
struct XAsyncBlock {
    XTaskQueueHandle queue;
    void *context;
    XAsyncCompletionRoutine *callback;
    unsigned char internal[sizeof(void *) * 4];
};
typedef enum { XAsyncOp_Begin, XAsyncOp_DoWork, XAsyncOp_GetResult, XAsyncOp_Cancel, XAsyncOp_Cleanup } XAsyncOp;
typedef struct { /* (guess) field order */
    XAsyncBlock *async;
    size_t bufferSize;
    void *buffer;
    void *context;
} XAsyncProviderData;
typedef HRESULT (CALLBACK XAsyncProvider)(XAsyncOp op, const XAsyncProviderData *data);
typedef HRESULT (CALLBACK XAsyncWork)(XAsyncBlock *asyncBlock);

/* internal logging: JACKBOX_LOG=1 */
extern int xgr_verbose;
#define XGR_LOG(...) do { if (xgr_verbose) { fprintf(stderr, "[xgameruntime] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* exported API used internally */
XGR_API HRESULT XTaskQueueSubmitCallback(XTaskQueueHandle q, XTaskQueuePort port, void *ctx, XTaskQueueCallback *cb);
XGR_API HRESULT XTaskQueueSubmitDelayedCallback(XTaskQueueHandle q, XTaskQueuePort port, uint32_t delayMs, void *ctx, XTaskQueueCallback *cb);
XGR_API bool XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle *q);
XGR_API void XTaskQueueCloseHandle(XTaskQueueHandle q);

/* internal helpers shared between modules */
uint32_t xgr_title_id(void);                 /* 0 if unknown */
uint64_t xgr_user_xuid(const void *user);    /* 0 if not a valid user */
