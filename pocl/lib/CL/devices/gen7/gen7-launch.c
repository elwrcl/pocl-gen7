/* gen7-launch.c - NDRange launch on Gen7

   Port of beignet's cl_command_queue_ND_range_gen7 (cl_command_queue_gen7.c),
   cl_kernel_set_arg (cl_kernel.c) and cl_command_queue_bind_* (cl_command_
   queue.c), Copyright © 2012 Intel Corporation, LGPL 2.1+. Argument values
   come from pocl's command node instead of beignet's cl_kernel.
*/

#include "pocl-gen7.h"
#include "gen7-trace.h"

#include "common.h"
#include "pocl_util.h"

#include "intel_defines.h"
#include "ocl_common_defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GEN7_ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

/* POCL_GEN7_STATS=1: at exit, print how busy the GPU was with this
   process's kernels and how long a kernel took from submit to completion. */
static struct
{
  int enabled;
  uint64_t kernels, gpu_ns, wait_ns, first_ns, last_ns;
} gen7_stats;

/* an integer surface format of bpp bytes per pixel */
static uint32_t
gen7_raw_format (uint32_t bpp, uint32_t fmt)
{
  switch (bpp)
    {
    case 1:
      return I965_SURFACEFORMAT_R8_UINT;
    case 2:
      return I965_SURFACEFORMAT_R16_UINT;
    case 4:
      return I965_SURFACEFORMAT_R32_UINT;
    case 8:
      return I965_SURFACEFORMAT_R32G32_UINT;
    case 16:
      return I965_SURFACEFORMAT_R32G32B32A32_UINT;
    default:
      return fmt;
    }
}

static uint64_t
gen7_host_ns (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec;
}

static void
gen7_stats_print (void)
{
  if (gen7_stats.kernels == 0)
    return;
  double wall = (gen7_stats.last_ns - gen7_stats.first_ns) / 1e6;
  fprintf (stderr,
           "gen7 stats: %llu kernels in %.0f ms, GPU busy %.0f ms (%.1f%%), "
           "avg kernel %.1f us, avg submit->done %.1f us\n",
           (unsigned long long)gen7_stats.kernels, wall,
           gen7_stats.gpu_ns / 1e6, wall > 0 ? 100 * gen7_stats.gpu_ns / 1e6 / wall : 0,
           gen7_stats.gpu_ns / 1e3 / gen7_stats.kernels,
           gen7_stats.wait_ns / 1e3 / gen7_stats.kernels);
}

static int
gen7_stats_on (void)
{
  static int init = 0;
  if (!init)
    {
      init = 1;
      const char *e = getenv ("POCL_GEN7_STATS");
      gen7_stats.enabled = e && *e && *e != '0';
      if (gen7_stats.enabled)
        atexit (gen7_stats_print);
    }
  return gen7_stats.enabled;
}

static uint32_t
gen7_sampler_to_clk (cl_sampler s)
{
  /* beignet cl_to_clk */
  int clk_address = CLK_ADDRESS_NONE;
  int clk_filter = CLK_FILTER_NEAREST;
  switch (s->addressing_mode)
    {
    case CL_ADDRESS_CLAMP: clk_address = CLK_ADDRESS_CLAMP; break;
    case CL_ADDRESS_CLAMP_TO_EDGE: clk_address = CLK_ADDRESS_CLAMP_TO_EDGE; break;
    case CL_ADDRESS_REPEAT: clk_address = CLK_ADDRESS_REPEAT; break;
    case CL_ADDRESS_MIRRORED_REPEAT: clk_address = CLK_ADDRESS_MIRRORED_REPEAT; break;
    default: clk_address = CLK_ADDRESS_NONE; break;
    }
  if (s->filter_mode == CL_FILTER_LINEAR)
    clk_filter = CLK_FILTER_LINEAR;
  return (clk_address << __CLK_ADDRESS_BASE)
         | ((s->normalized_coords ? 1 : 0) << __CLK_NORMALIZED_BASE)
         | clk_filter;
}

#define IS_SAMPLER_ARG(v) (v & __CLK_SAMPLER_ARG_KEY_BIT)
#define SAMPLER_ARG_ID(v) ((v & __CLK_SAMPLER_ARG_MASK) >> __CLK_SAMPLER_ARG_BASE)

