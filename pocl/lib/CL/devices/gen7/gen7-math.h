/* gen7-math.h - pure helpers of the gen7 driver (no GPU, no libgbe needed)

   Ported from beignet: local size selection (cl_api_kernel.c), curbe
   filling (cl_command_queue_gen7.c) and image layout (cl_mem.c). Kept free
   of hardware calls so they can be unit tested on any machine.
*/

#ifndef POCL_GEN7_MATH_H
#define POCL_GEN7_MATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* beignet's choice when the application passes local_work_size = NULL */
void gen7_pick_local_size (unsigned work_dim, const size_t global[3],
                           size_t local[3]);

/* Curbe offsets of the system values of one kernel; -1 = not used.
   Filled from libgbe (interp_kernel_get_curbe_offset) by gen7-launch.c. */
typedef struct gen7_curbe_layout
{
  int32_t local_size[3];
  int32_t enqueued_local_size[3];
  int32_t global_size[3];
  int32_t global_offset[3];
  int32_t group_num[3];
  int32_t thread_num;
  int32_t work_dim;
  int32_t local_id[3];
  int32_t block_ip;
  int32_t dw_block_ip;
  int32_t thread_id;
} gen7_curbe_layout_t;

/* Writes the values shared by all threads of a launch into curbe. */
void gen7_curbe_fill_system (char *curbe, const gen7_curbe_layout_t *l,
                             unsigned work_dim, const size_t offset[3],
                             const size_t global[3], const size_t local[3],
                             const size_t enqueued_local[3], size_t thread_n);

/* final_curbe holds thread_n copies of the shared curbe (cst_sz bytes each);
   writes the per-thread local ids, block IPs and thread ids into them. */
void gen7_fill_varying (char *final_curbe, const gen7_curbe_layout_t *l,
                        const size_t local[3], size_t simd_sz, size_t cst_sz,
                        size_t thread_n);

/* Number of HW threads for one work-group */
static inline size_t
gen7_thread_count (const size_t local[3], size_t simd_sz)
{
  size_t n = local[0] * local[1] * local[2];
  return (n + simd_sz - 1) / simd_sz;
}

/* Image layout (beignet _cl_mem_new_image for gen7). */
#define GEN7_NO_TILE 0
#define GEN7_TILE_X 1
#define GEN7_TILE_Y 2
/* beignet: "If sz is large than 128MB, map gtt may fail" -> no tiling */
#define GEN7_MAX_TILING_SIZE (128u * 1024 * 1024)

typedef struct gen7_image_layout
{
  /* hw view: image1d_buffer wider than image2d_max_width becomes 2D */
  size_t w, h, depth;
  int tiling;
  size_t row_pitch;
  size_t slice_pitch; /* 0 for 1D/2D */
  size_t size;        /* bo size */
} gen7_image_layout_t;

/* type: CL_MEM_OBJECT_IMAGE*; depth = depth (3D) or array size (arrays).
   default_tiling: GEN7_TILE_X normally, OCL_TILING env can override. */
int gen7_image_layout (unsigned type, size_t w, size_t h, size_t depth,
                       size_t bpp, int default_tiling, size_t max_2d_width,
                       gen7_image_layout_t *out);

/* beignet's alignment table for gen7 (intel_buffer_get_tiling_align) */
uint32_t gen7_tiling_align (int tiling, unsigned dim);

#ifdef __cplusplus
}
#endif

#endif
