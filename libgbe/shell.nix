{
  pkgs ? import (fetchTarball "https://github.com/NixOS/nixpkgs/archive/205fd4226592cc83fd4c0885a3e4c9c400efabb5.tar.gz") { },
}:

pkgs.mkShell {
  name = "beignet-test-shell";

  buildInputs = [
    pkgs.intel-ocl
    pkgs.ocl-icd
    pkgs.beignet
    pkgs.clinfo
  ];

  shellHook = ''
    export OCL_ICD_VENDORS="${pkgs.beignet}/etc/OpenCL/vendors"
    export NIXPKGS_ALLOW_UNFREE=1
  '';
}
