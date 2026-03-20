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

    greetd = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Configure greetd as the display manager with tuigreet launching Trixie.
        Requires pkgs.greetd and pkgs.greetd.tuigreet to be available.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    # ── Package available system-wide ────────────────────────────────────────
    environment.systemPackages = [ cfg.package ] ++ cfg.extraPackages;

    # ── Wayland session entry ────────────────────────────────────────────────
    services.displayManager.sessionPackages = [ cfg.package ];

    # ── greetd (optional) ────────────────────────────────────────────────────
    services.greetd = lib.mkIf cfg.greetd {
      enable = true;
      settings = {
        default_session = {
          command = "${pkgs.greetd.tuigreet}/bin/tuigreet --time --cmd trixie";
          user = "greeter";
        };
      };
    };

    # ── XWayland ─────────────────────────────────────────────────────────────
    programs.xwayland.enable = lib.mkIf cfg.xwayland.enable true;

    # ── XDG portals ──────────────────────────────────────────────────────────
    xdg.portal = {
      enable = true;
      wlr.enable = true;
      extraPortals = [ pkgs.xdg-desktop-portal-gtk ];
      # XDG_CURRENT_DESKTOP is set to "trixie:wlroots" by main.c.
      config = {
        common.default = [
          "wlr"
          "gtk"
        ];
        trixie = {
          default = [
            "wlr"
            "gtk"
          ];
          "org.freedesktop.impl.portal.FileChooser" = [ "gtk" ];
          "org.freedesktop.impl.portal.Screenshot" = [ "wlr" ];
          "org.freedesktop.impl.portal.ScreenCast" = [ "wlr" ];
        };
      };
    };

    # ── PAM services ─────────────────────────────────────────────────────────
    # trixie itself (for polkit / privilege escalation within the session)
    security.pam.services.trixie = { };
    # Screen lockers — swaylock and hyprlock both require a PAM service entry
    # matching their binary name. Without these, password auth silently fails.
    security.pam.services.swaylock = { };
    security.pam.services.hyprlock = { };

    # ── Polkit ───────────────────────────────────────────────────────────────
    security.polkit.enable = true;

    # ── dbus ─────────────────────────────────────────────────────────────────
    services.dbus.enable = true;

    # ── udev: DRM/KMS devices and input ──────────────────────────────────────
    services.udev.packages = [ pkgs.libinput ];

    # Grant the user access to /dev/dri/* (DRM) and /dev/input/*
    # video/input group membership is the canonical NixOS approach.
    users.groups.video = { };
    users.groups.input = { };

    # ── systemd graphical session target ─────────────────────────────────────
    # Trixie sets WAYLAND_DISPLAY and XDG_SESSION_TYPE before calling
    # wl_display_run. Importing them into the systemd --user session enables
    # xdg-desktop-portal, pipewire-media-session, and other session services
    # that gate on ConditionEnvironment=WAYLAND_DISPLAY.
    systemd.user.targets.trixie-session = {
      description = "Trixie compositor session";
      bindsTo = [ "graphical-session.target" ];
      wants = [ "graphical-session-pre.target" ];
      after = [ "graphical-session-pre.target" ];
    };

    # ── RTKit for low-latency audio (pipewire) ────────────────────────────────
    security.rtkit.enable = true;
  };
}
