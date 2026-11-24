#!/usr/bin/env bash
# Builds a self-contained Windows test bundle: dist/ourbox-win64.zip
set -euo pipefail
cd "$(dirname "$0")/.."
make -s
rm -rf dist/ourbox-win64 && mkdir -p dist/ourbox-win64/titles
cp build/win64/jackbox.exe build/win64/*.dll dist/ourbox-win64/
cp build/win64/titles/*.exe build/win64/titles/*.dll build/win64/titles/MicrosoftGame.config dist/ourbox-win64/titles/
cp tests/titles/shaders/rt.hlsl dist/ourbox-win64/titles/
[[ -f tests/titles/shaders/rt.dxil ]] && cp tests/titles/shaders/rt.dxil dist/ourbox-win64/titles/
cp tests/run_windows.ps1 dist/ourbox-win64/
(cd dist && rm -f ourbox-win64.zip && python3 -c "import shutil; shutil.make_archive('ourbox-win64', 'zip', '.', 'ourbox-win64')")
echo dist/ourbox-win64.zip
