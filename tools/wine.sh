#!/usr/bin/env bash
# Run a Windows program under Wine with a clean environment (the cloud
# container's large environment crashes Wine 9.0 at startup). JACKBOX_*
# variables are passed through.
# Usage: tools/wine.sh program.exe [args...]
mkdir -p "/run/user/$(id -u)" 2>/dev/null
pass=()
while IFS='=' read -r k _; do [[ $k == JACKBOX_* ]] && pass+=("$k=${!k}"); done < <(env)
exec env -i PATH=/usr/bin:/bin:/usr/lib/wine HOME="$HOME" \
  WINEPREFIX="${WINEPREFIX:-/tmp/jackbox-wineprefix}" WINEDEBUG="${WINEDEBUG:--all}" \
  "${pass[@]}" wine64 "$@"
