# pocl-gen7

OpenCL for Intel Ivy Bridge (HD 4000): pocl with the `gen7` device driver,
which compiles kernels with beignet's `libgbe`.

```
pocl-gen7/
  pocl/      jj repo, bookmark gen7   (pocl + lib/CL/devices/gen7)
  libgbe/    jj repo, bookmark gen7   (beignet, LLVM 21 port: libgbe + libocl)
  build/     release builds (release.sh)
  release.sh build both, copy into ~/copland/drivers/pocl-gen7
```

The development history, tests and results stay in `../opencl/`
(`notes/handoff.md`, `results/gen7/`, `tools/gen7/`).

## System install

`./release.sh` builds against the system's nixpkgs and copies the installed
tree to `~/copland/drivers/pocl-gen7`; copland's `hw-ivb-drivers` module
packages it (ICD, library paths) and `nixos-rebuild switch` installs it.

The installed driver finds libgbe and beignet's OpenCL C library in
`lib/beignet/` next to `lib/pocl/libpocl-devices-gen7.so`; the `OCL_*`
variables (`OCL_GBE_PATH`, `OCL_BITCODE_LIB_PATH`, ...) override it.
