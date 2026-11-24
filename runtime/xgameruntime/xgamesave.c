/*
 * XGameSave: OurBox's local save store (no cloud sync).
 *
 * Layout: <root>\<TitleId>\<configId>\<xuid>\<hex(container)>\<hex(blob)>.blob
 *   root = OURBOX_SAVE_DIR or %LOCALAPPDATA%\OurBox\saves
 * Names are hex-encoded so any title-chosen name is a valid file name.
 * Updates are crash-safe: blobs are staged as .tmp files, a journal lists the
 * operations, then they are applied; a leftover journal is replayed when the
 * provider is next opened.
 *
 * Signatures follow the public GDK reference. Struct layouts and error codes
 * marked (guess) need verifying against an authorized binary.
 */
#include "xgr.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

XGR_API HRESULT XAsyncBegin(XAsyncBlock *, void *, const void *, const char *, XAsyncProvider *);
XGR_API HRESULT XAsyncSchedule(XAsyncBlock *, uint32_t);
XGR_API void XAsyncComplete(XAsyncBlock *, HRESULT, size_t);
XGR_API HRESULT XAsyncGetResult(XAsyncBlock *, const void *, size_t, void *, size_t *);

#define GS_NOT_FOUND HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)     /* (guess) GDK uses E_GS_* codes */
#define GS_QUOTA_EXCEEDED HRESULT_FROM_WIN32(ERROR_DISK_FULL)    /* (guess) */
#define GS_DEFAULT_QUOTA (256ll << 20)                           /* (guess) per-user quota */
#define MAX_PATHW 1024

typedef struct XGameSaveProvider { char dir[MAX_PATHW]; int64_t quota; } XGameSaveProvider;
typedef struct XGameSaveContainer { XGameSaveProvider *p; char name[256]; char dir[MAX_PATHW]; } XGameSaveContainer;
typedef struct Op { int del; char blob[256]; char tmp[MAX_PATHW]; size_t size; struct Op *next; } Op;
typedef struct XGameSaveUpdate { XGameSaveContainer *c; char display[256]; Op *ops; int64_t added; } XGameSaveUpdate;
typedef XGameSaveProvider *XGameSaveProviderHandle;
typedef XGameSaveContainer *XGameSaveContainerHandle;
typedef XGameSaveUpdate *XGameSaveUpdateHandle;

typedef struct { /* (guess) layout */
    const char *name;
    const char *displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    time_t lastModifiedTime;
    bool needsSync;
} XGameSaveContainerInfo;
typedef struct { const char *name; uint32_t size; } XGameSaveBlobInfo; /* (guess) */
typedef struct { XGameSaveBlobInfo info; uint8_t *data; } XGameSaveBlob; /* (guess) */
typedef bool (CALLBACK XGameSaveContainerInfoCallback)(const XGameSaveContainerInfo *, void *);
typedef bool (CALLBACK XGameSaveBlobInfoCallback)(const XGameSaveBlobInfo *, void *);

/* ---- helpers ------------------------------------------------------------ */

static void hex_encode(const char *in, char *out, size_t outsz)
{
    static const char h[] = "0123456789abcdef";
    size_t j = 0;
    for (; *in && j + 3 < outsz; in++) { out[j++] = h[(uint8_t)*in >> 4]; out[j++] = h[(uint8_t)*in & 15]; }
    out[j] = 0;
}
static int hex_decode(const char *in, size_t n, char *out, size_t outsz)
{
    if (n % 2 || n / 2 + 1 > outsz) return 0;
    for (size_t i = 0; i < n; i += 2) {
        int a = in[i], b = in[i + 1];
        a = a <= '9' ? a - '0' : a - 'a' + 10; b = b <= '9' ? b - '0' : b - 'a' + 10;
        if (a < 0 || a > 15 || b < 0 || b > 15) return 0;
        out[i / 2] = (char)(a << 4 | b);
    }
    out[n / 2] = 0;
    return 1;
}
static int mkdirs(const char *path)
{
    char tmp[MAX_PATHW]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++) if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    return CreateDirectoryA(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}
static int valid_name(const char *n) { return n && *n && strlen(n) < 128; }
static void blob_path(const XGameSaveContainer *c, const char *blob, char *out, size_t n)
{
    char hx[260]; hex_encode(blob, hx, sizeof hx);
    snprintf(out, n, "%s\\%s.blob", c->dir, hx);
}
static int64_t dir_usage(const char *dir, uint32_t *files)
{
    char pat[MAX_PATHW]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    int64_t total = 0; if (files) *files = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == '.') continue;
        char sub[MAX_PATHW]; snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += dir_usage(sub, NULL);
        else if (strstr(fd.cFileName, ".blob")) { total += ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; if (files) (*files)++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return total;
}

