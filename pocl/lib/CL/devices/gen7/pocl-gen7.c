/* pocl-gen7.c - Intel Gen7 (Ivy Bridge) device driver: device setup,
   command scheduling and the ops table.

   The compiler and the hardware layer come from beignet; see pocl-gen7.h.
   Device limits match beignet's IVB GT2 device (cl_device_id.c,
   cl_gt_device.h, cl_gen7_device.h) so that applications see the same
   numbers through pocl as through the beignet ICD.
*/

#include "pocl-gen7.h"

#include "common.h"
#include "common_driver.h"
#include "devices.h"
#include "pocl_timing.h"
#include "pocl_version.h"
#include "pocl_util.h"
#include "utlist.h"

#include "cl_device_data.h"
#include "intel_defines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>

extern void intel_setup_callbacks (void);
extern int CompilerSupported (void);

#define GEN7_EXTENSIONS                                                       \
  "cl_khr_global_int32_base_atomics cl_khr_global_int32_extended_atomics "    \
  "cl_khr_local_int32_base_atomics cl_khr_local_int32_extended_atomics "      \
  "cl_khr_byte_addressable_store cl_khr_3d_image_writes "                     \
  "cl_khr_depth_images cl_khr_icd cl_khr_image2d_from_buffer "             \
  "cl_intel_subgroups cl_intel_subgroups_short "                              \
  "cl_intel_required_subgroup_size cl_intel_media_block_io "                  \
  "cl_khr_il_program cl_intel_device_attribute_query "                        \
  "cl_intel_va_api_media_sharing cl_khr_gl_sharing"

/* The GPU's maximum (RP0) frequency in MHz from i915's sysfs, for the DRM
   card with this PCI device id; IVB parts range from 1050 to 1350 MHz. */
static cl_uint
gen7_max_clock_mhz (uint32_t pci_id)
{
  for (int card = 0; card < 16; ++card)
    {
      char path[96];
      unsigned vendor = 0, device = 0, mhz = 0;
      snprintf (path, sizeof (path), "/sys/class/drm/card%d/device/vendor",
                card);
      FILE *f = fopen (path, "r");
      if (f == NULL)
        continue;
      int ok = fscanf (f, "%x", &vendor) == 1;
      fclose (f);
      snprintf (path, sizeof (path), "/sys/class/drm/card%d/device/device",
                card);
      if (!ok || vendor != 0x8086 || (f = fopen (path, "r")) == NULL)
        continue;
      ok = fscanf (f, "%x", &device) == 1;
      fclose (f);
      snprintf (path, sizeof (path), "/sys/class/drm/card%d/gt_RP0_freq_mhz",
                card);
      if (!ok || device != pci_id || (f = fopen (path, "r")) == NULL)
        continue;
      ok = fscanf (f, "%u", &mhz) == 1;
      fclose (f);
      if (ok && mhz > 0)
        return mhz;
    }
  return 1000;
}

static int gen7_callbacks_ready = 0;
static void *gen7_done_thread (void *arg);

static void
gen7_setup_callbacks (void)
{
  if (!gen7_callbacks_ready)
    {
      intel_setup_callbacks ();
      gen7_callbacks_ready = 1;
    }
}

