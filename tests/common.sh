# shellcheck shell=sh
# Sourced by the test scripts. Sets $here (tests), $top and $src (the
# repository) and $out (tests/output), and defines:
#
#   in_sdk <command line>   runs it in the repository, inside the Flatpak
#                           GIMP's SDK (gimp-devtools/gimp-build.sh)
#                           or natively with GIMP_FLATPAK=0
#   in_gimp <command...>    runs a command of the GIMP Flatpak (gegl,
#                           python3, gimp-console-3.2) or natively; the
#                           variables named in $pass_env are passed on, and
#                           $run_prefix (e.g. "--timeout=600") goes to
#                           gimp_run, and $extra_fs is one more folder the
#                           Flatpak may see (e.g. "DIR:ro")
#   snapshot_begin <name>   lists the user's folders of GIMP and other apps
#   snapshot_end <name>     lists them again; prints PASS or FAIL and
#                           returns 1 if anything changed
#
# Everything runs isolated from the user's own folders with the shared
# tests/isolate.sh (a copy of gimp-devtools/isolate.sh): HOME and
# the XDG folders in the throwaway tests/output/home, inside the Flatpak
# and for flatpak run itself, and no GVFS.
#
# Copyright 2026 David
# SPDX-License-Identifier: GPL-3.0-or-later

# ($here is set by the script that sources this one)
# shellcheck disable=SC2154
top=$(dirname "$here")
src=$top
out=$here/output
mkdir -p "$out"
GIMP_RUN_HOME=$out/home
export GIMP_RUN_HOME
mkdir -p "$GIMP_RUN_HOME"
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"

gimp_build=${GIMP_BUILD:-$devtools/gimp-build.sh}

in_sdk () {
    if [ -x "$gimp_build" ]; then
        # with GIMP_RUN_HOME, isolated as gimp-run.sh does it
        "$gimp_build" "$top" "$*"
    else
        # shellcheck disable=SC2016
        gimp_run --devel --filesystem="$top" \
          --env=PKG_CONFIG_PATH=/app/lib/pkgconfig:/app/share/pkgconfig \
          -- sh -c 'cd "$0" && sh -c "$1"' "$top" "$*"
    fi
}

in_gimp () {
    envs=
    for v in $pass_env; do
        eval "val=\${$v-}"
        envs="$envs --env=$v=$val"
    done
    # shellcheck disable=SC2086
    gimp_run $run_prefix --filesystem="$top" ${extra_fs:+--filesystem="$extra_fs"} $envs -- "$@"
}

snapshot_begin () {
    snapshot_take "$out/snapshot-$1.txt"
}

snapshot_end () {
    snapshot_check "$out/snapshot-$1.txt" "" || return 1
}