static void
gen7_set_sampler_arg_slot (uint32_t *samplers, uint32_t n, unsigned index,
                           uint32_t clk)
{
  for (uint32_t slot = 0; slot < n; slot++)
    if (IS_SAMPLER_ARG (samplers[slot]) && SAMPLER_ARG_ID (samplers[slot]) == index)
      {
        samplers[slot] = (samplers[slot] & (~__CLK_SAMPLER_MASK)) | clk;
        return;
      }
}

static gen7_mem_t *
gen7_arg_mem (cl_device_id dev, struct pocl_argument *a, cl_mem *mem_out)
{
  if (a->value == NULL)
    return NULL;
  cl_mem mem = *(cl_mem *)a->value;
  if (mem == NULL)
    return NULL;
  *mem_out = mem;
  if (mem->device_ptrs == NULL)
    return NULL;
  /* an image made from a buffer: pocl migrated the buffer; the image view
     of its bo is made on first use */
  if (mem->is_image && mem->buffer != NULL
      && mem->device_ptrs[dev->global_mem_id].mem_ptr == NULL)
    pocl_gen7_alloc_mem_obj (dev, mem, NULL);
  return mem->device_ptrs[dev->global_mem_id].mem_ptr;
}

/* OpenCL 1.2: program constants and every __constant argument go into one
   surface at BTI_CONSTANT (beignet cl_upload_constant_buffer). */
static int
gen7_upload_constants (cl_device_id dev, gen7_kernel_t *gk, gbe_program prog,
                       struct pocl_argument *args, char *curbe,
                       cl_gpgpu gpgpu)
{
  gbe_kernel k = gk->gbe;
  const int32_t arg_n = interp_kernel_get_arg_num (k);
  size_t global_const_size = interp_program_get_global_constant_size (prog);
  uint32_t raw_size = global_const_size;
  uint32_t aligned_size = GEN7_ALIGN (raw_size, 4);
  if (global_const_size == 0)
    aligned_size = 8; /* keep 0 an invalid address */

  for (int32_t i = 0; i < arg_n; ++i)
    {
      cl_mem mem;
      if (interp_kernel_get_arg_type (k, i) == GBE_ARG_CONSTANT_PTR
          && gen7_arg_mem (dev, &args[i], &mem))
        {
          uint32_t align = interp_kernel_get_arg_align (k, i);
          raw_size += mem->size;
          aligned_size = GEN7_ALIGN (aligned_size, align);
          aligned_size += mem->size;
        }
    }
  if (raw_size == 0)
    return 0;

  cl_buffer bo = cl_gpgpu_alloc_constant_buffer (gpgpu, aligned_size,
                                                 BTI_CONSTANT);
  if (bo == NULL)
    return -1;
  cl_buffer_map (bo, 1);
  char *cst = cl_buffer_get_virtual (bo);
  size_t offset = 0;
  if (global_const_size > 0)
    {
      interp_program_get_global_constant_data (prog, cst);
      offset += global_const_size;
    }
  else
    offset = 8;

  for (int32_t i = 0; i < arg_n; ++i)
    {
      cl_mem mem;
      gen7_mem_t *m;
      if (interp_kernel_get_arg_type (k, i) != GBE_ARG_CONSTANT_PTR
          || (m = gen7_arg_mem (dev, &args[i], &mem)) == NULL)
        continue;
      offset = GEN7_ALIGN (offset, interp_kernel_get_arg_align (k, i));
      int32_t curbe_offset = interp_kernel_get_curbe_offset (
          k, GBE_CURBE_KERNEL_ARGUMENT, i);
      if (curbe_offset < 0)
        continue;
      *(uint32_t *)(curbe + curbe_offset) = offset;
      char *src = gen7_mem_map (m, 0);
      memcpy (cst + offset, src + args[i].offset, mem->size - args[i].offset);
      gen7_mem_unmap (m);
      offset += mem->size;
    }
  cl_buffer_unmap (bo);
  return 0;
}

/* Builds the batch for an NDRange and submits it to the GPU without waiting.
   On success *job_out describes the batch; gen7_launch_finish completes it. */