void
pocl_gen7_init_device_ops (struct pocl_device_ops *ops)
{
  ops->device_name = "gen7";

  ops->probe = pocl_gen7_probe;
  ops->init = pocl_gen7_init;
  ops->uninit = pocl_gen7_uninit;
  ops->reinit = NULL;

  ops->alloc_mem_obj = pocl_gen7_alloc_mem_obj;
  ops->free = pocl_gen7_free;
  ops->import_dmabuf = pocl_gen7_import_dmabuf;
  ops->export_dmabuf = pocl_gen7_export_dmabuf;
  ops->alloc_subbuffer = pocl_gen7_alloc_subbuffer;
  ops->free_subbuffer = pocl_gen7_free_subbuffer;
  ops->read = pocl_gen7_read;
  ops->read_rect = pocl_gen7_read_rect;
  ops->write = pocl_gen7_write;
  ops->write_rect = pocl_gen7_write_rect;
  ops->copy = pocl_gen7_copy;
  ops->copy_rect = pocl_gen7_copy_rect;
  ops->memfill = pocl_gen7_memfill;
  ops->map_mem = pocl_gen7_map_mem;
  ops->unmap_mem = pocl_gen7_unmap_mem;
  ops->get_mapping_ptr = pocl_driver_get_mapping_ptr;
  ops->free_mapping_ptr = pocl_driver_free_mapping_ptr;

  ops->create_sampler = NULL;
  ops->free_sampler = NULL;
  ops->copy_image_rect = pocl_gen7_copy_image_rect;
  ops->write_image_rect = pocl_gen7_write_image_rect;
  ops->read_image_rect = pocl_gen7_read_image_rect;
  ops->map_image = pocl_gen7_map_image;
  ops->unmap_image = pocl_gen7_unmap_image;
  ops->fill_image = pocl_gen7_fill_image;

  ops->build_source = pocl_gen7_build_source;
  ops->build_binary = pocl_gen7_build_binary;
  ops->link_program = pocl_gen7_link_program;
  ops->free_program = pocl_gen7_free_program;
  ops->setup_metadata = pocl_gen7_setup_metadata;
  ops->supports_binary = pocl_gen7_supports_binary;
  ops->build_poclbinary = pocl_gen7_build_poclbinary;
  ops->create_kernel = pocl_gen7_create_kernel;
  ops->free_kernel = pocl_gen7_free_kernel;
  ops->compute_local_size = pocl_gen7_compute_local_size;
  ops->build_hash = pocl_gen7_build_hash;

  ops->run = pocl_gen7_run;
  ops->run_native = NULL;

  ops->submit = pocl_gen7_submit;
  ops->join = pocl_gen7_join;
  ops->flush = pocl_gen7_flush;
  ops->notify = pocl_gen7_notify;
  ops->broadcast = pocl_broadcast;
  ops->update_event = pocl_gen7_update_event;
  ops->free_event_data = pocl_gen7_free_event_data;
  ops->wait_event = pocl_gen7_wait_event;
  ops->notify_event_finished = pocl_gen7_notify_event_finished;
  ops->notify_cmdq_finished = pocl_gen7_notify_cmdq_finished;
  ops->init_queue = pocl_gen7_init_queue;
  ops->free_queue = pocl_gen7_free_queue;
  ops->get_device_info_ext = pocl_gen7_get_device_info_ext;
  ops->get_subgroup_info_ext = pocl_gen7_get_subgroup_info_ext;
}

unsigned int
pocl_gen7_probe (struct pocl_device_ops *ops)
{
  int env_count = pocl_device_get_env_count (ops->device_name);
  if (env_count == 0)
    return 0;

  gen7_setup_callbacks ();
  int pci_id = cl_driver_get_device_id ();
  if (!IS_IVYBRIDGE (pci_id))
    {
      if (env_count > 0)
        POCL_MSG_ERR ("gen7: no Ivy Bridge GPU found (PCI id 0x%x)\n",
                      pci_id);
      return 0;
    }
  if (!CompilerSupported ())
    {
      POCL_MSG_ERR ("gen7: could not load libgbe; set OCL_GBE_PATH and "
                    "OCL_INTERP_PATH\n");
      return 0;
    }
  return 1;
}

static const char *
gen7_device_name (uint32_t id)
{
  switch (id)
    {
    case PCI_CHIP_IVYBRIDGE_GT1: return "Intel(R) HD Graphics IvyBridge GT1";
    case PCI_CHIP_IVYBRIDGE_M_GT1: return "Intel(R) HD Graphics IvyBridge M GT1";
    case PCI_CHIP_IVYBRIDGE_S_GT1: return "Intel(R) HD Graphics IvyBridge S GT1";
    case PCI_CHIP_IVYBRIDGE_GT2: return "Intel(R) HD Graphics IvyBridge GT2";
    case PCI_CHIP_IVYBRIDGE_M_GT2: return "Intel(R) HD Graphics IvyBridge M GT2";
    case PCI_CHIP_IVYBRIDGE_S_GT2: return "Intel(R) HD Graphics IvyBridge S GT2";
    default: return "Intel(R) HD Graphics Family";
    }
}

