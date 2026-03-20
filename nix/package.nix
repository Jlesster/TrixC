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
  makeWrapper,
  grim,
  slurp,
  wl-clipboard,
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
    makeWrapper
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

  # --wipe is only appropriate in the dev shell; it breaks nix build's
  # incremental build tracking and can cause spurious full rebuilds.
  mesonFlags = [ ];

  postInstall = ''
    install -Dm755 trixiectl $out/bin/trixiectl

    # Wrap the trixie binary so that grim/slurp/wl-copy are always on PATH
    # regardless of the user's profile. screenshot.c exec's these directly.
    wrapProgram $out/bin/trixie \
      --prefix PATH : ${
        lib.makeBinPath [
          grim
          slurp
          wl-clipboard
        ]
      }
  ''
  + lib.optionalString withXwayland ''
    # Make xwayland available to the wrapped compositor PATH
    wrapProgram $out/bin/trixie \
      --prefix PATH : ${lib.makeBinPath [ xwayland ]}
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
