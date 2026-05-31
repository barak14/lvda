# SPDX-License-Identifier: GPL-2.0
# lvda-ctl: userspace CLI / liveness daemon. Pure stdenv build — `src` is the
# repo root (the flake passes `self` as src).
{ lib, stdenv, src }:

stdenv.mkDerivation {
  pname = "lvda-ctl";
  version = "0.1.0";
  inherit src;

  enableParallelBuilding = true;

  buildPhase = ''
    runHook preBuild
    make -C tools/lvda-ctl
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 tools/lvda-ctl/lvda-ctl $out/bin/lvda-ctl
    install -Dm644 uapi/lvda.h             $out/include/lvda/lvda.h
    runHook postInstall
  '';

  meta = with lib; {
    description = "Control CLI / liveness daemon for the lvda virtual display driver";
    homepage = "https://github.com/_/lvda";
    license = licenses.gpl2Only;
    platforms = [ "x86_64-linux" ];
    mainProgram = "lvda-ctl";
  };
}
