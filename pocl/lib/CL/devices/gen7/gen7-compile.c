/* gen7-compile.c - program build through beignet's libgbe

   Follows beignet's cl_program.c: clBuildProgram compiles the source to Gen
   code with compiler_program_new_from_source; clCompileProgram keeps LLVM IR
   (compile_from_source) and clLinkProgram links IR programs and builds Gen
   code. The serialized libgbe program is kept in program->binaries[] and in
   the program cache dir as "program.gen", which pocl puts into its pocl
   binaries (device->serialize_entries).
*/

#include "pocl-gen7.h"
#include "gen7-trace.h"

#include "common.h"
#include "pocl_cache.h"
#include "pocl_file_util.h"
#include "pocl_util.h"

#include "cl_device_data.h"

#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GEN7_BUILD_LOG_SIZE (1 << 20)

/* beignet binary headers (cl_program.c binary_type_header) */
static const unsigned char gen7_hdr_gen[5] = { 1, 'G', 'E', 'N', 'C' };
static const unsigned char gen7_hdr_obj[5] = { 1, 'B', 'C', 0xC0, 0xDE };
static const unsigned char gen7_hdr_lib[5] = { 2, 'B', 'C', 0xC0, 0xDE };

static int
gen7_is_gen_binary (const unsigned char *b, size_t n)
{
  return n >= 5 && memcmp (b, gen7_hdr_gen, 5) == 0;
}

static int
gen7_is_llvm_binary (const unsigned char *b, size_t n)
{
  return n >= 5
         && (memcmp (b, gen7_hdr_obj, 5) == 0
             || memcmp (b, gen7_hdr_lib, 5) == 0);
}

static uint32_t
gen7_pci_id (cl_program program, cl_uint device_i)
{
  gen7_device_t *d = program->devices[device_i]->data;
  return d->pci_id;
}

static void
gen7_set_build_log (cl_program program, cl_uint device_i, const char *log,
                    size_t log_size)
{
  POCL_MEM_FREE (program->build_log[device_i]);
  if (log_size == 0)
    return;
  program->build_log[device_i] = malloc (log_size + 1);
  memcpy (program->build_log[device_i], log, log_size);
  program->build_log[device_i][log_size] = 0;
}

static void
gen7_program_path (char *path, cl_program program, cl_uint device_i)
{
  /* <cachedir>/program.bc -> <cachedir>/program.gen */
  pocl_cache_program_bc_path (path, program, device_i);
  size_t len = strlen (path);
  assert (len > 3 && strcmp (path + len - 3, ".bc") == 0);
  strcpy (path + len - 3, ".gen");
}

/* Serializes the libgbe program into program->binaries[device_i] and the
   cache dir. type: 0 = executable, 1 = compiled object, 2 = library. */
static int
gen7_store_program (cl_program program, cl_uint device_i, gbe_program gbe,
                    int type)
{
  char *bin = NULL;
  size_t size = compiler_program_serialize_to_binary (gbe, &bin, type);
  if (size == 0 || bin == NULL)
    return CL_BUILD_PROGRAM_FAILURE;

  POCL_MEM_FREE (program->binaries[device_i]);
  program->binaries[device_i] = (unsigned char *)bin;
  program->binary_sizes[device_i] = size;

  char path[POCL_MAX_PATHNAME_LENGTH];
  if (pocl_cache_create_program_cachedir (program, device_i, bin, size, path))
    return CL_BUILD_PROGRAM_FAILURE;
  gen7_program_path (path, program, device_i);
  if (!pocl_exists (path) && pocl_write_file (path, bin, size, 0))
    return CL_BUILD_PROGRAM_FAILURE;
  return CL_SUCCESS;
}

static void
gen7_set_program (cl_program program, cl_uint device_i, gbe_program gbe)
{
  gen7_program_t *p = program->data[device_i];
  if (p == NULL)
    p = program->data[device_i] = calloc (1, sizeof (gen7_program_t));
  else if (p->gbe)
    interp_program_delete (p->gbe);
  p->gbe = gbe;
}

