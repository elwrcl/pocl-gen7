# pocl-gen7

Probably only working OpenCL Driver for Intel Ivy Bridge (HD 4000):
pocl with the `gen7` device driver,
which compiles kernels with beignet's `libgbe`.

# clinfo

clinfo
Number of platforms 1
Platform Name Portable Computing Language
Platform Vendor The pocl project
Platform Version OpenCL 3.0 PoCL 7.2 Linux, Release, RELOC, SPIR-V, LLVM 21.1.8, SLEEF, POCL_DEBUG
Platform Profile FULL_PROFILE
Platform Extensions cl_khr_icd cl_khr_priority_hints cl_khr_throttle_hints cl_khr_create_command_queue cl_pocl_content_size cl_ext_buffer_device_address
Platform Extensions with Version cl_khr_icd 0x800000 (2.0.0)
cl_khr_priority_hints 0x400000 (1.0.0)
cl_khr_throttle_hints 0x400000 (1.0.0)
cl_khr_create_command_queue 0x400000 (1.0.0)
cl_pocl_content_size 0x400000 (1.0.0)
cl_ext_buffer_device_address 0x9001 (0.9.1)
Platform Numeric Version 0xc00000 (3.0.0)
Platform Extensions function suffix POCL
Platform Host timer resolution 1ns

