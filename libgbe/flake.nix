{
  description = "beignet — Ivy Bridge (HD 4000) OpenCL, LLVM 10 üzerine";

  inputs = {
    # nixos-23.11: beignet paketi ile llvmPackages_10'un birlikte bulunduğu
    # son sürüm (24.05'te ikisi de kaldırıldı). İkisi de cache.nixos.org'da.
    nixpkgs.url = "github:NixOS/nixpkgs/205fd4226592cc83fd4c0885a3e4c9c400efabb5";
    # Yalnız LLVM yükseltme denemeleri için (llvm21 devShell'i); pocl'ın LLVM'i.
    nixpkgs-new.url = "github:NixOS/nixpkgs/151fa4e8ddfdd8dd25d945ad94ed54a13de9f6e4";
    # The system's nixpkgs (NixOS 26.11, glibc 2.42, LLVM 21.1.8): libgbe is
    # loaded into system applications through pocl-gen7, so it must not need
    # a newer glibc than theirs (unstable's libLLVM needs GLIBC_2.44).
    nixpkgs-sys.url = "github:NixOS/nixpkgs/e158d9ed9b51c98974c5e66e1ba1c9e0255fecaa";
  };

  outputs =
    { self, nixpkgs, nixpkgs-new, nixpkgs-sys }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      pkgsNew = import nixpkgs-new { inherit system; };
      pkgsSys = import nixpkgs-sys { inherit system; };
      llvm = pkgs.llvmPackages_10;

      # 23.11'de ocl-icd'nin OpenCL.pc'si "Requires: OpenCL-Headers" diyor ama
      # opencl-headers paketi bir .pc dosyası kurmuyor. Eksik dosyayı ekler.
      openclHeadersPc = pkgs.runCommand "opencl-headers-pc" { } ''
        mkdir -p $out/lib/pkgconfig
        cat > $out/lib/pkgconfig/OpenCL-Headers.pc <<EOF
        Name: OpenCL-Headers
        Description: Khronos OpenCL Headers
        Version: ${pkgs.opencl-headers.version}
        Cflags: -I${pkgs.opencl-headers}/include
        EOF
      '';

      mkDev = p: llvm: extra: p.mkShell {
        name = "beignet-llvm${p.lib.versions.major llvm.llvm.version}";

        nativeBuildInputs = with p; [
          cmake
          pkg-config
          python3
          llvm.clang
        ];

        buildInputs = with p; [
          llvm.libllvm
          llvm.libclang
          xorg.libX11
          xorg.libXext
          xorg.libpthreadstubs
          libdrm
          xorg.libXdmcp
          xorg.libXdamage
          ocl-icd
          libGL
          clinfo
        ] ++ extra;

        # libclang kütüphaneleri ayrı çıktıda; nixpkgs paketi de bunu
        # -DCLANG_LIBRARY_DIR ile veriyor. cmake'e -D olarak geçilmeli.
        CLANG_LIBRARY_DIR = "${llvm.libclang.lib}/lib";
      };
    in
    {
      packages.${system} = {
        # Referans: 23.11 paketi (fc5f430 + clang_llvm.patch, LLVM 6).
        # Bugün iyi çalışan hal; her değişiklik buna karşı ölçülür.
        beignet-ref = pkgs.beignet;
        # 23.11'deki passthru.utests bu yüzden derlenmiyor (bkz. openclHeadersPc).
        utests-ref = pkgs.beignet.utests.overrideAttrs (old: {
          buildInputs = old.buildInputs ++ [ openclHeadersPc ];
          # Yeni GCC'de <limits> dolaylı gelmiyor (AUR: utests-add-limits).
          postPatch = (old.postPatch or "") + ''
            sed -i '1i #include <limits>' utests/compiler_{subgroup,workgroup}_{reduce,scan_inclusive,scan_exclusive}.cpp
          '';
          # libutests.so derleme dizininden bağlanıyor; RPATH'i en baştan kurulum
          # yerine yazdır (sonradan patchelf binary'yi bozuyor).
          cmakeFlags = (old.cmakeFlags or [ ]) ++ [
            "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
            "-DCMAKE_INSTALL_RPATH=${placeholder "out"}/lib"
          ];
        });
        clpeak = pkgs.clpeak;
      };

      devShells.${system} =
        {
          default = mkDev pkgs llvm [ openclHeadersPc ];
          # LLVM yükseltme denemeleri (Aşama 0.7): yalnız derleme hatalarını görmek için.
          # 23.11'de libXdamage xfixes'i yayıyordu, unstable'da yaymıyor.
          llvm21 = mkDev pkgsNew pkgsNew.llvmPackages_21 [ pkgsNew.opencl-headers pkgsNew.libxfixes pkgsNew.libpciaccess pkgsNew.libxcb ];
          llvm21-sys = mkDev pkgsSys pkgsSys.llvmPackages_21 [ pkgsSys.opencl-headers pkgsSys.libxfixes pkgsSys.libpciaccess pkgsSys.libxcb ];
        }
        // builtins.listToAttrs (map (v: {
          name = "llvm${toString v}";
          value = mkDev pkgs pkgs."llvmPackages_${toString v}" [ openclHeadersPc ];
        }) [ 11 12 13 14 15 16 17 ]);
    };
}
