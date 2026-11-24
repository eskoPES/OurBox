# JackBox

A clean-room compatibility layer for software built for **Xbox Series X|S**,
in the spirit of Wine/Proton: game code is x86-64 like a PC, so it runs
natively, and JackBox replaces the Xbox-only system interfaces (XGameRuntime,
GameInput, D3D12.X...) underneath it.

**Status: v0.1 scaffold.** Nothing playable yet. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the plan, the honest limits,
and the roadmap.

## What works today

* `jackbox inspect <file>`: identifies XVD/XVC packages (header metadata only)
  and PE32+ executables.
* `jackbox imports <exe>`: lists every imported module, who provides it
  (host Win32/Wine, JackBox, or unknown), and which Xbox-specific functions are
  still missing. This is the to-do list for running a given title.
* `runtime/`: a first clean-room `xgameruntime.dll` (7 entry points), built
  for x86-64 Windows with clang + lld-link, no Microsoft SDK required.
* `samples/hello`: a tiny GDK-style program that links against it.

```sh
cargo test                      # unit tests + builds runtime and sample
./runtime/build.sh
cargo run -- imports runtime/build/hello.exe
```

## Ground rules

* Clean room: public docs and your own hardware only. No Microsoft headers,
  libraries, leaked SDKs, keys or firmware in this repo.
* JackBox does not decrypt retail packages and never will. You supply content
  you have the legal right to run.
* Unverified facts are marked **(guess)** in code and docs.
