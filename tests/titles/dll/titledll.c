/* A title-local DLL, preferred base 0x140000000 (same as the title), so the
 * loader must relocate it. Exercises recursive mapping and DllMain. */
#include <windows.h>
static int g_attached;
static int g_table[3] = { 10, 20, 30 };
static int *g_ptr = &g_table[2]; /* absolute pointer: needs a DIR64 relocation */
__declspec(dllexport) int titledll_value(void) { return *g_ptr + g_attached; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)r; if (reason == DLL_PROCESS_ATTACH) g_attached = 12; return TRUE; }
