/* pocl-gen7.h - Intel Gen7 (Ivy Bridge) driver built on beignet

   Compiler: beignet's libgbe through its C interface (hw/cl_gbe_loader.*).
   Hardware: beignet's intel_driver/intel_gpgpu layer (hw/), driven through
   the cl_driver.h callback table it installs (intel_setup_callbacks).
*/

#ifndef POCL_GEN7_H
#define POCL_GEN7_H

#include "pocl_cl.h"
#include "prototypes.inc"

GEN_PROTOTYPES (gen7)

void pocl_gen7_compute_local_size (cl_device_id dev, cl_kernel kernel,
                                   unsigned device_i, size_t max_group_size,
                                   size_t global_x, size_t global_y,
                                   size_t global_z, size_t *local_x,
                                   size_t *local_y, size_t *local_z);

void pocl_gen7_free_subbuffer (cl_device_id dev, cl_mem sub);
cl_int pocl_gen7_get_device_info_ext (cl_device_id dev,
                                      cl_device_info param_name,
                                      size_t param_value_size,
                                      void *param_value,
                                      size_t *param_value_size_ret);
cl_int pocl_gen7_get_subgroup_info_ext (
    cl_device_id dev, cl_kernel kernel, unsigned program_device_i,
    cl_kernel_sub_group_info param_name, size_t input_value_size,
    const void *input_value, size_t param_value_size, void *param_value,
    size_t *param_value_size_ret);

#ifdef GEN7_DRIVER_INTERNAL

#include "cl_driver.h"
#include "cl_gbe_loader.h"
#include "gen7-math.h"

/* beignet's IVB GT2 numbers (cl_device_id.c, cl_gen7_device.h) */
#define GEN7_MAX_SAMPLERS 16
#define GEN7_MAX_IMAGE_FORMATS 128

/* One submitted NDRange batch, completed by the completion thread */
typedef struct gen7_job
{
  _cl_command_node *node;
  cl_gpgpu gpgpu;
  void *printf_info;
  int printf_num;
  int profiling;
  int timestamps;     /* batch records PIPE_CONTROL timestamps */
  uint64_t t_submit;  /* host time, for POCL_GEN7_STATS */
  struct gen7_job *next;
} gen7_job_t;

/* event->data */
typedef struct gen7_event
{
  pocl_cond_t cond;          /* broadcast when the event finishes */
  volatile int in_flight;    /* NDRange batch is on the GPU */
} gen7_event_t;

/* queue->data */
typedef struct gen7_queue
{
  pocl_cond_t cond;          /* broadcast when the queue drains */
} gen7_queue_t;

typedef struct gen7_device
{
  cl_driver drv;      /* intel_driver_t */
  cl_bool available;  /* dev->available points here */
  cl_buffer_mgr bufmgr;
  uint32_t pci_id;
  uint32_t max_compute_unit;
  uint32_t max_thread_per_unit;
  uint32_t scratch_mem_size;
  int default_tiling; /* GEN7_TILE_X unless OCL_TILING says otherwise */

  /* one batch is built at a time; the bufmgr is not thread safe */
  pocl_lock_t hw_lock;

  /* command scheduling (basic-driver style) */
  pocl_lock_t cq_lock;
  _cl_command_node *ready_list;
  _cl_command_node *command_list;

  /* submitted batches, completed in order by done_thread */
  pthread_t done_thread;
  pocl_lock_t done_lock;
  pocl_cond_t done_cond;
  gen7_job_t *inflight_head, *inflight_tail;
  int shutdown;

  cl_image_format image_formats[GEN7_MAX_IMAGE_FORMATS];
  cl_uint num_image_formats;
} gen7_device_t;

/* mem->device_ptrs[dev->global_mem_id].mem_ptr */
typedef struct gen7_mem
{
  cl_buffer bo;
  size_t offset; /* of the data in the bo (userptr: host pointer's page offset) */
  size_t size;   /* bo size */
  int userptr;   /* bo wraps the application's host pointer */
  /* images only */
  int is_image;
  gen7_image_layout_t layout;
  uint32_t intel_fmt;
  uint32_t bpp;
} gen7_mem_t;

/* program->data[device_i] */
typedef struct gen7_program
{
  gbe_program gbe;
} gen7_program_t;

/* kernel->data[device_i]: one per cl_kernel instance */
typedef struct gen7_kernel
{
  gbe_kernel gbe;
  cl_buffer code_bo;
  uint32_t simd;
  uint32_t curbe_size;
  uint32_t samplers[GEN7_MAX_SAMPLERS]; /* compile-time samplers + arg slots */
  uint32_t sampler_sz;
  ImageInfo *images;
  uint32_t image_sz;
  int32_t stack_size;
  gen7_curbe_layout_t layout;
} gen7_kernel_t;

/* pocl-gen7.c */
uint64_t gen7_gpu_timestamp_ns (gen7_device_t *d);

/* gen7-image-fmt.c */
cl_int cl_image_byte_per_pixel (const cl_image_format *fmt, uint32_t *bpp);
uint32_t cl_image_get_intel_format (const cl_image_format *fmt);
cl_uint gen7_supported_image_formats (cl_image_format *out, cl_uint max);
#define INTEL_UNSUPPORTED_FORMAT ((uint32_t)~0x0u)

/* gen7-mem.c */
void *gen7_mem_map (gen7_mem_t *m, int write);
void gen7_mem_unmap (gen7_mem_t *m);
cl_int pocl_gen7_import_dmabuf (cl_device_id dev, cl_mem mem, int fd,
                                size_t size, size_t offset, size_t row_pitch);
cl_int pocl_gen7_export_dmabuf (cl_device_id dev, cl_mem mem, int *fd);
int gen7_image_info (cl_device_id dev, cl_mem img, gen7_image_layout_t *layout,
                     uint32_t *intel_fmt, uint32_t *bpp);

/* gen7-spirv.c */
int gen7_spirv_unsupported (const char *il, size_t size, char *why,
                            size_t why_len);
int gen7_spirv_to_bitcode (cl_program program, cl_uint device_i, char **bc,
                           uint64_t *bc_size);
int gen7_spirv_roundtrip (cl_program program, cl_uint device_i);

/* gen7-launch.c */
cl_int gen7_launch_submit (gen7_device_t *d, _cl_command_node *cmd,
                           gen7_job_t **job_out);
void gen7_launch_finish (gen7_device_t *d, gen7_job_t *job);

#endif /* GEN7_DRIVER_INTERNAL */

#endif