/* Journal: one line per op: "W <hexblob> <tmp path>" or "D <hexblob>". */
static void replay_journal(const char *cdir)
{
    char jp[MAX_PATHW]; snprintf(jp, sizeof jp, "%s\\update.journal", cdir);
    FILE *f = fopen(jp, "r");
    if (!f) return;
    char line[MAX_PATHW + 300];
    int complete = 0;
    char *lines[512]; int nl = 0;
    while (fgets(line, sizeof line, f) && nl < 512) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "COMMIT")) { complete = 1; break; }
        lines[nl++] = _strdup(line);
    }
    fclose(f);
    for (int i = 0; i < nl; i++) {
        char op = lines[i][0], hx[300], tmp[MAX_PATHW] = "";
        if (sscanf(lines[i] + 2, "%299s %1023[^\n]", hx, tmp) < 1) continue;
        char fin[MAX_PATHW]; snprintf(fin, sizeof fin, "%s\\%s.blob", cdir, hx);
        if (complete) {
            if (op == 'W') MoveFileExA(tmp, fin, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            else if (op == 'D') DeleteFileA(fin);
        } else if (op == 'W') DeleteFileA(tmp); /* never committed: roll back */
        free(lines[i]);
    }
    DeleteFileA(jp);
}

/* ---- provider ----------------------------------------------------------- */

XGR_API HRESULT XGameSaveInitializeProvider(const void *user, const char *configurationId, bool syncOnDemand, XGameSaveProviderHandle *out)
{
    (void)syncOnDemand;
    if (!out || !configurationId) return E_INVALIDARG;
    uint64_t xuid = xgr_user_xuid(user);
    if (!xuid) return E_INVALIDARG;
    XGameSaveProvider *p = calloc(1, sizeof *p);
    if (!p) return E_OUTOFMEMORY;
    char root[MAX_PATH];
    if (!GetEnvironmentVariableA("OURBOX_SAVE_DIR", root, sizeof root)) {
        char la[MAX_PATH] = "."; GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la);
        snprintf(root, sizeof root, "%s\\OurBox\\saves", la);
    }
    char cfg[260]; hex_encode(configurationId, cfg, sizeof cfg);
    snprintf(p->dir, sizeof p->dir, "%s\\%08X\\%s\\%016llX", root, xgr_title_id(), cfg, (unsigned long long)xuid);
    if (!mkdirs(p->dir)) { free(p); return HRESULT_FROM_WIN32(GetLastError()); }
    p->quota = GS_DEFAULT_QUOTA;
    /* replay interrupted updates */
    char pat[MAX_PATHW]; snprintf(pat, sizeof pat, "%s\\*", p->dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            char cd[MAX_PATHW]; snprintf(cd, sizeof cd, "%s\\%s", p->dir, fd.cFileName); replay_journal(cd);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    XGR_LOG("XGameSave provider at %s", p->dir);
    *out = p;
    return S_OK;
}

typedef struct { const void *user; char cfg[256]; bool sync; XGameSaveProviderHandle result; HRESULT hr; } InitCtx;
static HRESULT CALLBACK init_provider_op(XAsyncOp op, const XAsyncProviderData *d)
{
    InitCtx *c = d->context;
    switch (op) {
    case XAsyncOp_Begin: return XAsyncSchedule(d->async, 0);
    case XAsyncOp_DoWork:
        c->hr = XGameSaveInitializeProvider(c->user, c->cfg, c->sync, &c->result);
        XAsyncComplete(d->async, c->hr, SUCCEEDED(c->hr) ? sizeof c->result : 0);
        return S_OK;
    case XAsyncOp_GetResult: *(XGameSaveProviderHandle *)d->buffer = c->result; c->result = NULL; return S_OK;
    case XAsyncOp_Cleanup: free(c); return S_OK;
    default: return S_OK;
    }
}
XGR_API HRESULT XGameSaveInitializeProviderAsync(const void *user, const char *configurationId, bool syncOnDemand, XAsyncBlock *async)
{
    if (!configurationId) return E_INVALIDARG;
    InitCtx *c = calloc(1, sizeof *c);
    if (!c) return E_OUTOFMEMORY;
    c->user = user; c->sync = syncOnDemand; snprintf(c->cfg, sizeof c->cfg, "%s", configurationId);
    HRESULT hr = XAsyncBegin(async, c, (const void *)XGameSaveInitializeProviderAsync, "XGameSaveInitializeProviderAsync", init_provider_op);
    if (FAILED(hr)) free(c);
    return hr;
}
XGR_API HRESULT XGameSaveInitializeProviderResult(XAsyncBlock *async, XGameSaveProviderHandle *out)
{
    if (!out) return E_POINTER;
    return XAsyncGetResult(async, (const void *)XGameSaveInitializeProviderAsync, sizeof *out, out, NULL);
}