cl_int
pocl_gen7_init (unsigned j, cl_device_id dev, const char *parameters)
{
  gen7_device_t *d = calloc (1, sizeof (gen7_device_t));
  if (d == NULL)
    return CL_OUT_OF_HOST_MEMORY;

  gen7_setup_callbacks ();
  d->drv = cl_driver_new (NULL);
  if (d->drv == NULL)
    {
      free (d);
      return CL_DEVICE_NOT_FOUND;
    }
  d->bufmgr = cl_driver_get_bufmgr (d->drv);
  d->pci_id = cl_driver_get_device_id ();
  POCL_INIT_LOCK (d->hw_lock);
  POCL_INIT_LOCK (d->cq_lock);
  POCL_INIT_LOCK (d->done_lock);
  POCL_INIT_COND (d->done_cond);
  pthread_create (&d->done_thread, NULL, gen7_done_thread, d);

  if (IS_IVB_GT1 (d->pci_id))
    {
      d->max_compute_unit = 6;
      d->max_thread_per_unit = 6;
    }
  else
    {
      d->max_compute_unit = 16;
      d->max_thread_per_unit = 8;
    }
  d->scratch_mem_size = 12 << 10;
  d->default_tiling = GEN7_TILE_X;
  const char *tiling = getenv ("OCL_TILING");
  if (tiling && (tiling[0] == '0' || tiling[0] == '1' || tiling[0] == '2'))
    d->default_tiling = tiling[0] - '0';

  dev->data = d;
  d->available = CL_TRUE;
  dev->available = &d->available;
  pocl_init_default_device_infos (dev, GEN7_EXTENSIONS);

  dev->type = CL_DEVICE_TYPE_GPU;
  dev->vendor_id = 0x8086;
  dev->short_name = "gen7";
  dev->long_name = gen7_device_name (d->pci_id);
  dev->vendor = "Intel";
  dev->driver_version = POCL_VERSION_FULL " gen7";
  /* OpenCL 3.0 with none of the optional 2.x features (no SVM, generic
     address space, pipes, device enqueue, C11 atomics, read-write images);
     OpenCL C 1.2 and 3.0 (libgbe maps CL3.0 onto its 1.2 library) */
  SETUP_DEVICE_CL_VERSION (dev, 3, 0);
  dev->features
      = "__opencl_c_images __opencl_c_3d_image_writes __opencl_c_int64";
  dev->profile = "FULL_PROFILE";

  dev->max_compute_units = d->max_compute_unit;
  dev->max_clock_frequency = gen7_max_clock_mhz (d->pci_id);
  dev->max_work_item_dimensions = 3;
  dev->max_work_item_sizes[0] = dev->max_work_item_sizes[1]
      = dev->max_work_item_sizes[2] = 512;
  dev->max_work_group_size = 512;
  dev->preferred_wg_size_multiple = 16;
  dev->address_bits = 32;

  dev->preferred_vector_width_char = 16;
  dev->preferred_vector_width_short = 8;
  dev->preferred_vector_width_int = 4;
  dev->preferred_vector_width_long = 2;
  dev->preferred_vector_width_float = 4;
  dev->preferred_vector_width_double = 0;
  dev->preferred_vector_width_half = 0;
  dev->native_vector_width_char = 8;
  dev->native_vector_width_short = 8;
  dev->native_vector_width_int = 4;
  dev->native_vector_width_long = 2;
  dev->native_vector_width_float = 4;
  dev->native_vector_width_double = 2;
  dev->native_vector_width_half = 8;

  dev->single_fp_config = CL_FP_INF_NAN | CL_FP_ROUND_TO_NEAREST;
  dev->half_fp_config = 0;
  dev->double_fp_config = 0;
  dev->has_64bit_long = 1;

  /* memory: beignet clamps 2 GB to half the RAM, single alloc to 3/4 */
  uint64_t gmem = 2ULL << 30;
  struct sysinfo info;
  if (sysinfo (&info) == 0)
    {
      uint64_t half_ram = (uint64_t)info.totalram * info.mem_unit / 2;
      if (half_ram < gmem)
        gmem = half_ram;
    }
  gmem &= ~((1ULL << 20) - 1);
  dev->global_mem_size = gmem;
  dev->max_mem_alloc_size = (gmem * 3 / 4) & ~((1ULL << 20) - 1);
  /* measurement knob (tools/gen7 gen7_check memlimit): raise both limits to
     probe what the GTT really takes */
  const char *lim = getenv ("POCL_GEN7_MEM_LIMIT_MIB");
  if (lim && atoll (lim) > 0)
    dev->global_mem_size = dev->max_mem_alloc_size
        = (uint64_t)atoll (lim) << 20;
  dev->global_mem_cache_type = CL_READ_WRITE_CACHE;
  dev->global_mem_cacheline_size = 64;
  dev->global_mem_cache_size = 8 << 10;
  dev->local_mem_type = CL_LOCAL;
  dev->local_mem_size = 64 << 10;
  dev->max_constant_buffer_size = 128 * 1024 * 1024;
  dev->max_constant_args = 8;
  dev->max_parameter_size = 1024;
  dev->mem_base_addr_align = sizeof (cl_long) * 16; /* bytes: 1024 bits */
  dev->min_data_type_align_size = sizeof (cl_long) * 16;
  dev->host_unified_memory = CL_TRUE;
  dev->error_correction_support = CL_FALSE;
  dev->endian_little = CL_TRUE;
  dev->printf_buffer_size = 1 << 20;
  dev->profiling_timer_resolution = 80;
  dev->has_own_timer = CL_TRUE;
  /* pocl resolves dependencies, the scheduler runs whatever is ready, and
     i915 runs our batches in order: out-of-order queues are safe (SYCL
     runtimes create them by default) */
  dev->on_host_queue_props
      = CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE;
  dev->execution_capabilities = CL_EXEC_KERNEL;
  dev->compiler_available = CL_TRUE;
  dev->linker_available = CL_TRUE;
  dev->max_sub_devices = 0;
  dev->num_partition_properties = 0;
  dev->num_partition_types = 0;

  /* images */
  dev->image_support = CL_TRUE;
  dev->max_read_image_args = BTI_MAX_READ_IMAGE_ARGS;
  dev->max_write_image_args = BTI_MAX_WRITE_IMAGE_ARGS;
  dev->max_read_write_image_args = 0; /* no __opencl_c_read_write_images */
  /* Gen7 surface limits; Intel's own Windows driver for this GPU reports
     the same (2D 16384, 3D 2048 in every dimension) */
  dev->image2d_max_width = 16384;
  dev->image2d_max_height = 16384;
  dev->image3d_max_width = 2048;
  dev->image3d_max_height = 2048;
  dev->image3d_max_depth = 2048;
  dev->image_max_array_size = 2048;
  dev->image_max_buffer_size = 65536;
  /* cl_khr_image2d_from_buffer: a linear surface in the buffer's bo; any
     pitch that is a whole number of pixels, page-aligned base (beignet) */
  dev->image_pitch_alignment = 1;
  dev->image_base_address_alignment = 4096;
  dev->max_samplers = GEN7_MAX_SAMPLERS;
  d->num_image_formats
      = gen7_supported_image_formats (d->image_formats, GEN7_MAX_IMAGE_FORMATS);
  for (unsigned i = 0; i < NUM_OPENCL_IMAGE_TYPES; ++i)
    {
      dev->image_formats[i] = d->image_formats;
      dev->num_image_formats[i] = d->num_image_formats;
    }

  /* SPIR-V input (gen7-spirv.c) */
  dev->supported_spir_v_versions = "SPIR-V_1.2 SPIR-V_1.1 SPIR-V_1.0";
  pocl_setup_ils_with_version (dev);
  pocl_setup_opencl_c_with_version (dev, CL_TRUE);
  pocl_setup_features_with_version (dev);
  pocl_setup_extensions_with_version (dev);

  /* the gen7 binary goes into pocl binaries next to the metadata */
  static const char *serialize_entries[] = { "/program.gen" };
  dev->num_serialize_entries = 1;
  dev->serialize_entries = serialize_entries;

  return CL_SUCCESS;
}

