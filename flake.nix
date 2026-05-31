# Nix flake for lvda.
#
#   nix build .#lvda-ctl                   # userspace CLI
#   nix build .#lvda                       # kernel module (vs linuxPackages_latest)
#
# NixOS users: pull the module + driver via the exported NixOS module:
#
#   inputs.lvda.url = "github:_/lvda";
#   ...
#   imports = [ inputs.lvda.nixosModules.default ];
#   services.lvda.enable = true;
#
# Supporting derivations live in packaging/nix/.
{
  description = "lvda — virtual display driver for game streaming";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forSystem = nixpkgs.lib.genAttrs systems;
      pkgsFor = system: nixpkgs.legacyPackages.${system};
    in {
      packages = forSystem (system:
        let pkgs = pkgsFor system; in {
          lvda-ctl = pkgs.callPackage ./packaging/nix/lvda-ctl.nix {
            src = self;
          };
          # Kernel module needs a kernel. linuxPackages_latest keeps `nix build`
          # self-contained; the NixOS module rebuilds against the consumer's
          # chosen kernelPackages so the module always matches the booted kernel.
          lvda = pkgs.linuxPackages_latest.callPackage ./packaging/nix/lvda.nix {
            src = self;
          };
          default = self.packages.${system}.lvda-ctl;
        });

      nixosModules.default = import ./packaging/nix/module.nix self;
    };
}
