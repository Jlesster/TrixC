{
  description = "Trixie — wlroots Wayland compositor with LuaJIT scripting";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      home-manager,
      ...
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;

      pkgsFor =
        system:
        import nixpkgs {
          inherit system;
          overlays = [ self.overlays.default ];
        };
    in
    {
      # ── Overlay ─────────────────────────────────────────────────────────────
      overlays.default = final: prev: {
        trixie = final.callPackage ./nix/package.nix {
          inherit (final) grim slurp wl-clipboard;
        };
      };

      # ── Package ─────────────────────────────────────────────────────────────
      packages = forAllSystems (system: rec {
        trixie = (pkgsFor system).trixie;
        default = trixie;
      });

      # ── Dev shell ────────────────────────────────────────────────────────────
      devShells = forAllSystems (
        system:
        let
          pkgs = pkgsFor system;
        in
        {
          default = pkgs.mkShell {
            name = "trixie-dev";
            inputsFrom = [ pkgs.trixie ];
            packages = with pkgs; [
              # Debugging / tooling
              gdb
              valgrind
              clang-tools # clangd, clang-format
              bear # compile_commands.json for clangd
              lua-language-server
              # Runtime tools for quick testing
              foot
              grim
              slurp
              wl-clipboard
            ];
            shellHook = ''
              echo "trixie dev shell"
              echo "  ninja -C build        — rebuild"
              echo "  bear -- ninja -C build — regenerate compile_commands.json"
            '';
          };
        }
      );

      # ── NixOS module ─────────────────────────────────────────────────────────
      nixosModules.default = import ./nix/nixos-module.nix { inherit self; };
      nixosModules.trixie = self.nixosModules.default;

      # ── Home Manager module ──────────────────────────────────────────────────
      homeManagerModules.default = import ./nix/hm-module.nix { inherit self; };
      homeManagerModules.trixie = self.homeManagerModules.default;

      # ── Formatter ────────────────────────────────────────────────────────────
      formatter = forAllSystems (system: (pkgsFor system).nixfmt-rfc-style);
    };
}