/* Writes the clCompileProgram headers below a temp dir, as beignet does. */
static int
gen7_write_headers (char *dir, cl_uint n, const cl_program *headers,
                    const char **names)
{
  for (cl_uint i = 0; i < n; i++)
    {
      if (names[i] == NULL || headers[i] == NULL || headers[i]->source == NULL)
        continue;
      char path[POCL_MAX_PATHNAME_LENGTH];
      if (snprintf (path, sizeof (path), "%s/%s", dir, names[i])
          >= (int)sizeof (path))
        return -1;
      char *dirc = strdup (path);
      pocl_mkdir_p (dirname (dirc));
      free (dirc);
      if (pocl_write_file (path, headers[i]->source,
                           strlen (headers[i]->source), 0))
        return -1;
    }
  return 0;
}

int
pocl_gen7_build_source (cl_program program, cl_uint device_i,
                        cl_uint num_input_headers,
                        const cl_program *input_headers,
                        const char **header_include_names, int link_program)
{
  if (link_program && num_input_headers == 0
      && getenv ("POCL_GEN7_SPIRV_ROUNDTRIP"))
    return gen7_spirv_roundtrip (program, device_i);

  const char *options = program->compiler_options ? program->compiler_options
                                                  : "";
  char *log = calloc (1, GEN7_BUILD_LOG_SIZE);
  size_t log_size = 0;
  gbe_program gbe;
  double t0 = gen7_trace_ms ();
  gen7_trace_program (program->source, options, -1, 0);

  if (link_program)
    gbe = compiler_program_new_from_source (
        gen7_pci_id (program, device_i), program->source, GEN7_BUILD_LOG_SIZE,
        options, log, &log_size);
  else
    {
      char tmpl[] = "/tmp/pocl-gen7.XXXXXX";
      char *dir = mkdtemp (tmpl);
      if (dir == NULL
          || gen7_write_headers (dir, num_input_headers, input_headers,
                                 header_include_names))
        {
          free (log);
          return CL_COMPILE_PROGRAM_FAILURE;
        }
      gbe = compiler_program_compile_from_source (
          gen7_pci_id (program, device_i), program->source, dir,
          GEN7_BUILD_LOG_SIZE, options, log, &log_size);
    }

  gen7_set_build_log (program, device_i, log, log_size);
  gen7_trace_program (program->source, options, gen7_trace_ms () - t0,
                      gbe != NULL);
  int invalid_options = log_size > 0
                        && strstr (log, "error: error reading 'options'");
  free (log);
  if (gbe == NULL)
    return invalid_options ? CL_INVALID_BUILD_OPTIONS
                           : (link_program ? CL_BUILD_PROGRAM_FAILURE
                                           : CL_COMPILE_PROGRAM_FAILURE);

  gen7_set_program (program, device_i, gbe);
  return gen7_store_program (program, device_i, gbe, link_program ? 0 : 1);
}

int
pocl_gen7_supports_binary (cl_device_id device, size_t length,
                           const char *binary)
{
  const unsigned char *b = (const unsigned char *)binary;
  return gen7_is_gen_binary (b, length) || gen7_is_llvm_binary (b, length)
         || pocl_bitcode_is_spirv_execmodel_kernel (binary, length, 0);
}

