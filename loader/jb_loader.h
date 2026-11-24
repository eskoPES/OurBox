#pragma once
#include <windows.h>
#include <stdint.h>

#define JB_EXIT_LOAD_FAILED    0x4A420001 /* "JB" + 1 */
#define JB_EXIT_UNIMPLEMENTED  0x4A420002

typedef struct {
    SIZE_T PeakWorkingSetSize;
    SIZE_T PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS_EX_JB;

const char *jb_runtime_module_for(const char *dll);
void jb_set_title_image(void *base, const char *title_path);
/* Loader-provided replacements for host functions whose answer must describe
 * the title rather than jackbox.exe (module handle, file name, command line). */
void *jb_host_override(const char *dll, const char *func);
int jb_get_memory_counters(PROCESS_MEMORY_COUNTERS_EX_JB *out);

typedef struct {
    SIZE_T committed, peak_committed;
    unsigned long allocations, large_page_fallbacks, stripped_flag_calls;
} JbMemStats;
LPVOID WINAPI jb_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL WINAPI jb_VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
void jb_memory_stats(JbMemStats *out);
