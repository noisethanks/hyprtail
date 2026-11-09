{
  description = "Cursor trail plugin for Hyprland";

  inputs = {
    hyprland.url = "github:hyprwm/Hyprland/v0.56.2";
    nixpkgs.follows = "hyprland/nixpkgs";
    systems.follows = "hyprland/systems";
  };

  outputs =
    {
      self,
      hyprland,
      nixpkgs,
      systems,
      ...
    }:
    let
      inherit (nixpkgs) lib;
      eachSystem = lib.genAttrs (import systems);

      pkgsFor = eachSystem (
        system:
        import nixpkgs {
          localSystem.system = system;
          overlays = [
            self.overlays.default
            hyprland.overlays.hyprland-packages
          ];
        }
      );
    in
    {
      overlays.default = final: prev: {
        hyprlandPlugins = (prev.hyprlandPlugins or { }) // {
          hyprtail = final.hyprlandPlugins.mkHyprlandPlugin {
            pluginName = "hyprtail";
            version = lib.fileContents ./VERSION;
            src = ./.;

            # The sandbox has no .git for the Makefile to read the revision from.
            env.HYPRTAIL_REV = self.shortRev or self.dirtyShortRev or "unknown";

            installPhase = ''
              runHook preInstall
              install -Dm755 out/hyprtail.so $out/lib/libhyprtail.so
              runHook postInstall
            '';

            meta = {
              homepage = "https://github.com/noisethanks/hyprtail";
              description = "Cursor trail with a user-programmable ribbon shader";
              license = lib.licenses.bsd3;
              platforms = lib.platforms.linux;
              maintainers = [
                {
                  name = "noisethanks";
                  github = "noisethanks";
                }
              ];
            };
          };
        };
      };

      packages = eachSystem (system: {
        inherit (pkgsFor.${system}.hyprlandPlugins) hyprtail;
        default = self.packages.${system}.hyprtail;
      });

      checks = eachSystem (
        system:
        let
          pkgs = pkgsFor.${system};
        in
        {
          inherit (self.packages.${system}) hyprtail;

          # The host-free tests (`make test-unit`, `make test-compat`) on the
          # plugin's own build, then a check that the built .so embeds every
          # preset. Never loads Hyprland.
          unit = self.packages.${system}.hyprtail.overrideAttrs (old: {
            pname = "hyprtail-unit";
            doCheck = true;
            nativeBuildInputs = (old.nativeBuildInputs or [ ]) ++ [
              pkgs.glslang
              pkgs.pkg-config
            ];
            checkPhase = ''
              runHook preCheck
              make test-unit test-compat
              for conf in hyprtail/presets/*.conf; do
                want="$(sed -n 's/^description = //p' "$conf")"
                if [ -z "$want" ] || ! grep -qaF -- "$want" out/hyprtail.so; then
                  echo "error: out/hyprtail.so does not embed $conf (description '$want')" >&2
                  exit 1
                fi
              done
              runHook postCheck
            '';
          });
        }
      );

      # CI only: the VM smoke test (needs KVM, kept out of `checks`) and the
      # Hyprland the plugin is built against.
      legacyPackages = eachSystem (system: {
        smoke = import ./nix/smoke.nix {
          pkgs = pkgsFor.${system};
          hyprland = hyprland.packages.${system}.hyprland-with-tests;
          hyprtail = self.packages.${system}.hyprtail;
        };
        inherit (pkgsFor.${system}) hyprland;
      });

      devShells = eachSystem (
        system:
        let
          pkgs = pkgsFor.${system};
        in
        {
          ci = pkgs.mkShell.override { inherit (pkgs.hyprland) stdenv; } {
            name = "hyprtail-ci";
            inputsFrom = [ pkgs.hyprlandPlugins.hyprtail ];
            packages = [
              pkgs.pkg-config
              pkgs.glslang
            ];
          };
        }
      );
    };
}
