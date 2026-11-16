/* gen7-mem.c - buffers and images of the gen7 driver

   Every cl_mem is one i915 buffer object. Transfers run on the CPU through
   bo maps: IVB shares the LLC with the CPU, and mapping a bo waits for the
   GPU work that uses it. Tiled images are accessed through GTT maps
   (hardware detiling), like beignet does.
*/

#include "pocl-gen7.h"
#include "gen7-trace.h"

#include "common.h"
#include "common_driver.h"
#include "pocl_util.h"

#include <intel_bufmgr.h>
#include <i915_drm.h>
#include <string.h>
#include <unistd.h>

#define GEN7_PAGE 4096
#define GEN7_ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

static gen7_device_t *
gen7_dev (cl_device_id dev)
{
  return dev->data;
}

void *
gen7_mem_map (gen7_mem_t *m, int write)
{
  if (m->is_image && m->layout.tiling != GEN7_NO_TILE)
    cl_buffer_map_gtt (m->bo);
  else
    cl_buffer_map (m->bo, 1);
  return (char *)cl_buffer_get_virtual (m->bo) + m->offset;
}

void
gen7_mem_unmap (gen7_mem_t *m)
{
  if (m->is_image && m->layout.tiling != GEN7_NO_TILE)
    cl_buffer_unmap_gtt (m->bo);
  else
    cl_buffer_unmap (m->bo);
}

/* Layout of an image as the hardware sees it (also for 1D buffer images,
   which live in their buffer's bo). */
int
gen7_image_info (cl_device_id dev, cl_mem img, gen7_image_layout_t *layout,
                 uint32_t *intel_fmt, uint32_t *bpp)
{
  cl_image_format fmt
      = { img->image_channel_order, img->image_channel_data_type };
  if (cl_image_byte_per_pixel (&fmt, bpp) != CL_SUCCESS)
    return -1;
  *intel_fmt = cl_image_get_intel_format (&fmt);
  if (*intel_fmt == INTEL_UNSUPPORTED_FORMAT)
    return -1;
  size_t depth = img->type == CL_MEM_OBJECT_IMAGE3D ? img->image_depth
                                                    : img->image_array_size;
  return gen7_image_layout (img->type, img->image_width, img->image_height,
                            depth, *bpp, gen7_dev (dev)->default_tiling,
                            dev->image2d_max_width, layout);
}

