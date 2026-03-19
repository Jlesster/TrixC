project(
  'trixie',
  'c',
  version: '0.5.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
  ],
)

cc = meson.get_compiler('c')

# ── Options ──────────────────────────────────────────────────────────────────

# Defined via meson_options.txt or inline below (meson >= 1.1 supports both)
xwayland_opt = get_option('xwayland')

# ── Dependencies ─────────────────────────────────────────────────────────────

wayland_server  = dependency('wayland-server')
wayland_client  = dependency('wayland-client')
wayland_protos  = dependency('wayland-protocols')
wlroots         = dependency('wlroots-0.18')
libdrm          = dependency('libdrm')
libxkbcommon    = dependency('xkbcommon')
libinput        = dependency('libinput')
pixman          = dependency('pixman-1')
egl             = dependency('egl')
glesv2          = dependency('glesv2')
freetype        = dependency('freetype2')
harfbuzz        = dependency('harfbuzz')
fontconfig      = dependency('fontconfig')
luajit          = dependency('luajit')
math            = cc.find_library('m', required: true)

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
  wayland_scanner_dep.get_variable('wayland_scanner'),
  native: true,
)

xwayland_deps = []
xwayland_cflags = []
if xwayland_opt
  xwayland_dep = dependency('xwayland', required: true)
  xcb_icccm    = dependency('xcb-icccm', required: true)
  xwayland_deps   = [xwayland_dep, xcb_icccm]
  xwayland_cflags = ['-DHAS_XWAYLAND']
endif

# ── Wayland protocol codegen ─────────────────────────────────────────────────

wl_proto_dir = wayland_protos.get_variable('pkgdatadir')

xdg_shell_xml = wl_proto_dir / 'stable/xdg-shell/xdg-shell.xml'

xdg_shell_c = custom_target(
  'xdg-shell-protocol-c',
  input:   xdg_shell_xml,
  output:  'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_h = custom_target(
  'xdg-shell-protocol-h',
  input:   xdg_shell_xml,
  output:  'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

# ── Sources ───────────────────────────────────────────────────────────────────

sources = files(
  'src/anim.c',
  'src/bar.c',
  'src/config.c',
  'src/deco.c',
  'src/gesture.c',
  'src/ipc.c',
  'src/layout.c',
  'src/lua.c',
  'src/lua_reload_bindings.c',
  'src/main.c',
  'src/reload.c',
  'src/screenshot.c',
  'src/shader.c',
  'src/twm.c',
)

# ── Executable ────────────────────────────────────────────────────────────────

executable(
  'trixie',
  sources + [xdg_shell_c, xdg_shell_h],
  dependencies: [
    wayland_server,
    wayland_client,
    wlroots,
    libdrm,
    libxkbcommon,
    libinput,
    pixman,
    egl,
    glesv2,
    freetype,
    harfbuzz,
    fontconfig,
    luajit,
    math,
  ] + xwayland_deps,
  c_args: [
    '-DHAVE_WLR_GLES2_FBO',
    '-D_POSIX_C_SOURCE=200809L',
  ] + xwayland_cflags,
  install: true,
)

# ── trixiectl ─────────────────────────────────────────────────────────────────

executable(
  'trixiectl',
  'trixiectl.c',
  install: true,
)

# ── Desktop entry ─────────────────────────────────────────────────────────────

install_data(
  'data/trixie.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
)
