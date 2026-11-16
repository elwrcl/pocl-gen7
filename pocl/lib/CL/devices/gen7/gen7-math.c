/* gen7-math.c - pure helpers of the gen7 driver, see gen7-math.h */

#include "gen7-math.h"

#include <CL/cl.h>
#include <string.h>

#define GEN7_ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

void
gen7_pick_local_size (unsigned work_dim, const size_t global[3],
                      size_t local[3])
{
  /* beignet clEnqueueNDRangeKernel: per dimension, the largest divisor of
     the global size up to 64, keeping the product <= 256. */
  unsigned i, j, max_dim_size = 64, max_group_size = 256;
  local[0] = local[1] = local[2] = 1;
  for (i = 0; i < work_dim; i++)
    for (j = max_dim_size; j > 1; j--)
      if (global[i] % j == 0 && j <= max_group_size)
        {
          local[i] = j;
          max_group_size = max_group_size / j;
          max_dim_size
              = max_group_size > max_dim_size ? max_dim_size : max_group_size;
          break;
        }
}

void
gen7_curbe_fill_system (char *curbe, const gen7_curbe_layout_t *l,
                        unsigned work_dim, const size_t offset[3],
                        const size_t global[3], const size_t local[3],
                        const size_t enqueued_local[3], size_t thread_n)
{
#define UPLOAD(OFF, VALUE)                                                    \
  if ((OFF) >= 0)                                                             \
    *(uint32_t *)(curbe + (OFF)) = (uint32_t)(VALUE);
  for (unsigned d = 0; d < 3; d++)
    {
      UPLOAD (l->local_size[d], local[d]);
      UPLOAD (l->enqueued_local_size[d], enqueued_local[d]);
      UPLOAD (l->global_size[d], global[d]);
      UPLOAD (l->global_offset[d], offset[d]);
      UPLOAD (l->group_num[d], global[d] / enqueued_local[d]
                                   + (global[d] % enqueued_local[d] ? 1 : 0));
    }
  UPLOAD (l->thread_num, thread_n);
  UPLOAD (l->work_dim, work_dim);
#undef UPLOAD
}

void
gen7_fill_varying (char *data, const gen7_curbe_layout_t *l,
                   const size_t local[3], size_t simd_sz, size_t cst_sz,
                   size_t thread_n)
{
  /* beignet cl_set_varying_payload. Lanes past the end of the work-group
     get block IP 0xffff, which disables them. */
  size_t i, j, k, curr = 0;
  const size_t lanes = thread_n * simd_sz;
  for (k = 0; k < local[2]; ++k)
    for (j = 0; j < local[1]; ++j)
      for (i = 0; i < local[0]; ++i, ++curr)
        {
          char *t = data + (curr / simd_sz) * cst_sz;
          size_t lane = curr % simd_sz;
          if (l->local_id[0] >= 0)
            ((uint32_t *)(t + l->local_id[0]))[lane] = (uint32_t)i;
          if (l->local_id[1] >= 0)
            ((uint32_t *)(t + l->local_id[1]))[lane] = (uint32_t)j;
          if (l->local_id[2] >= 0)
            ((uint32_t *)(t + l->local_id[2]))[lane] = (uint32_t)k;
          if (l->block_ip >= 0)
            ((uint16_t *)(t + l->block_ip))[lane] = 0;
          if (l->dw_block_ip >= 0)
            ((uint32_t *)(t + l->dw_block_ip))[lane] = 0;
        }
  for (; curr < lanes; ++curr)
    {
      char *t = data + (curr / simd_sz) * cst_sz;
      size_t lane = curr % simd_sz;
      if (l->local_id[0] >= 0)
        ((uint32_t *)(t + l->local_id[0]))[lane] = 0;
      if (l->local_id[1] >= 0)
        ((uint32_t *)(t + l->local_id[1]))[lane] = 0;
      if (l->local_id[2] >= 0)
        ((uint32_t *)(t + l->local_id[2]))[lane] = 0;
      if (l->block_ip >= 0)
        ((uint16_t *)(t + l->block_ip))[lane] = 0xffff;
      if (l->dw_block_ip >= 0)
        ((uint32_t *)(t + l->dw_block_ip))[lane] = 0xffff;
    }
  if (l->thread_id >= 0)
    for (i = 0; i < thread_n; ++i)
      *(uint32_t *)(data + i * cst_sz + l->thread_id) = (uint32_t)i;
}

uint32_t
gen7_tiling_align (int tiling, unsigned dim)
{
  /* intel_buffer_get_tiling_align for gen_ver 7 */
  switch (tiling)
    {
    case GEN7_TILE_X:
      return dim == 0 ? 512 : dim == 1 ? 8 : 2;
    case GEN7_TILE_Y:
      return dim == 0 ? 128 : dim == 1 ? 32 : 2;
    default:
      return 2; /* CL_NO_TILE: vertical alignment */
    }
}

int
gen7_image_layout (unsigned type, size_t w, size_t h, size_t depth,
                   size_t bpp, int default_tiling, size_t max_2d_width,
                   gen7_image_layout_t *out)
{
  int tiling = GEN7_NO_TILE;
  size_t pitch, aligned_h;

  switch (type)
    {
    case CL_MEM_OBJECT_IMAGE1D:
      h = depth = 1;
      break;
    case CL_MEM_OBJECT_IMAGE1D_BUFFER:
      /* simulated with a 2D image when wider than the 2D limit */
      h = (w + max_2d_width - 1) / max_2d_width;
      w = w > max_2d_width ? max_2d_width : w;
      depth = 1;
      break;
    case CL_MEM_OBJECT_IMAGE2D:
      tiling = default_tiling;
      depth = 1;
      break;
    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
      h = 1;
      break;
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
    case CL_MEM_OBJECT_IMAGE3D:
      tiling = default_tiling;
      break;
    default:
      return -1;
    }

  for (;;)
    {
      if (tiling == GEN7_NO_TILE)
        pitch = w * bpp;
      else
        pitch = GEN7_ALIGN (w * bpp, gen7_tiling_align (tiling, 0));
      aligned_h = GEN7_ALIGN (h, gen7_tiling_align (tiling, 1));
      out->size = pitch * aligned_h * depth;
      if (tiling != GEN7_NO_TILE && out->size > GEN7_MAX_TILING_SIZE)
        {
          tiling = GEN7_NO_TILE;
          continue;
        }
      break;
    }

  out->w = w;
  out->h = h;
  out->depth = depth;
  out->tiling = tiling;
  out->row_pitch = pitch;
  if (type == CL_MEM_OBJECT_IMAGE1D || type == CL_MEM_OBJECT_IMAGE2D
      || type == CL_MEM_OBJECT_IMAGE1D_BUFFER)
    out->slice_pitch = 0;
  else
    out->slice_pitch = pitch * GEN7_ALIGN (h, gen7_tiling_align (tiling, 2));
  return 0;
}
