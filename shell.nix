{
  pkgs ? import <nixpkgs> { },
}:

let
  wlroots =
    if pkgs ? wlroots_0_18 then
      pkgs.wlroots_0_18
    else if pkgs ? wlroots then
      pkgs.wlroots
    else
      throw "No wlroots found in nixpkgs";
in
pkgs.mkShell {
  name = "trixie-dev";

  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    wayland-scanner
    gcc
    python3 # needed by fix2.sh
  ];

  buildInputs = with pkgs; [
    # Wayland
    wayland
    wayland-protocols
    libxkbcommon

    # wlroots
    wlroots

    # Input / hardware
    libinput
    libevdev
    eudev

    # Graphics — mesa provides GLES2/gl2.h, EGL, etc.
    mesa
    libdrm
    pixman

    # Font
    freetype
    harfbuzz
    fontconfig

    # Lua
    luajit

    # XWayland
    xwayland
    libxcb
    xcbutilwm

    # LSP
    clang-tools
  ];

  shellHook = ''
    echo "=== Trixie dev shell ==="
    for pkg in wlroots-0.18 wlroots wayland-server luajit lua5.1 freetype2 harfbuzz; do
      ver=$(pkg-config --modversion $pkg 2>/dev/null)
      if [ -n "$ver" ]; then
        echo "  $pkg: $ver"
      else
        echo "  $pkg: NOT FOUND"
      fi
    done

    # Verify GLES2 header is reachable
    if [ -f "$(nix-store -r $(nix eval --raw nixpkgs#mesa.dev 2>/dev/null) 2>/dev/null)/include/GLES2/gl2.h" ] 2>/dev/null; then
      echo "  GLES2/gl2.h: OK"
    else
      echo "  GLES2/gl2.h: check mesa.dev"
    fi

    echo ""
    echo "Run: rm -rf build && meson setup build && ninja -C build"
  '';
}
