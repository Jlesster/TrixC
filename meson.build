project('trixie', 'c',
  version: '0.4.0',
  default_options: [
    'c_std=c11',
    'warning_level=1',
    'werror=false',
    'optimization=2',
  ]
)

cc = meson.get_compiler('c')

# ── Dependencies ───────────────────────────────────────────────────────────────
wlroots = dependency('wlroots-0.18', required: false)
if not wlroots.found()
  wlroots = dependency('wlroots', required: true)
endif

wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
wayland_p      = dependency('wayland-protocols')
xkbcommon      = dependency('xkbcommon')
libinput       = dependency('libinput')
pixman         = dependency('pixman-1')
drm            = dependency('libdrm')
freetype       = dependency('freetype2')
harfbuzz       = dependency('harfbuzz')
fontconf       = dependency('fontconfig')
threads        = dependency('threads')
mathlib        = cc.find_library('m')

lua = dependency('luajit', required: false)
if not lua.found()
  lua = dependency('lua5.1', required: false)
endif
if not lua.found()
  lua = dependency('lua', required: true)
endif

xwayland_dep  = dependency('xwayland', required: false)
xcb_dep       = dependency('xcb',      required: false)
have_xwayland = xwayland_dep.found() and xcb_dep.found()

# ── GLES2 / EGL — NixOS libglvnd path detection ───────────────────────────────
# libglvnd on NixOS has no pkgconfig. Find the header and lib independently,
# then wire up -I / -L / -l flags directly on the executable target.

find_gles_h = run_command('bash', '-c',
  'find /nix/store -maxdepth 5 -name "gl2.h" -path "*/GLES2/gl2.h" 2>/dev/null | head -1 || true',
  check: false)
gles_header = find_gles_h.stdout().strip()

find_gles_so = run_command('bash', '-c',
  # Prefer the libglvnd whose store hash matches the -dev output we found
  # headers in. Strip "-dev" from the header store path to get the lib path.
  'h=' + gles_header + '; ' +
  'dev_dir=$(dirname $(dirname $(dirname $h))); ' +
  'lib_dir=${dev_dir%-dev}; ' +
  'so=$lib_dir/lib/libGLESv2.so; ' +
  'if [ -f "$so" ]; then echo $so; ' +
  'else find /nix/store -maxdepth 4 -name "libGLESv2.so" -not -path "*/electron*" 2>/dev/null | head -1; ' +
  'fi',
  check: false)
gles_so = find_gles_so.stdout().strip()

mesa_gl_inc  = []
mesa_gl_link = []

if gles_header != ''
  inc_dir = run_command('bash', '-c',
    'dirname $(dirname ' + gles_header + ')',
    check: true).stdout().strip()
  mesa_gl_inc = ['-I' + inc_dir]
  message('GLES2 headers: ' + inc_dir)
else
  message('WARNING: GLES2/gl2.h not found — shader will not compile')
endif

if gles_so != ''
  lib_dir = run_command('bash', '-c',
    'dirname ' + gles_so,
    check: true).stdout().strip()
  mesa_gl_link = ['-L' + lib_dir, '-lGLESv2', '-lEGL']
  message('GLES2 libs:    ' + lib_dir)
else
  message('WARNING: libGLESv2.so not found — linker will likely fail')
endif

# ── Wayland scanner ────────────────────────────────────────────────────────────
wayland_scanner = find_program('wayland-scanner')
wp_dir     = wayland_p.get_variable('pkgdatadir')
proto_dir  = meson.project_source_root() / 'include/protocols'
fs         = import('fs')
proto_headers = []

