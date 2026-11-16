#=============================================================================
#   CMake build system files for the gen7 (Ivy Bridge) device driver
#
#   The compiler is beignet's libgbe (loaded at runtime, see hw/cl_gbe_loader.cpp);
#   hw/ is beignet's intel_driver/intel_gpgpu/intel_batchbuffer layer.
#=============================================================================

add_pocl_device_library(pocl-devices-gen7
  pocl-gen7.c pocl-gen7.h
  gen7-compile.c
  gen7-mem.c
  gen7-launch.c
  gen7-trace.c
  gen7-math.c gen7-math.h
  gen7-image-fmt.c
  gen7-spirv.c
  hw/cl_alloc.c
  hw/cl_driver_defs.c
  hw/cl_gbe_loader.cpp
  hw/intel_batchbuffer.c
  hw/intel_driver.c
  hw/intel_gpgpu.c)

target_include_directories(pocl-devices-gen7 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/hw
  ${BEIGNET_SRC_DIR}/backend/src/backend
  ${BEIGNET_SRC_DIR}/backend/src)

target_compile_definitions(pocl-devices-gen7 PRIVATE
  GEN7_DRIVER_INTERNAL
  GBE_OBJECT_DIR="${GEN7_GBE_PATH}"
  INTERP_OBJECT_DIR="${GEN7_INTERP_PATH}"
  GEN7_SAMPLER_CLAMP_BORDER_WORKAROUND
  HAS_USERPTR
  HAS_BO_SET_SOFTPIN)

# beignet's hw code predates C99 strictness and pocl's warning set
set_source_files_properties(
  hw/cl_alloc.c hw/cl_driver_defs.c hw/intel_batchbuffer.c
  hw/intel_driver.c hw/intel_gpgpu.c
  PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing")

if(ENABLE_LOADABLE_DRIVERS)
  target_link_libraries(pocl-devices-gen7 PRIVATE
    PkgConfig::LIBDRM_INTEL ${CMAKE_DL_LIBS} ${PTHREAD_LIBRARY})
else()
  target_link_libraries(pocl-devices-gen7 PRIVATE PkgConfig::LIBDRM_INTEL)
endif()

# GPU-free unit tests of the pure helpers (local size, curbe layout, image layout)
add_executable(gen7-math-test tests/test_gen7_math.c gen7-math.c)
target_include_directories(gen7-math-test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/hw
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR})