XGR_API void XGameSaveCloseProvider(XGameSaveProviderHandle p) { free(p); }

XGR_API HRESULT XGameSaveGetRemainingQuota(XGameSaveProviderHandle p, int64_t *remaining)
{
    if (!p || !remaining) return E_INVALIDARG;
    *remaining = p->quota - dir_usage(p->dir, NULL);
    return S_OK;
}

/* ---- containers --------------------------------------------------------- */

XGR_API HRESULT XGameSaveCreateContainer(XGameSaveProviderHandle p, const char *name, XGameSaveContainerHandle *out)
{
    if (!p || !out || !valid_name(name)) return E_INVALIDARG;
    XGameSaveContainer *c = calloc(1, sizeof *c);
    if (!c) return E_OUTOFMEMORY;
    c->p = p; snprintf(c->name, sizeof c->name, "%s", name);
    char hx[260]; hex_encode(name, hx, sizeof hx);
    snprintf(c->dir, sizeof c->dir, "%s\\%s", p->dir, hx);
    *out = c; /* directory is created on first submitted update */
    return S_OK;
}
XGR_API void XGameSaveCloseContainer(XGameSaveContainerHandle c) { free(c); }

static void remove_tree(const char *dir)
{
    char pat[MAX_PATHW]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (fd.cFileName[0] == '.') continue; char f[MAX_PATHW]; snprintf(f, sizeof f, "%s\\%s", dir, fd.cFileName); DeleteFileA(f); }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

XGR_API HRESULT XGameSaveDeleteContainer(XGameSaveProviderHandle p, const char *name)
{
    if (!p || !valid_name(name)) return E_INVALIDARG;
    char hx[260], dir[MAX_PATHW]; hex_encode(name, hx, sizeof hx);
    snprintf(dir, sizeof dir, "%s\\%s", p->dir, hx);
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) return GS_NOT_FOUND;
    remove_tree(dir);
    return S_OK;
}

