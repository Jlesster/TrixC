{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland,
  wayland-protocols,
  wayland-scanner,
  wlroots_0_18, # trixie targets wlroots 0.18.x
  libdrm,
  libxkbcommon,
  libinput,
  pixman,
  mesa, # provides libEGL + libGLESv2
  freetype,
  harfbuzz,
  fontconfig,
  luajit,
  xwayland,
  xcbutilwm, # xcb-icccm, needed by wlroots xwayland
  libGL,
  # Optional — set to null to disable
  withXwayland ? true,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "trixie";
  version = "0.5.0";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    wayland
    wayland-protocols
    wlroots_0_18
    libdrm
    libxkbcommon
    libinput
    pixman
    mesa # EGL + GLES2
    libGL
    freetype
    harfbuzz
    fontconfig
    luajit
  ]
  ++ lib.optionals withXwayland [
    xwayland
    xcbutilwm
  ];

  mesonFlags = [
    (lib.mesonBool "xwayland" withXwayland)
  ];

  # wlroots 0.18 exposes wlr_gles2_renderer_get_buffer_fbo — enable the
  # saturation shader path.
  env.NIX_CFLAGS_COMPILE = "-DHAVE_WLR_GLES2_FBO";

  postInstall = ''
    # Install the trixiectl IPC client alongside the compositor
    install -Dm755 trixiectl $out/bin/trixiectl
  '';

  meta = {
    description = "wlroots Wayland compositor with LuaJIT scripting";
    homepage = "https://github.com/yourname/trixie";
    license = lib.licenses.mit;
    maintainers = [ ];
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
    mainProgram = "trixie";
  };
})
