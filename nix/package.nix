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
    wayland-protocols
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
