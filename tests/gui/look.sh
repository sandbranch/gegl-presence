#!/bin/sh
# Starts GIMP on Broadway with the photo (PRESENCE_GUI_PHOTO), does the
# given steps of gimp-devtools/gui/cdp.mjs
# (positions on the page) and leaves a screenshot of the page in
# tests/output/gui/look.png; then stops GIMP. For finding positions.
#
#   tests/gui/look.sh [step...]
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=SCRIPTDIR/common.sh
. "$here/common.sh"
start_gimp look || { echo "GIMP did not show the picture (log: $gui_out/gimp-look.log)"; exit 1; }
$cdp "$view" "$@" wait:800 shot:"$gui_out/look.png" || exit 1
echo "$gui_out/look.png"
