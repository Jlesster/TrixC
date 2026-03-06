project('trixie', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ]
)

cc = meson.get_compiler('c')

# ── Dependencies ──────────────────────────────────────────────────────────────

wlroots     = dependency('wlroots-0.18', required : true)
wayland_srv = dependency('wayland-server', required : true)
xkbcommon   = dependency('xkbcommon', required : true)
pixman      = dependency('pixman-1', required : true)
libm        = cc.find_library('m', required : true)

# ── Wayland protocol headers ───────────────────────────────────────────────

wayland_scanner   = find_program('wayland-scanner', required : true)
wayland_protocols = dependency('wayland-protocols', required : true)
wp_dir = wayland_protocols.get_variable('pkgdatadir')

# xdg-shell (from wayland-protocols)
xdg_shell_header = custom_target(
  'xdg-shell-protocol-h',
  input  : wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  output : 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

# wlr-layer-shell (from wlroots)
wlroots_pkgdatadir = wlroots.get_variable('pkgdatadir', default_value : '')
if wlroots_pkgdatadir != ''
  layer_shell_xml = wlroots_pkgdatadir / 'wlr-layer-shell-unstable-v1.xml'
else
  layer_shell_xml = meson.current_source_dir() / 'wlr-layer-shell-unstable-v1.xml'
endif

layer_shell_header = custom_target(
  'wlr-layer-shell-unstable-v1-protocol-h',
  input  : layer_shell_xml,
  output : 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

proto_headers = [xdg_shell_header, layer_shell_header]

# ── Sources ───────────────────────────────────────────────────────────────────

sources = files(
  'main.c',
  'twm.c',
  'layout.c',
  'anim.c',
  'config.c',
  'bar.c',
  'deco.c',
  'ipc.c',
)

# ── Compile flags ─────────────────────────────────────────────────────────────

add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-DWLR_USE_UNSTABLE',
  language : 'c',
)

# ── Executable ────────────────────────────────────────────────────────────────

executable('trixie',
  sources + [proto_headers],
  dependencies : [wlroots, wayland_srv, xkbcommon, pixman, libm],
  include_directories : include_directories('.'),
  install : true,
)

# ── trixiectl (IPC client) ────────────────────────────────────────────────────

executable('trixiectl',
  'trixiectl.c',
  install : true,
)