cl_int
pocl_gen7_uninit (unsigned j, cl_device_id dev)
{
  gen7_device_t *d = dev->data;
  if (d == NULL)
    return CL_SUCCESS;
  POCL_LOCK (d->done_lock);
  d->shutdown = 1;
  POCL_SIGNAL_COND (d->done_cond);
  POCL_UNLOCK (d->done_lock);
  pthread_join (d->done_thread, NULL);
  POCL_DESTROY_COND (d->done_cond);
  POCL_DESTROY_LOCK (d->done_lock);
  cl_driver_delete (d->drv);
  POCL_DESTROY_LOCK (d->hw_lock);
  POCL_DESTROY_LOCK (d->cq_lock);
  free (d);
  dev->data = NULL;
  return CL_SUCCESS;
}

char *
pocl_gen7_build_hash (cl_device_id dev)
{
  gen7_device_t *d = dev->data;
  char *res = malloc (64);
  snprintf (res, 64, "gen7-%04x-gbe", d->pci_id);
  return res;
}

void
pocl_gen7_compute_local_size (cl_device_id dev, cl_kernel kernel,
                              unsigned device_i, size_t max_group_size,
                              size_t global_x, size_t global_y,
                              size_t global_z, size_t *local_x,
                              size_t *local_y, size_t *local_z)
{
  size_t g[3] = { global_x, global_y, global_z }, l[3];
  unsigned dim = global_z > 1 ? 3 : (global_y > 1 ? 2 : 1);
  gen7_pick_local_size (dim, g, l);
  *local_x = l[0];
  *local_y = l[1];
  *local_z = l[2];
}