Platform Name Portable Computing Language
Number of devices 2
Device ID (this run) 0x63c65a5b21f0
Device Name cpu-ivybridge-Intel(R) Core(TM) i5-3210M CPU @ 2.50GHz
Device Vendor GenuineIntel
Device Vendor ID 0x10006
Device Version OpenCL 3.0 PoCL HSTR: cpu-x86_64-unknown-linux-gnu-ivybridge
Device UUID addb9e6c-c130-6627-d134-5684387dd2ef
Driver UUID 14d0b97e-3b50-7807-2047-5aec3432945e
Valid Device LUID No
Device LUID 0000-000000000000
Device Node Mask 0x1
Device Numeric Version 0xc00000 (3.0.0)
Driver Version 7.2
Device OpenCL C Version OpenCL C 1.2 PoCL
Device OpenCL C all versions OpenCL C 0x400000 (1.0.0)
OpenCL C 0x401000 (1.1.0)
OpenCL C 0x402000 (1.2.0)
OpenCL C 0xc00000 (3.0.0)
Device OpenCL C features __opencl_c_3d_image_writes 0xc00000 (3.0.0)
__opencl_c_images 0xc00000 (3.0.0)
__opencl_c_atomic_order_acq_rel 0xc00000 (3.0.0)
__opencl_c_atomic_order_seq_cst 0xc00000 (3.0.0)
__opencl_c_atomic_scope_device 0xc00000 (3.0.0)
__opencl_c_atomic_scope_all_devices 0xc00000 (3.0.0)
__opencl_c_generic_address_space 0xc00000 (3.0.0)
__opencl_c_work_group_collective_functions 0xc00000 (3.0.0)
__opencl_c_integer_dot_product_input_4x8bit 0xc00000 (3.0.0)
__opencl_c_integer_dot_product_input_4x8bit_packed 0xc00000 (3.0.0)
__opencl_c_program_scope_global_variables 0xc00000 (3.0.0)
__opencl_c_subgroups 0xc00000 (3.0.0)
__opencl_c_read_write_images 0xc00000 (3.0.0)
__opencl_c_subgroups 0xc00000 (3.0.0)
__opencl_c_fp16 0xc00000 (3.0.0)
__opencl_c_fp64 0xc00000 (3.0.0)
__opencl_c_kernel_clock_scope_device 0xc00000 (3.0.0)
__opencl_c_kernel_clock_scope_work_group 0xc00000 (3.0.0)
__opencl_c_kernel_clock_scope_sub_group 0xc00000 (3.0.0)
__opencl_c_ext_fp32_global_atomic_add 0xc00000 (3.0.0)
__opencl_c_ext_fp32_local_atomic_add 0xc00000 (3.0.0)
__opencl_c_ext_fp32_global_atomic_min_max 0xc00000 (3.0.0)
__opencl_c_ext_fp32_local_atomic_min_max 0xc00000 (3.0.0)
__opencl_c_ext_fp64_global_atomic_add 0xc00000 (3.0.0)
__opencl_c_ext_fp64_local_atomic_add 0xc00000 (3.0.0)
__opencl_c_ext_fp64_global_atomic_min_max 0xc00000 (3.0.0)
__opencl_c_ext_fp64_local_atomic_min_max 0xc00000 (3.0.0)
__opencl_c_int64 0xc00000 (3.0.0)
Latest conformance test passed v2026-03-25-00
Device Type CPU
Device Profile FULL_PROFILE
Device Available Yes
Compiler Available Yes
Linker Available Yes
Max compute units 4
Max clock frequency 3100MHz
Device Partition (core)
Max number of sub-devices 4
Supported partition types equally, by counts
Supported affinity domains (n/a)
Max work item dimensions 3
Max work item sizes 4096x4096x4096
Max work group size 4096
Preferred work group size multiple (device) 8
Preferred work group size multiple (kernel) 8
Max sub-groups per work group 128
Sub-group sizes (Intel) 1, 2, 4, 8, 16, 32, 64, 128, 256, 512
Preferred / native vector sizes
char 16 / 16
short 16 / 16
int 8 / 8
long 4 / 4
half 16 / 16 (cl_khr_fp16)
float 8 / 8
double 4 / 4 (cl_khr_fp64)
Integer dot product capabilities 4x8bit packed, 4x8bit
4x8bit dot product acceleration (n/a)
4x8bit packed dot product acceleration (n/a)
Half-precision Floating-point support (cl_khr_fp16)
Denormals No
Infinity and NANs Yes
Round to nearest Yes
Round to zero No
Round to infinity No
IEEE754-2008 fused multiply-add No
Support is emulated in software No
Single-precision Floating-point support (core)
Denormals Yes
Infinity and NANs Yes
Round to nearest Yes
Round to zero Yes
Round to infinity Yes
IEEE754-2008 fused multiply-add No
Support is emulated in software No
Correctly-rounded divide and sqrt operations Yes
Double-precision Floating-point support (cl_khr_fp64)
Denormals Yes
Infinity and NANs Yes
Round to nearest Yes
Round to zero Yes
Round to infinity Yes
IEEE754-2008 fused multiply-add No
Support is emulated in software No
Address bits 64, Little-Endian
Global memory size 12343160832 (11.5GiB)
Error Correction support No
Max memory allocation 12343160832 (11.5GiB)
Unified memory for Host and Device Yes
Shared Virtual Memory (SVM) capabilities (core)
Coarse-grained buffer sharing Yes
Fine-grained buffer sharing Yes
Fine-grained system sharing Yes
Atomics Yes
Unified Shared Memory (USM) (cl_intel_unified_shared_memory)
Host USM capabilities (Intel) USM access, USM atomic access
Device USM capabilities (Intel) USM access, USM atomic access
Single-Device USM caps (Intel) USM access, USM atomic access
Cross-Device USM caps (Intel) (n/a)
Shared System USM caps (Intel) (n/a)
Minimum alignment for any data type 128 bytes
Alignment of base address 1024 bits (128 bytes)
Preferred alignment for atomics
SVM 64 bytes
Global 64 bytes
Local 64 bytes
Atomic memory capabilities relaxed, acquire/release, sequentially-consistent, work-group scope, device scope, all-devices scope
Atomic fence capabilities relaxed, acquire/release, sequentially-consistent, work-item scope, work-group scope, device scope
Half-precision Floating-point atomic capabilities
Global Load/Store No
Global Add No
Global Min/Max No
Local Load/Store No
Local Add No
Local Min/Max No
Single-precision Floating-point atomic capabilities
Global Load/Store No
Global Add Yes
Global Min/Max Yes
Local Load/Store No
Local Add Yes
Local Min/Max Yes
Double-precision Floating-point atomic capabilities
Global Load/Store No
Global Add Yes
Global Min/Max Yes
Local Load/Store No
Local Add Yes
Local Min/Max Yes
Max size for global variable 64000 (62.5KiB)
Preferred total size of global vars 262144 (256KiB)
Global Memory cache type Read/Write
Global Memory cache size 3145728 (3MiB)
Global Memory cache line size 64 bytes
Image support Yes
Max number of samplers per kernel 16
Max size for 1D images from buffer 771447552 pixels
Max 1D or 2D image array size 2048 images
Base address alignment for 2D image buffers 0 bytes
Pitch alignment for 2D image buffers 0 pixels
Max 2D image size 16384x16384 pixels
Max 3D image size 2048x2048x2048 pixels
Max number of read image args 128
Max number of write image args 128
Max number of read/write image args 128
Pipe support No
Max number of pipe args 0
Max active pipe reservations 0
Max pipe packet size 0
Local memory type Global
Local memory size 262144 (256KiB)
Max number of constant args 8
Max constant buffer size 262144 (256KiB)
Generic address space support Yes
Max size of kernel argument 1024
Queue properties (on host)
Out-of-order execution Yes
Profiling Yes
Device enqueue capabilities (n/a)
Queue properties (on device)
Out-of-order execution No
Profiling No
Preferred size 0
Max size 0
Max queues on device 0
Max events on device 0
Command buffer capabilities kernel printf, simultaneous use, multiple queue
Supported queue properties for command buffer
Out-of-order execution Yes
Profiling Yes
Required queue properties for command buffer
Out-of-order execution No
Profiling No
Prefer user sync for interop Yes
Profiling timer resolution 1ns
Execution capabilities
Run OpenCL kernels Yes
Run native kernels Yes
Non-uniform work-groups No
Work-group collective functions Yes
Sub-group independent forward progress Yes
IL version SPIR-V_1.5 SPIR-V_1.4 SPIR-V_1.3 SPIR-V_1.2 SPIR-V_1.1 SPIR-V_1.0
ILs with version SPIR-V 0x405000 (1.5.0)
SPIR-V 0x404000 (1.4.0)
SPIR-V 0x403000 (1.3.0)
SPIR-V 0x402000 (1.2.0)
SPIR-V 0x401000 (1.1.0)
SPIR-V 0x400000 (1.0.0)
SPIR-V extended instruction sets OpenCL.std
SPIR-V extensions SPV_KHR_bit_instructions
SPV_KHR_integer_dot_product
SPV_KHR_shader_clock
SPV_KHR_linkonce_odr
SPV_KHR_no_integer_wrap_decoration
SPV_EXT_shader_atomic_float_add
SPV_EXT_shader_atomic_float_min_max
SPIR-V capabilities Addresses (4)
Float16Buffer (8)
Int16 (22)
Int8 (39)
Kernel (6)
Linkage (5)
Vector16 (7)
Int64 (11)
Image1D (44)
ImageBasic (13)
ImageBuffer (47)
LiteralSampler (20)
Sampled1D (43)
SampledBuffer (46)
ImageReadWrite (14)
GenericPointer (38)
Groups (18)
SubgroupDispatch (58)
BitInstructions (6025)
Float16 (9)
Float64 (10)
Int64Atomics (12)
DotProduct / DotProductKHR (6019)
DotProductInput4x8BitPacked / DotProductInput4x8BitPackedKHR (6018)
DotProductInput4x8Bit / DotProductInput4x8BitKHR (6017)
ShaderClockKHR (5055)
GroupNonUniformBallot (64)
GroupNonUniformShuffle (65)
AtomicFloat32AddEXT (6033)
AtomicFloat32MinMaxEXT (5612)
AtomicFloat64AddEXT (6034)
AtomicFloat64MinMaxEXT (5613)
printf() buffer size 16777216 (16MiB)
Built-in kernels pocl.add.i8;org.khronos.openvx.scale_image.nn.u8;org.khronos.openvx.scale_image.bl.u8;org.khronos.openvx.tensor_convert_depth.wrap.u8.f32;
Built-in kernels with version pocl.add.i8 0x402000 (1.2.0)
org.khronos.openvx.scale_image.nn.u8 0x402000 (1.2.0)
org.khronos.openvx.scale_image.bl.u8 0x402000 (1.2.0)
org.khronos.openvx.tensor_convert_depth.wrap.u8.f32 0x402000 (1.2.0)
Device Extensions cl_khr_byte_addressable_store cl_khr_global_int32_base_atomics cl_khr_global_int32_extended_atomics cl_khr_local_int32_base_atomics cl_khr_local_int32_extended_atomics cl_khr_3d_image_writes cl_intel_unified_shared_memory cl_ext_buffer_device_address cl_khr_device_uuid cl_khr_suggested_local_work_size cl_khr_integer_dot_product cl_ext_float_atomics cl_khr_extended_bit_ops cl_pocl_svm_rect cl_pocl_command_buffer_svm cl_khr_command_buffer cl_khr_command_buffer_multi_device cl_khr_command_buffer_mutable_dispatch cl_pocl_command_buffer_host_buffer cl_khr_subgroups cl_khr_subgroup_ballot cl_khr_subgroup_shuffle cl_intel_subgroups cl_intel_subgroups_short cl_intel_subgroups_char cl_intel_required_subgroup_size cl_exp_tensor cl_khr_il_program cl_khr_spirv_queries cl_khr_spirv_linkonce_odr cl_khr_spirv_no_integer_wrap_decoration cl_khr_fp16 cl_khr_fp64 cl_khr_kernel_clock cl_khr_int64_base_atomics cl_khr_int64_extended_atomics
Device Extensions with Version cl_khr_byte_addressable_store 0x400000 (1.0.0)
cl_khr_global_int32_base_atomics 0x400000 (1.0.0)
cl_khr_global_int32_extended_atomics 0x400000 (1.0.0)
cl_khr_local_int32_base_atomics 0x400000 (1.0.0)
cl_khr_local_int32_extended_atomics 0x400000 (1.0.0)
cl_khr_3d_image_writes 0x400000 (1.0.0)
cl_intel_unified_shared_memory 0x400000 (1.0.0)
cl_ext_buffer_device_address 0x400002 (1.0.2)
cl_khr_device_uuid 0x400000 (1.0.0)
cl_khr_suggested_local_work_size 0x400000 (1.0.0)
cl_khr_integer_dot_product 0x400000 (1.0.0)
cl_ext_float_atomics 0x400000 (1.0.0)
cl_khr_extended_bit_ops 0x400000 (1.0.0)
cl_pocl_svm_rect 0x9000 (0.9.0)
cl_pocl_command_buffer_svm 0x9000 (0.9.0)
cl_khr_command_buffer 0x9006 (0.9.6)
cl_khr_command_buffer_multi_device 0x9001 (0.9.1)
cl_khr_command_buffer_mutable_dispatch 0x9003 (0.9.3)
cl_pocl_command_buffer_host_buffer 0x9000 (0.9.0)
cl_khr_subgroups 0x400000 (1.0.0)
cl_khr_subgroup_ballot 0x400000 (1.0.0)
cl_khr_subgroup_shuffle 0x400000 (1.0.0)
cl_intel_subgroups 0x400000 (1.0.0)
cl_intel_subgroups_short 0x400000 (1.0.0)
cl_intel_subgroups_char 0x400000 (1.0.0)
cl_intel_required_subgroup_size 0x400000 (1.0.0)
cl_exp_tensor 0x1000 (0.1.0)
cl_khr_il_program 0x801000 (2.1.0)
cl_khr_spirv_queries 0x400000 (1.0.0)
cl_khr_spirv_linkonce_odr 0x400000 (1.0.0)
cl_khr_spirv_no_integer_wrap_decoration 0x400000 (1.0.0)
cl_khr_fp16 0x400000 (1.0.0)
cl_khr_fp64 0x400000 (1.0.0)
cl_khr_kernel_clock 0x400000 (1.0.0)
cl_khr_int64_base_atomics 0x400000 (1.0.0)
cl_khr_int64_extended_atomics 0x400000 (1.0.0)

