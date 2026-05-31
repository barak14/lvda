# SPDX-License-Identifier: GPL-2.0
# lvda kernel module derivation. NixOS doesn't use DKMS; the module is built
# ahead-of-time against a specific kernel store path and added to
# boot.extraModulePackages. The NixOS module in ./module.nix re-evaluates this
# derivation with the consumer's kernelPackages so the .ko always matches the
# booted kernel.
{ lib, stdenv, kernel, src }:

stdenv.mkDerivation {
  pname = "lvda";
  version = "0.1.0-${kernel.version}";
  inherit src;

  # Kernel modules built out-of-tree set their own hardening via Kbuild;
  # nixpkgs' default userland hardening flags would conflict.
  hardeningDisable = [ "pic" "format" "fortify" "stackprotector" "strictoverflow" ];

  nativeBuildInputs = kernel.moduleBuildDependencies;

  buildPhase = ''
    runHook preBuild
    make -C module \
      KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    make -C module \
      KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
      INSTALL_MOD_PATH=$out \
      modules_install
    runHook postInstall
  '';

  meta = with lib; {
    description = "Virtual display driver for Sunshine/Apollo streaming (Linux ${kernel.version})";
    homepage = "https://github.com/_/lvda";
    license = licenses.gpl2Only;
    platforms = [ "x86_64-linux" ];
  };
}
