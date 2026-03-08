project(
  'Trixie',
  'c',
  version: '0.1.0',
  default_options: [
    'warning_level=3',
    'c_std=c11',
    'buildtype=debugoptimized',
  ],
)

# ── Sources ──────────────────────────────────────────────────────────────────
src = files(
  'src/anim.c',
  'src/bar.c',
  'src/bar_worker.c',
  'src/config.c',
  'src/deco.c',
  'src/files_panel.c',
  'src/gesture.c',
  'src/ipc.c',
  'src/layout.c',
  'src/lsp_panel.c',
  'src/lua.c',
  'src/main.c',
  'src/nvim_panel.c',
  'src/overlay.c',
  'src/run_panel.c',
  'src/screenshot.c',
  'src/twm.c',
)

# ── Include directories ──────────────────────────────────────────────────────
inc = include_directories('include', 'include/protocols')

# ── Dependencies ─────────────────────────────────────────────────────────────
wayland_protocols_dep = dependency('wayland-protocols', required: true)
wp_dir = wayland_protocols_dep.get_variable('pkgdatadir')
libinput_dep = dependency('libinput', required: true)
history_dep = dependency('history', required: true)
lua_dep = dependency('luajit', required: true)
libdrm_dep = dependency('libdrm', required: true)
pixman_1_dep = dependency('pixman-1', required: true)
fontconfig_dep = dependency('fontconfig', required: true)
harfbuzz_gobject_dep = dependency('harfbuzz-gobject', required: true)
spandsp_dep = dependency('spandsp', required: true)
wlroots_0_18_dep = dependency('wlroots-0.18', required: true)
jack_dep = dependency('jack', required: true)
json_c_dep = dependency('json-c', required: true)
girara_dep = dependency('girara', required: true)
wayland_server_dep = dependency('wayland-server', required: true)
xkbcommon_dep = dependency('xkbcommon', required: true)
freetype_dep = dependency('freetype2')
harfbuzz_dep = dependency('harfbuzz')
# ── Wayland protocol generation ──────────────────────────────────────────────
wayland_scanner = find_program('wayland-scanner')

protocol_src = []
xdg_shell_h = custom_target(
  'xdg-shell-client-header',
  input: wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_c = custom_target(
  'xdg-shell-private-code',
  input: wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [xdg_shell_h, xdg_shell_c]

wlr_layer_shell_unstable_v1_h = custom_target(
  'wlr-layer-shell-unstable-v1-client-header',
  input: files('include/protocols/wlr-layer-shell-unstable-v1.xml'),
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
wlr_layer_shell_unstable_v1_c = custom_target(
  'wlr-layer-shell-unstable-v1-private-code',
  input: files('include/protocols/wlr-layer-shell-unstable-v1.xml'),
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [wlr_layer_shell_unstable_v1_h, wlr_layer_shell_unstable_v1_c]

wlr_output_power_management_unstable_v1_h = custom_target(
  'wlr-output-power-management-unstable-v1-client-header',
  input: files('include/protocols/wlr-output-power-management-unstable-v1.xml'),
  output: 'wlr-output-power-management-unstable-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
wlr_output_power_management_unstable_v1_c = custom_target(
  'wlr-output-power-management-unstable-v1-private-code',
  input: files('include/protocols/wlr-output-power-management-unstable-v1.xml'),
  output: 'wlr-output-power-management-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [
  wlr_output_power_management_unstable_v1_h,
  wlr_output_power_management_unstable_v1_c,
]

tablet_v2_h = custom_target(
  'tablet-v2-client-header',
  input: wp_dir / 'stable/tablet/tablet-v2.xml',
  output: 'tablet-v2-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
tablet_v2_c = custom_target(
  'tablet-v2-private-code',
  input: wp_dir / 'stable/tablet/tablet-v2.xml',
  output: 'tablet-v2-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [tablet_v2_h, tablet_v2_c]

content_type_v1_h = custom_target(
  'content-type-v1-client-header',
  input: wp_dir / 'staging/content-type/content-type-v1.xml',
  output: 'content-type-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
content_type_v1_c = custom_target(
  'content-type-v1-private-code',
  input: wp_dir / 'staging/content-type/content-type-v1.xml',
  output: 'content-type-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [content_type_v1_h, content_type_v1_c]

cursor_shape_v1_h = custom_target(
  'cursor-shape-v1-client-header',
  input: wp_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  output: 'cursor-shape-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
cursor_shape_v1_c = custom_target(
  'cursor-shape-v1-private-code',
  input: wp_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  output: 'cursor-shape-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [cursor_shape_v1_h, cursor_shape_v1_c]

tearing_control_v1_h = custom_target(
  'tearing-control-v1-client-header',
  input: wp_dir / 'staging/tearing-control/tearing-control-v1.xml',
  output: 'tearing-control-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
tearing_control_v1_c = custom_target(
  'tearing-control-v1-private-code',
  input: wp_dir / 'staging/tearing-control/tearing-control-v1.xml',
  output: 'tearing-control-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [tearing_control_v1_h, tearing_control_v1_c]

fullscreen_shell_unstable_v1_h = custom_target(
  'fullscreen-shell-unstable-v1-client-header',
  input: wp_dir / 'unstable/fullscreen-shell/fullscreen-shell-unstable-v1.xml',
  output: 'fullscreen-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
fullscreen_shell_unstable_v1_c = custom_target(
  'fullscreen-shell-unstable-v1-private-code',
  input: wp_dir / 'unstable/fullscreen-shell/fullscreen-shell-unstable-v1.xml',
  output: 'fullscreen-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [fullscreen_shell_unstable_v1_h, fullscreen_shell_unstable_v1_c]

pointer_constraints_unstable_v1_h = custom_target(
  'pointer-constraints-unstable-v1-client-header',
  input: wp_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  output: 'pointer-constraints-unstable-v1-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
pointer_constraints_unstable_v1_c = custom_target(
  'pointer-constraints-unstable-v1-private-code',
  input: wp_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  output: 'pointer-constraints-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
protocol_src += [pointer_constraints_unstable_v1_h, pointer_constraints_unstable_v1_c]

# ── Executable ───────────────────────────────────────────────────────────────
exe = executable(
  'Trixie',
  src + protocol_src,
  include_directories: inc,
  dependencies: [
    history_dep,
    libdrm_dep,
    pixman_1_dep,
    fontconfig_dep,
    harfbuzz_gobject_dep,
    spandsp_dep,
    wlroots_0_18_dep,
    jack_dep,
    json_c_dep,
    girara_dep,
    wayland_server_dep,
    xkbcommon_dep,
    freetype_dep,
    lua_dep,
    harfbuzz_dep,
    libinput_dep,
  ],
  c_args: ['-DWLR_USE_UNSTABLE', '-D_POSIX_C_SOURCE=200809L'],
  link_args: ['-Wl,--as-needed'],
  install: false,
)
executable(
  'trixiectl',
  sources: ['src/trixiectl.c'],
  install: true,
  # no special deps needed — just libc
)