int
pocl_gen7_build_binary (cl_program program, cl_uint device_i,
                        int link_program, int spir_build)
{
  uint32_t id = gen7_pci_id (program, device_i);

  if (program->program_il && program->program_il_size
      && program->binaries[device_i] == NULL
      && program->pocl_binaries[device_i] == NULL)
    {
      /* SPIR-V: translate to LLVM IR; libgbe takes it like a SPIR binary
         (its 1 'B' 'C' header, "compiled object") */
      char *bc = NULL;
      uint64_t bc_size = 0;
      int err = gen7_spirv_to_bitcode (program, device_i, &bc, &bc_size);
      if (err != CL_SUCCESS)
        return err;
      program->binaries[device_i] = malloc (bc_size + 1);
      program->binaries[device_i][0] = 1;
      memcpy (program->binaries[device_i] + 1, bc, bc_size);
      program->binary_sizes[device_i] = bc_size + 1;
      free (bc);
    }

  if (program->pocl_binaries[device_i] && program->binaries[device_i] == NULL)
    {
      /* clCreateProgramWithBinary unpacked a pocl binary into the cache
         dir; our program is in program.gen there */
      char path[POCL_MAX_PATHNAME_LENGTH];
      gen7_program_path (path, program, device_i);
      char *content = NULL;
      uint64_t size = 0;
      if (pocl_read_file (path, &content, &size))
        POCL_RETURN_ERROR_ON (1, CL_INVALID_BINARY,
                              "gen7: pocl binary has no %s\n", path);
      program->binaries[device_i] = (unsigned char *)content;
      program->binary_sizes[device_i] = size;
    }

  const unsigned char *bin = program->binaries[device_i];
  size_t size = program->binary_sizes[device_i];
  POCL_RETURN_ERROR_ON ((bin == NULL), CL_INVALID_BINARY,
                        "gen7: no binary\n");

  gbe_program gbe = NULL;
  if (gen7_is_gen_binary (bin, size))
    gbe = interp_program_new_from_binary (id, (const char *)bin, size);
  else if (gen7_is_llvm_binary (bin, size))
    {
      gbe = compiler_program_new_from_llvm_binary (id, (const char *)bin,
                                                   size);
      if (gbe && link_program)
        {
          /* executable from an IR binary: generate Gen code now */
          char *log = calloc (1, GEN7_BUILD_LOG_SIZE);
          size_t log_size = 0;
          compiler_program_build_from_llvm (gbe, GEN7_BUILD_LOG_SIZE, log,
                                            &log_size,
                                            program->compiler_options);
          gen7_set_build_log (program, device_i, log, log_size);
          free (log);
          gen7_set_program (program, device_i, gbe);
          return gen7_store_program (program, device_i, gbe, 0);
        }
    }
  POCL_RETURN_ERROR_ON ((gbe == NULL), CL_INVALID_BINARY,
                        "gen7: libgbe rejected the binary\n");

  gen7_set_program (program, device_i, gbe);
  /* make sure the cache dir (and program.gen) exists for pocl binaries */
  char *copy = malloc (size);
  memcpy (copy, bin, size);
  int err = CL_SUCCESS;
  char path[POCL_MAX_PATHNAME_LENGTH];
  if (pocl_cache_create_program_cachedir (program, device_i, (char *)copy,
                                          size, path))
    err = CL_BUILD_PROGRAM_FAILURE;
  else
    {
      gen7_program_path (path, program, device_i);
      if (!pocl_exists (path) && pocl_write_file (path, copy, size, 0))
        err = CL_BUILD_PROGRAM_FAILURE;
    }
  free (copy);
  return err;
}

int
pocl_gen7_link_program (cl_program program, cl_uint device_i,
                        cl_uint num_input_programs,
                        const cl_program *input_programs, int create_library)
{
  uint32_t id = gen7_pci_id (program, device_i);
  cl_device_id dev = program->devices[device_i];
  char *log = calloc (1, GEN7_BUILD_LOG_SIZE);
  size_t log_size = 0;

  gbe_program gbe = compiler_program_new_gen_program (id, NULL, NULL, NULL);
  for (cl_uint i = 0; i < num_input_programs; i++)
    {
      cl_program in = input_programs[i];
      /* find the input program's data for this device */
      gen7_program_t *ip = NULL;
      for (cl_uint j = 0; j < in->num_devices; j++)
        if (in->devices[j] == dev)
          ip = in->data[j];
      if (ip == NULL || ip->gbe == NULL
          || compiler_program_link_program (gbe, ip->gbe, GEN7_BUILD_LOG_SIZE,
                                            log, &log_size))
        {
          gen7_set_build_log (program, device_i, log, log_size);
          free (log);
          interp_program_delete (gbe);
          return CL_LINK_PROGRAM_FAILURE;
        }
    }

  if (!create_library)
    compiler_program_build_from_llvm (gbe, GEN7_BUILD_LOG_SIZE, log,
                                      &log_size, program->compiler_options);
  gen7_set_build_log (program, device_i, log, log_size);
  free (log);

  gen7_set_program (program, device_i, gbe);
  return gen7_store_program (program, device_i, gbe, create_library ? 2 : 0);
}

