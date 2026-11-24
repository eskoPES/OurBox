/* Loader support: module redirection table and process info. */
#include <windows.h>
#include <psapi.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include "jb_loader.h"

/* Xbox-only modules that JackBox replaces. Names marked (guess) come from
 * public GDK documentation and header names and must be verified against an
 * authorized console binary. */
static const struct { const char *title_dll, *jackbox_dll; } k_redirects[] = {
    { "xgameruntime.dll", "xgameruntime.dll" }, /* (guess) */
    { "d3d12_x.dll",      "jb_d3d12x.dll"    }, /* (guess) */
    { "d3d12_xs.dll",     "jb_d3d12x.dll"    }, /* (guess) */
    { "gameinput.dll",    "jb_gameinput.dll" },
};

const char *jb_runtime_module_for(const char *dll)
{
    for (size_t i = 0; i < sizeof k_redirects / sizeof *k_redirects; i++)
        if (!_stricmp(dll, k_redirects[i].title_dll)) return k_redirects[i].jackbox_dll;
    return NULL;
}

static HMODULE g_title_base;
static char g_title_path[MAX_PATH];
static WCHAR g_title_pathw[MAX_PATH];
static char *g_cmdline;
static WCHAR *g_cmdlinew;

/* Command line the title sees: "<title path>" <args after the title>. */
static void build_cmdline(void)
{
    const WCHAR *w = GetCommandLineW();
    int q = 0, skipped = 0;
    /* skip argv[0] (jackbox.exe) and argv[1] (title) using CommandLineToArgv rules for quoting */
    while (*w && skipped < 2) {
        while (*w == L' ' || *w == L'\t') w++;
        while (*w && (q || (*w != L' ' && *w != L'\t'))) { if (*w == L'"') q = !q; w++; }
        skipped++;
    }
    while (*w == L' ' || *w == L'\t') w++;
    size_t n = wcslen(g_title_pathw) + wcslen(w) + 4;
    g_cmdlinew = HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR));
    swprintf(g_cmdlinew, n, *w ? L"\"%ls\" %ls" : L"\"%ls\"", g_title_pathw, w);
    int an = WideCharToMultiByte(CP_ACP, 0, g_cmdlinew, -1, NULL, 0, NULL, NULL);
    g_cmdline = HeapAlloc(GetProcessHeap(), 0, an);
    WideCharToMultiByte(CP_ACP, 0, g_cmdlinew, -1, g_cmdline, an, NULL, NULL);
}

void jb_set_title_image(void *base, const char *title_path)
{
    if (base) { g_title_base = base; return; }
    snprintf(g_title_path, sizeof g_title_path, "%s", title_path);
    MultiByteToWideChar(CP_ACP, 0, title_path, -1, g_title_pathw, MAX_PATH);
    build_cmdline();
}

static HMODULE WINAPI ov_GetModuleHandleA(LPCSTR n) { return n ? GetModuleHandleA(n) : g_title_base; }
static HMODULE WINAPI ov_GetModuleHandleW(LPCWSTR n) { return n ? GetModuleHandleW(n) : g_title_base; }
static BOOL WINAPI ov_GetModuleHandleExA(DWORD f, LPCSTR n, HMODULE *m)
{ if (!n && m) { *m = g_title_base; return TRUE; } return GetModuleHandleExA(f, n, m); }
static BOOL WINAPI ov_GetModuleHandleExW(DWORD f, LPCWSTR n, HMODULE *m)
{ if (!n && m) { *m = g_title_base; return TRUE; } return GetModuleHandleExW(f, n, m); }
static DWORD WINAPI ov_GetModuleFileNameA(HMODULE h, LPSTR buf, DWORD size)
{
    if (h && h != g_title_base) return GetModuleFileNameA(h, buf, size);
    DWORD n = (DWORD)strlen(g_title_path);
    if (!size) return 0;
    lstrcpynA(buf, g_title_path, size);
    if (n >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    return n;
}
static DWORD WINAPI ov_GetModuleFileNameW(HMODULE h, LPWSTR buf, DWORD size)
{
    if (h && h != g_title_base) return GetModuleFileNameW(h, buf, size);
    DWORD n = (DWORD)wcslen(g_title_pathw);
    if (!size) return 0;
    lstrcpynW(buf, g_title_pathw, size);
    if (n >= size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return size; }
    return n;
}
static LPSTR WINAPI ov_GetCommandLineA(void) { return g_cmdline; }
static LPWSTR WINAPI ov_GetCommandLineW(void) { return g_cmdlinew; }

void *jb_host_override(const char *dll, const char *func)
{
    if (_strnicmp(dll, "kernel32", 8) && _strnicmp(dll, "kernelbase", 10) && _strnicmp(dll, "api-ms-win-core-", 16)) return NULL;
    static const struct { const char *name; void *fn; } t[] = {
        { "GetModuleHandleA", ov_GetModuleHandleA }, { "GetModuleHandleW", ov_GetModuleHandleW },
        { "GetModuleHandleExA", ov_GetModuleHandleExA }, { "GetModuleHandleExW", ov_GetModuleHandleExW },
        { "GetModuleFileNameA", ov_GetModuleFileNameA }, { "GetModuleFileNameW", ov_GetModuleFileNameW },
        { "GetCommandLineA", ov_GetCommandLineA }, { "GetCommandLineW", ov_GetCommandLineW },
        { "VirtualAlloc", jb_VirtualAlloc }, { "VirtualFree", jb_VirtualFree },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(func, t[i].name)) return t[i].fn;
    return NULL;
}

int jb_get_memory_counters(PROCESS_MEMORY_COUNTERS_EX_JB *out)
{
    PROCESS_MEMORY_COUNTERS pmc = { sizeof pmc };
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
    out->PeakWorkingSetSize = pmc.PeakWorkingSetSize;
    out->PeakPagefileUsage = pmc.PeakPagefileUsage;
    return 1;
}
