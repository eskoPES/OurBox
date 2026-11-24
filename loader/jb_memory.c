/*
 * Title virtual-memory interposer.
 *
 * Title calls to VirtualAlloc/VirtualFree go through here so JackBox can
 *  - account committed memory per title (current, peak) for the report,
 *  - fall back from large/huge pages when the host cannot provide them
 *    (Windows needs SeLockMemoryPrivilege; consoles always have them),
 *  - strip allocation-type and protection bits that are not valid on the PC
 *    host, logging each distinct one once so console-only flags are visible.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "jb_loader.h"

#ifndef MEM_64K_PAGES
#define MEM_64K_PAGES (MEM_LARGE_PAGES | MEM_PHYSICAL)
#endif

/* Allocation-type and protection bits the PC host accepts. */
static const DWORD k_host_alloc_bits = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_RESET_UNDO | MEM_TOP_DOWN |
                                       MEM_WRITE_WATCH | MEM_PHYSICAL | MEM_LARGE_PAGES | 0x80000000 /* MEM_4MB_PAGES */;
static const DWORD k_host_prot_bits = 0xFF | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE | 0x40000000 /* PAGE_TARGETS_INVALID */;

typedef struct { void *base; SIZE_T committed; } Region;
static SRWLOCK g_lock = SRWLOCK_INIT;
static Region *g_regions; static size_t g_nregions, g_capregions;
static JbMemStats g_stats;
static DWORD g_logged_alloc_bits, g_logged_prot_bits;

static Region *find(void *base)
{
    for (size_t i = 0; i < g_nregions; i++) if (g_regions[i].base == base) return &g_regions[i];
    return NULL;
}

static void account_commit(void *base, SIZE_T bytes)
{
    AcquireSRWLockExclusive(&g_lock);
    Region *r = find(base);
    if (!r) {
        if (g_nregions == g_capregions) {
            g_capregions = g_capregions ? g_capregions * 2 : 256;
            g_regions = HeapReAlloc(GetProcessHeap(), 0, g_regions, g_capregions * sizeof *g_regions);
        }
        if (!g_regions) { g_regions = HeapAlloc(GetProcessHeap(), 0, g_capregions * sizeof *g_regions); }
        r = &g_regions[g_nregions++]; r->base = base; r->committed = 0;
    }
    r->committed += bytes;
    g_stats.committed += bytes;
    if (g_stats.committed > g_stats.peak_committed) g_stats.peak_committed = g_stats.committed;
    g_stats.allocations++;
    ReleaseSRWLockExclusive(&g_lock);
}

static void account_release(void *base)
{
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < g_nregions; i++) {
        if (g_regions[i].base == base) {
            g_stats.committed -= g_regions[i].committed;
            g_regions[i] = g_regions[--g_nregions];
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

static SIZE_T round_up(SIZE_T v, SIZE_T a) { return (v + a - 1) & ~(a - 1); }

LPVOID WINAPI jb_VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    DWORD bad_a = type & ~k_host_alloc_bits, bad_p = prot & ~k_host_prot_bits;
    if (bad_a & ~g_logged_alloc_bits) { fprintf(stderr, "[jackbox] memory: stripping non-host allocation flags 0x%08lx (console-only? see docs/STATUS.md)\n", bad_a); g_logged_alloc_bits |= bad_a; }
    if (bad_p & ~g_logged_prot_bits) { fprintf(stderr, "[jackbox] memory: stripping non-host protection flags 0x%08lx\n", bad_p); g_logged_prot_bits |= bad_p; }
    type &= k_host_alloc_bits; prot &= k_host_prot_bits;
    if (bad_a || bad_p) InterlockedIncrement((LONG *)&g_stats.stripped_flag_calls);

    LPVOID p = VirtualAlloc(addr, size, type, prot);
    if (!p && (type & (MEM_LARGE_PAGES | 0x80000000))) {
        /* Host cannot give large pages: use normal pages, keep the size the
         * title expects (large-page sizes are already multiples of 64K). */
        SIZE_T lp = GetLargePageMinimum();
        DWORD t2 = type & ~(MEM_LARGE_PAGES | 0x80000000);
        p = VirtualAlloc(addr, lp ? round_up(size, lp) : size, t2, prot);
        if (p) InterlockedIncrement((LONG *)&g_stats.large_page_fallbacks);
    }
    if (p && (type & MEM_COMMIT)) {
        /* Committing inside an existing reservation is accounted against the
         * reservation base. */
        MEMORY_BASIC_INFORMATION mbi;
        void *base = VirtualQuery(p, &mbi, sizeof mbi) ? mbi.AllocationBase : p;
        account_commit(base, round_up(size, 4096));
    }
    return p;
}

BOOL WINAPI jb_VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    BOOL ok = VirtualFree(addr, size, type);
    if (ok && (type & MEM_RELEASE)) account_release(addr);
    return ok;
}

void jb_memory_stats(JbMemStats *out)
{
    AcquireSRWLockShared(&g_lock);
    *out = g_stats;
    ReleaseSRWLockShared(&g_lock);
}
