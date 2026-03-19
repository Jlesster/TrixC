{ self }:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.programs.trixie;
in
{
  options.programs.trixie = {
    enable = lib.mkEnableOption "Trixie Wayland compositor";

    package = lib.mkPackageOption pkgs "trixie" {
      default = [ "trixie" ];
      extraDescription = ''
        Override to use a locally-built version, e.g.
        `self.packages.''${pkgs.system}.trixie`.
      '';
    };

    # Runtime tools that trixie.spawn() / exec_once() reference.
    # These are added to the system PATH so the compositor can find them
    # regardless of whether the user has them in their own profile.
    extraPackages = lib.mkOption {
      type = lib.types.listOf lib.types.package;
      default = with pkgs; [
        grim
        slurp
        wl-clipboard # wl-copy used by screenshot.c
        pamixer
        playerctl
        brightnessctl
        dunst
        swaybg
        wl-clipboard # wl-paste for cliphist
        cliphist
        xdg-desktop-portal
        xdg-desktop-portal-wlr
        polkit_gnome
      ];
      description = ''
        Extra packages made available on PATH when running under Trixie.
        These cover the defaults in modules/autostart.lua and modules/binds.lua.
        Add your own tools here or replace the list entirely.
      '';
    };

    xwayland.enable = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = "Enable XWayland support for running X11 applications.";
    };
  };

  config = lib.mkIf cfg.enable {
    # ── Package available system-wide ────────────────────────────────────────
    environment.systemPackages = [ cfg.package ] ++ cfg.extraPackages;

    # ── Wayland session entry ────────────────────────────────────────────────
    # Registers trixie in the display manager session list
    # (GDM, SDDM, greetd, etc.)
    services.displayManager.sessionPackages = [ cfg.package ];

    # ── XWayland ─────────────────────────────────────────────────────────────
    programs.xwayland.enable = lib.mkIf cfg.xwayland.enable true;

    # ── XDG portals ──────────────────────────────────────────────────────────
    xdg.portal = {
      enable = true;
      wlr.enable = true;
      extraPortals = [ pkgs.xdg-desktop-portal-gtk ];
      config.trixie = {
        default = [
          "wlr"
          "gtk"
        ];
        "org.freedesktop.impl.portal.FileChooser" = [ "gtk" ];
      };
    };

    # ── Security / PAM for screen lockers ────────────────────────────────────
    security.pam.services.trixie = { };

    # ── Polkit ───────────────────────────────────────────────────────────────
    security.polkit.enable = true;

    # ── dbus (required for portals and many desktop services) ────────────────
    services.dbus.enable = true;

    # ── udev rules for DRM / input (already handled by systemd-logind
    #    when seat0 is active, but explicit here for clarity)
    services.udev.packages = [ pkgs.libinput ];

    # ── Environment variables set for every Trixie session ──────────────────
    # Written into /etc/environment so greetd / getty sessions see them.
    # Most of these are also set by trixie's own main.c setenv() calls,
    # but having them in PAM env means child processes inherit them correctly
    # even before the compositor's setenv() runs.
    environment.sessionVariables = {
      XDG_CURRENT_DESKTOP = "trixie:wlroots";
      XDG_SESSION_TYPE = "wayland";
      QT_QPA_PLATFORM = "wayland;xcb";
      QT_WAYLAND_DISABLE_WINDOWDECORATION = "1";
      GDK_BACKEND = "wayland,x11";
      MOZ_ENABLE_WAYLAND = "1";
      CLUTTER_BACKEND = "wayland";
      EGL_PLATFORM = "wayland";
    };
  };
}
