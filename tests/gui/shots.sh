#!/bin/sh
# The filters' dialogs in GIMP on a Broadway display, driven with a
# headless Chrome, for the screenshots in docs/: the Filters > Enhance
# menu and the Colors menu with the filters, Clarity, Texture, Dehaze
# and Whites and Blacks in split view on CC0 and public domain photos of
# tests/images (tests/images/fetch.py); and Dehaze applied from the menu
# with OK, the image saved, and the XCF checked in headless GIMP: the
# layer keeps the filter, live, with its settings. Run tests/gimp-check.sh
# first (it puts the modules in place). Lists the user's folders before
# and after and fails if anything changed.
#
#   tests/gui/shots.sh                       all
#   PRESENCE_SHOTS="clarity dehaze" tests/gui/shots.sh   some
#     (menus clarity texture dehaze wb nde)
#
# Prints PASS or FAIL per step and exits non-zero if one failed.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=SCRIPTDIR/common.sh
. "$here/common.sh"
docs=$top/docs
images=$top/tests/images
mkdir -p "$docs"
status=0
pass () { echo "PASS  gui_$1"; }
fail () { echo "FAIL  gui_$1"; status=1; }

# the dialog: the canvas made last that is neither the whole page nor
# small; Broadway draws each window as a canvas with its shadow (26
# pixels here), the title bar's middle 36 pixels below the canvas's top.
# (Menus that were open keep their canvases, above the dialog, so the
# order of the canvases on the page is used, not their z-index.)
dialog_rect () {
    $cdp "$view" "eval:(() => { const c = [...document.querySelectorAll('canvas')]
      .map(e => e.getBoundingClientRect())
      .filter(r => r.width < 1200 && r.height > 150 && r.height < 780).pop();
      return c ? [c.left, c.top, c.width, c.height].map(Math.round).join(',') : '' })()" \
      2>/dev/null | tail -1
}

