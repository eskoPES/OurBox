/*
 * jackbox.exe: native loader for x86-64 console executables.
 *
 * Maps a PE32+ title image into this process, relocates it, binds imports
 * (JackBox runtime modules first, then the host), installs diagnostic traps
 * for anything unresolved, sets up static TLS and x64 unwind tables, applies
 * section protections and jumps to the entry point. The title's x86-64 code
 * runs natively on the host CPU.
 *
 * Usage: jackbox.exe [options] <title.exe> [title args...]
 *   env JACKBOX_UNIMPL=abort|continue   policy when a trap is hit (default abort)
 *   env JACKBOX_REPORT=<path>           write a diagnostics report at exit
 *   env JACKBOX_LOG=1                   verbose loader log
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "jb_loader.h"

/* ------------------------------------------------------------------------ */
/* Logging and diagnostics                                                   */
/* ------------------------------------------------------------------------ */

static int g_verbose;
static int g_unimpl_continue;
static const char *g_report_path;

#define LOG(...) do { if (g_verbose) { fprintf(stderr, "[jackbox] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define ERR(...) do { fprintf(stderr, "[jackbox] error: " __VA_ARGS__); fputc('\n', stderr); } while (0)

typedef struct Unresolved {
    char dll[64];
    char name[128];
    volatile LONG hits;
    struct Unresolved *next;
} Unresolved;

static Unresolved *g_unresolved;
static int g_unresolved_count;
static CRITICAL_SECTION g_report_lock;

typedef struct MappedModule {
    char name[MAX_PATH];
    uint8_t *base;
    uint32_t size;
    struct MappedModule *next;
} MappedModule;
static MappedModule *g_modules;

static const MappedModule *module_for(const void *addr)
{
    for (MappedModule *m = g_modules; m; m = m->next)
        if ((const uint8_t *)addr >= m->base && (const uint8_t *)addr < m->base + m->size) return m;
    return NULL;
}

static void write_report(void)
{
    if (!g_report_path) return;
    FILE *f = fopen(g_report_path, "w");
    if (!f) return;
    fprintf(f, "# jackbox diagnostics report\n");
    for (MappedModule *m = g_modules; m; m = m->next)
        fprintf(f, "module %s base=%p size=0x%x\n", m->name, (void *)m->base, m->size);
    PROCESS_MEMORY_COUNTERS_EX_JB pmc;
    if (jb_get_memory_counters(&pmc))
        fprintf(f, "memory peak_working_set=%llu peak_commit=%llu\n",
                (unsigned long long)pmc.PeakWorkingSetSize, (unsigned long long)pmc.PeakPagefileUsage);
    fprintf(f, "unresolved_imports %d\n", g_unresolved_count);
    for (Unresolved *u = g_unresolved; u; u = u->next)
        fprintf(f, "unresolved %s!%s hits=%ld\n", u->dll, u->name, u->hits);
    fclose(f);
}

/* Called by every diagnostic trap. Reached by `jmp`, so our return address is
 * the title's call site. Returns E_NOTIMPL in continue mode. */
static __attribute__((noinline)) uint64_t jb_unimpl_handler(Unresolved *u)
{
    void *caller = __builtin_return_address(0);
    LONG n = InterlockedIncrement(&u->hits);
    const MappedModule *m = module_for(caller);
    if (n <= 3)
        fprintf(stderr, "[jackbox] UNIMPLEMENTED %s!%s called from %s+0x%llx\n", u->dll, u->name,
                m ? m->name : "?", m ? (unsigned long long)((uint8_t *)caller - m->base) : (unsigned long long)(uintptr_t)caller);
    if (!g_unimpl_continue) {
        fprintf(stderr, "[jackbox] aborting (set JACKBOX_UNIMPL=continue to return E_NOTIMPL instead)\n");
        write_report();
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), JB_EXIT_UNIMPLEMENTED);
    }
    return 0x80004001ull; /* E_NOTIMPL */
}

/* Executable pool for trap thunks. Each thunk:
 *   48 B9 <imm64>   mov rcx, Unresolved*
 *   48 B8 <imm64>   mov rax, jb_unimpl_handler
 *   FF E0           jmp rax                                                 */
