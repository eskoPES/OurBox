# JackBox status and milestones

Each milestone has a pass/fail check that `make test` (or `tests/run_titles.sh`)
runs. Test titles are Windows-ABI x86-64 programs built with clang
(MSVC-style TLS and SEH codegen). **They validate the loader and runtime only.
Passing them is not evidence of Xbox console compatibility.**

| # | Milestone | Status | Check |
|---|---|---|---|
| M1a | Cross toolchain + Wine test runner | done | `make` builds, `tools/wine.sh` runs |
| M1b | Native loader: map, relocate, imports, title-local DLLs + DllMain, static TLS for up to 8 images and all threads, `.pdata` unwind registration, section protections, entry | done | t_basic, t_tls, t_seh |
| M1c | Diagnostics: unresolved imports listed at load, bound to traps that report `dll!func` + call site, abort or `E_NOTIMPL` policy; crash filter reports `module+offset`; report file with peak memory | done | t_unimpl (both policies) |
| M1d | XGameRuntime core, XSystem, XTaskQueue (manual, thread pool, serialized, immediate, delayed, terminate) and XAsync (provider model, results, cancel, XAsyncRun) | done | t_xgameruntime |
| M1e | Loader overrides so the title sees itself: GetModuleHandle(NULL), GetModuleFileName, GetCommandLine | done | t_basic |
| M2 | D3D12 path: `jb_d3d12x.dll` + offscreen triangle title, pixel readback checksum, frame time | next | |
| M3 | Shader pipeline: DXIL via host; console precompiled shader blobs | blocked (see below) | |
| M4 | Virtual memory: XMemAlloc-style APIs, fixed-address and large-page mappings, GPU-visible memory | planned | |
| M5 | GameInput -> host input, XAudio2 via host, XUser/XGameSave offline | planned | |

## Precise blockers

1. **No authorized console binaries.** Console (`Gaming.Xbox.Scarlett.x64`)
   builds need the GDK with Xbox Extensions (GDKX), which is licensed to
   registered developers. The public Xbox-GDK-Samples repository is source
   only (MIT) and builds PC targets with the public GDK, so it cannot produce
   console binaries here, and a PC build of a sample does not prove console
   compatibility. Needed: console builds of the samples from a GDKX licensee,
   with permission to use them for testing.
2. **Console module names and D3D12.X surface are undocumented publicly.** The
   names `xgameruntime.dll`, `d3d12_x.dll`, `d3d12_xs.dll` in
   `loader/jb_support.c` are (guess). The D3D12.X extensions and the console
   resource/descriptor layouts are in GDKX headers under NDA. Needed: an
   authorized console binary to read its import table (`jackbox imports`
   prints it), or public documentation.
3. **Console shaders.** Console titles ship precompiled shaders for the
   console GPU, not portable DXIL **(guess: per title)**. Running them needs a
   GPU ISA to DXIL/SPIR-V translator. Needs real shader blobs to develop
   against.
4. **Retail content is encrypted** with keys held in console hardware. Out of
   scope by design: JackBox never decrypts retail packages.

## Measured (cloud CI under Wine 9.0, no GPU)

See `build/win64/results.tsv` after a run: per-title result, exit code, wall
time (dominated by Wine startup, about 2.5 s) and peak working set.