static HRESULT enum_containers(XGameSaveProviderHandle p, const char *prefix, const char *exact, void *ctx, XGameSaveContainerInfoCallback *cb)
{
    if (!p || !cb) return E_INVALIDARG;
    char pat[MAX_PATHW]; snprintf(pat, sizeof pat, "%s\\*", p->dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    int found = 0;
    if (h == INVALID_HANDLE_VALUE) return exact ? GS_NOT_FOUND : S_OK;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        char name[256];
        if (!hex_decode(fd.cFileName, strlen(fd.cFileName), name, sizeof name)) continue;
        if (exact && strcmp(name, exact)) continue;
        if (prefix && strncmp(name, prefix, strlen(prefix))) continue;
        char dir[MAX_PATHW]; snprintf(dir, sizeof dir, "%s\\%s", p->dir, fd.cFileName);
        char disp[256] = "", dp[MAX_PATHW]; snprintf(dp, sizeof dp, "%s\\display.txt", dir);
        FILE *df = fopen(dp, "rb"); if (df) { size_t n = fread(disp, 1, sizeof disp - 1, df); disp[n] = 0; fclose(df); }
        XGameSaveContainerInfo info = { name, disp, 0, 0, 0, false };
        info.totalSize = (uint64_t)dir_usage(dir, &info.blobCount);
        ULARGE_INTEGER t; t.LowPart = fd.ftLastWriteTime.dwLowDateTime; t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        info.lastModifiedTime = (time_t)(t.QuadPart / 10000000ull - 11644473600ull);
        found++;
        if (!cb(&info, ctx)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return exact && !found ? GS_NOT_FOUND : S_OK;
}
XGR_API HRESULT XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle p, void *ctx, XGameSaveContainerInfoCallback *cb)
{ return enum_containers(p, NULL, NULL, ctx, cb); }
XGR_API HRESULT XGameSaveEnumerateContainerInfoByName(XGameSaveProviderHandle p, const char *prefix, void *ctx, XGameSaveContainerInfoCallback *cb)
{ return enum_containers(p, prefix, NULL, ctx, cb); }
XGR_API HRESULT XGameSaveGetContainerInfo(XGameSaveProviderHandle p, const char *name, void *ctx, XGameSaveContainerInfoCallback *cb)
{ return enum_containers(p, NULL, name, ctx, cb); }

/* ---- blobs -------------------------------------------------------------- */

static HRESULT enum_blobs(XGameSaveContainerHandle c, const char *prefix, void *ctx, XGameSaveBlobInfoCallback *cb)
{
    if (!c || !cb) return E_INVALIDARG;
    char pat[MAX_PATHW]; snprintf(pat, sizeof pat, "%s\\*.blob", c->dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return S_OK;
    do {
        char name[256];
        size_t n = strlen(fd.cFileName) - 5;
        if (!hex_decode(fd.cFileName, n, name, sizeof name)) continue;
        if (prefix && strncmp(name, prefix, strlen(prefix))) continue;
        XGameSaveBlobInfo info = { name, fd.nFileSizeLow };
        if (!cb(&info, ctx)) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return S_OK;
}
XGR_API HRESULT XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle c, void *ctx, XGameSaveBlobInfoCallback *cb)
{ return enum_blobs(c, NULL, ctx, cb); }
XGR_API HRESULT XGameSaveEnumerateBlobInfoByName(XGameSaveContainerHandle c, const char *prefix, void *ctx, XGameSaveBlobInfoCallback *cb)
{ return enum_blobs(c, prefix, ctx, cb); }

typedef struct { char names[64][256]; uint32_t n; } NameList;
static bool CALLBACK collect(const XGameSaveBlobInfo *i, void *ctx)
{ NameList *l = ctx; if (l->n < 64) snprintf(l->names[l->n++], 256, "%s", i->name); return true; }

/* Fills blobData with countOfBlobs XGameSaveBlob entries followed by their
 * names and contents. (guess) buffer layout; titles only read the entries. */
XGR_API HRESULT XGameSaveReadBlobData(XGameSaveContainerHandle c, const char **blobNames, uint32_t *countOfBlobs, size_t blobsSize, XGameSaveBlob *blobData)
{
    if (!c || !countOfBlobs || !blobData) return E_INVALIDARG;
    NameList *all = NULL;
    uint32_t n = *countOfBlobs;
    const char *names_buf[64];
    if (!blobNames || n == 0) {
        all = calloc(1, sizeof *all);
        enum_blobs(c, NULL, all, collect);
        n = all->n;
        for (uint32_t i = 0; i < n; i++) names_buf[i] = all->names[i];
        blobNames = names_buf;
    }
    size_t need = n * sizeof(XGameSaveBlob);
    for (uint32_t i = 0; i < n; i++) {
        char path[MAX_PATHW]; blob_path(c, blobNames[i], path, sizeof path);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) { free(all); return GS_NOT_FOUND; }
        need += strlen(blobNames[i]) + 1 + fa.nFileSizeLow;
    }
    if (blobsSize < need) { free(all); return E_NOT_SUFFICIENT_BUFFER; }
    uint8_t *cur = (uint8_t *)(blobData + n);
    for (uint32_t i = 0; i < n; i++) {
        size_t ln = strlen(blobNames[i]) + 1;
        memcpy(cur, blobNames[i], ln); blobData[i].info.name = (const char *)cur; cur += ln;
        char path[MAX_PATHW]; blob_path(c, blobNames[i], path, sizeof path);
        FILE *f = fopen(path, "rb");
        if (!f) { free(all); return GS_NOT_FOUND; }
        size_t got = fread(cur, 1, (size_t)(((uint8_t *)blobData + blobsSize) - cur), f); fclose(f);
        blobData[i].data = cur; blobData[i].info.size = (uint32_t)got; cur += got;
    }
    *countOfBlobs = n;
    free(all);
    return S_OK;
}

/* ---- updates ------------------------------------------------------------ */

XGR_API HRESULT XGameSaveCreateUpdate(XGameSaveContainerHandle c, const char *displayName, XGameSaveUpdateHandle *out)
{
    if (!c || !out) return E_INVALIDARG;
    XGameSaveUpdate *u = calloc(1, sizeof *u);
    if (!u) return E_OUTOFMEMORY;
    u->c = c; snprintf(u->display, sizeof u->display, "%s", displayName ? displayName : "");
    *out = u;
    return S_OK;
}

XGR_API void XGameSaveCloseUpdate(XGameSaveUpdateHandle u)
{
    if (!u) return;
    for (Op *o = u->ops, *n; o; o = n) { n = o->next; if (!o->del && o->tmp[0]) DeleteFileA(o->tmp); free(o); }
    free(u);
}

XGR_API HRESULT XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle u, const char *name, const uint8_t *data, size_t size)
{
    if (!u || !valid_name(name) || (!data && size)) return E_INVALIDARG;
    if (!mkdirs(u->c->dir)) return HRESULT_FROM_WIN32(GetLastError());
    Op *o = calloc(1, sizeof *o);
    if (!o) return E_OUTOFMEMORY;
    snprintf(o->blob, sizeof o->blob, "%s", name);
    o->size = size;
    snprintf(o->tmp, sizeof o->tmp, "%s\\%p.%lu.tmp", u->c->dir, (void *)o, GetCurrentThreadId());
    FILE *f = fopen(o->tmp, "wb");
    if (!f || fwrite(data, 1, size, f) != size) { if (f) fclose(f); free(o); return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT); }
    fflush(f); fclose(f);
    u->added += (int64_t)size;
    o->next = u->ops; u->ops = o;
    return S_OK;
}