static uint8_t *g_thunk_pool, *g_thunk_cur, *g_thunk_end;

static void *make_trap(const char *dll, const char *name)
{
    Unresolved *u = calloc(1, sizeof *u);
    snprintf(u->dll, sizeof u->dll, "%s", dll);
    snprintf(u->name, sizeof u->name, "%s", name);
    u->next = g_unresolved; g_unresolved = u; g_unresolved_count++;

    if (!g_thunk_pool || g_thunk_cur + 32 > g_thunk_end) {
        g_thunk_pool = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        g_thunk_cur = g_thunk_pool; g_thunk_end = g_thunk_pool + 0x10000;
    }
    uint8_t *t = g_thunk_cur; g_thunk_cur += 32;
    uint64_t a = (uint64_t)(uintptr_t)u, h = (uint64_t)(uintptr_t)&jb_unimpl_handler;
    t[0] = 0x48; t[1] = 0xB9; memcpy(t + 2, &a, 8);
    t[10] = 0x48; t[11] = 0xB8; memcpy(t + 12, &h, 8);
    t[20] = 0xFF; t[21] = 0xE0;
    return t;
}

static char g_title_dir[MAX_PATH];
static char g_runtime_dir[MAX_PATH];
static char g_title_path[MAX_PATH];

/* ------------------------------------------------------------------------ */
/* Static TLS                                                                */
/* ------------------------------------------------------------------------ */
/*
 * A manually mapped image is unknown to the OS loader, so it has no slot in
 * TEB->ThreadLocalStoragePointer. jackbox.exe carries its own (otherwise
 * unused) static TLS block; we give the title jackbox's _tls_index and, on
 * every thread, replace that slot with a fresh copy of the title's template.
 * jackbox.exe's TLS callback does this for threads created later.
 */
extern ULONG _tls_index;
static __thread volatile char g_tls_anchor[16]; /* forces a TLS directory */

/* Each mapped image with static TLS needs its own slot. Slot sources:
 * jackbox.exe's own index, then jb_tlsN.dll helpers (tiny DLLs whose only job
 * is to own an OS-assigned TLS index). */
#define JB_MAX_TLS_IMAGES 8
typedef struct {
    const uint8_t *tmpl;
    size_t tmpl_size, zero_fill;
    PIMAGE_TLS_CALLBACK *callbacks;
    void *base;
    ULONG slot;
} TlsImage;
static TlsImage g_tls_images[JB_MAX_TLS_IMAGES];
static volatile LONG g_tls_count;

static int tls_acquire_slot(ULONG *slot)
{
    int n = g_tls_count;
    if (n == 0) { *slot = _tls_index; return 1; }
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s\\jb_tls%d.dll", g_runtime_dir, n);
    HMODULE h = LoadLibraryA(path);
    ULONG (*get)(void) = h ? (void *)GetProcAddress(h, "jb_tls_slot") : NULL;
    if (!get) { ERR("TLS slot helper %s missing", path); return 0; }
    *slot = get();
    return 1;
}

static void tls_install_image(const TlsImage *t)
{
    void **slots = *(void ***)((uint8_t *)NtCurrentTeb() + 0x58);
    size_t total = t->tmpl_size + t->zero_fill;
    uint8_t *block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total ? total : 8);
    memcpy(block, t->tmpl, t->tmpl_size);
    void *old = slots[t->slot];
    slots[t->slot] = block;
    if (old) HeapFree(GetProcessHeap(), 0, old);
}

static void tls_run_image_callbacks(const TlsImage *t, DWORD reason)
{
    if (!t->callbacks) return;
    for (PIMAGE_TLS_CALLBACK *cb = t->callbacks; *cb; cb++) (*cb)(t->base, reason, NULL);
}

static const TlsImage *tls_image_for(void *base)
{
    for (int i = 0; i < g_tls_count; i++) if (g_tls_images[i].base == base) return &g_tls_images[i];
    return NULL;
}

