# JackBox: architecture and roadmap

JackBox is a clean-room effort to run software built for Xbox Series X|S on a PC.
It is a **compatibility layer**, in the spirit of Wine/Proton, not a cycle-level
emulator. This document says what is realistic, what is not, and the order we
build things in. Statements marked **(guess)** are unverified and need checking.

## 1. Where the field stands (November 2026)

| Project | Target | Approach | Status |
|---|---|---|---|
| xemu | original Xbox (2001) | LLE: full-system emulation of the Pentium III + NV2A, needs a user-dumped BIOS | Mature, most of the library plays |
| Cxbx-Reloaded | original Xbox | HLE: runs x86 code natively, swaps the Xbox kernel/D3D8 for Windows equivalents | Playable for many titles |
| Xenia | Xbox 360 | JIT of PowerPC to x64, HLE of the 360 kernel, GPU to D3D12/Vulkan | Large part of the library boots |
| (none) | Xbox One / Series | — | No public emulator runs retail Xbox One or Series games |

Recent "emulation on Xbox Series" news (Switch and PS5 emulators running *on* a
Series console in Dev Mode) is the opposite direction and does not help us.

Two lessons carry over:
* **Cxbx-Reloaded is our closest model.** When guest and host share an ISA you
  do not emulate the CPU, you replace the operating system underneath it.
* **Xenia's GPU work is the hard part**, and it is the same here.

## 2. What an Xbox Series X|S is

* **CPU:** 8-core AMD Zen 2, x86-64. Same ISA as a PC, so game code can run
  natively. No JIT needed.
* **GPU:** custom AMD RDNA 2 (52 CUs on Series X, 20 on S). Games talk to it
  through **D3D12.X**, a console flavour of Direct3D 12 with extra APIs, fixed
  memory layouts and precompiled shaders.
* **Memory:** unified GDDR6 (16 GB on X, 10 GB on S) shared by CPU and GPU.
* **OS:** a Hyper-V based hypervisor running a host OS, a shared OS (dashboard)
  and a game OS partition. The game OS is a trimmed Windows kernel, so titles see
  a Win32-like API surface.
* **SDK:** the Microsoft GDK. On console it adds XGameRuntime (XGame*, XUser,
  XTaskQueue, XStore, XGameSave...), GameInput, XAudio2, D3D12.X and the console
  memory APIs.
* **Packages:** games ship as XVC/XVD containers, signed and encrypted
  (AES-XTS). Content keys are protected by the console's security processor and
  never leave it in the clear.

## 3. The honest constraint

Retail Series packages are encrypted and the keys live in console hardware.
There is no clean-room way to get them, and JackBox will **never** ship keys,
firmware, leaked SDKs or DRM circumvention. So:

* JackBox can **parse** XVD/XVC metadata (headers are unencrypted and publicly
  documented by community research such as xvdtool).
* JackBox can **run** only code the user has legal access to in unencrypted form:
  * their own homebrew and test programs,
  * GDK-built PC executables they own (many Series games also ship on PC via the
    GDK, with largely the same code and a large overlap in APIs),
  * content they have dumped and decrypted themselves under their local law.
    JackBox does not help with that step.

That makes the realistic first goal **"run GDK console-style binaries on Linux
and Windows"**, not "boot a retail disc."

## 4. Architecture

```
   game.exe (PE32+, x86-64)
        |  imports
        v
 +---------------------------------------------------------+
 | JackBox loader (pe crate, later a Wine-side loader)     |
 |  - parses PE, classifies every import                   |
 |  - maps Xbox-only modules to JackBox runtime DLLs       |
 +---------------------------------------------------------+
        |                       |                       |
  Win32 / api-ms-win-*     XGameRuntime, XUser,     D3D12.X (d3d12_x,
  (Wine or real Windows)   XTaskQueue, GameInput,   d3d12_xs) -> D3D12
                           XAudio2 shims            -> vkd3d-proton -> Vulkan
                           (runtime/ in this repo)
```

Components:

1. **`crates/xvd`**: XVD/XVC header parser. Metadata only. Done in v0.1.
2. **`crates/pe`**: PE32+ parser with import table walking. Done in v0.1.
3. **`crates/hle`**: catalog of known Xbox-specific modules and exports, with
   implementation status. Drives the coverage report. Done in v0.1 (catalog is
   small and partly **(guess)**).
4. **`crates/cli`**: `jackbox inspect <file>` and `jackbox imports <exe>`
   coverage report. Done in v0.1.
5. **`runtime/`**: clean-room reimplementations of Xbox-only DLLs as real
   Windows DLLs, built with clang + lld-link (no Microsoft SDK needed).
   v0.1 has a first `xgameruntime.dll` with a handful of entry points.
6. **Loader integration (next):** run under Wine/Proton with DLL overrides so
   `xgameruntime.dll` etc. resolve to ours. On Windows, the same DLLs sit next
   to the exe.
7. **D3D12.X translation (the long pole):** map D3D12.X extensions and console
   resource layouts onto standard D3D12, then let vkd3d-proton reach Vulkan.
   Precompiled console shaders (RDNA2 ISA, not DXIL) will need either a
   translator from AMD GCN/RDNA ISA to SPIR-V or recovering DXIL where titles
   keep it **(guess: varies per title)**.
8. **Memory model:** emulate the console's unified-memory and fixed virtual
   address assumptions (XMemAlloc-style APIs, large pages).

## 5. Roadmap

| Milestone | Goal | How we know it works |
|---|---|---|
| M0 (this) | Scaffold, parsers, coverage report, first runtime DLL | Tests pass; `jackbox imports` on our own DLL |
| M1 | Hello-world GDK-style exe runs under Wine with our xgameruntime.dll | Prints title ID via XGameGetXboxTitleId |
| M2 | XTaskQueue + XAsync implemented; XUser/XGameSave stubs that succeed offline | Clean-room sample using async calls completes |
| M3 | GameInput -> SDL/evdev, XAudio2 -> FAudio | Controller and sound in a sample |
| M4 | D3D12.X subset -> D3D12 -> vkd3d-proton | A triangle from a console-style D3D12 sample |
| M5 | First owned GDK PC title boots to menu | Coverage report at 100% for its imports |

## 6. Rules for contributors

* Clean room: implement from public documentation (learn.microsoft.com GDK
  reference, community research) and behaviour observed on hardware you own.
  Never copy Microsoft headers, libraries or leaked material into this repo.
* No keys, firmware, or decryption of retail content.
* Mark unverified facts with **(guess)** in code and docs.
