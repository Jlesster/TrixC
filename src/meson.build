project(
  'trixie',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

# ── Dependencies ──────────────────────────────────────────────────────────────

# Accept wlroots 0.18 or 0.17
wlroots = dependency('wlroots-0.18', required: false)
if not wlroots.found()
  wlroots = dependency('wlroots-0.17', required: true)
endif

wayland_srv = dependency('wayland-server', required: true)
xkbcommon = dependency('xkbcommon', required: true)
pixman = dependency('pixman-1', required: true)
freetype = dependency('freetype2', required: true)
harfbuzz = dependency('harfbuzz', required: true)
fontconfig = dependency('fontconfig', required: true)
libm = cc.find_library('m', required: true)

# ── Wayland protocol headers ───────────────────────────────────────────────

wayland_scanner = find_program('wayland-scanner', required: true)
wayland_protocols = dependency('wayland-protocols', required: true)
wp_dir = wayland_protocols.get_variable('pkgdatadir')

xdg_shell_header = custom_target(
  'xdg-shell-protocol-h',
  input: wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

wlroots_pkgdatadir = wlroots.get_variable('pkgdatadir', default_value: '')
if wlroots_pkgdatadir != ''
  layer_shell_xml = wlroots_pkgdatadir / 'wlr-layer-shell-unstable-v1.xml'
else
  layer_shell_xml = meson.current_source_dir() / 'wlr-layer-shell-unstable-v1.xml'
endif

layer_shell_header = custom_target(
  'wlr-layer-shell-unstable-v1-protocol-h',
  input: layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

proto_headers = [xdg_shell_header, layer_shell_header]

# ── Sources ───────────────────────────────────────────────────────────────────

sources = files(
  'anim.c',
  'bar.c',
  'config.c',
  'deco.c',
  'ipc.c',
  'layout.c',
  'main.c',
  'twm.c',
)

# ── Compile flags ─────────────────────────────────────────────────────────────

add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-DWLR_USE_UNSTABLE',
  language: 'c',
)

# ── Executable ────────────────────────────────────────────────────────────────

executable(
  'trixie',
  sources + [proto_headers],
  dependencies: [
    wlroots,
    wayland_srv,
    xkbcommon,
    pixman,
    freetype,
    harfbuzz,
    fontconfig,
    libm,
  ],
  include_directories: include_directories('.'),
  install: true,
)

# ── trixiectl (IPC client) ────────────────────────────────────────────────────

executable(
  'trixiectl',
  'trixiectl.c',
  install: true,
)