cl_int
pocl_gen7_alloc_mem_obj (cl_device_id dev, cl_mem mem, void *host_ptr)
{
  gen7_device_t *d = gen7_dev (dev);
  pocl_mem_identifier *p = &mem->device_ptrs[dev->global_mem_id];
  if (p->mem_ptr != NULL)
    return CL_SUCCESS;

  gen7_mem_t *m = calloc (1, sizeof (gen7_mem_t));
  if (m == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  if (mem->is_image && mem->buffer != NULL)
    {
      /* 1D image buffer or 2D image from a buffer: storage is the buffer's
         bo, linear, with the image's row pitch */
      cl_mem b = mem->buffer;
      cl_int err = pocl_gen7_alloc_mem_obj (dev, b, NULL);
      if (err != CL_SUCCESS)
        {
          free (m);
          return err;
        }
      gen7_mem_t *bm = b->device_ptrs[dev->global_mem_id].mem_ptr;
      cl_buffer_reference (bm->bo);
      m->bo = bm->bo;
      m->offset = bm->offset;
      m->size = bm->size;
      m->is_image = 1;
      gen7_image_info (dev, mem, &m->layout, &m->intel_fmt, &m->bpp);
      m->layout.tiling = GEN7_NO_TILE;
      if (mem->type == CL_MEM_OBJECT_IMAGE2D)
        {
          m->layout.row_pitch = mem->image_row_pitch;
          m->layout.size = mem->image_row_pitch * mem->image_height;
        }
    }
  else if (mem->is_image)
    {
      m->is_image = 1;
      if (gen7_image_info (dev, mem, &m->layout, &m->intel_fmt, &m->bpp))
        {
          free (m);
          return CL_IMAGE_FORMAT_NOT_SUPPORTED;
        }
      POCL_LOCK (d->hw_lock);
      m->bo = cl_buffer_alloc (d->bufmgr, "CL image", m->layout.size, 64);
      if (m->bo && m->layout.tiling != GEN7_NO_TILE)
        cl_buffer_set_tiling (m->bo, m->layout.tiling, m->layout.row_pitch);
      POCL_UNLOCK (d->hw_lock);
      m->size = m->layout.size;
      gen7_trace ("image %p alloc type %#x %zux%zux%zu order %#x type %#x "
                  "bpp %u tiling %d pitch %zu size %zu",
                  (void *)mem, mem->type, mem->image_width, mem->image_height,
                  mem->image_depth, mem->image_channel_order,
                  mem->image_channel_data_type, m->bpp, m->layout.tiling,
                  m->layout.row_pitch, m->layout.size);
    }
  else
    {
      /* beignet: zero copy for USE_HOST_PTR when pointer and size are
         cache line aligned; the bo starts at the page below the pointer */
      void *hp = mem->mem_host_ptr;
      if ((mem->flags & CL_MEM_USE_HOST_PTR) && hp
          && ((uintptr_t)hp % 64) == 0 && (mem->size % 64) == 0)
        {
          uintptr_t base = (uintptr_t)hp & ~(uintptr_t)(GEN7_PAGE - 1);
          m->offset = (uintptr_t)hp - base;
          m->size = GEN7_ALIGN (mem->size + m->offset, GEN7_PAGE);
          POCL_LOCK (d->hw_lock);
          m->bo = cl_buffer_alloc_userptr (d->bufmgr, "CL userptr memory object",
                                           (void *)base, m->size, 0);
          POCL_UNLOCK (d->hw_lock);
          if (m->bo)
            m->userptr = 1;
          else
            m->offset = 0;
        }
      if (m->bo == NULL)
        {
          m->size = mem->size;
          POCL_LOCK (d->hw_lock);
          m->bo = cl_buffer_alloc (d->bufmgr, "CL memory object", m->size, 64);
          POCL_UNLOCK (d->hw_lock);
        }
    }

  if (m->bo == NULL)
    {
      free (m);
      return CL_MEM_OBJECT_ALLOCATION_FAILURE;
    }

  p->mem_ptr = m;
  p->device_addr = NULL;
  /* a userptr bo *is* the host copy */
  p->version = m->userptr ? mem->mem_host_ptr_version : 0;
  return CL_SUCCESS;
}

/* clCreateBufferFromFdINTEL / clCreateImageFromFdINTEL (beignet's
   cl_mem_new_image_from_fd): the dma-buf's GEM object is the device memory.
   Its tiling was chosen by whoever allocated it (VA-API surfaces are
   usually Y-tiled) and is read back from the kernel, never changed. */
cl_int
pocl_gen7_import_dmabuf (cl_device_id dev, cl_mem mem, int fd, size_t size,
                         size_t offset, size_t row_pitch)
{
  gen7_device_t *d = gen7_dev (dev);
  pocl_mem_identifier *p = &mem->device_ptrs[dev->global_mem_id];
  POCL_RETURN_ERROR_ON ((p->mem_ptr != NULL), CL_INVALID_MEM_OBJECT,
                        "memory object already has device memory\n");
  gen7_mem_t *m = calloc (1, sizeof (gen7_mem_t));
  if (m == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  POCL_LOCK (d->hw_lock);
  m->bo = (cl_buffer)drm_intel_bo_gem_create_from_prime (
      (drm_intel_bufmgr *)d->bufmgr, fd, (int)size);
  uint32_t tiling = I915_TILING_NONE, swizzle;
  if (m->bo)
    drm_intel_bo_get_tiling ((drm_intel_bo *)m->bo, &tiling, &swizzle);
  POCL_UNLOCK (d->hw_lock);
  if (m->bo == NULL)
    {
      free (m);
      POCL_RETURN_ERROR_ON (1, CL_INVALID_VALUE,
                            "can't import dma-buf fd %d (%zu bytes)\n", fd,
                            size);
    }
  m->offset = offset;
  m->size = size;
  if (mem->is_image)
    {
      m->is_image = 1;
      if (gen7_image_info (dev, mem, &m->layout, &m->intel_fmt, &m->bpp))
        {
          cl_buffer_unreference (m->bo);
          free (m);
          return CL_IMAGE_FORMAT_NOT_SUPPORTED;
        }
      m->layout.tiling = tiling == I915_TILING_X   ? GEN7_TILE_X
                         : tiling == I915_TILING_Y ? GEN7_TILE_Y
                                                   : GEN7_NO_TILE;
      m->layout.row_pitch = row_pitch;
      m->layout.size = size;
      mem->image_row_pitch = row_pitch;
    }

  gen7_trace ("import %p fd %d size %zu offset %zu pitch %zu tiling %u image "
              "%d %zux%zu order %#x type %#x bpp %u",
              (void *)mem, fd, size, offset, row_pitch, tiling, mem->is_image,
              mem->image_width, mem->image_height, mem->image_channel_order,
              mem->image_channel_data_type, m->bpp);
  p->mem_ptr = m;
  p->device_addr = NULL;
  /* the contents are the device's: nothing to migrate in */
  p->version = mem->latest_version;
  return CL_SUCCESS;
}

/* clGetMemObjectFdIntel */
cl_int
pocl_gen7_export_dmabuf (cl_device_id dev, cl_mem mem, int *fd)
{
  gen7_mem_t *m = mem->device_ptrs[dev->global_mem_id].mem_ptr;
  if (m == NULL)
    return CL_INVALID_MEM_OBJECT;
  return drm_intel_bo_gem_export_to_prime ((drm_intel_bo *)m->bo, fd) ? CL_INVALID_OPERATION
                                                      : CL_SUCCESS;
}

void
pocl_gen7_free (cl_device_id dev, cl_mem mem)
{
  pocl_mem_identifier *p = &mem->device_ptrs[dev->global_mem_id];
  gen7_mem_t *m = p->mem_ptr;
  if (m == NULL)
    return;
  cl_buffer_unreference (m->bo);
  free (m);
  p->mem_ptr = NULL;
  p->version = 0;
}

/* A sub-buffer shares its parent's bo; pocl would otherwise store
   "parent mem_ptr + origin", which is not a gen7_mem_t. */
cl_int
pocl_gen7_alloc_subbuffer (cl_device_id dev, cl_mem sub)
{
  pocl_mem_identifier *p = &sub->device_ptrs[dev->global_mem_id];
  gen7_mem_t *pm = sub->parent->device_ptrs[dev->global_mem_id].mem_ptr;
  gen7_mem_t *m = calloc (1, sizeof (gen7_mem_t));
  if (m == NULL)
    return CL_OUT_OF_HOST_MEMORY;
  *m = *pm;
  m->offset = pm->offset + sub->origin;
  cl_buffer_reference (m->bo);
  p->mem_ptr = m;
  return CL_SUCCESS;
}

void
pocl_gen7_free_subbuffer (cl_device_id dev, cl_mem sub)
{
  pocl_gen7_free (dev, sub);
}

/****************************** buffers ***********************************/

void
pocl_gen7_read (void *data, void *__restrict__ dst, pocl_mem_identifier *src_id,
                cl_mem src_buf, size_t offset, size_t size)
{
  gen7_mem_t *m = src_id->mem_ptr;
  double t0 = gen7_trace_ms ();
  char *p = gen7_mem_map (m, 0);
  if (p + offset != dst)
    memcpy (dst, p + offset, size);
  gen7_mem_unmap (m);
  if (gen7_trace_on ())
    gen7_trace ("read %p %zu bytes tiling %d %.2f ms", (void *)src_buf, size,
                m->is_image ? m->layout.tiling : 0, gen7_trace_ms () - t0);
}

void
pocl_gen7_write (void *data, const void *__restrict__ src,
                 pocl_mem_identifier *dst_id, cl_mem dst_buf, size_t offset,
                 size_t size)
{
  gen7_mem_t *m = dst_id->mem_ptr;
  double t0 = gen7_trace_ms ();
  char *p = gen7_mem_map (m, 1);
  if (p + offset != src)
    memcpy (p + offset, src, size);
  gen7_mem_unmap (m);
  if (gen7_trace_on ())
    gen7_trace ("write %p %zu bytes tiling %d %.2f ms", (void *)dst_buf, size,
                m->is_image ? m->layout.tiling : 0, gen7_trace_ms () - t0);
}

void
pocl_gen7_copy (void *data, pocl_mem_identifier *dst_id, cl_mem dst_buf,
                pocl_mem_identifier *src_id, cl_mem src_buf, size_t dst_offset,
                size_t src_offset, size_t size)
{
  gen7_mem_t *dm = dst_id->mem_ptr, *sm = src_id->mem_ptr;
  char *dp = gen7_mem_map (dm, 1);
  char *sp = (sm->bo == dm->bo) ? dp - dm->offset + sm->offset
                                : gen7_mem_map (sm, 0);
  memmove (dp + dst_offset, sp + src_offset, size);
  if (sm->bo != dm->bo)
    gen7_mem_unmap (sm);
  gen7_mem_unmap (dm);
}

void
pocl_gen7_read_rect (void *data, void *__restrict__ dst_host_ptr,
                     pocl_mem_identifier *src_id, cl_mem src_buf,
                     const size_t *buffer_origin, const size_t *host_origin,
                     const size_t *region, size_t buffer_row_pitch,
                     size_t buffer_slice_pitch, size_t host_row_pitch,
                     size_t host_slice_pitch)
{
  gen7_mem_t *m = src_id->mem_ptr;
  char *p = gen7_mem_map (m, 0);
  pocl_driver_read_rect_memcpy (data, dst_host_ptr, p, buffer_origin,
                                host_origin, region, buffer_row_pitch,
                                buffer_slice_pitch, host_row_pitch,
                                host_slice_pitch);
  gen7_mem_unmap (m);
}

void
pocl_gen7_write_rect (void *data, const void *__restrict__ src_host_ptr,
                      pocl_mem_identifier *dst_id, cl_mem dst_buf,
                      const size_t *buffer_origin, const size_t *host_origin,
                      const size_t *region, size_t buffer_row_pitch,
                      size_t buffer_slice_pitch, size_t host_row_pitch,
                      size_t host_slice_pitch)
{
  gen7_mem_t *m = dst_id->mem_ptr;
  char *p = gen7_mem_map (m, 1);
  pocl_driver_write_rect_memcpy (data, src_host_ptr, p, buffer_origin,
                                 host_origin, region, buffer_row_pitch,
                                 buffer_slice_pitch, host_row_pitch,
                                 host_slice_pitch);
  gen7_mem_unmap (m);
}

void
pocl_gen7_copy_rect (void *data, pocl_mem_identifier *dst_id, cl_mem dst_buf,
                     pocl_mem_identifier *src_id, cl_mem src_buf,
                     const size_t *dst_origin, const size_t *src_origin,
                     const size_t *region, size_t dst_row_pitch,
                     size_t dst_slice_pitch, size_t src_row_pitch,
                     size_t src_slice_pitch)
{
  gen7_mem_t *dm = dst_id->mem_ptr, *sm = src_id->mem_ptr;
  char *dp = gen7_mem_map (dm, 1);
  char *sp = (sm->bo == dm->bo) ? dp - dm->offset + sm->offset
                                : gen7_mem_map (sm, 0);
  pocl_driver_copy_rect_memcpy (NULL, dp, sp, dst_origin, src_origin, region,
                                dst_row_pitch, dst_slice_pitch, src_row_pitch,
                                src_slice_pitch);
  if (sm->bo != dm->bo)
    gen7_mem_unmap (sm);
  gen7_mem_unmap (dm);
}

void
pocl_gen7_memfill (void *data, pocl_mem_identifier *dst_id, cl_mem dst_buf,
                   size_t size, size_t offset, const void *__restrict__ pattern,
                   size_t pattern_size)
{
  gen7_mem_t *m = dst_id->mem_ptr;
  char *p = gen7_mem_map (m, 1);
  pocl_fill_aligned_buf_with_pattern (p, offset, size, pattern, pattern_size);
  gen7_mem_unmap (m);
}

cl_int
pocl_gen7_map_mem (void *data, pocl_mem_identifier *src_id, cl_mem src_buf,
                   mem_mapping_t *map)
{
  if (map->map_flags & CL_MAP_WRITE_INVALIDATE_REGION)
    return CL_SUCCESS;
  pocl_gen7_read (data, map->host_ptr, src_id, src_buf, map->offset,
                  map->size);
  return CL_SUCCESS;
}

cl_int
pocl_gen7_unmap_mem (void *data, pocl_mem_identifier *dst_id, cl_mem dst_buf,
                     mem_mapping_t *map)
{
  if (map->map_flags != CL_MAP_READ)
    pocl_gen7_write (data, map->host_ptr, dst_id, dst_buf, map->offset,
                     map->size);
  return CL_SUCCESS;
}

/****************************** images ************************************/

/* Byte origin/region and pitches of an image region in our layout. For 1D
   arrays pocl puts the layer in [1]; our layers are slice_pitch apart. */
static void
gen7_image_rect (cl_mem img, gen7_mem_t *m, const size_t *origin,
                 const size_t *region, size_t o[3], size_t r[3],
                 size_t *row_pitch, size_t *slice_pitch)
{
  uint32_t bpp = m->bpp;
  if (!m->is_image) /* the buffer behind a 2D image from a buffer */
    {
      cl_image_format f
          = { img->image_channel_order, img->image_channel_data_type };
      cl_image_byte_per_pixel (&f, &bpp);
    }
  size_t px = bpp;
  o[0] = origin[0] * px;
  o[1] = origin[1];
  o[2] = origin[2];
  r[0] = region[0] * px;
  r[1] = region[1];
  r[2] = region[2];
  *row_pitch = m->layout.row_pitch;
  *slice_pitch = m->layout.slice_pitch;
  if (img->type == CL_MEM_OBJECT_IMAGE1D_ARRAY)
    *row_pitch = m->layout.slice_pitch;
  if (img->type == CL_MEM_OBJECT_IMAGE1D_BUFFER)
    *row_pitch = img->image_width * px; /* one linear scanline */
  else if (img->buffer != NULL)
    {
      /* 2D image from a buffer: pocl may hand us the buffer's memory,
         which has no image layout; the image is linear with its pitch */
      *row_pitch = img->image_row_pitch;
      *slice_pitch = 0;
    }
}

static void *
gen7_buffer_or_host (pocl_mem_identifier *id, const void *host, size_t offset,
                     gen7_mem_t **mapped)
{
  *mapped = NULL;
  if (host)
    return (char *)host + offset;
  *mapped = id->mem_ptr;
  return (char *)gen7_mem_map (*mapped, 1) + offset;
}

cl_int
pocl_gen7_read_image_rect (void *data, cl_mem src_image,
                           pocl_mem_identifier *src_id,
                           void *__restrict__ dst_host_ptr,
                           pocl_mem_identifier *dst_id, const size_t *origin,
                           const size_t *region, size_t dst_row_pitch,
                           size_t dst_slice_pitch, size_t dst_offset)
{
  gen7_mem_t *m = src_id->mem_ptr, *bm;
  size_t o[3], r[3], rp, sp;
  const size_t zero[3] = { 0, 0, 0 };
  gen7_image_rect (src_image, m, origin, region, o, r, &rp, &sp);
  if (dst_row_pitch == 0)
    dst_row_pitch = r[0];
  if (dst_slice_pitch == 0)
    dst_slice_pitch = dst_row_pitch * region[1];
  double t0 = gen7_trace_ms ();
  char *dst = gen7_buffer_or_host (dst_id, dst_host_ptr, dst_offset, &bm);
  char *p = gen7_mem_map (m, 0);
  pocl_driver_read_rect_memcpy (data, dst, p, o, zero, r, rp, sp,
                                dst_row_pitch, dst_slice_pitch);
  gen7_mem_unmap (m);
  if (bm)
    gen7_mem_unmap (bm);
  if (gen7_trace_on ())
    gen7_trace ("read_image %p origin %zu,%zu region %zux%zux%zu pitch %zu "
                "-> %zu tiling %d %.2f ms",
                (void *)src_image, origin[0], origin[1], region[0], region[1],
                region[2], rp, dst_row_pitch, m->layout.tiling,
                gen7_trace_ms () - t0);
  return CL_SUCCESS;
}

cl_int
pocl_gen7_write_image_rect (void *data, cl_mem dst_image,
                            pocl_mem_identifier *dst_id,
                            const void *__restrict__ src_host_ptr,
                            pocl_mem_identifier *src_id, const size_t *origin,
                            const size_t *region, size_t src_row_pitch,
                            size_t src_slice_pitch, size_t src_offset)
{
  gen7_mem_t *m = dst_id->mem_ptr, *bm;
  size_t o[3], r[3], rp, sp;
  const size_t zero[3] = { 0, 0, 0 };
  gen7_image_rect (dst_image, m, origin, region, o, r, &rp, &sp);
  if (src_row_pitch == 0)
    src_row_pitch = r[0];
  if (src_slice_pitch == 0)
    src_slice_pitch = src_row_pitch * region[1];
  double t0 = gen7_trace_ms ();
  const char *src = gen7_buffer_or_host (src_id, src_host_ptr, src_offset, &bm);
  char *p = gen7_mem_map (m, 1);
  pocl_driver_write_rect_memcpy (data, src, p, o, zero, r, rp, sp,
                                 src_row_pitch, src_slice_pitch);
  gen7_mem_unmap (m);
  if (bm)
    gen7_mem_unmap (bm);
  if (gen7_trace_on ())
    gen7_trace ("write_image %p origin %zu,%zu region %zux%zux%zu %zu -> "
                "pitch %zu tiling %d %.2f ms",
                (void *)dst_image, origin[0], origin[1], region[0], region[1],
                region[2], src_row_pitch, rp, m->layout.tiling,
                gen7_trace_ms () - t0);
  return CL_SUCCESS;
}

cl_int
pocl_gen7_copy_image_rect (void *data, cl_mem src_image, cl_mem dst_image,
                           pocl_mem_identifier *src_id,
                           pocl_mem_identifier *dst_id,
                           const size_t *src_origin, const size_t *dst_origin,
                           const size_t *region)
{
  gen7_mem_t *sm = src_id->mem_ptr, *dm = dst_id->mem_ptr;
  size_t so[3], doo[3], r[3], srp, ssp, drp, dsp;
  gen7_image_rect (src_image, sm, src_origin, region, so, r, &srp, &ssp);
  gen7_image_rect (dst_image, dm, dst_origin, region, doo, r, &drp, &dsp);

  double t0 = gen7_trace_ms ();
  char *dp = gen7_mem_map (dm, 1);
  char *sp = (sm->bo == dm->bo) ? dp : gen7_mem_map (sm, 0);
  pocl_driver_copy_rect_memcpy (NULL, dp, sp, doo, so, r, drp, dsp, srp, ssp);
  if (sm->bo != dm->bo)
    gen7_mem_unmap (sm);
  gen7_mem_unmap (dm);
  if (gen7_trace_on ())
    gen7_trace ("copy_image %p -> %p region %zux%zux%zu tiling %d -> %d "
                "%.2f ms",
                (void *)src_image, (void *)dst_image, region[0], region[1],
                region[2], sm->layout.tiling, dm->layout.tiling,
                gen7_trace_ms () - t0);
  return CL_SUCCESS;
}

cl_int
pocl_gen7_map_image (void *data, pocl_mem_identifier *mem_id,
                     cl_mem src_image, mem_mapping_t *map)
{
  if (map->map_flags & CL_MAP_WRITE_INVALIDATE_REGION)
    return CL_SUCCESS;
  return pocl_gen7_read_image_rect (data, src_image, mem_id, map->host_ptr,
                                    NULL, map->origin, map->region,
                                    map->row_pitch, map->slice_pitch, 0);
}

cl_int
pocl_gen7_unmap_image (void *data, pocl_mem_identifier *mem_id,
                       cl_mem dst_image, mem_mapping_t *map)
{
  if (map->map_flags == CL_MAP_READ)
    return CL_SUCCESS;
  return pocl_gen7_write_image_rect (data, dst_image, mem_id, map->host_ptr,
                                     NULL, map->origin, map->region,
                                     map->row_pitch, map->slice_pitch, 0);
}

cl_int
pocl_gen7_fill_image (void *data, cl_mem image, pocl_mem_identifier *mem_id,
                      const size_t *origin, const size_t *region,
                      cl_uint4 orig_pixel, pixel_t fill_pixel,
                      size_t pixel_size)
{
  gen7_mem_t *m = mem_id->mem_ptr;
  size_t o[3], r[3], rp, sp;
  gen7_image_rect (image, m, origin, region, o, r, &rp, &sp);
  char *p = gen7_mem_map (m, 1);
  for (size_t z = 0; z < r[2]; z++)
    for (size_t y = 0; y < r[1]; y++)
      {
        char *row = p + (o[2] + z) * sp + (o[1] + y) * rp + o[0];
        for (size_t x = 0; x < region[0]; x++)
          memcpy (row + x * pixel_size, fill_pixel, pixel_size);
      }
  gen7_mem_unmap (m);
  return CL_SUCCESS;
}
