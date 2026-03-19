{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland,
  wayland-protocols,
  wayland-scanner,
  wlroots_0_18,
  libdrm,
  libxkbcommon,
  libinput,
  pixman,
  mesa,
  libglvnd,
  freetype,
  harfbuzz,
  fontconfig,
  luajit,
  xwayland,
  xcbutilwm,
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
    wayland-protocols # needed for linux-dmabuf-unstable-v1, alpha-modifier, etc.
  ];

  buildInputs = [
    wayland
    wayland-protocols
    wlroots_0_18
    libdrm
    libxkbcommon
    libinput
    pixman
    mesa
    libglvnd.dev # provides egl.pc and glesv2.pc
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
    "--wipe"
    # Enable wlr_gles2_renderer_get_buffer_fbo (added in wlroots 0.18.2).
    # The shader.c code guards on HAVE_WLR_GLES2_FBO; meson.build should set it.
    "-Dwlr_gles2_fbo=enabled"
  ];

  postInstall = ''
    install -Dm755 trixiectl $out/bin/trixiectl
  '';

  passthru.providedSessions = [ "Trixie" ];

  meta = {
    description = "wlroots Wayland compositor with LuaJIT scripting";
    homepage = "https://github.com/Jlesster/TrixC";
    license = lib.licenses.mit;
    maintainers = [ ];
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
    mainProgram = "trixie";
  };
})
