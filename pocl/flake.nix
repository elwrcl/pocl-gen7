{
  description = "pocl + gen7 (Ivy Bridge, beignet backend) driver";

  inputs = {
    # The system's nixpkgs (NixOS 26.11, glibc 2.42, LLVM 21): the driver is
    # loaded into system applications (mpv/SVP), so it must not need a newer
    # glibc than theirs. (nixpkgs-unstable 151fa4e8 has glibc 2.44, whose
    # cosh/sinh versions made libpocl.so unloadable in mpv.)
    nixpkgs.url = "github:NixOS/nixpkgs/e158d9ed9b51c98974c5e66e1ba1c9e0255fecaa";
  };

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      llvm = pkgs.llvmPackages_21;
      byName = prefix: list:
        pkgs.lib.findFirst (p: pkgs.lib.hasPrefix prefix (p.name or "")) null list;
      # pocl looks for clang, opt, llc, llvm-spirv, ... next to llvm-config.
      # In nixpkgs they live in separate outputs, so join them into one bin/.
      llvmBin = pkgs.symlinkJoin {
        name = "pocl-llvm-bin";
        paths = [
          llvm.llvm
          llvm.llvm.dev
          (byName "clang-pocl" pkgs.pocl.nativeBuildInputs)
          (byName "SPIRV-LLVM-Translator" pkgs.pocl.buildInputs)
        ];
        # pocl resolves llvm-config's real path and searches that directory,
        # so it must not be a symlink into llvm-dev.
        postBuild = ''
          rm $out/bin/llvm-config
          printf '#!${pkgs.runtimeShell}\nexec ${llvm.llvm.dev}/bin/llvm-config "$@"\n' > $out/bin/llvm-config
          chmod +x $out/bin/llvm-config
        '';
      };
      # AdaptiveCpp with its OpenCL backend. Its build git clones the
      # AdaptiveCpp SPIRV-LLVM-Translator fork (branch llvm_release_200, for
      # the LLVM 20 it is built with), which in turn clones SPIRV-Headers, and
      # the OpenCL C/C++ headers; all are pinned here and passed in as local
      # sources.
      adaptivecppCL = (pkgs.adaptivecpp.override { openclSupport = true; }).overrideAttrs (old: {
        buildInputs = old.buildInputs ++ [
          pkgs.ocl-icd
          pkgs.opencl-headers
        ];
        # The OpenCL runtime backend FetchContent's these (src/runtime).
        cmakeFlags = old.cmakeFlags ++ [
          "-DFETCHCONTENT_SOURCE_DIR_OCL-HEADERS=${oclHeadersSrc}"
          "-DFETCHCONTENT_SOURCE_DIR_OCL-CXX-HEADERS=${oclCxxHeadersSrc}"
        ];
        postPatch = old.postPatch + ''
          substituteInPlace src/compiler/llvm-to-backend/CMakeLists.txt \
            --replace-fail 'GIT_REPOSITORY https://github.com/AdaptiveCpp/SPIRV-LLVM-Translator' \
                           'SOURCE_DIR ${spirvTranslatorSrc}' \
            --replace-fail 'GIT_TAG origin/''${LLVMSPIRV_BRANCH}' "" \
            --replace-fail 'GIT_SHALLOW ON' "" \
            --replace-fail 'GIT_REMOTE_UPDATE_STRATEGY CHECKOUT' "" \
            --replace-fail '-DLLVM_SPIRV_BUILD_EXTERNAL:BOOL=YES' \
                           '-DLLVM_SPIRV_BUILD_EXTERNAL:BOOL=YES -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR:PATH=${spirvHeadersSrc}'
        '';
      });
      spirvTranslatorSrc = pkgs.fetchFromGitHub {
        owner = "AdaptiveCpp";
        repo = "SPIRV-LLVM-Translator";
        rev = "f0ae76f12c62ede090e57ece8c986f4c3c971a71"; # llvm_release_200
        hash = "sha256-5XHmos+J4iF5LbyhrNcIPxuVYsNyQgw/kb2A9wX0GRA=";
      };
      # The commit in the translator's spirv-headers-tag.conf.
      spirvHeadersSrc = pkgs.fetchFromGitHub {
        owner = "KhronosGroup";
        repo = "SPIRV-Headers";
        rev = "0e710677989b4326ac974fd80c5308191ed80965";
        hash = "sha256-Pdmgk2pTQCYiaLc6s3g14ZHa3AIds6Jbt8cX061aR7A=";
      };
      oclHeadersSrc = pkgs.fetchFromGitHub {
        owner = "KhronosGroup";
        repo = "OpenCL-Headers";
        rev = "265df85aec478d14a5c5880d7bb92d7dd52714ef";
        hash = "sha256-NUHXpaR/zKB9rEvIP9VFnVIYZ25vXj1TM2xanoVSRV0=";
      };
      oclCxxHeadersSrc = pkgs.fetchFromGitHub {
        owner = "KhronosGroup";
        repo = "OpenCL-CLHPP";
        rev = "67d100e70612341707725b6648ccca4c10b0dc31";
        hash = "sha256-q9NrF4vEmCEXV59rQVvEKrkBH3iqJA9Ehe2Sz0JFMiY=";
      };
    in
    {
      # SYCL compilers for the stage 2 samples (tools/sycl): DPC++ (oneAPI)
      # and AdaptiveCpp, both through pocl's OpenCL ICD.
      devShells.${system} = {
      sycl = pkgs.mkShell {
        name = "pocl-gen7-sycl";
        packages = [
          pkgs.intel-llvm
          adaptivecppCL
          pkgs.ocl-icd
          pkgs.clinfo
        ];
      };

      default = pkgs.mkShell {
        name = "pocl-gen7";
        inputsFrom = [ pkgs.pocl ];
        nativeBuildInputs = [ pkgs.ninja ];
        buildInputs = [
          pkgs.libdrm
          pkgs.clinfo
          llvm.llvm.dev
        ];
        LLVM_CONFIG = "${llvmBin}/bin/llvm-config";
        LLVM_BIN = "${llvmBin}/bin";
        # The cc-wrapper hardening flags break clang's spir/kernel-lib targets.
        NIX_HARDENING_ENABLE = "";
      };
      };
    };
}