static void NTAPI jb_tls_callback(PVOID h, DWORD reason, PVOID r)
{
    (void)h; (void)r; (void)g_tls_anchor;
    int n = g_tls_count;
    if (reason == DLL_THREAD_ATTACH) {
        for (int i = 0; i < n; i++) tls_install_image(&g_tls_images[i]);
        for (int i = 0; i < n; i++) tls_run_image_callbacks(&g_tls_images[i], DLL_THREAD_ATTACH);
    } else if (reason == DLL_THREAD_DETACH) {
        for (int i = n - 1; i >= 0; i--) tls_run_image_callbacks(&g_tls_images[i], DLL_THREAD_DETACH);
    }
}
__attribute__((section(".CRT$XLJ"), used)) PIMAGE_TLS_CALLBACK jb_tls_cb_ptr = jb_tls_callback;

/* ------------------------------------------------------------------------ */
/* PE mapping                                                                */
/* ------------------------------------------------------------------------ */


static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = n; return b;
}

static int file_exists(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static HMODULE resolve_module(const char *dll, int *is_mapped, uint8_t **mapped_base);
static uint8_t *map_image(const char *path, const char *name);

static int apply_relocations(uint8_t *base, IMAGE_NT_HEADERS64 *nt, int64_t delta)
{
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!delta) return 1;
    if (!d.VirtualAddress || !d.Size) {
        ERR("image needs relocation but has no .reloc");
        return 0;
    }
    uint8_t *p = base + d.VirtualAddress, *end = p + d.Size;
    while (p < end) {
        IMAGE_BASE_RELOCATION *blk = (IMAGE_BASE_RELOCATION *)p;
        if (blk->SizeOfBlock < sizeof *blk) break;
        WORD *e = (WORD *)(blk + 1);
        size_t n = (blk->SizeOfBlock - sizeof *blk) / 2;
        for (size_t i = 0; i < n; i++) {
            int type = e[i] >> 12, off = e[i] & 0xFFF;
            uint8_t *at = base + blk->VirtualAddress + off;
            if (type == IMAGE_REL_BASED_DIR64) *(uint64_t *)at += delta;
            else if (type == IMAGE_REL_BASED_HIGHLOW) *(uint32_t *)at += (uint32_t)delta;
            else if (type != IMAGE_REL_BASED_ABSOLUTE) { ERR("unsupported relocation type %d", type); return 0; }
        }
        p += blk->SizeOfBlock;
    }
    return 1;
}

static void *lookup_export(uint8_t *base, const char *name, WORD ordinal)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress) return NULL;
    IMAGE_EXPORT_DIRECTORY *ex = (IMAGE_EXPORT_DIRECTORY *)(base + d.VirtualAddress);
    DWORD *funcs = (DWORD *)(base + ex->AddressOfFunctions);
    DWORD *names = (DWORD *)(base + ex->AddressOfNames);
    WORD *ords = (WORD *)(base + ex->AddressOfNameOrdinals);
    DWORD rva = 0;
    if (name) {
        for (DWORD i = 0; i < ex->NumberOfNames; i++)
            if (!strcmp((char *)base + names[i], name)) { rva = funcs[ords[i]]; break; }
    } else if (ordinal >= ex->Base && ordinal - ex->Base < ex->NumberOfFunctions) {
        rva = funcs[ordinal - ex->Base];
    }
    if (!rva) return NULL;
    if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) {
        /* forwarder "DLL.Func" */
        char fwd[256]; snprintf(fwd, sizeof fwd, "%s", (char *)base + rva);
        char *dot = strchr(fwd, '.'); if (!dot) return NULL; *dot = 0;
        char dll[300]; snprintf(dll, sizeof dll, "%s.dll", fwd);
        int mapped; uint8_t *mb; HMODULE h = resolve_module(dll, &mapped, &mb);
        if (mapped) return lookup_export(mb, dot + 1, 0);
        return h ? (void *)GetProcAddress(h, dot + 1) : NULL;
    }
    return base + rva;
}