int
pocl_gen7_free_program (cl_device_id device, cl_program program,
                        unsigned program_device_i)
{
  gen7_program_t *p = program->data[program_device_i];
  if (p)
    {
      if (p->gbe)
        interp_program_delete (p->gbe);
      free (p);
      program->data[program_device_i] = NULL;
    }
  return 0;
}

int
pocl_gen7_build_poclbinary (cl_program program, cl_uint device_i)
{
  /* program.gen was written to the cache dir when the program was built */
  return CL_SUCCESS;
}

/*************************** kernel metadata *****************************/

static char *
gen7_arg_string (gbe_kernel k, uint32_t i, uint32_t what)
{
  const char *s = interp_kernel_get_arg_info (k, i, what);
  return strdup (s ? s : "");
}

static void
gen7_fill_arg_info (gbe_kernel k, uint32_t i, struct pocl_argument_info *a)
{
  enum gbe_arg_type t = interp_kernel_get_arg_type (k, i);
  switch (t)
    {
    case GBE_ARG_GLOBAL_PTR:
    case GBE_ARG_CONSTANT_PTR:
    case GBE_ARG_LOCAL_PTR:
      a->type = POCL_ARG_TYPE_POINTER;
      break;
    case GBE_ARG_IMAGE:
      a->type = POCL_ARG_TYPE_IMAGE;
      break;
    case GBE_ARG_SAMPLER:
      a->type = POCL_ARG_TYPE_SAMPLER;
      break;
    case GBE_ARG_PIPE:
      a->type = POCL_ARG_TYPE_PIPE;
      break;
    default:
      a->type = POCL_ARG_TYPE_NONE;
      a->type_size = interp_kernel_get_arg_size (k, i);
      break;
    }

  /* same mapping as beignet cl_get_kernel_arg_info */
  size_t as = (size_t)interp_kernel_get_arg_info (k, i,
                                                  GBE_GET_ARG_INFO_ADDRSPACE);
  if (t == GBE_ARG_LOCAL_PTR || as == 3)
    a->address_qualifier = CL_KERNEL_ARG_ADDRESS_LOCAL;
  else if (t == GBE_ARG_CONSTANT_PTR || as == 2)
    a->address_qualifier = CL_KERNEL_ARG_ADDRESS_CONSTANT;
  else if (t == GBE_ARG_GLOBAL_PTR || t == GBE_ARG_IMAGE || as == 1 || as == 4)
    a->address_qualifier = CL_KERNEL_ARG_ADDRESS_GLOBAL;
  else
    a->address_qualifier = CL_KERNEL_ARG_ADDRESS_PRIVATE;

  const char *acc = interp_kernel_get_arg_info (k, i, GBE_GET_ARG_INFO_ACCESS);
  if (acc && !strcmp (acc, "write_only"))
    a->access_qualifier = CL_KERNEL_ARG_ACCESS_WRITE_ONLY;
  else if (acc && !strcmp (acc, "read_only"))
    a->access_qualifier = CL_KERNEL_ARG_ACCESS_READ_ONLY;
  else if (acc && !strcmp (acc, "read_write"))
    a->access_qualifier = CL_KERNEL_ARG_ACCESS_READ_WRITE;
  else
    a->access_qualifier = CL_KERNEL_ARG_ACCESS_NONE;
  /* images default to read_only */
  if (t == GBE_ARG_IMAGE && a->access_qualifier == CL_KERNEL_ARG_ACCESS_NONE)
    a->access_qualifier = CL_KERNEL_ARG_ACCESS_READ_ONLY;

  const char *q = interp_kernel_get_arg_info (k, i, GBE_GET_ARG_INFO_TYPEQUAL);
  a->type_qualifier = CL_KERNEL_ARG_TYPE_NONE;
  if (q)
    {
      if (strstr (q, "const")
          && (t == GBE_ARG_GLOBAL_PTR || t == GBE_ARG_CONSTANT_PTR
              || t == GBE_ARG_LOCAL_PTR))
        a->type_qualifier |= CL_KERNEL_ARG_TYPE_CONST;
      if (strstr (q, "volatile"))
        a->type_qualifier |= CL_KERNEL_ARG_TYPE_VOLATILE;
      if (strstr (q, "restrict"))
        a->type_qualifier |= CL_KERNEL_ARG_TYPE_RESTRICT;
      if (strstr (q, "pipe"))
        a->type_qualifier = CL_KERNEL_ARG_TYPE_PIPE;
    }

  a->type_name = gen7_arg_string (k, i, GBE_GET_ARG_INFO_TYPE);
  a->name = gen7_arg_string (k, i, GBE_GET_ARG_INFO_NAME);
}

