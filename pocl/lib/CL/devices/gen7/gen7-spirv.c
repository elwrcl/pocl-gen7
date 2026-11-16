/* gen7-spirv.c - SPIR-V input for the gen7 driver

   SPIR-V is translated back to LLVM IR with llvm-spirv the way libgbe needs
   it: OpenCL 1.2 builtin names (--spirv-target-env=CL1.2) and kernel
   argument names (libgbe finds kernels by them). libgbe then links the IR
   with beignet's libocl like a SPIR binary; images/samplers are fixed up
   there (llvm_spirv_handles.cpp).
*/

#include "pocl-gen7.h"

#include "pocl_cache.h"
#include "pocl_file_util.h"
#include "pocl_llvm.h"
#include "pocl_run_command.h"
#include "pocl_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SPIR-V opcodes and enumerants used below */
#define SPV_MAGIC 0x07230203u
#define SPV_OP_CAPABILITY 17
#define SPV_OP_MEMORY_MODEL 14
#define SPV_CAP_FLOAT64 10
#define SPV_CAP_INT64_ATOMICS 12

int
gen7_spirv_unsupported (const char *il, size_t size, char *why, size_t why_len)
{
  const uint32_t *w = (const uint32_t *)il;
  size_t n = size / 4;
  if (n < 5 || w[0] != SPV_MAGIC)
    {
      snprintf (why, why_len, "gen7: not a SPIR-V module");
      return 1;
    }
  for (size_t i = 5; i < n;)
    {
      uint32_t count = w[i] >> 16, op = w[i] & 0xffff;
      if (count == 0 || i + count > n)
        {
          snprintf (why, why_len, "gen7: malformed SPIR-V");
          return 1;
        }
      if (op == SPV_OP_CAPABILITY && count >= 2)
        {
          if (w[i + 1] == SPV_CAP_FLOAT64)
            {
              snprintf (why, why_len,
                        "gen7: SPIR-V capability Float64 not supported");
              return 1;
            }
          if (w[i + 1] == SPV_CAP_INT64_ATOMICS)
            {
              snprintf (why, why_len,
                        "gen7: SPIR-V capability Int64Atomics not supported");
              return 1;
            }
        }
      i += count;
    }
  return 0;
}

static void
gen7_log (cl_program program, cl_uint device_i, const char *msg)
{
  pocl_append_to_buildlog (program, device_i, strdup (msg), strlen (msg));
}

/* "--spec-const=id:type:value ..." for the constants set by the
   application; types come from llvm-spirv --spec-const-info. NULL if none. */
static char *
gen7_spec_const_arg (cl_program program, const char *spv_path)
{
  int any = 0;
  for (size_t i = 0; i < program->num_spec_consts; i++)
    any |= program->spec_const_is_set[i];
  if (!any)
    return NULL;

  char info[8192];
  size_t info_len = sizeof (info) - 1;
  const char *args[] = { LLVM_SPIRV, "--spec-const-info", spv_path, NULL };
  if (pocl_run_command_capture_output (info, &info_len, args) != 0)
    return NULL;
  info[info_len] = 0;

  size_t cap = 64 + program->num_spec_consts * 48;
  char *arg = malloc (cap);
  strcpy (arg, "--spec-const=");
  for (size_t i = 0; i < program->num_spec_consts; i++)
    {
      if (!program->spec_const_is_set[i])
        continue;
      char key[64], type[8] = "";
      snprintf (key, sizeof (key), "Spec const id = %u,",
                program->spec_const_ids[i]);
      const char *p = strstr (info, key);
      const char *t = p ? strstr (p, "type = ") : NULL;
      if (t && sscanf (t, "type = %7s", type) != 1)
        type[0] = 0;
      if (!type[0])
        continue;
      char one[48];
      uint64_t v = program->spec_const_values[i];
      if (type[0] == 'f')
        {
          /* float values are passed as hex bit patterns */
          snprintf (one, sizeof (one), "%s%u:%s:0x%llx",
                    arg[13] ? " " : "", program->spec_const_ids[i], type,
                    (unsigned long long)v);
        }
      else
        snprintf (one, sizeof (one), "%s%u:%s:%llu", arg[13] ? " " : "",
                  program->spec_const_ids[i], type, (unsigned long long)v);
      strcat (arg, one);
    }
  return arg;
}

