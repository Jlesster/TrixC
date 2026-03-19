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
  freetype,
  harfbuzz,
  fontconfig,
  luajit,
  xwayland,
  xcbutilwm,
  libGL,
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
    mesa
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

  env.NIX_CFLAGS_COMPILE = "-DHAVE_WLR_GLES2_FBO";

  postInstall = ''
    install -Dm755 trixiectl $out/bin/trixiectl
  '';

  # ── Required by services.displayManager.sessionPackages ─────────────────
  # NixOS reads this to know what session names the package provides.
  # Must match the Name= field in the .desktop file exactly.
  passthru.providedSessions = [ "trixie" ];

  meta = {
    description = "wlroots Wayland compositor with LuaJIT scripting";
    homepage = "https://github.com/Jlesster/TrixC";
    license = lib.licenses.mit;
    maintainers = [ ];
    # Use stdenv.hostPlatform.system instead of the deprecated `system`
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
    mainProgram = "trixie";
  };
})