cl_int
gen7_launch_submit (gen7_device_t *d, _cl_command_node *cmd,
                    gen7_job_t **job_out)
{
  cl_device_id dev = cmd->device;
  _cl_command_run *run = &cmd->command.run;
  cl_kernel kernel = run->kernel;
  unsigned dev_i = cmd->program_device_i;
  gen7_kernel_t *gk = kernel->data[dev_i];
  gen7_program_t *gp = kernel->program->data[dev_i];
  gbe_kernel k = gk->gbe;
  struct pocl_context *pc = &run->pc;
  cl_event event = cmd->sync.event.event;
  int profiling = event->queue
                  && (event->queue->properties & CL_QUEUE_PROFILING_ENABLE);
  int timestamps = profiling || gen7_stats_on () || gen7_trace_on ();

  size_t local[3], global[3], offset[3];
  for (int i = 0; i < 3; i++)
    {
      local[i] = pc->local_size[i];
      global[i] = pc->num_groups[i] * pc->local_size[i];
      offset[i] = pc->global_offset[i];
    }
  const size_t dim_off[3] = { 0, 0, 0 };
  const uint32_t simd = gk->simd;
  const size_t thread_n = gen7_thread_count (local, simd);
  size_t cst_sz = gk->curbe_size;
  int32_t scratch_sz = interp_kernel_get_scratch_size (k);
  size_t global_size = global[0] * global[1] * global[2];
  size_t local_size = local[0] * local[1] * local[2];

  if (scratch_sz > (int32_t)d->scratch_mem_size)
    {
      POCL_MSG_ERR ("gen7: out of scratch memory %d\n", scratch_sz);
      return CL_OUT_OF_RESOURCES;
    }

  char *curbe = calloc (1, cst_sz ? cst_sz : 32);
  uint32_t samplers[GEN7_MAX_SAMPLERS];
  memcpy (samplers, gk->samplers, sizeof (samplers));

  /* curbe step 1: system values and argument values */
  gen7_curbe_fill_system (curbe, &gk->layout, pc->work_dim, offset, global,
                          local, local, thread_n);

  const uint32_t arg_n = interp_kernel_get_arg_num (k);
  int32_t slm_offset = interp_kernel_get_slm_size (k);
  for (uint32_t i = 0; i < arg_n; ++i)
    {
      struct pocl_argument *a = &run->arguments[i];
      int32_t off
          = interp_kernel_get_curbe_offset (k, GBE_CURBE_KERNEL_ARGUMENT, i);
      switch (interp_kernel_get_arg_type (k, i))
        {
        case GBE_ARG_VALUE:
          if (off >= 0 && a->value)
            memcpy (curbe + off, a->value, a->size);
          break;
        case GBE_ARG_LOCAL_PTR:
          slm_offset = GEN7_ALIGN (slm_offset, interp_kernel_get_arg_align (k, i));
          if (off >= 0)
            *(uint32_t *)(curbe + off) = slm_offset;
          slm_offset += a->size;
          break;
        case GBE_ARG_SAMPLER:
          {
            uint32_t clk = gen7_sampler_to_clk (*(cl_sampler *)a->value);
            gen7_set_sampler_arg_slot (samplers, gk->sampler_sz, i, clk);
            if (off >= 0)
              memcpy (curbe + off, &clk, 4);
          }
          break;
        default:
          break; /* buffers/images are bound below */
        }
    }
  if (slm_offset > (int32_t)dev->local_mem_size)
    {
      POCL_MSG_ERR ("gen7: out of shared local memory %d\n", slm_offset);
      free (curbe);
      return CL_OUT_OF_RESOURCES;
    }

  cl_gpgpu_kernel gkernel;
  memset (&gkernel, 0, sizeof (gkernel));
  gkernel.name = kernel->name;
  gkernel.grf_blocks = 128;
  gkernel.bo = gk->code_bo;
  gkernel.barrierID = 0;
  gkernel.use_slm = interp_kernel_use_slm (k);
  gkernel.thread_n = thread_n;
  gkernel.curbe_sz = cst_sz;
  gkernel.slm_sz = slm_offset;

  cl_int err = CL_OUT_OF_RESOURCES;
  POCL_LOCK (d->hw_lock);
  cl_gpgpu gpgpu = cl_gpgpu_new (d->drv);
  void *printf_info = interp_dup_printfset (k);
  cl_gpgpu_set_printf_info (gpgpu, printf_info);

  if (cl_gpgpu_state_init (gpgpu, d->max_compute_unit * d->max_thread_per_unit,
                           cst_sz / 32, timestamps)
      != 0)
    goto out;

  int printf_num = interp_get_printf_num (printf_info);
  if (printf_num)
    {
      size_t buf_size = global_size * sizeof (int) * 16 * printf_num;
      if (buf_size > 16 * 1024 * 1024)
        buf_size = 16 * 1024 * 1024;
      if (buf_size < 1 * 1024 * 1024)
        buf_size = 1 * 1024 * 1024;
      if (cl_gpgpu_set_printf_buffer (gpgpu, buf_size,
                                      interp_get_printf_buf_bti (printf_info)))
        goto out;
    }

  uint32_t prof_bti = interp_get_profiling_bti (k);
  if (prof_bti != 0)
    {
      /* beignet cl_bind_profiling (OCL_PROFILING kernels) */
      int nts = simd == 16 ? 3 : 5;
      int thread_num = 0;
      for (int i = 0; i < nts; i++)
        {
          int32_t o = interp_kernel_get_curbe_offset (
              k, GBE_CURBE_PROFILING_TIMESTAMP0 + i, 0);
          memset (curbe + o, 0, sizeof (uint32_t) * 8 * (simd == 16 ? 2 : 1));
        }
      thread_num = (local_size + simd - 1) / simd;
      int32_t o = interp_kernel_get_curbe_offset (
          k, GBE_CURBE_PROFILING_BUF_POINTER, 0);
      thread_num *= global_size / local_size;
      if (cl_gpgpu_set_profiling_buffer (gpgpu, thread_num * 128 + 4, o,
                                         prof_bti))
        goto out;
      cl_gpgpu_set_profiling_info (gpgpu, interp_dup_profiling (k));
    }
  else
    cl_gpgpu_set_profiling_info (gpgpu, NULL);

  /* buffers (beignet cl_command_queue_bind_surface) */
  for (uint32_t i = 0; i < arg_n; ++i)
    {
      enum gbe_arg_type t = interp_kernel_get_arg_type (k, i);
      if (t != GBE_ARG_GLOBAL_PTR && t != GBE_ARG_PIPE)
        continue;
      cl_mem mem;
      gen7_mem_t *m = gen7_arg_mem (dev, &run->arguments[i], &mem);
      int32_t off
          = interp_kernel_get_curbe_offset (k, GBE_CURBE_KERNEL_ARGUMENT, i);
      if (m == NULL || off < 0)
        continue; /* NULL buffer: the curbe slot stays 0 */
      cl_gpgpu_bind_buf (gpgpu, m->bo, off,
                         m->offset + run->arguments[i].offset, mem->size,
                         interp_kernel_get_arg_bti (k, i));
    }

  /* images (beignet cl_command_queue_bind_image) */
  /* pocl's image copy kernel (pocl_image_copy.c) moves the pixels as raw
     integers of their size, so it copies every format bit for bit */
  const int raw_copy = strcmp (kernel->name, "pocl_copy_image2d_raw") == 0;
  for (uint32_t i = 0; i < gk->image_sz; i++)
    {
      ImageInfo *ii = &gk->images[i];
      cl_mem mem;
      gen7_mem_t *m = gen7_arg_mem (dev, &run->arguments[ii->arg_idx], &mem);
      if (m == NULL)
        continue;
      gen7_image_layout_t *l = &m->layout;
      uint32_t fmt = raw_copy ? gen7_raw_format (m->bpp, m->intel_fmt)
                              : m->intel_fmt;
      if (ii->wSlot >= 0)
        *(uint32_t *)(curbe + ii->wSlot) = l->w;
      if (ii->hSlot >= 0)
        *(uint32_t *)(curbe + ii->hSlot) = l->h;
      if (ii->depthSlot >= 0)
        *(uint32_t *)(curbe + ii->depthSlot) = l->depth;
      if (ii->channelOrderSlot >= 0)
        *(uint32_t *)(curbe + ii->channelOrderSlot)
            = raw_copy ? CL_RGBA : mem->image_channel_order;
      if (ii->dataTypeSlot >= 0)
        *(uint32_t *)(curbe + ii->dataTypeSlot)
            = raw_copy ? CL_UNSIGNED_INT32 : mem->image_channel_data_type;
      cl_gpgpu_bind_image (gpgpu, ii->idx, m->bo, m->offset, fmt,
                           mem->type, m->bpp, l->w, l->h, l->depth,
                           l->row_pitch, l->slice_pitch,
                           (cl_gpgpu_tiling)l->tiling);
      /* gen7 workaround from beignet */
      if (mem->type == CL_MEM_OBJECT_IMAGE1D_ARRAY)
        cl_gpgpu_bind_image (gpgpu, ii->idx + BTI_WORKAROUND_IMAGE_OFFSET,
                             m->bo, m->offset, fmt, mem->type, m->bpp,
                             l->w, l->h, l->depth, l->row_pitch,
                             l->slice_pitch, (cl_gpgpu_tiling)l->tiling);
    }

  cl_gpgpu_bind_sampler (gpgpu, samplers, gk->sampler_sz);

  if (cl_gpgpu_set_scratch (gpgpu, scratch_sz) != 0)
    goto out;

  /* stack (beignet cl_bind_stack) */
  if (gk->stack_size)
    {
      int32_t off_buf = interp_kernel_get_curbe_offset (
          k, GBE_CURBE_EXTRA_ARGUMENT, GBE_STACK_BUFFER);
      int32_t stack_sz = gk->stack_size * simd * d->max_compute_unit
                         * d->max_thread_per_unit;
      cl_driver_enlarge_stack_size (d->drv, &stack_sz);
      int32_t off_sz
          = interp_kernel_get_curbe_offset (k, GBE_CURBE_STACK_SIZE, 0);
      if (off_sz >= 0)
        *(uint64_t *)(curbe + off_sz) = stack_sz;
      cl_gpgpu_set_stack (gpgpu, off_buf, stack_sz, BTI_PRIVATE);
    }

  if (gen7_upload_constants (dev, gk, gp->gbe, run->arguments, curbe, gpgpu))
    goto out;

  cl_gpgpu_states_setup (gpgpu, &gkernel);

  /* curbe step 2: per-thread copies with local ids */
  if (cst_sz > 0)
    {
      char *final_curbe = malloc (thread_n * cst_sz);
      for (size_t i = 0; i < thread_n; ++i)
        memcpy (final_curbe + cst_sz * i, curbe, cst_sz);
      gen7_fill_varying (final_curbe, &gk->layout, local, simd, cst_sz,
                         thread_n);
      int r = cl_gpgpu_upload_curbes (gpgpu, final_curbe, thread_n * cst_sz);
      free (final_curbe);
      if (r != 0)
        goto out;
    }

  if (cl_gpgpu_batch_reset (gpgpu, 256 + 256) != 0)
    goto out;
  cl_gpgpu_batch_start (gpgpu);
  cl_gpgpu_walker (gpgpu, simd, thread_n, offset, dim_off, global, local);
  cl_gpgpu_batch_end (gpgpu, 0);

  if (cl_gpgpu_flush (gpgpu) < 0)
    goto out;
  POCL_UNLOCK (d->hw_lock);
  free (curbe);

  gen7_job_t *job = calloc (1, sizeof (gen7_job_t));
  job->node = cmd;
  job->gpgpu = gpgpu;
  job->printf_info = printf_info;
  job->printf_num = printf_num;
  job->profiling = profiling;
  job->timestamps = timestamps;
  job->t_submit = gen7_host_ns ();
  if (gen7_trace_on ())
    gen7_trace ("submit %s %zux%zux%zu local %zux%zux%zu", kernel->name,
                global[0], global[1], global[2], local[0], local[1], local[2]);
  *job_out = job;
  return CL_SUCCESS;

out:
  if (printf_info)
    {
      interp_release_printf_info (printf_info);
      cl_gpgpu_set_printf_info (gpgpu, NULL);
    }
  cl_gpgpu_delete (gpgpu);
  POCL_UNLOCK (d->hw_lock);
  free (curbe);
  return err;
}