XGR_API HRESULT XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle u, const char *name)
{
    if (!u || !valid_name(name)) return E_INVALIDARG;
    Op *o = calloc(1, sizeof *o);
    if (!o) return E_OUTOFMEMORY;
    o->del = 1; snprintf(o->blob, sizeof o->blob, "%s", name);
    o->next = u->ops; u->ops = o;
    return S_OK;
}

XGR_API HRESULT XGameSaveSubmitUpdate(XGameSaveUpdateHandle u)
{
    if (!u) return E_INVALIDARG;
    XGameSaveContainer *c = u->c;
    int64_t remaining = c->p->quota - dir_usage(c->p->dir, NULL);
    if (u->added > remaining) return GS_QUOTA_EXCEEDED;
    if (!mkdirs(c->dir)) return HRESULT_FROM_WIN32(GetLastError());
    /* Ops were pushed newest first; apply oldest first. */
    Op *rev = NULL; for (Op *o = u->ops, *n; o; o = n) { n = o->next; o->next = rev; rev = o; } u->ops = rev;
    char jp[MAX_PATHW]; snprintf(jp, sizeof jp, "%s\\update.journal", c->dir);
    FILE *j = fopen(jp, "w");
    if (!j) return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    for (Op *o = u->ops; o; o = o->next) {
        char hx[260]; hex_encode(o->blob, hx, sizeof hx);
        if (o->del) fprintf(j, "D %s\n", hx); else fprintf(j, "W %s %s\n", hx, o->tmp);
    }
    fprintf(j, "COMMIT\n"); fflush(j); fclose(j);
    replay_journal(c->dir); /* applies and removes the journal */
    if (u->display[0]) {
        char dp[MAX_PATHW]; snprintf(dp, sizeof dp, "%s\\display.txt", c->dir);
        FILE *df = fopen(dp, "wb"); if (df) { fputs(u->display, df); fclose(df); }
    }
    for (Op *o = u->ops; o; o = o->next) o->tmp[0] = 0; /* consumed */
    return S_OK;
}

static HRESULT CALLBACK submit_op(XAsyncOp op, const XAsyncProviderData *d)
{
    switch (op) {
    case XAsyncOp_Begin: return XAsyncSchedule(d->async, 0);
    case XAsyncOp_DoWork: XAsyncComplete(d->async, XGameSaveSubmitUpdate(d->context), 0); return S_OK;
    default: return S_OK;
    }
}
XGR_API HRESULT XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle u, XAsyncBlock *async)
{
    if (!u) return E_INVALIDARG;
    return XAsyncBegin(async, u, (const void *)XGameSaveSubmitUpdateAsync, "XGameSaveSubmitUpdateAsync", submit_op);
}
XGR_API HRESULT XGameSaveSubmitUpdateResult(XAsyncBlock *async)
{
    return XAsyncGetResult(async, (const void *)XGameSaveSubmitUpdateAsync, 0, NULL, NULL);
}
