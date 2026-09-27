#!/bin/sh
# Starts the Flatpak GIMP on a Broadway display, on the port
# PRESENCE_BROADWAY_PORT (8087 by default; the page is http://127.0.0.1:port/)
# with tests/gui/gui-script.py (PRESENCE_GUI_PHOTO: the photo it opens), the
# operations of tests/output/gimp-check-modules (tests/gimp-check.sh
# puts them there) and the throwaway profile and home of the tests.
# broadwayd stops when GIMP quits, also when GIMP fails. GIMP loads no
# fonts (--no-fonts). GIMP runs isolated from the user's folders
# (tests/isolate.sh).
#
# GIMP starts when tests/output/gui/page-open exists: GIMP places its
# windows for the size of the Broadway screen, which is that of the page
# once a browser shows it (common.sh does so first).
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
tests=$(dirname "$here")
# shellcheck source=SCRIPTDIR/../common.sh
here_gui=$here
here=$tests
. "$tests/common.sh"
here=$here_gui
gui_out=$out/gui
profile=$out/gimp-profile
port=${PRESENCE_BROADWAY_PORT:-8087}
display=$((port - 8080))
mkdir -p "$gui_out" "$profile"
# no Welcome dialog: GIMP shows it when the profile is of an older
# version, or when asked to
if ! grep -q config-version "$profile/gimprc" 2>/dev/null; then
    version=$(LC_ALL=C flatpak info org.gimp.GIMP | sed -n 's/^ *Version: *//p')
    printf '(config-version "%s")\n' "$version" >> "$profile/gimprc"
fi
grep -q show-welcome-dialog "$profile/gimprc" 2>/dev/null ||
  echo '(show-welcome-dialog no)' >> "$profile/gimprc"
# only GIMP's own plug-ins, not the Flatpak's extensions: G'MIC-Qt
# crashes at start on Broadway in this setup (a message box over the
# screenshots), and the tests need none of them
grep -q plug-in-path "$profile/gimprc" 2>/dev/null ||
  echo '(plug-in-path "${gimp_dir}/plug-ins:${gimp_plug_in_dir}/plug-ins")' >> "$profile/gimprc"
# the filter dialogs' options as GIMP starts them ("Merge filter" off):
# GIMP keeps them between runs
rm -f "$profile/tool-options/gimp-operation-tool"
# one window at the page's top left corner, the size of the page, with
# the Layers and Channels docks on the right; GIMP rewrites the file
# when it quits
size=${PRESENCE_VIEW:-1280,860}
page=0
cat > "$profile/sessionrc" <<SESSION
(session-info "toplevel"
    (factory-entry "gimp-single-image-window")
    (position 0 0)
    (size ${size%,*} ${size#*,})
    (open-on-exit)
    (aux-info
        (left-docks-width "0")
        (right-docks-width "250")
        (maximized "yes"))
    (gimp-dock
        (side right)
        (book
            (current-page $page)
            (dockable "gimp-layer-list"
                (tab-style icon)
                (preview-size 32)
                (aux-info
                    (show-button-bar "true")))
            (dockable "gimp-channel-list"
                (tab-style icon)
                (preview-size 32)
                (aux-info
                    (show-button-bar "true"))))))
(hide-docks no)
(single-window-mode yes)
(show-tabs yes)
SESSION
# broadwayd and GIMP in one shell inside the Flatpak (gimp_run of
# tests/isolate.sh: the throwaway home, no GVFS)
gimp_run --env=GDK_BACKEND=broadway --env=BROADWAY_DISPLAY=:$display \
  --env=GIMP3_DIRECTORY="$profile" --env=PRESENCE_GUI_OUT="$gui_out" \
  --env=GEGL_PATH="$out/gimp-check-modules:/app/lib/gegl-0.4" \
  --env=PRESENCE_GUI_PHOTO="${PRESENCE_GUI_PHOTO:-}" --env=PRESENCE_GUI_NAME="${PRESENCE_GUI_NAME:-gui}" \
  --filesystem="$top" -- sh -c \
  "broadwayd --port $port :$display & bw=\$!; trap 'kill \$bw' EXIT; \
   i=0; while [ ! -f '$gui_out/page-open' ] && [ \$i -lt 300 ]; do sleep 0.2; i=\$((i+1)); done; \
   gimp-3.2 --new-instance --no-splash --no-fonts \
   --batch-interpreter python-fu-eval -b \"exec(open('$here/gui-script.py').read())\""
