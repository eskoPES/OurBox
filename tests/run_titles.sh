#!/usr/bin/env bash
# JackBox regression suite: runs every test title under jackbox.exe and checks
# its result. On Linux it uses Wine (tools/wine.sh); on Windows (MSYS2) it
# runs natively. Writes build/win64/results.tsv with time and peak memory.
set -u
cd "$(dirname "$0")/.."
B=build/win64
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then RUN=(); else RUN=(tools/wine.sh); fi
pass=0; fail=0; skipped=0
printf "title\tresult\texit\tms\tpeak_working_set\ttitle_peak_committed\tunresolved\timage_hash\tavg_frame_ms\n" > $B/results.tsv
run() { # name expected_exit [env...]
  local t=$1 want=$2; shift 2
  local rep; rep=$(mktemp)
  local t0=$(date +%s%N)
  out=$(env "$@" JACKBOX_REPORT="$(winepath_of "$rep")" timeout 120 "${RUN[@]}" $B/jackbox.exe $B/titles/$t.exe 2>&1)
  local rc=$?
  local ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  local ws=$(sed -n 's/^memory peak_working_set=\([0-9]*\).*/\1/p' "$rep")
  local tm=$(sed -n 's/^title_memory .*peak_committed=\([0-9]*\).*/\1/p' "$rep")
  local un=$(sed -n 's/^unresolved_imports //p' "$rep")
  local ok=0
  [[ $rc -eq $want ]] && ok=1
  [[ $want -eq 0 ]] && ! grep -q "RESULT PASS" <<<"$out" && ok=0
  local gold; gold=$(awk -v t="$t" '$1==t{print $2}' tests/golden.tsv)
  if [[ -n $gold ]] && ! grep -q "IMAGE hash=$gold" <<<"$out"; then ok=0; echo "      image hash mismatch: wanted $gold"; fi
  if [[ $rc -eq 0 && $want -eq 0 ]] && grep -q "RESULT SKIP" <<<"$out"; then
    skipped=$((skipped+1)); r=SKIP; echo "SKIP  $t ($(sed -n 's/^RESULT SKIP //p' <<<"$out"))"
  elif ((ok)); then pass=$((pass+1)); echo "PASS  $t (${ms} ms)"; r=PASS
  else fail=$((fail+1)); echo "FAIL  $t (exit $rc, wanted $want)"; sed 's/^/      /' <<<"$out" | grep -v -E "rundll32|configuration" ; r=FAIL; fi
  local ih=$(sed -n 's/^IMAGE hash=\([0-9a-f]*\).*/\1/p' <<<"$out")
  local fm=$(sed -n 's/^PERF .*avg_frame_ms=\([0-9.]*\).*/\1/p' <<<"$out")
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$t" "$r" "$rc" "$ms" "${ws:-}" "${tm:-}" "${un:-}" "${ih:-}" "${fm:-}" >> $B/results.tsv
  rm -f "$rep"
}
winepath_of() { if ((${#RUN[@]})); then echo "Z:${1//\//\\}"; else echo "$1"; fi; }

run t_basic 0
run t_tls 0
run t_seh 0
run t_xgameruntime 0
run t_memory 0
run t_gamesave 0
run t_d3d12_triangle 0
run t_d3d12_compute 0
run t_d3d12_raytrace 0
# Unimplemented import: trap fires, loader exits JB_EXIT_UNIMPLEMENTED (0x4A420002;
# Wine reports only the low byte to Linux).
if ((${#RUN[@]})); then UNIMPL=2; else UNIMPL=$((0x4A420002)); fi
run t_unimpl $UNIMPL
run t_unimpl 77 JACKBOX_UNIMPL=continue     # trap returns E_NOTIMPL instead
echo "passed $pass, failed $fail, skipped $skipped"
exit $((fail > 0))