/* Waits for the job's batch, then collects printf output and profiling
   timestamps and frees the batch state. Runs on the completion thread. */
void
gen7_launch_finish (gen7_device_t *d, gen7_job_t *job)
{
  cl_gpgpu gpgpu = job->gpgpu;
  cl_event event = job->node->sync.event.event;

  void *batch = cl_gpgpu_ref_batch_buf (gpgpu);
  cl_gpgpu_sync (batch);
  cl_gpgpu_unref_batch_buf (batch);

  POCL_LOCK (d->hw_lock);
  if (job->printf_num)
    {
      void *addr = cl_gpgpu_map_printf_buffer (gpgpu);
      interp_output_printf (job->printf_info, addr);
      cl_gpgpu_unmap_printf_buffer (gpgpu);
    }
  void *prof = cl_gpgpu_get_profiling_info (gpgpu);
  if (prof)
    {
      interp_output_profiling (prof, cl_gpgpu_map_profiling_buffer (gpgpu));
      cl_gpgpu_unmap_profiling_buffer (gpgpu);
    }

  if (gen7_stats.enabled)
    {
      uint64_t s = 0, e = 0, now = gen7_host_ns ();
      cl_gpgpu_event_get_exec_timestamp (gpgpu, 0, &s);
      cl_gpgpu_event_get_exec_timestamp (gpgpu, 1, &e);
      if (e < s)
        e += (1ULL << 32) * 80;
      if (gen7_stats.kernels++ == 0)
        gen7_stats.first_ns = job->t_submit;
      gen7_stats.gpu_ns += e - s;
      gen7_stats.wait_ns += now - job->t_submit;
      gen7_stats.last_ns = now;
    }

  if (gen7_trace_on ())
    {
      uint64_t s = 0, e = 0;
      cl_gpgpu_event_get_exec_timestamp (gpgpu, 0, &s);
      cl_gpgpu_event_get_exec_timestamp (gpgpu, 1, &e);
      if (e < s)
        e += (1ULL << 32) * 80;
      struct pocl_context *pc = &job->node->command.run.pc;
      gen7_trace ("run %s %zux%zux%zu local %zux%zux%zu gpu %.3f ms, submit->"
                  "done %.3f ms",
                  job->node->command.run.kernel->name,
                  pc->num_groups[0] * pc->local_size[0],
                  pc->num_groups[1] * pc->local_size[1],
                  pc->num_groups[2] * pc->local_size[2], pc->local_size[0],
                  pc->local_size[1], pc->local_size[2], (e - s) / 1e6,
                  (gen7_host_ns () - job->t_submit) / 1e6);
    }

  if (job->profiling)
    {
      /* 32-bit tick counters * 80 ns; put them on the submit time's epoch */
      uint64_t start = 0, end = 0;
      const uint64_t period = (1ULL << 32) * 80;
      cl_gpgpu_event_get_exec_timestamp (gpgpu, 0, &start);
      cl_gpgpu_event_get_exec_timestamp (gpgpu, 1, &end);
      uint64_t base = event->time_submit - (event->time_submit % period);
      start += base;
      end += base;
      if (start < event->time_submit)
        {
          start += period;
          end += period;
        }
      if (end < start)
        end += period;
      event->time_start = start;
      event->time_end = end;
    }

  if (job->printf_info)
    {
      interp_release_printf_info (job->printf_info);
      cl_gpgpu_set_printf_info (gpgpu, NULL);
    }
  cl_gpgpu_delete (gpgpu);
  POCL_UNLOCK (d->hw_lock);
  free (job);
}

void
pocl_gen7_run (void *data, _cl_command_node *cmd)
{
  gen7_device_t *d = data;
  gen7_job_t *job = NULL;
  cl_int err = gen7_launch_submit (d, cmd, &job);
  if (err == CL_SUCCESS)
    gen7_launch_finish (d, job);
  else
    POCL_MSG_ERR ("gen7: kernel %s launch failed (%d)\n",
                  cmd->command.run.kernel->name, err);
}