# ── Stable protocols ───────────────────────────────────────────────────────────
foreach proto : ['presentation-time', 'viewporter', 'xdg-shell']
  xml = proto_dir / proto + '.xml'
  if not fs.exists(xml)
    xml = wp_dir / 'stable' / proto / proto + '.xml'
  endif
  proto_headers += custom_target(proto + '-protocol.h',
    output:  proto + '-protocol.h',
    input:   xml,
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# ── Staging protocols ──────────────────────────────────────────────────────────
staging_protos = [
  ['content-type',     'v1'],
  ['cursor-shape',     'v1'],
  ['fractional-scale', 'v1'],
  ['tearing-control',  'v1'],
  ['xdg-activation',   'v1'],
]
foreach entry : staging_protos
  proto   = entry[0]
  version = entry[1]
  name    = proto + '-' + version
  xml = proto_dir / name + '.xml'
  if not fs.exists(xml)
    xml = wp_dir / 'staging' / proto / name + '.xml'
  endif
  proto_headers += custom_target(name + '-protocol.h',
    output:  name + '-protocol.h',
    input:   xml,
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# ── Unstable protocols ─────────────────────────────────────────────────────────
unstable_protos = [
  ['idle-inhibit',        'v1'],
  ['pointer-constraints', 'v1'],
  ['pointer-gestures',    'v1'],
  ['primary-selection',   'v1'],
  ['relative-pointer',    'v1'],
  ['xdg-decoration',      'v1'],
  ['xdg-output',          'v1'],
]
foreach entry : unstable_protos
  proto   = entry[0]
  version = entry[1]
  name    = proto + '-unstable-' + version
  xml = proto_dir / name + '.xml'
  if not fs.exists(xml)
    xml = wp_dir / 'unstable' / proto / name + '.xml'
  endif
  proto_headers += custom_target(name + '-protocol.h',
    output:  name + '-protocol.h',
    input:   xml,
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# ── wlr protocols ─────────────────────────────────────────────────────────────
wlr_protos = [
  'wlr-layer-shell-unstable-v1',
  'wlr-output-power-management-unstable-v1',
]
wlroots_prefix = wlroots.get_variable(pkgconfig: 'prefix', default_value: '/usr')
foreach name : wlr_protos
  xml = proto_dir / name + '.xml'
  if not fs.exists(xml)
    candidates = [
      wlroots_prefix / 'share/wlroots/protocol' / name + '.xml',
      wlroots_prefix / 'share/wlroots-0.18/protocol' / name + '.xml',
      '/run/current-system/sw/share/wlroots/protocol' / name + '.xml',
    ]
    found = ''
    foreach c : candidates
      if found == '' and fs.exists(c)
        found = c
      endif
    endforeach
    if found != ''
      xml = found
    else
      warning('Protocol XML not found for ' + name + ' — build may fail')
    endif
  endif
  proto_headers += custom_target(name + '-protocol.h',
    output:  name + '-protocol.h',
    input:   xml,
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# ── Compiler flags ─────────────────────────────────────────────────────────────
add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-DWLR_USE_UNSTABLE',
  '-Wno-unused-parameter',
  '-Wno-builtin-macro-redefined',
  language: 'c',
)

if have_xwayland
  add_project_arguments('-DHAS_XWAYLAND', language: 'c')
  message('XWayland: enabled')
else
  message('XWayland: disabled')
endif

if cc.has_function('wlr_gles2_renderer_get_buffer_fbo', dependencies: [wlroots])
  add_project_arguments('-DHAVE_WLR_GLES2_FBO', language: 'c')
  message('shader: wlr_gles2_renderer_get_buffer_fbo found — saturation enabled')
else
  message('shader: wlr_gles2_renderer_get_buffer_fbo not found — saturation disabled')
endif

# ── Sources ────────────────────────────────────────────────────────────────────
trixie_src = [
  'src/reload.c',
  'src/lua_reload_bindings.c',
  'src/anim.c',
  'src/bar.c',
  'src/config.c',
  'src/deco.c',
  'src/gesture.c',
  'src/ipc.c',
  'src/layout.c',
  'src/lua.c',
  'src/main.c',
  'src/screenshot.c',
  'src/shader.c',
  'src/twm.c',
] + proto_headers

trixie_deps = [
  wlroots, wayland_server, xkbcommon, libinput,
  pixman, drm, freetype, harfbuzz, fontconf,
  lua, mathlib, threads,
]
if have_xwayland
  trixie_deps += [xwayland_dep, xcb_dep]
endif

executable('trixie',
  trixie_src,
  dependencies: trixie_deps,
  c_args:    mesa_gl_inc,
  link_args: mesa_gl_link,
  include_directories: [
    include_directories('include'),
    include_directories('src'),
    include_directories('.'),
  ],
  install: true,
)

executable('trixiectl',
  'src/trixiectl.c',
  install: true,
)