static int bind_imports(uint8_t *base, IMAGE_NT_HEADERS64 *nt)
{
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d.VirtualAddress) return 1;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + d.VirtualAddress); imp->Name; imp++) {
        const char *dll = (const char *)base + imp->Name;
        int mapped = 0; uint8_t *mbase = NULL;
        HMODULE h = resolve_module(dll, &mapped, &mbase);
        if (!h && !mapped) LOG("module %s not found: all its imports become traps", dll);
        uint64_t *lookup = (uint64_t *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        uint64_t *iat = (uint64_t *)(base + imp->FirstThunk);
        for (; *lookup; lookup++, iat++) {
            const char *fname = NULL; WORD ord = 0; char ordname[16];
            if (*lookup & IMAGE_ORDINAL_FLAG64) { ord = (WORD)(*lookup & 0xFFFF); snprintf(ordname, sizeof ordname, "#%u", ord); }
            else fname = ((IMAGE_IMPORT_BY_NAME *)(base + (*lookup & 0x7FFFFFFF)))->Name;
            void *fn = fname ? jb_host_override(dll, fname) : NULL;
            if (fn) LOG("%s!%s -> JackBox loader override", dll, fname);
            else if (mapped) fn = lookup_export(mbase, fname, ord);
            else if (h) fn = (void *)GetProcAddress(h, fname ? fname : (LPCSTR)(uintptr_t)ord);
            if (!fn) {
                fn = make_trap(dll, fname ? fname : ordname);
                LOG("unresolved %s!%s -> trap", dll, fname ? fname : ordname);
            }
            *iat = (uint64_t)(uintptr_t)fn;
        }
    }
    return 1;
}

static void protect_sections(uint8_t *base, IMAGE_NT_HEADERS64 *nt)
{
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    DWORD old;
    VirtualProtect(base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        DWORD c = s->Characteristics, prot;
        int x = !!(c & IMAGE_SCN_MEM_EXECUTE), w = !!(c & IMAGE_SCN_MEM_WRITE), r = !!(c & IMAGE_SCN_MEM_READ);
        if (x) prot = w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
        else prot = w ? PAGE_READWRITE : (r ? PAGE_READONLY : PAGE_NOACCESS);
        DWORD sz = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (sz) VirtualProtect(base + s->VirtualAddress, sz, prot, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), base, nt->OptionalHeader.SizeOfImage);
}

static void register_unwind(uint8_t *base, IMAGE_NT_HEADERS64 *nt)
{
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d.VirtualAddress || !d.Size) return;
    if (!RtlAddFunctionTable((PRUNTIME_FUNCTION)(base + d.VirtualAddress), d.Size / sizeof(RUNTIME_FUNCTION), (DWORD64)base))
        ERR("RtlAddFunctionTable failed; exceptions in title code will not unwind");
}

static int setup_tls(uint8_t *base, IMAGE_NT_HEADERS64 *nt)
{
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!d.VirtualAddress) return 1;
    if (g_tls_count >= JB_MAX_TLS_IMAGES) { ERR("more than %d mapped images with static TLS", JB_MAX_TLS_IMAGES); return 0; }
    IMAGE_TLS_DIRECTORY64 *t = (IMAGE_TLS_DIRECTORY64 *)(base + d.VirtualAddress);
    TlsImage *ti = &g_tls_images[g_tls_count];
    if (!tls_acquire_slot(&ti->slot)) return 0;
    ti->tmpl = (const uint8_t *)(uintptr_t)t->StartAddressOfRawData;
    ti->tmpl_size = (size_t)(t->EndAddressOfRawData - t->StartAddressOfRawData);
    ti->zero_fill = t->SizeOfZeroFill;
    ti->callbacks = (PIMAGE_TLS_CALLBACK *)(uintptr_t)t->AddressOfCallBacks;
    ti->base = base;
    if (t->AddressOfIndex) *(ULONG *)(uintptr_t)t->AddressOfIndex = ti->slot;
    /* Existing threads (other than this one) do not get a block: JackBox maps
     * titles before they create threads, so only the main thread exists. */
    tls_install_image(ti);
    InterlockedIncrement(&g_tls_count);
    LOG("static TLS: %zu bytes template + %zu zero fill, slot %lu", ti->tmpl_size, ti->zero_fill, ti->slot);
    return 1;
}