# moves the dialog to the top right corner of the page; sets vx, vy (its
# visible top left corner) and vw (its visible width)
move_dialog () {
    r=$(dialog_rect)
    [ -n "$r" ] || return 1
    l=${r%%,*}; r=${r#*,}; t=${r%%,*}; r=${r#*,}; w=${r%%,*}
    vw=$((w - 52))
    vx=$((1278 - vw))
    vy=120
    gx=$((l + w / 2)); gy=$((t + 36))
    $cdp "$view" down:$gx,$gy move:$(( (gx + vx + vw / 2) / 2 )),$(( (gy + vy + 10) / 2 )) \
      move:$((vx + vw / 2)),$((vy + 10)) up:$((vx + vw / 2 + 1)),$((vy + 10)) wait:1200 >/dev/null
}
# a point in the dialog, from its visible top left corner
p () { echo "$((vx + $1)),$((vy + $2))"; }
# the x of value v on a slider from lo to hi (the bar starts 7 pixels in
# and ends 48 before the dialog's right edge)
slider () { echo $(( 7 + ($1 - $2) * (vw - 55) / ($3 - $2) )); }


FILTERS=426,49
COLORS=321,49
# Filters > Enhance: its submenu opens on hover and Right
enhance () {
    $cdp "$view" click:$FILTERS wait:1000 move:450,175 move:460,176 wait:1500 key:Right \
      wait:1500 "$@" >/dev/null
}
# the three filters in Filters > Enhance (at its end, with only GIMP's own
# plug-ins: tests/gui/start.sh), and Whites and Blacks in Colors
CLARITY=689,425
DEHAZE=693,450
TEXTURE=692,475
WB=389,753

open_dialog () {
    move_dialog || { fail "$1: no dialog"; return 1; }
    pass "$1: the dialog opened"
}

session () {
    name=$1
    PRESENCE_GUI_NAME=$name
    case $name in
        clarity) PRESENCE_GUI_PHOTO=$images/alps-grenoble.jpg ;;
        texture) PRESENCE_GUI_PHOTO=$images/face-portrait.jpg ;;
        wb) PRESENCE_GUI_PHOTO=$images/hazy-summer-valley.jpg ;;
        *) PRESENCE_GUI_PHOTO=$images/hazy-mountains-fire.jpg ;;
    esac
    [ -f "$PRESENCE_GUI_PHOTO" ] || { fail "$name: no $PRESENCE_GUI_PHOTO (tests/images/fetch.py)"; return; }
    export PRESENCE_GUI_NAME PRESENCE_GUI_PHOTO
    tries=0
    until start_gimp "$name"; do
        stop_all
        tries=$((tries + 1))
        [ $tries -lt 3 ] || { fail "$name: GIMP did not start (log: $gui_out/gimp-$name.log)"; return; }
    done
    case $name in
    menus)
        enhance shot:"$docs/enhance-menu.png" key:Escape key:Escape wait:800 &&
          $cdp "$view" click:$COLORS wait:1500 shot:"$docs/colors-menu.png" key:Escape >/dev/null &&
          pass "menus: Filters > Enhance and Colors with the filters";;
    clarity)
        enhance click:$CLARITY wait:4000
        open_dialog clarity || return
        $cdp "$view" click:"$(p "$(slider 60 -100 100)" 115)" wait:1500 \
          click:"$(p 268 224)" wait:8000 shot:"$docs/clarity.png" >/dev/null &&
          pass "clarity: amount 60, split view";;
    texture)
        enhance click:$TEXTURE wait:4000
        open_dialog texture || return
        $cdp "$view" click:"$(p "$(slider -70 -100 100)" 115)" wait:1500 \
          click:"$(p 268 224)" wait:8000 shot:"$docs/texture.png" >/dev/null &&
          pass "texture: amount about -70 (smoothing skin), split view";;
    dehaze)
        enhance click:$DEHAZE wait:4000
        open_dialog dehaze || return
        $cdp "$view" click:"$(p "$(slider 60 -100 100)" 115)" wait:1500 \
          click:"$(p 269 219)" wait:8000 shot:"$docs/dehaze.png" >/dev/null &&
          pass "dehaze: amount 60, split view";;
    wb)
        $cdp "$view" click:$COLORS wait:1500 click:$WB wait:4000 >/dev/null
        open_dialog wb || return
        $cdp "$view" click:"$(p "$(slider 60 -100 100)" 115)" wait:1500 \
          click:"$(p "$(slider -50 -100 100)" 143)" wait:1500 \
          click:"$(p $((vw - 101)) 250)" wait:6000 shot:"$docs/whites-blacks.png" >/dev/null &&
          pass "wb: whites 60, blacks -50, split view";;
    nde)
        enhance click:$DEHAZE wait:4000
        open_dialog nde || return
        $cdp "$view" click:"$(p "$(slider 60 -100 100)" 115)" wait:3000 \
          click:"$(p 319 254)" wait:6000 shot:"$gui_out/nde.png" >/dev/null
        touch "$gui_out/save"
        if wait_for "$gui_out/saved" 60; then
            pass "nde: the image saved with the filter"
        else
            fail "nde: not saved (log: $gui_out/gimp-nde.log)"
        fi;;
    esac
    stop_all
}

snapshot_begin gui
rm -f "$gui_out/gui-nde.xcf"
for s in ${PRESENCE_SHOTS:-menus clarity texture dehaze wb nde}; do
    session "$s"
done

# the filter in the saved XCF, in headless GIMP
xcf=$gui_out/gui-nde.xcf
if [ -f "$xcf" ]; then
    GIMP3_DIRECTORY="$out/gimp-profile"
    GEGL_PATH="$out/gimp-check-modules:/app/lib/gegl-0.4"
    export GIMP3_DIRECTORY GEGL_PATH
    pass_env="GIMP3_DIRECTORY GEGL_PATH"
    run_prefix="--timeout=600" in_gimp gimp-console-3.2 --no-interface --no-data --no-fonts \
      --batch-interpreter python-fu-eval -b "
import gi
gi.require_version('Gimp', '3.0')
from gi.repository import Gimp, Gio
img = Gimp.file_load(Gimp.RunMode.NONINTERACTIVE, Gio.File.new_for_path('$xcf'))
for l in img.get_layers():
    for f in l.get_filters():
        print('FILTER', l.get_name(), f.get_operation_name(), round(f.get_config().get_property('amount')))
" --quit >"$gui_out/xcf-check.log" 2>&1
    if grep -qE "^FILTER .* presence:dehaze (5[5-9]|6[0-5])$" "$gui_out/xcf-check.log"; then
        pass "nde: after loading the XCF the layer has the live filter presence:dehaze, amount about 60"
    else
        fail "nde: the XCF's layer: $(grep FILTER "$gui_out/xcf-check.log")"
    fi
fi

snapshot_end gui || status=1
[ $status = 0 ] && echo "gui: all passed" || echo "gui: FAILED (screenshots in docs, logs in $gui_out)"
exit $status