Device ID (this run) 0x63c65a5b8ef0
Device Name Intel(R) HD Graphics IvyBridge M GT2
Device Vendor Intel
Device Vendor ID 0x8086
Device Version OpenCL 3.0 PoCL HSTR: gen7-0166-gbe
Device Numeric Version 0xc00000 (3.0.0)
Driver Version 7.2 gen7
Device OpenCL C Version OpenCL C 1.2 PoCL
Device OpenCL C all versions OpenCL C 0x400000 (1.0.0)
OpenCL C 0x401000 (1.1.0)
OpenCL C 0x402000 (1.2.0)
OpenCL C 0xc00000 (3.0.0)
Device OpenCL C features __opencl_c_images 0xc00000 (3.0.0)
__opencl_c_3d_image_writes 0xc00000 (3.0.0)
__opencl_c_int64 0xc00000 (3.0.0)
Latest conformance test passed (n/a)
Device Type GPU
Device Profile FULL_PROFILE
Device Available Yes
Compiler Available Yes
Linker Available Yes
Max compute units 16
Max clock frequency 1100MHz
Device IP (Intel) 0x1c00000 (7.0.0)
Device ID (Intel) 358
Slices (Intel) 1
Sub-slices per slice (Intel) 2
EUs per sub-slice (Intel) 8
Threads per EU (Intel) 8
Feature capabilities (Intel) (n/a)
Device Partition (core)
Max number of sub-devices 0
Supported partition types None
Supported affinity domains (n/a)
Max work item dimensions 3
Max work item sizes 512x512x512
Max work group size 512
Preferred work group size multiple (device) 16
Preferred work group size multiple (kernel) 16
Max sub-groups per work group 0
Sub-group sizes (Intel) 8, 16
Preferred / native vector sizes
char 16 / 8
short 8 / 8
int 4 / 4
long 2 / 2
half 0 / 8 (n/a)
float 4 / 4
double 0 / 2 (n/a)
Half-precision Floating-point support (n/a)
Single-precision Floating-point support (core)
Denormals No
Infinity and NANs Yes
Round to nearest Yes
Round to zero No
Round to infinity No
IEEE754-2008 fused multiply-add No
Support is emulated in software No
Correctly-rounded divide and sqrt operations No
Double-precision Floating-point support (n/a)
Address bits 32, Little-Endian
Global memory size 2147483648 (2GiB)
Error Correction support No
Max memory allocation 1610612736 (1.5GiB)
Unified memory for Host and Device Yes
Shared Virtual Memory (SVM) capabilities (core)
Coarse-grained buffer sharing No
Fine-grained buffer sharing No
Fine-grained system sharing No
Atomics No
Minimum alignment for any data type 128 bytes
Alignment of base address 1024 bits (128 bytes)
Preferred alignment for atomics
SVM 64 bytes
Global 64 bytes
Local 64 bytes
Atomic memory capabilities relaxed, work-group scope
Atomic fence capabilities relaxed, acquire/release, work-group scope
Max size for global variable 0
Preferred total size of global vars 0
Global Memory cache type Read/Write
Global Memory cache size 8192 (8KiB)
Global Memory cache line size 64 bytes
Image support Yes
Max number of samplers per kernel 16
Max size for 1D images from buffer 65536 pixels
Max 1D or 2D image array size 2048 images
Base address alignment for 2D image buffers 4096 bytes
Pitch alignment for 2D image buffers 1 pixels
Max 2D image size 16384x16384 pixels
Max 3D image size 2048x2048x2048 pixels
Max number of read image args 128
Max number of write image args 8
Max number of read/write image args 0
Pipe support No
Max number of pipe args 0
Max active pipe reservations 0
Max pipe packet size 0
Local memory type Local
Local memory size 65536 (64KiB)
Max number of constant args 8
Max constant buffer size 134217728 (128MiB)
Generic address space support No
Max size of kernel argument 1024
Queue properties (on host)
Out-of-order execution Yes
Profiling Yes
Device enqueue capabilities (n/a)
Queue properties (on device)
Out-of-order execution No
Profiling No
Preferred size 0
Max size 0
Max queues on device 0
Max events on device 0
Prefer user sync for interop Yes
Profiling timer resolution 80ns
Execution capabilities
Run OpenCL kernels Yes
Run native kernels No
Non-uniform work-groups No
Work-group collective functions No
Sub-group independent forward progress No
IL version SPIR-V_1.2 SPIR-V_1.1 SPIR-V_1.0
ILs with version SPIR-V 0x402000 (1.2.0)
SPIR-V 0x401000 (1.1.0)
SPIR-V 0x400000 (1.0.0)
printf() buffer size 1048576 (1024KiB)
Built-in kernels (n/a)
Built-in kernels with version (n/a)
Device Extensions cl_khr_global_int32_base_atomics cl_khr_global_int32_extended_atomics cl_khr_local_int32_base_atomics cl_khr_local_int32_extended_atomics cl_khr_byte_addressable_store cl_khr_3d_image_writes cl_khr_depth_images cl_khr_icd cl_khr_image2d_from_buffer cl_intel_subgroups cl_intel_subgroups_short cl_intel_required_subgroup_size cl_intel_media_block_io cl_khr_il_program cl_intel_device_attribute_query cl_intel_va_api_media_sharing cl_khr_gl_sharing
Device Extensions with Version cl_khr_global_int32_base_atomics 0x400000 (1.0.0)
cl_khr_global_int32_extended_atomics 0x400000 (1.0.0)
cl_khr_local_int32_base_atomics 0x400000 (1.0.0)
cl_khr_local_int32_extended_atomics 0x400000 (1.0.0)
cl_khr_byte_addressable_store 0x400000 (1.0.0)
cl_khr_3d_image_writes 0x400000 (1.0.0)
cl_khr_depth_images 0x800000 (2.0.0)
cl_khr_image2d_from_buffer 0x400000 (1.0.0)
cl_intel_subgroups 0x400000 (1.0.0)
cl_intel_subgroups_short 0x400000 (1.0.0)
cl_intel_required_subgroup_size 0x400000 (1.0.0)
cl_khr_il_program 0x801000 (2.1.0)
cl_intel_device_attribute_query 0x400000 (1.0.0)

