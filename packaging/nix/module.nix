# SPDX-License-Identifier: GPL-2.0
# NixOS module for lvda. Imported via the flake:
#
#   imports = [ inputs.lvda.nixosModules.default ];
#   services.lvda.enable = true;
#
# The `self` argument is the flake's source (passed in by flake.nix); we use
# it as `src` for both derivations so the module rebuilds whenever the flake
# input bumps.
self: { config, lib, pkgs, ... }:

let
  inherit (lib) mkEnableOption mkIf mkOption types;
  cfg = config.services.lvda;

  # Build the kernel module against the consumer's chosen kernelPackages, so
  # the .ko always matches the booted kernel (kernel upgrades rebuild it).
  lvdaModule = config.boot.kernelPackages.callPackage
    "${self}/packaging/nix/lvda.nix" { src = self; };

  lvdaCtl = pkgs.callPackage
    "${self}/packaging/nix/lvda-ctl.nix" { src = self; };
in {
  options.services.lvda = {
    enable = mkEnableOption "the lvda virtual display driver";

    maxMonitors = mkOption {
      type = types.ints.between 1 32;
      default = 1;
      description = ''
        Maximum number of virtual monitors the lvda card exposes. Raise only
        when you stream to multiple clients at once.
      '';
    };

    group = mkOption {
      type = types.str;
      default = "lvda";
      description = ''
        Unix group owning /dev/lvda (mode 0660). Add the streaming user (or
        your login) to this group:
          users.users.alice.extraGroups = [ "lvda" ];
      '';
    };
  };

  config = mkIf cfg.enable {
    boot.extraModulePackages = [ lvdaModule ];
    boot.kernelModules = [ "lvda" ];
    boot.extraModprobeConfig = ''
      options lvda lvda_max_monitors=${toString cfg.maxMonitors}
    '';

    environment.systemPackages = [ lvdaCtl ];

    users.groups.${cfg.group} = {};

    services.udev.extraRules = ''
      KERNEL=="lvda", SUBSYSTEM=="misc", GROUP="${cfg.group}", MODE="0660"
    '';

    systemd.tmpfiles.rules = [
      "d /run/lvda 0770 root ${cfg.group} -"
    ];
  };
}
