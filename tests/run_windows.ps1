# OurBox regression suite for Windows hosts (no Wine). Run from the folder
# that holds jackbox.exe, the runtime DLLs and titles\.
#   powershell -ExecutionPolicy Bypass -File run_windows.ps1
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$loader = Join-Path $root 'jackbox.exe'
$titles = Join-Path $root 'titles'
$golden = @{ 't_d3d12_triangle' = 'd8ee3505'; 't_d3d12_compute' = '673c9dc5' }
$runs = @(
  @{ t = 't_basic'; want = 0 }, @{ t = 't_tls'; want = 0 }, @{ t = 't_seh'; want = 0 },
  @{ t = 't_xgameruntime'; want = 0 }, @{ t = 't_memory'; want = 0 }, @{ t = 't_gamesave'; want = 0 },
  @{ t = 't_d3d12_triangle'; want = 0 }, @{ t = 't_d3d12_compute'; want = 0 }, @{ t = 't_d3d12_raytrace'; want = 0 },
  @{ t = 't_unimpl'; want = 0x4A420002 }, @{ t = 't_unimpl'; want = 77; env = 'continue' }
)
$results = @("title`tresult`texit`tms`tpeak_working_set`ttitle_peak_committed`tunresolved`timage_hash`tavg_frame_ms`tnote")
$pass = 0; $fail = 0; $skip = 0
foreach ($r in $runs) {
  $report = [IO.Path]::GetTempFileName()
  $env:JACKBOX_REPORT = $report
  if ($r.env) { $env:JACKBOX_UNIMPL = $r.env } else { Remove-Item Env:JACKBOX_UNIMPL -ErrorAction SilentlyContinue }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $out = & $loader (Join-Path $titles "$($r.t).exe") 2>&1 | Out-String
  $rc = $LASTEXITCODE
  $ms = $sw.ElapsedMilliseconds
  $rep = Get-Content $report -ErrorAction SilentlyContinue | Out-String
  $ws = if ($rep -match 'peak_working_set=(\d+)') { $Matches[1] } else { '' }
  $tc = if ($rep -match 'title_memory .*peak_committed=(\d+)') { $Matches[1] } else { '' }
  $un = if ($rep -match 'unresolved_imports (\d+)') { $Matches[1] } else { '' }
  $ih = if ($out -match 'IMAGE hash=([0-9a-f]+)') { $Matches[1] } else { '' }
  $fm = if ($out -match 'avg_frame_ms=([0-9.]+)') { $Matches[1] } else { '' }
  $note = ''
  if ($rc -eq 0 -and $r.want -eq 0 -and $out -match 'RESULT SKIP (.*)') { $res = 'SKIP'; $note = $Matches[1].Trim(); $skip++ }
  else {
    $ok = ($rc -eq $r.want)
    if ($r.want -eq 0 -and $out -notmatch 'RESULT PASS') { $ok = $false }
    if ($golden.ContainsKey($r.t) -and $ih -ne $golden[$r.t]) { $ok = $false; $note = "hash $ih != $($golden[$r.t])" }
    if ($ok) { $res = 'PASS'; $pass++ } else { $res = 'FAIL'; $fail++ }
  }
  Write-Host ("{0,-5} {1} ({2} ms) {3}" -f $res, $r.t, $ms, $note)
  if ($res -eq 'FAIL') { $out -split "`n" | ForEach-Object { Write-Host "      $_" } }
  $results += "$($r.t)`t$res`t$rc`t$ms`t$ws`t$tc`t$un`t$ih`t$fm`t$note"
  Remove-Item $report -ErrorAction SilentlyContinue
}
$results | Set-Content (Join-Path $root 'results.tsv')
Write-Host "passed $pass, failed $fail, skipped $skip"
exit [int]($fail -gt 0)
