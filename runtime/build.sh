#!/usr/bin/env bash
# Builds JackBox runtime DLLs for x86-64 Windows with clang + lld-link.
# No Microsoft SDK is used: the sources are freestanding.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$here/build"; mkdir -p "$out"
for mod in xgameruntime; do
  clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -nostdlib \
        -c "$here/$mod/$mod.c" -o "$out/$mod.obj"
  lld-link /dll /noentry /nodefaultlib /machine:x64 /out:"$out/$mod.dll" "$out/$mod.obj"
  echo "built $out/$mod.dll"
done

# Sample program (samples/hello) linked against the runtime.
s="$here/../samples/hello"
lld-link /lib /def:"$s/kernel32.def" /machine:x64 /out:"$out/kernel32.lib" >/dev/null
lld-link /lib /def:"$s/xgameruntime_extra.def" /machine:x64 /out:"$out/xgr_sample.lib" >/dev/null
clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -nostdlib -c "$s/hello.c" -o "$out/hello.obj"
lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x64 \
         /out:"$out/hello.exe" "$out/hello.obj" "$out/xgr_sample.lib" "$out/kernel32.lib"
echo "built $out/hello.exe"