static uint8_t *map_image(const char *path, const char *name)
{
    size_t len; uint8_t *file = read_file(path, &len);
    if (!file) { ERR("cannot read %s", path); return NULL; }
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file;
    if (len < sizeof *dos || dos->e_magic != IMAGE_DOS_SIGNATURE) { ERR("%s: not a PE file", path); free(file); return NULL; }
    IMAGE_NT_HEADERS64 *fnt = (IMAGE_NT_HEADERS64 *)(file + dos->e_lfanew);
    if (fnt->Signature != IMAGE_NT_SIGNATURE || fnt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        fnt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        ERR("%s: not an x86-64 PE32+ image", path); free(file); return NULL;
    }
    SIZE_T size = fnt->OptionalHeader.SizeOfImage;
    uint8_t *want = (uint8_t *)(uintptr_t)fnt->OptionalHeader.ImageBase;
    uint8_t *base = VirtualAlloc(want, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) base = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) { ERR("cannot allocate %zu bytes for %s", (size_t)size, name); free(file); return NULL; }
    LOG("mapping %s at %p (preferred %p, size 0x%zx)", name, (void *)base, (void *)want, (size_t)size);

    memcpy(base, file, fnt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(fnt);
    for (int i = 0; i < fnt->FileHeader.NumberOfSections; i++, s++) {
        if (s->SizeOfRawData && s->PointerToRawData + s->SizeOfRawData <= len) {
            DWORD n = s->SizeOfRawData;
            if (s->Misc.VirtualSize && n > s->Misc.VirtualSize) n = s->Misc.VirtualSize;
            memcpy(base + s->VirtualAddress, file + s->PointerToRawData, n);
        }
    }
    free(file);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);

    MappedModule *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base; m->size = (uint32_t)size; m->next = g_modules; g_modules = m;

    if (!apply_relocations(base, nt, (int64_t)(base - want))) return NULL;
    nt->OptionalHeader.ImageBase = (ULONGLONG)(uintptr_t)base;
    if (!bind_imports(base, nt)) return NULL;
    if (!setup_tls(base, nt)) return NULL;
    register_unwind(base, nt);
    protect_sections(base, nt);
    return base;
}

/* Title-local DLLs are mapped by us (so their imports also go through
 * JackBox); JackBox runtime modules and host DLLs use the OS loader. */
typedef struct LoadedDll { char name[MAX_PATH]; HMODULE h; uint8_t *mapped; struct LoadedDll *next; } LoadedDll;
static LoadedDll *g_dlls;

static HMODULE resolve_module(const char *dll, int *is_mapped, uint8_t **mapped_base)
{
    *is_mapped = 0; *mapped_base = NULL;
    for (LoadedDll *l = g_dlls; l; l = l->next)
        if (!_stricmp(l->name, dll)) { *is_mapped = l->mapped != NULL; *mapped_base = l->mapped; return l->h; }

    LoadedDll *l = calloc(1, sizeof *l);
    snprintf(l->name, sizeof l->name, "%s", dll);
    l->next = g_dlls; g_dlls = l;

    char path[MAX_PATH * 2];
    const char *jb = jb_runtime_module_for(dll);
    if (jb) {
        snprintf(path, sizeof path, "%s\\%s", g_runtime_dir, jb);
        l->h = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (l->h) { LOG("%s -> JackBox runtime %s", dll, path); return l->h; }
        ERR("JackBox runtime module %s missing (%lu); imports from %s become traps", path, GetLastError(), dll);
        return NULL;
    }
    snprintf(path, sizeof path, "%s\\%s", g_title_dir, dll);
    if (file_exists(path)) {
        uint8_t *b = map_image(path, dll);
        if (b) {
            l->mapped = b; *is_mapped = 1; *mapped_base = b;
            IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
            const TlsImage *ti = tls_image_for(b);
            if (ti) tls_run_image_callbacks(ti, DLL_PROCESS_ATTACH);
            if (nt->OptionalHeader.AddressOfEntryPoint) {
                BOOL (WINAPI *dllmain)(HINSTANCE, DWORD, LPVOID) = (void *)(b + nt->OptionalHeader.AddressOfEntryPoint);
                if (!dllmain((HINSTANCE)b, DLL_PROCESS_ATTACH, NULL)) ERR("%s DllMain failed", dll);
            }
            return (HMODULE)b;
        }
    }
    l->h = LoadLibraryA(dll);
    if (l->h) LOG("%s -> host", dll);
    return l->h;
}

