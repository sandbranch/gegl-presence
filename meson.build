project('gegl-presence', 'c',
  version: '0.1',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.60.0',
  default_options: ['warning_level=1', 'buildtype=debugoptimized'],
)

gegl_dep = dependency('gegl-0.4', version: '>= 0.4.62')
babl_dep = dependency('babl-0.1', version: '>= 0.1.100')
math_dep = meson.get_compiler('c').find_library('m', required: false)

# GEGL loads user operations from $XDG_DATA_HOME/gegl-0.4/plug-ins, which is
# ~/.local/share/gegl-0.4/plug-ins, or for the Flatpak version of GIMP
# ~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins.
moduledir = get_option('moduledir')
if moduledir == ''
  moduledir = gegl_dep.get_variable(pkgconfig: 'pluginsdir')
endif

# each operation is a module of its own
operations = {
  'clarity': ['operations/clarity.c'],
  'texture': ['operations/texture.c'],
  'dehaze': ['operations/dehaze.c'],
  'whites-blacks': ['operations/whites-blacks.c'],
}

foreach op, sources : operations
  shared_module(op,
    sources,
    name_prefix: '',
    include_directories: 'operations',
    dependencies: [gegl_dep, babl_dep, math_dep],
    c_args: ['-DGETTEXT_PACKAGE="gegl-presence"'],
    install: true,
    install_dir: moduledir,
  )
endforeach

test_c_args = ['-DGEGL_PLUGINSDIR="@0@"'.format(
  gegl_dep.get_variable(pkgconfig: 'pluginsdir'))]

# numerical checks of the operations: tests/check.sh, or meson test -C build
check = executable('check', 'tests/check.c',
  dependencies: [gegl_dep, babl_dep, math_dep],
  c_args: test_c_args,
  install: false,
)
test('operations', check, args: [meson.current_build_dir()], timeout: 1800)


# how long the operations take on a 24 megapixel image: tests/bench.sh
executable('bench', 'tests/bench.c',
  dependencies: [gegl_dep, babl_dep, math_dep],
  c_args: test_c_args,
  install: false,
)
