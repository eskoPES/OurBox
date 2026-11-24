/* Console-style memory patterns: large reservations, fixed high addresses,
 * large-page requests (must fall back on hosts without the privilege),
 * commit inside reservations, write and verify, release. */
#include <windows.h>
#include <stdint.h>
#include "jbtest.h"
int main(void)
{
    /* 8 GB reserve, commit 256 MB piecewise, touch every page. */
    SIZE_T reserve = 8ull << 30, chunk = 64ull << 20;
    uint8_t *r = VirtualAlloc(NULL, reserve, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(r != NULL);
    for (int i = 0; r && i < 4; i++) {
        uint8_t *c = VirtualAlloc(r + i * chunk, chunk, MEM_COMMIT, PAGE_READWRITE);
        CHECK(c == r + i * chunk);
        for (SIZE_T o = 0; c && o < chunk; o += 4096) c[o] = (uint8_t)(o >> 12);
    }
    int ok = 1;
    for (SIZE_T o = 0; r && o < 4 * chunk; o += 4096) ok &= r[o] == (uint8_t)((o % chunk) >> 12);
    CHECK(ok);
    CHECK(VirtualFree(r, 0, MEM_RELEASE));

    /* Fixed address in the high user range. */
    void *want = (void *)0x0000100000000000ull;
    uint8_t *f = VirtualAlloc(want, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(f == want);
    if (f) { f[12345] = 0x5A; CHECK(f[12345] == 0x5A); VirtualFree(f, 0, MEM_RELEASE); }

    /* Large pages: consoles always grant them; JackBox falls back if the host cannot. */
    SIZE_T lp = GetLargePageMinimum(); if (!lp) lp = 2 << 20;
    uint8_t *l = VirtualAlloc(NULL, 4 * lp, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    CHECK(l != NULL);
    if (l) { l[4 * lp - 1] = 1; VirtualFree(l, 0, MEM_RELEASE); }
    DONE();
}