int
pocl_gen7_setup_metadata (cl_device_id device, cl_program program,
                          unsigned program_device_i)
{
  gen7_program_t *p = program->data[program_device_i];
  if (p == NULL || p->gbe == NULL)
    return 0;
  gen7_device_t *d = device->data;
  unsigned n = interp_program_get_kernel_num (p->gbe);
  program->num_kernels = n;
  if (n == 0)
    return 1;
  program->kernel_meta = calloc (n, sizeof (pocl_kernel_metadata_t));

  for (unsigned i = 0; i < n; i++)
    {
      gbe_kernel k = interp_program_get_kernel (p->gbe, i);
      pocl_kernel_metadata_t *m = &program->kernel_meta[i];
      m->name = strdup (interp_kernel_get_name (k));
      m->num_args = interp_kernel_get_arg_num (k);
      m->arg_info = calloc (m->num_args ? m->num_args : 1,
                            sizeof (struct pocl_argument_info));
      for (unsigned a = 0; a < m->num_args; a++)
        gen7_fill_arg_info (k, a, &m->arg_info[a]);
      m->has_arg_metadata = POCL_HAS_KERNEL_ARG_ADDRESS_QUALIFIER
                            | POCL_HAS_KERNEL_ARG_ACCESS_QUALIFIER
                            | POCL_HAS_KERNEL_ARG_TYPE_NAME
                            | POCL_HAS_KERNEL_ARG_TYPE_QUALIFIER
                            | POCL_HAS_KERNEL_ARG_NAME;

      size_t wg[3] = { 0, 0, 0 };
      interp_kernel_get_compile_wg_size (k, wg);
      memcpy (m->reqd_wg_size, wg, sizeof (wg));
      const char *attr = interp_kernel_get_attributes (k);
      m->attributes = (attr && attr[0]) ? strdup (attr) : NULL;

      unsigned nd = program->num_devices;
      m->data = calloc (nd, sizeof (void *));
      m->max_workgroup_size = calloc (nd, sizeof (size_t));
      m->preferred_wg_multiple = calloc (nd, sizeof (size_t));
      m->local_mem_size = calloc (nd, sizeof (cl_ulong));
      m->private_mem_size = calloc (nd, sizeof (cl_ulong));
      m->spill_mem_size = calloc (nd, sizeof (cl_ulong));
      uint32_t simd = interp_kernel_get_simd_width (k);
      /* beignet cl_get_kernel_max_wg_sz */
      size_t max_wg;
      if (!interp_kernel_use_slm (k))
        max_wg = simd * 64;
      else
        {
          size_t sub_slices = IS_IVB_GT1 (d->pci_id) ? 1 : 2;
          size_t threads
              = d->max_compute_unit * d->max_thread_per_unit / sub_slices;
          max_wg = (threads > 64 ? 64 : threads) * simd;
        }
      if (max_wg > device->max_work_group_size)
        max_wg = device->max_work_group_size;
      m->max_workgroup_size[program_device_i] = max_wg;
      m->preferred_wg_multiple[program_device_i] = simd;
      m->local_mem_size[program_device_i] = interp_kernel_get_slm_size (k);
      m->private_mem_size[program_device_i] = interp_kernel_get_stack_size (k);
    }
  return 1;
}

/******************************* kernels **********************************/