int
gen7_spirv_to_bitcode (cl_program program, cl_uint device_i, char **bc,
                       uint64_t *bc_size)
{
  char why[160];
  if (gen7_spirv_unsupported (program->program_il, program->program_il_size,
                              why, sizeof (why)))
    {
      gen7_log (program, device_i, why);
      return CL_BUILD_PROGRAM_FAILURE;
    }

  char spv[POCL_MAX_PATHNAME_LENGTH], out[POCL_MAX_PATHNAME_LENGTH];
  pocl_cache_tempname (spv, ".spv", NULL);
  pocl_cache_tempname (out, ".bc", NULL);
  if (pocl_write_file (spv, program->program_il, program->program_il_size, 0))
    return CL_BUILD_PROGRAM_FAILURE;

  char *spec = gen7_spec_const_arg (program, spv);
  const char *args[] = { LLVM_SPIRV,
                         "-r",
                         "--spirv-target-env=CL1.2",
                         "--spirv-gen-kernel-arg-name-md",
                         /* OpenCL type names / qualifiers, when the producer
                            kept them (SPIR-V itself has no "float*" or
                            "const"); libgbe also reads pointee types there */
                         "--preserve-ocl-kernel-arg-type-metadata-through-string",
                         "-o",
                         out,
                         spv,
                         spec,
                         NULL };
  char log[8192];
  size_t log_len = sizeof (log) - 1;
  int r = pocl_run_command_capture_output (log, &log_len, args);
  log[log_len] = 0;
  free (spec);
  pocl_remove (spv);
  if (r != 0 || pocl_read_file (out, bc, bc_size) != 0)
    {
      gen7_log (program, device_i, "gen7: llvm-spirv failed:\n");
      gen7_log (program, device_i, log);
      pocl_remove (out);
      return CL_BUILD_PROGRAM_FAILURE;
    }
  pocl_remove (out);
  return CL_SUCCESS;
}

/* POCL_GEN7_SPIRV_ROUNDTRIP=1 (testing): clBuildProgram from source goes
   through SPIR-V. The source is compiled with pocl's clang for the spir
   target (the device's extensions enabled, the application's options passed
   on), turned into SPIR-V, and built from IL. With the nix clang wrapper,
   run with NIX_HARDENING_ENABLE= set. */
int
gen7_spirv_roundtrip (cl_program program, cl_uint device_i)
{
  cl_device_id dev = program->devices[device_i];
  char cl[POCL_MAX_PATHNAME_LENGTH], bc[POCL_MAX_PATHNAME_LENGTH],
      spv[POCL_MAX_PATHNAME_LENGTH];
  pocl_cache_tempname (cl, ".cl", NULL);
  pocl_cache_tempname (bc, ".bc", NULL);
  pocl_cache_tempname (spv, ".spv", NULL);
  if (pocl_write_file (cl, program->source, strlen (program->source), 0))
    return CL_BUILD_PROGRAM_FAILURE;

  /* -Xclang -cl-ext=+ext1,+ext2,... from the device's extension list */
  char ext[2048] = "-cl-ext=";
  const char *e = dev->extensions;
  while (e && *e)
    {
      while (*e == ' ')
        e++;
      const char *end = strchr (e, ' ');
      size_t n = end ? (size_t)(end - e) : strlen (e);
      if (n && strlen (ext) + n + 3 < sizeof (ext))
        {
          if (ext[8])
            strcat (ext, ",");
          strcat (ext, "+");
          strncat (ext, e, n);
        }
      e += n;
    }

  const char *args[256];
  int a = 0;
  args[a++] = CLANGCC;
  args[a++] = "-target";
  args[a++] = "spir";
  args[a++] = "-emit-llvm";
  args[a++] = "-c";
  args[a++] = "-O2";
  args[a++] = "-Xclang";
  args[a++] = "-finclude-default-header";
  args[a++] = "-Xclang";
  args[a++] = ext;
  char *opts = strdup (program->compiler_options ? program->compiler_options
                                                 : "");
  int has_std = strstr (opts, "-cl-std=") != NULL;
  if (!has_std)
    args[a++] = "-cl-std=CL1.2";
  for (char *t = strtok (opts, " \t"); t && a < 240; t = strtok (NULL, " \t"))
    args[a++] = t;
  args[a++] = "-o";
  args[a++] = bc;
  args[a++] = cl;
  args[a] = NULL;

  char log[16384];
  size_t log_len = sizeof (log) - 1;
  int r = pocl_run_command_capture_output (log, &log_len, args);
  log[log_len] = 0;
  free (opts);
  pocl_remove (cl);
  if (r != 0)
    {
      gen7_log (program, device_i, "gen7 roundtrip: clang failed:\n");
      gen7_log (program, device_i, log);
      return CL_BUILD_PROGRAM_FAILURE;
    }

  const char *fwd[] = { LLVM_SPIRV, "--spirv-ext=+all",
                        "--preserve-ocl-kernel-arg-type-metadata-through-string",
                        bc, "-o", spv, NULL };
  log_len = sizeof (log) - 1;
  r = pocl_run_command_capture_output (log, &log_len, fwd);
  log[log_len] = 0;
  pocl_remove (bc);
  char *il = NULL;
  uint64_t il_size = 0;
  if (r != 0 || pocl_read_file (spv, &il, &il_size) != 0)
    {
      gen7_log (program, device_i, "gen7 roundtrip: llvm-spirv failed:\n");
      gen7_log (program, device_i, log);
      pocl_remove (spv);
      return CL_BUILD_PROGRAM_FAILURE;
    }
  pocl_remove (spv);

  POCL_MEM_FREE (program->program_il);
  program->program_il = il;
  program->program_il_size = il_size;
  POCL_MEM_FREE (program->binaries[device_i]);
  program->binary_sizes[device_i] = 0;
  return pocl_gen7_build_binary (program, device_i, 1, 0);
}
