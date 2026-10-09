#!/usr/bin/env bash
# Release build of pocl-gen7 (pocl + the gen7 device on beignet's libgbe) for
# the system: configures build/libgbe and build/pocl once, builds them,
# installs into a staging directory and copies the tree into copland, where
# hw-ivb-drivers packages it.
#
#   ./release.sh                      then: nixos-rebuild switch
#
# Both are built against the system's nixpkgs (libgbe: its llvm21-sys shell,
# pocl: its default shell), so they link the system's LLVM 21 and libraries.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
dest=${COPLAND_DRIVERS:-/home/elars/copland/drivers}/pocl-gen7
cd "$here"

profile() { # name flake-ref
  [ -e ".profiles/$1" ] || nix develop --profile ".profiles/$1" "$2" -c true
}
mkdir -p .profiles build
profile libgbe ./libgbe#llvm21-sys
profile pocl ./pocl

# libgbe: the compiler (libgbe, libgbeinterp) and beignet's OpenCL C library
if [ ! -f build/libgbe/Makefile ]; then
  clang_lib=$(nix eval --raw --inputs-from ./libgbe \
    nixpkgs-sys#llvmPackages_21.clang-unwrapped.lib.outPath)/lib
  nix develop .profiles/libgbe -c cmake -S libgbe -B build/libgbe \
    -DCLANG_LIBRARY_DIR="$clang_lib" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBEIGNET_INSTALL_DIR=/usr/lib/beignet/ -DENABLE_OPENCL_20=ON \
    -DOCLICD_COMPAT=ON -DBUILD_EXAMPLES=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
fi
nix develop .profiles/libgbe -c cmake --build build/libgbe -j "$(nproc)"

# pocl with the gen7 and host CPU (pthread) devices, on the system's shared
# LLVM (a static one makes libpocl 116 MB, over GitHub's file limit)
if [ ! -f build/pocl/build.ninja ]; then
  nix develop .profiles/pocl -c sh -c 'cmake -S pocl -B build/pocl -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib -DWITH_LLVM_CONFIG="$LLVM_CONFIG" \
    -DSTATIC_LLVM=OFF -DENABLE_ICD=ON -DPOCL_ICD_ABSOLUTE_PATH=OFF \
    -DENABLE_GEN7=ON -DBEIGNET_SRC_DIR="$PWD/libgbe" \
    -DGEN7_GBE_PATH=/usr/lib/beignet/libgbe.so \
    -DGEN7_INTERP_PATH=/usr/lib/beignet/libgbeinterp.so \
    -DENABLE_HOST_CPU_DEVICES=ON -DENABLE_SPIRV=ON -DENABLE_TESTS=OFF \
    -DENABLE_EXAMPLES=OFF -DENABLE_POCLCC=OFF -DENABLE_POCL_BUILDING=OFF \
    -DINSTALL_OPENCL_HEADERS=OFF'
fi
nix develop .profiles/pocl -c ninja -C build/pocl

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
nix develop .profiles/libgbe -c env DESTDIR="$stage" cmake --install build/libgbe >/dev/null
nix develop .profiles/pocl -c env DESTDIR="$stage" cmake --install build/pocl --strip >/dev/null

# the system copy: pocl's libraries, its kernel library, libgbe next to the
# gen7 device (lib/beignet, see cl_gbe_loader.cpp)
rm -rf "$dest.new"
mkdir -p "$dest.new/lib"
cp -a "$stage"/usr/lib/libpocl.so* "$stage/usr/lib/pocl" "$dest.new/lib/"
mkdir -p "$dest.new/lib/beignet"
for f in libgbe.so libgbeinterp.so beignet.bc beignet_20.bc beignet.pch beignet_20.pch include; do
  cp -a "$stage/usr/lib/beignet/$f" "$dest.new/lib/beignet/"
done
strip --strip-unneeded "$dest.new"/lib/beignet/*.so
mkdir -p "$dest.new/share"
cp -a "$stage/usr/share/pocl" "$dest.new/share/"
{
  printf 'pocl   %s\n' "$(cd pocl && jj log -r @- --no-graph -T 'commit_id.short() ++ " " ++ description.first_line()')"
  printf 'libgbe %s\n' "$(cd libgbe && jj log -r @- --no-graph -T 'commit_id.short() ++ " " ++ description.first_line()')"
} > "$dest.new/SOURCE"
rm -rf "$dest"
mv "$dest.new" "$dest"
du -sh "$dest"
cat "$dest/SOURCE"