NULL platform behavior
clGetPlatformInfo(NULL, CL_PLATFORM_NAME, ...) Portable Computing Language
clGetDeviceIDs(NULL, CL_DEVICE_TYPE_ALL, ...) Success [POCL]
clCreateContext(NULL, ...) [default] Success [POCL]
clCreateContextFromType(NULL, CL_DEVICE_TYPE_DEFAULT) Success (1)
Platform Name Portable Computing Language
Device Name cpu-ivybridge-Intel(R) Core(TM) i5-3210M CPU @ 2.50GHz
clCreateContextFromType(NULL, CL_DEVICE_TYPE_CPU) Success (1)
Platform Name Portable Computing Language
Device Name cpu-ivybridge-Intel(R) Core(TM) i5-3210M CPU @ 2.50GHz
clCreateContextFromType(NULL, CL_DEVICE_TYPE_GPU) Success (1)
Platform Name Portable Computing Language
Device Name Intel(R) HD Graphics IvyBridge M GT2
clCreateContextFromType(NULL, CL_DEVICE_TYPE_ACCELERATOR) No devices found in platform
clCreateContextFromType(NULL, CL_DEVICE_TYPE_CUSTOM) No devices found in platform
clCreateContextFromType(NULL, CL_DEVICE_TYPE_ALL) Success (2)
Platform Name Portable Computing Language
Device Name cpu-ivybridge-Intel(R) Core(TM) i5-3210M CPU @ 2.50GHz
Device Name Intel(R) HD Graphics IvyBridge M GT2

ICD loader properties
ICD loader Name OpenCL ICD Loader
ICD loader Vendor OCL Icd free software
ICD loader Version 2.3.5
ICD loader Profile OpenCL 3.1