int
pocl_gen7_create_kernel (cl_device_id device, cl_program program,
                         cl_kernel kernel, unsigned device_i)
{
  gen7_device_t *d = device->data;
  gen7_program_t *p = program->data[device_i];
  if (p == NULL || p->gbe == NULL)
    return CL_INVALID_PROGRAM_EXECUTABLE;
  gbe_kernel k = interp_program_get_kernel_by_name (p->gbe, kernel->name);
  if (k == NULL)
    return CL_INVALID_KERNEL_NAME;

  gen7_kernel_t *gk = calloc (1, sizeof (gen7_kernel_t));
  gk->gbe = k;
  gk->simd = interp_kernel_get_simd_width (k);
  gk->curbe_size = interp_kernel_get_curbe_size (k);
  gk->stack_size = interp_kernel_get_stack_size (k);

  /* upload the Gen code (beignet cl_kernel_setup) */
  const uint32_t code_sz = interp_kernel_get_code_size (k);
  POCL_LOCK (d->hw_lock);
  gk->code_bo = cl_buffer_alloc (d->bufmgr, "CL kernel", code_sz, 64u);
  cl_buffer_subdata (gk->code_bo, 0, code_sz, interp_kernel_get_code (k));
  POCL_UNLOCK (d->hw_lock);

  gk->sampler_sz = interp_kernel_get_sampler_size (k);
  assert (gk->sampler_sz <= GEN7_MAX_SAMPLERS);
  if (gk->sampler_sz)
    interp_kernel_get_sampler_data (k, gk->samplers);
  gk->image_sz = interp_kernel_get_image_size (k);
  if (gk->image_sz)
    {
      gk->images = calloc (gk->image_sz, sizeof (ImageInfo));
      interp_kernel_get_image_data (k, gk->images);
    }

  gen7_curbe_layout_t *l = &gk->layout;
#define OFF(E) interp_kernel_get_curbe_offset (k, E, 0)
  l->local_size[0] = OFF (GBE_CURBE_LOCAL_SIZE_X);
  l->local_size[1] = OFF (GBE_CURBE_LOCAL_SIZE_Y);
  l->local_size[2] = OFF (GBE_CURBE_LOCAL_SIZE_Z);
  l->enqueued_local_size[0] = OFF (GBE_CURBE_ENQUEUED_LOCAL_SIZE_X);
  l->enqueued_local_size[1] = OFF (GBE_CURBE_ENQUEUED_LOCAL_SIZE_Y);
  l->enqueued_local_size[2] = OFF (GBE_CURBE_ENQUEUED_LOCAL_SIZE_Z);
  l->global_size[0] = OFF (GBE_CURBE_GLOBAL_SIZE_X);
  l->global_size[1] = OFF (GBE_CURBE_GLOBAL_SIZE_Y);
  l->global_size[2] = OFF (GBE_CURBE_GLOBAL_SIZE_Z);
  l->global_offset[0] = OFF (GBE_CURBE_GLOBAL_OFFSET_X);
  l->global_offset[1] = OFF (GBE_CURBE_GLOBAL_OFFSET_Y);
  l->global_offset[2] = OFF (GBE_CURBE_GLOBAL_OFFSET_Z);
  l->group_num[0] = OFF (GBE_CURBE_GROUP_NUM_X);
  l->group_num[1] = OFF (GBE_CURBE_GROUP_NUM_Y);
  l->group_num[2] = OFF (GBE_CURBE_GROUP_NUM_Z);
  l->thread_num = OFF (GBE_CURBE_THREAD_NUM);
  l->work_dim = OFF (GBE_CURBE_WORK_DIM);
  l->local_id[0] = OFF (GBE_CURBE_LOCAL_ID_X);
  l->local_id[1] = OFF (GBE_CURBE_LOCAL_ID_Y);
  l->local_id[2] = OFF (GBE_CURBE_LOCAL_ID_Z);
  l->block_ip = OFF (GBE_CURBE_BLOCK_IP);
  l->dw_block_ip = l->block_ip < 0 ? OFF (GBE_CURBE_DW_BLOCK_IP) : -1;
  l->thread_id = OFF (GBE_CURBE_THREAD_ID);
#undef OFF

  kernel->data[device_i] = gk;
  return CL_SUCCESS;
}

int
pocl_gen7_free_kernel (cl_device_id device, cl_program program,
                       cl_kernel kernel, unsigned device_i)
{
  gen7_kernel_t *gk = kernel->data[device_i];
  if (gk == NULL)
    return CL_SUCCESS;
  if (gk->code_bo)
    cl_buffer_unreference (gk->code_bo);
  free (gk->images);
  free (gk);
  kernel->data[device_i] = NULL;
  return CL_SUCCESS;
}
