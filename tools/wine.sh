#!/usr/bin/env bash
# Run a Windows program under Wine with a clean environment (the cloud
# container's large environment crashes Wine 9.0 at startup). JACKBOX_*
# variables are passed through.
# Usage: tools/wine.sh program.exe [args...]
mkdir -p "/run/user/$(id -u)" 2>/dev/null
# D3D12 under Wine needs Vulkan through an X display; start a virtual one.
if [[ -z "${DISPLAY:-}" ]]; then
  export DISPLAY=:99
  if ! [[ -e /tmp/.X11-unix/X99 ]] && command -v Xvfb >/dev/null; then
    (Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &); sleep 1
  fi
fi
pass=(DISPLAY="$DISPLAY")
while IFS="=" read -r k _; do [[ $k == JACKBOX_* || $k == OURBOX_* ]] && pass+=("$k=${!k}"); done < <(env)
exec env -i PATH=/usr/bin:/bin:/usr/lib/wine HOME="$HOME" \
  WINEPREFIX="${WINEPREFIX:-/tmp/jackbox-wineprefix}" WINEDEBUG="${WINEDEBUG:--all}" \
  "${pass[@]}" wine64 "$@"