/***************************** timestamps ********************************/

uint64_t
gen7_gpu_timestamp_ns (gen7_device_t *d)
{
  uint64_t ts = 0;
  cl_gpgpu_event_get_gpu_cur_timestamp (d->drv, &ts);
  return ts;
}

/* Allocates the event data at QUEUED. Profiling uses the GPU timestamp
   register for every point, as beignet does, so QUEUED <= SUBMIT <= START
   <= END holds on one time base. Kernel START/END are written by
   gen7_launch_finish from the batch's PIPE_CONTROL timestamps. */
void
pocl_gen7_update_event (cl_device_id dev, cl_event event)
{
  gen7_device_t *d = dev->data;
  if (event->data == NULL && event->status == CL_QUEUED)
    {
      gen7_event_t *e = calloc (1, sizeof (gen7_event_t));
      POCL_INIT_COND (e->cond);
      event->data = e;
    }

  if (event->queue == NULL
      || !(event->queue->properties & CL_QUEUE_PROFILING_ENABLE))
    return;
  int kernel = event->command_type == CL_COMMAND_NDRANGE_KERNEL;
  switch (event->status)
    {
    case CL_QUEUED:
      event->time_queue = gen7_gpu_timestamp_ns (d);
      break;
    case CL_SUBMITTED:
      event->time_submit = gen7_gpu_timestamp_ns (d);
      if (event->time_submit < event->time_queue)
        event->time_submit = event->time_queue;
      break;
    case CL_RUNNING:
      if (!kernel)
        event->time_start = gen7_gpu_timestamp_ns (d);
      break;
    case CL_COMPLETE:
      if (!kernel)
        event->time_end = gen7_gpu_timestamp_ns (d);
      break;
    default:
      break;
    }
}

void
pocl_gen7_free_event_data (cl_event event)
{
  gen7_event_t *e = event->data;
  if (e == NULL)
    return;
  POCL_DESTROY_COND (e->cond);
  free (e);
  event->data = NULL;
}

void
pocl_gen7_wait_event (cl_device_id device, cl_event event)
{
  gen7_event_t *e = event->data;
  POCL_LOCK_OBJ (event);
  while (event->status > CL_COMPLETE)
    POCL_WAIT_COND (e->cond, event->pocl_lock);
  POCL_UNLOCK_OBJ (event);
}

void
pocl_gen7_notify_event_finished (cl_event event)
{
  gen7_event_t *e = event->data;
  if (e)
    POCL_BROADCAST_COND (e->cond);
}

int
pocl_gen7_init_queue (cl_device_id device, cl_command_queue queue)
{
  gen7_queue_t *q = calloc (1, sizeof (gen7_queue_t));
  POCL_INIT_COND (q->cond);
  queue->data = q;
  return CL_SUCCESS;
}

int
pocl_gen7_free_queue (cl_device_id device, cl_command_queue queue)
{
  gen7_queue_t *q = queue->data;
  if (q)
    {
      POCL_DESTROY_COND (q->cond);
      free (q);
    }
  queue->data = NULL;
  return CL_SUCCESS;
}

void
pocl_gen7_notify_cmdq_finished (cl_command_queue cq)
{
  /* called with cq locked */
  gen7_queue_t *q = cq->data;
  POCL_BROADCAST_COND (q->cond);
}

/***************************** scheduling ********************************/

/* Commands run in the thread that makes them ready, as in the basic driver,
   except NDRanges: their batch is submitted and the completion thread
   finishes the event when the GPU is done. */

