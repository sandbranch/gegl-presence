# shellcheck shell=sh
# Sourced by the scripts in tests/gui: starts GIMP (start.sh) and a
# headless Chrome looking at the Broadway page. Sets $gui_out, $cdp and
# $view; start_gimp starts both and waits until the picture is shown;
# stop_all stops the Chrome and the GIMP it started, and only those
# (also on exit).
#
# Needs a headless Chrome (google-chrome or chromium), node 22 and
# ../gimp-devtools (or GIMP_PLUGIN_DEVTOOLS) for gui/cdp.mjs.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
# ($here is set by the script that sources this one: tests/gui)
# shellcheck disable=SC2154
gui=$here
here=$(dirname "$gui")
# shellcheck source=SCRIPTDIR/../common.sh
. "$here/common.sh"
here=$gui
gui_out=$out/gui
devtools=${GIMP_PLUGIN_DEVTOOLS:-$top/../gimp-devtools}
cdp="node $devtools/gui/cdp.mjs"
view=size:${PRESENCE_VIEW:-1280,860}

chrome=$(command -v google-chrome || command -v chromium || command -v chromium-browser)
[ -n "$chrome" ] || { echo "SKIP  gui: no Chrome or Chromium"; exit 0; }
command -v node >/dev/null 2>&1 || { echo "SKIP  gui: no node"; exit 0; }
[ -f "$devtools/gui/cdp.mjs" ] || { echo "SKIP  gui: no $devtools/gui/cdp.mjs"; exit 0; }
[ -f "$out/gimp-check-modules/clarity.so" ] ||
  { echo "FAIL  gui: run tests/gimp-check.sh first (it puts the modules in place)"; exit 1; }
mkdir -p "$gui_out"

# the Flatpak instance of our GIMP: the one whose sandbox runs our
# gui-script.py
ours () {
    flatpak ps --columns=instance,child-pid,application 2>/dev/null |
      while read -r instance pid app; do
          [ "$app" = org.gimp.GIMP ] || continue
          { tr '\0' ' ' < "/proc/$pid/cmdline"; } 2>/dev/null |
            grep -qF "$gui/gui-script.py" && echo "$instance"
      done
}
[ -z "$(ours)" ] || { echo "FAIL  gui: the test's GIMP is already running"; exit 1; }
chrome_pid=
gimp_pid=
stop_all () {
    touch "$gui_out/quit"
    i=0
    while [ -n "$(ours)" ] && [ $i -lt 10 ]; do sleep 1; i=$((i + 1)); done
    for instance in $(ours); do
        flatpak kill "$instance" 2>/dev/null
    done
    [ -n "$gimp_pid" ] && wait "$gimp_pid" 2>/dev/null
    gimp_pid=
    if [ -n "$chrome_pid" ]; then
        kill "$chrome_pid" 2>/dev/null
        wait "$chrome_pid" 2>/dev/null
        sleep 1
        rm -rf "$gui_out/chrome.$$"
    fi
    chrome_pid=
}
trap stop_all EXIT
trap 'exit 1' INT TERM HUP

# a free port for Broadway, 8087 to 8179
free_broadway_port () {
    python3 -c '
import socket
for port in range(8087, 8180):
    try:
        s = socket.socket(socket.AF_INET6)
        s.bind(("::", port))
        s.close()
        print(port)
        break
    except OSError:
        pass'
}

wait_for () {
    i=0
    while [ ! -f "$1" ] && [ $i -lt "${2:-60}" ]; do
        sleep 1
        i=$((i + 1))
    done
    [ -f "$1" ]
}

# start_gimp <name>: GIMP with PRESENCE_GUI_PHOTO, the Chrome, the page
start_gimp () {
    PRESENCE_BROADWAY_PORT=$(free_broadway_port)
    export PRESENCE_BROADWAY_PORT
    page=http://127.0.0.1:$PRESENCE_BROADWAY_PORT/
    rm -f "$gui_out/ready" "$gui_out/page-open" "$gui_out/quit" "$gui_out/save" \
          "$gui_out/saved"
    "$here/start.sh" >"$gui_out/gimp-$1.log" 2>&1 &
    gimp_pid=$!
    CDP_PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    export CDP_PORT
    "$chrome" --headless=new --remote-debugging-port="$CDP_PORT" \
      --window-size="${PRESENCE_VIEW:-1280,860}" \
      --user-data-dir="$gui_out/chrome.$$" --password-store=basic about:blank \
      >/dev/null 2>&1 &
    chrome_pid=$!
    i=0
    until python3 -c "import socket; socket.create_connection(('127.0.0.1', $PRESENCE_BROADWAY_PORT), 1)" \
            2>/dev/null || [ $i -gt 120 ]; do
        sleep 0.5
        i=$((i + 1))
    done
    i=0
    until $cdp "$view" nav:"$page" wait:1500 >/dev/null 2>&1 || [ $i -gt 10 ]; do
        sleep 1
        i=$((i + 1))
    done
    touch "$gui_out/page-open"
    wait_for "$gui_out/ready" 400 && sleep 4
}