/* Crash diagnostics: report the faulting instruction as module+offset. */
static LONG WINAPI jb_crash_filter(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const MappedModule *m = module_for(er->ExceptionAddress);
    char where[MAX_PATH + 32];
    if (m) snprintf(where, sizeof where, "%s+0x%llx", m->name, (unsigned long long)((uint8_t *)er->ExceptionAddress - m->base));
    else {
        HMODULE h = NULL; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, er->ExceptionAddress, &h))
            GetModuleFileNameA(h, name, sizeof name);
        snprintf(where, sizeof where, "%s+0x%llx", name, (unsigned long long)((uint8_t *)er->ExceptionAddress - (uint8_t *)h));
    }
    fprintf(stderr, "[jackbox] CRASH exception 0x%08lx at %s (thread %lu)", er->ExceptionCode, where, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        fprintf(stderr, ", %s address %p", er->ExceptionInformation[0] ? "write" : "read", (void *)er->ExceptionInformation[1]);
    fputc('\n', stderr);
    write_report();
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ------------------------------------------------------------------------ */

static void dirname_of(const char *path, char *out, size_t n)
{
    char full[MAX_PATH]; GetFullPathNameA(path, sizeof full, full, NULL);
    snprintf(out, n, "%s", full);
    char *s = strrchr(out, '\\'); if (s) *s = 0;
}

int main(int argc, char **argv)
{
    g_verbose = getenv("JACKBOX_LOG") && *getenv("JACKBOX_LOG") != '0';
    g_unimpl_continue = getenv("JACKBOX_UNIMPL") && !strcmp(getenv("JACKBOX_UNIMPL"), "continue");
    g_report_path = getenv("JACKBOX_REPORT");
    InitializeCriticalSection(&g_report_lock);
    SetUnhandledExceptionFilter(jb_crash_filter);
    if (argc < 2) {
        fprintf(stderr, "usage: jackbox <title.exe> [args...]\n");
        return 2;
    }
    char self[MAX_PATH]; GetModuleFileNameA(NULL, self, sizeof self);
    dirname_of(self, g_runtime_dir, sizeof g_runtime_dir);
    dirname_of(argv[1], g_title_dir, sizeof g_title_dir);
    SetEnvironmentVariableA("JACKBOX_TITLE_DIR", g_title_dir);

    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    GetFullPathNameA(argv[1], sizeof g_title_path, g_title_path, NULL);
    jb_set_title_image(NULL, g_title_path);
    const char *tname = strrchr(argv[1], '\\') ? strrchr(argv[1], '\\') + 1 : (strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1]);
    uint8_t *base = map_image(argv[1], tname);
    if (!base) return JB_EXIT_LOAD_FAILED;
    QueryPerformanceCounter(&t1);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    LOG("title mapped in %.2f ms; %d unresolved import(s)", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, g_unresolved_count);
    if (g_unresolved_count) {
        fprintf(stderr, "[jackbox] %d unresolved import(s) bound to diagnostic traps:\n", g_unresolved_count);
        for (Unresolved *u = g_unresolved; u; u = u->next) fprintf(stderr, "[jackbox]   %s!%s\n", u->dll, u->name);
    }
    atexit(write_report);
    jb_set_title_image(base, g_title_path);


    const TlsImage *ti = tls_image_for(base);
    if (ti) tls_run_image_callbacks(ti, DLL_PROCESS_ATTACH);
    int (*entry)(void) = (void *)(base + nt->OptionalHeader.AddressOfEntryPoint);
    LOG("entering title at %p", (void *)entry);
    int rc = entry();
    LOG("title entry returned %d", rc);
    return rc;
}