static void *
gen7_done_thread (void *arg)
{
  gen7_device_t *d = arg;
  for (;;)
    {
      POCL_LOCK (d->done_lock);
      while (d->inflight_head == NULL && !d->shutdown)
        POCL_WAIT_COND (d->done_cond, d->done_lock);
      gen7_job_t *job = d->inflight_head;
      if (job == NULL)
        {
          POCL_UNLOCK (d->done_lock);
          return NULL;
        }
      d->inflight_head = job->next;
      if (d->inflight_head == NULL)
        d->inflight_tail = NULL;
      POCL_UNLOCK (d->done_lock);

      cl_event event = job->node->sync.event.event;
      gen7_launch_finish (d, job);
      POCL_UPDATE_EVENT_COMPLETE_MSG (event, "Event Enqueue NDRange       ");
    }
}

static void
gen7_start_kernel (gen7_device_t *d, _cl_command_node *node)
{
  cl_event event = node->sync.event.event;
  gen7_job_t *job = NULL;
  pocl_update_event_running (event);
  cl_int err = gen7_launch_submit (d, node, &job);
  if (err != CL_SUCCESS)
    {
      POCL_UPDATE_EVENT_FAILED_MSG (CL_FAILED, event,
                                    "gen7: kernel launch failed\n");
      return;
    }
  gen7_event_t *e = event->data;
  if (e)
    e->in_flight = 1;

  POCL_LOCK (d->done_lock);
  if (d->inflight_tail)
    d->inflight_tail->next = job;
  else
    d->inflight_head = job;
  d->inflight_tail = job;
  POCL_SIGNAL_COND (d->done_cond);
  POCL_UNLOCK (d->done_lock);
}

static void
gen7_command_scheduler (gen7_device_t *d)
{
  _cl_command_node *node;
  while ((node = d->ready_list))
    {
      assert (node->sync.event.event->status == CL_SUBMITTED);
      CDL_DELETE (d->ready_list, node);
      POCL_UNLOCK (d->cq_lock);
      if (node->type == CL_COMMAND_NDRANGE_KERNEL)
        gen7_start_kernel (d, node);
      else
        pocl_exec_command (node);
      POCL_LOCK (d->cq_lock);
    }
}

/* An NDRange whose only unfinished dependencies are gen7 NDRanges already
   on the GPU can be submitted right away: i915 runs the batches of one
   context in order (beignet relies on the same). Called with event locked. */
static int
gen7_deps_in_flight (cl_device_id dev, cl_event event)
{
  event_node *n;
  LL_FOREACH (event->wait_list, n)
    {
      cl_event dep = n->event;
      gen7_event_t *e = dep->data;
      if (dep->queue == NULL || dep->queue->device != dev
          || dep->command_type != CL_COMMAND_NDRANGE_KERNEL || e == NULL
          || !e->in_flight)
        return 0;
    }
  return 1;
}

void
pocl_gen7_submit (_cl_command_node *node, cl_command_queue cq)
{
  gen7_device_t *d = node->device->data;
  cl_event event = node->sync.event.event;
  node->state = POCL_COMMAND_READY;
  POCL_LOCK (d->cq_lock);
  pocl_command_push (node, &d->ready_list, &d->command_list);
  if (event->status == CL_QUEUED && node->type == CL_COMMAND_NDRANGE_KERNEL
      && gen7_deps_in_flight (node->device, event))
    {
      pocl_update_event_submitted (event);
      CDL_DELETE (d->command_list, node);
      CDL_PREPEND (d->ready_list, node);
    }
  POCL_UNLOCK_OBJ (event);
  gen7_command_scheduler (d);
  POCL_UNLOCK (d->cq_lock);
}

void
pocl_gen7_flush (cl_device_id device, cl_command_queue cq)
{
  gen7_device_t *d = device->data;
  POCL_LOCK (d->cq_lock);
  gen7_command_scheduler (d);
  POCL_UNLOCK (d->cq_lock);
}

void
pocl_gen7_join (cl_device_id device, cl_command_queue cq)
{
  pocl_gen7_flush (device, cq);
  gen7_queue_t *q = cq->data;
  POCL_LOCK_OBJ (cq);
  while (cq->command_count != 0)
    POCL_WAIT_COND (q->cond, cq->pocl_lock);
  POCL_UNLOCK_OBJ (cq);
}

void
pocl_gen7_notify (cl_device_id device, cl_event event, cl_event finished)
{
  gen7_device_t *d = device->data;
  _cl_command_node *volatile node = event->command;

  if (finished->status < CL_COMPLETE)
    {
      pocl_unlock_events_inorder (event, finished);
      pocl_update_event_failed (CL_FAILED, NULL, 0, event, NULL);
      pocl_lock_events_inorder (finished, event);
      return;
    }

  if (node->state != POCL_COMMAND_READY)
    return;

  if (event->status == CL_QUEUED
      && (pocl_command_is_ready (event)
          || (node->type == CL_COMMAND_NDRANGE_KERNEL
              && gen7_deps_in_flight (device, event))))
    {
      pocl_update_event_submitted (event);
      POCL_LOCK (d->cq_lock);
      CDL_DELETE (d->command_list, node);
      CDL_PREPEND (d->ready_list, node);
      POCL_UNLOCK_OBJ (event);
      gen7_command_scheduler (d);
      POCL_LOCK_OBJ (event);
      POCL_UNLOCK (d->cq_lock);
    }
}

/*************************** extension queries ***************************/

cl_int
pocl_gen7_get_device_info_ext (cl_device_id dev, cl_device_info param_name,
                               size_t param_value_size, void *param_value,
                               size_t *param_value_size_ret)
{
  switch (param_name)
    {
    case CL_DEVICE_SUB_GROUP_SIZES_INTEL:
      {
        /* the SIMD widths libgbe compiles for */
        size_t sizes[] = { 8, 16 };
        POCL_RETURN_GETINFO_ARRAY (size_t, 2, sizes);
      }
    /* cl_intel_device_attribute_query (SYCL runtimes ask Intel devices) */
    case CL_DEVICE_IP_VERSION_INTEL:
      POCL_RETURN_GETINFO (cl_version, CL_MAKE_VERSION (7, 0, 0));
    case CL_DEVICE_ID_INTEL:
      POCL_RETURN_GETINFO (cl_uint, ((gen7_device_t *)dev->data)->pci_id);
    case CL_DEVICE_NUM_SLICES_INTEL:
      POCL_RETURN_GETINFO (cl_uint, 1);
    case CL_DEVICE_NUM_SUB_SLICES_PER_SLICE_INTEL:
      POCL_RETURN_GETINFO (cl_uint,
                           IS_IVB_GT1 (((gen7_device_t *)dev->data)->pci_id) ? 1 : 2);
    case CL_DEVICE_NUM_EUS_PER_SUB_SLICE_INTEL:
      POCL_RETURN_GETINFO (cl_uint,
                           IS_IVB_GT1 (((gen7_device_t *)dev->data)->pci_id) ? 6 : 8);
    case CL_DEVICE_NUM_THREADS_PER_EU_INTEL:
      POCL_RETURN_GETINFO (cl_uint,
                           ((gen7_device_t *)dev->data)->max_thread_per_unit);
    case CL_DEVICE_FEATURE_CAPABILITIES_INTEL:
      POCL_RETURN_GETINFO (cl_bitfield, 0); /* no DP4A, no DPAS */
    default:
      return CL_INVALID_VALUE;
    }
}

cl_int
pocl_gen7_get_subgroup_info_ext (cl_device_id dev, cl_kernel kernel,
                                 unsigned program_device_i,
                                 cl_kernel_sub_group_info param_name,
                                 size_t input_value_size,
                                 const void *input_value,
                                 size_t param_value_size, void *param_value,
                                 size_t *param_value_size_ret)
{
  gen7_kernel_t *gk = kernel->data[program_device_i];
  size_t simd = gk ? gk->simd : 16;
  size_t local = 1;
  if (input_value)
    for (size_t i = 0; i < input_value_size / sizeof (size_t); i++)
      local *= ((const size_t *)input_value)[i];

  switch (param_name)
    {
    case CL_KERNEL_MAX_SUB_GROUP_SIZE_FOR_NDRANGE:
      POCL_RETURN_GETINFO (size_t, simd);
    case CL_KERNEL_SUB_GROUP_COUNT_FOR_NDRANGE:
      POCL_RETURN_GETINFO (size_t, (local + simd - 1) / simd);
    default:
      return CL_INVALID_VALUE;
    }
}
