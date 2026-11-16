/* cl_khr_gl_sharing on top of Mesa's MESA_GLinterop and the drivers'
   dma-buf import (ops->import_dmabuf): the GL object is exported as a
   dma-buf with glXGLInteropExportObjectMESA (or the EGL variant) and the
   cl_mem is bound to the same memory, without copying.

   libGL is not linked: the application that owns the GL context has it
   loaded and the entry points are looked up through glXGetProcAddress /
   eglGetProcAddress found with dlsym(RTLD_DEFAULT).

   Permission is hereby granted, free of charge, to any person obtaining a
   copy of this software and associated documentation files (the "Software"),
   to deal in the Software without restriction, including without limitation
   the rights to use, copy, modify, merge, publish, distribute, sublicense,
   and/or sell copies of the Software, and to permit persons to whom the
   Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
   IN THE SOFTWARE.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "pocl_cl.h"
#include "pocl_gl_sharing.h"
#include "pocl_util.h"

/* Mesa's include/GL/mesa_glinterop.h ABI */
enum
{
  MESA_GLINTEROP_SUCCESS = 0,
  MESA_GLINTEROP_OUT_OF_RESOURCES,
  MESA_GLINTEROP_OUT_OF_HOST_MEMORY,
  MESA_GLINTEROP_INVALID_OPERATION,
  MESA_GLINTEROP_INVALID_VERSION,
  MESA_GLINTEROP_INVALID_DISPLAY,
  MESA_GLINTEROP_INVALID_CONTEXT,
  MESA_GLINTEROP_INVALID_TARGET,
  MESA_GLINTEROP_INVALID_OBJECT,
  MESA_GLINTEROP_INVALID_MIP_LEVEL,
  MESA_GLINTEROP_UNSUPPORTED
};

enum
{
  MESA_GLINTEROP_ACCESS_READ_WRITE = 0,
  MESA_GLINTEROP_ACCESS_READ_ONLY,
  MESA_GLINTEROP_ACCESS_WRITE_ONLY
};

struct mesa_glinterop_device_info
{
  uint32_t version;
  uint32_t pci_segment_group;
  uint32_t pci_bus;
  uint32_t pci_device;
  uint32_t pci_function;
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t driver_data_size;
  void *driver_data;
  char device_uuid[16];
};

struct mesa_glinterop_export_in
{
  uint32_t version;
  unsigned target;
  unsigned obj;
  unsigned miplevel;
  uint32_t access;
  uint32_t out_driver_data_size;
  void *out_driver_data;
};

struct mesa_glinterop_export_out
{
  uint32_t version;
  int dmabuf_fd;
  unsigned internal_format;
  ptrdiff_t buf_offset;
  ptrdiff_t buf_size;
  unsigned view_minlevel;
  unsigned view_numlevels;
  unsigned view_minlayer;
  unsigned view_numlayers;
  uint32_t out_driver_data_written;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t stride;
  uint64_t modifier;
};

struct mesa_glinterop_flush_out
{
  uint32_t version;
  void **sync;
  int *fence_fd;
};

/* GLX and EGL variants take (display, context, ...) */
typedef int (*query_device_info_fn) (void *dpy, void *ctx,
                                     struct mesa_glinterop_device_info *out);
typedef int (*export_object_fn) (void *dpy, void *ctx,
                                 struct mesa_glinterop_export_in *in,
                                 struct mesa_glinterop_export_out *out);
typedef int (*flush_objects_fn) (void *dpy, void *ctx, unsigned count,
                                 struct mesa_glinterop_export_in *in,
                                 struct mesa_glinterop_flush_out *out);
typedef void *(*get_proc_address_fn) (const char *name);

/* GL enums */
#define GL_NONE 0
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_RECTANGLE 0x84F5
#define GL_TEXTURE_1D_ARRAY 0x8C18
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_RENDERBUFFER 0x8D41
#define GL_ARRAY_BUFFER 0x8892

typedef struct
{
  void *gl_ctx;
  void *dpy;
  int egl;
} gl_ctx_t;

typedef struct
{
  query_device_info_fn query;
  export_object_fn export;
  flush_objects_fn flush;
} interop_fns_t;

static int
interop_fns (int egl, interop_fns_t *f)
{
  get_proc_address_fn gpa = (get_proc_address_fn)dlsym (
      RTLD_DEFAULT, egl ? "eglGetProcAddress" : "glXGetProcAddressARB");
  if (gpa == NULL && !egl)
    gpa = (get_proc_address_fn)dlsym (RTLD_DEFAULT, "glXGetProcAddress");
  if (gpa == NULL)
    return 0;
  f->query = (query_device_info_fn)gpa (
      egl ? "eglGLInteropQueryDeviceInfoMESA"
          : "glXGLInteropQueryDeviceInfoMESA");
  f->export = (export_object_fn)gpa (egl ? "eglGLInteropExportObjectMESA"
                                         : "glXGLInteropExportObjectMESA");
  f->flush = (flush_objects_fn)gpa (egl ? "eglGLInteropFlushObjectsMESA"
                                        : "glXGLInteropFlushObjectsMESA");
  return f->query && f->export && f->flush;
}

static cl_int
interop_error (int err)
{
  switch (err)
    {
    case MESA_GLINTEROP_SUCCESS:
      return CL_SUCCESS;
    case MESA_GLINTEROP_OUT_OF_RESOURCES:
      return CL_OUT_OF_RESOURCES;
    case MESA_GLINTEROP_OUT_OF_HOST_MEMORY:
      return CL_OUT_OF_HOST_MEMORY;
    case MESA_GLINTEROP_INVALID_DISPLAY:
    case MESA_GLINTEROP_INVALID_CONTEXT:
      return CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR;
    case MESA_GLINTEROP_INVALID_TARGET:
    case MESA_GLINTEROP_INVALID_OBJECT:
      return CL_INVALID_GL_OBJECT;
    case MESA_GLINTEROP_INVALID_MIP_LEVEL:
      return CL_INVALID_MIP_LEVEL;
    default:
      return CL_INVALID_OPERATION;
    }
}

static cl_int
gl_ctx_from_properties (const cl_context_properties *p, gl_ctx_t *g)
{
  void *glx = NULL, *egl = NULL;
  memset (g, 0, sizeof (*g));
  for (; p && p[0] != 0; p += 2)
    switch (p[0])
      {
      case CL_GL_CONTEXT_KHR:
        g->gl_ctx = (void *)p[1];
        break;
      case CL_GLX_DISPLAY_KHR:
        glx = (void *)p[1];
        break;
      case CL_EGL_DISPLAY_KHR:
        egl = (void *)p[1];
        break;
      }
  if (g->gl_ctx == NULL)
    return CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR;
  if (glx && egl)
    return CL_INVALID_OPERATION;
  if (glx == NULL && egl == NULL)
    return CL_INVALID_GL_SHAREGROUP_REFERENCE_KHR;
  g->dpy = egl ? egl : glx;
  g->egl = egl != NULL;
  return CL_SUCCESS;
}

static int
device_shares_gl (cl_device_id dev)
{
  return dev->ops->import_dmabuf != NULL && dev->extensions
         && strstr (dev->extensions, "cl_khr_gl_sharing");
}

int
pocl_gl_device_for_context (cl_device_id dev,
                            const cl_context_properties *properties)
{
  gl_ctx_t g;
  interop_fns_t f;
  if (!device_shares_gl (dev)
      || gl_ctx_from_properties (properties, &g) != CL_SUCCESS)
    return 0;
  if (!interop_fns (g.egl, &f))
    {
      POCL_MSG_PRINT_INFO ("GL implementation has no MESA_GLinterop\n");
      return 0;
    }
  struct mesa_glinterop_device_info info;
  memset (&info, 0, sizeof (info));
  info.version = 1;
  int err = f.query (g.dpy, g.gl_ctx, &info);
  if (err != MESA_GLINTEROP_SUCCESS)
    {
      POCL_MSG_PRINT_INFO ("GL device query failed (%d)\n", err);
      return 0;
    }
  POCL_MSG_PRINT_INFO ("GL context is on PCI %04x:%04x\n", info.vendor_id,
                       info.device_id);
  return info.vendor_id == dev->vendor_id;
}

static int
format_from_gl (unsigned internal_format, cl_image_format *fmt)
{
  static const struct
  {
    unsigned gl;
    cl_channel_order order;
    cl_channel_type type;
  } table[] = {
    { 0x8058 /* GL_RGBA8 */, CL_RGBA, CL_UNORM_INT8 },
    { 0x8C43 /* GL_SRGB8_ALPHA8 */, CL_sRGBA, CL_UNORM_INT8 },
    { 0x1908 /* GL_RGBA */, CL_RGBA, CL_UNORM_INT8 },
    { 0x8367 /* GL_UNSIGNED_INT_8_8_8_8_REV */, CL_RGBA, CL_UNORM_INT8 },
    { 0x80E1 /* GL_BGRA */, CL_BGRA, CL_UNORM_INT8 },
    { 0x8D8E /* GL_RGBA8I */, CL_RGBA, CL_SIGNED_INT8 },
    { 0x8D88 /* GL_RGBA16I */, CL_RGBA, CL_SIGNED_INT16 },
    { 0x8D82 /* GL_RGBA32I */, CL_RGBA, CL_SIGNED_INT32 },
    { 0x8D7C /* GL_RGBA8UI */, CL_RGBA, CL_UNSIGNED_INT8 },
    { 0x8D76 /* GL_RGBA16UI */, CL_RGBA, CL_UNSIGNED_INT16 },
    { 0x8D70 /* GL_RGBA32UI */, CL_RGBA, CL_UNSIGNED_INT32 },
    { 0x8F97 /* GL_RGBA8_SNORM */, CL_RGBA, CL_SNORM_INT8 },
    { 0x805B /* GL_RGBA16 */, CL_RGBA, CL_UNORM_INT16 },
    { 0x8F9B /* GL_RGBA16_SNORM */, CL_RGBA, CL_SNORM_INT16 },
    { 0x881A /* GL_RGBA16F */, CL_RGBA, CL_HALF_FLOAT },
    { 0x8814 /* GL_RGBA32F */, CL_RGBA, CL_FLOAT },
    { 0x8229 /* GL_R8 */, CL_R, CL_UNORM_INT8 },
    { 0x8F94 /* GL_R8_SNORM */, CL_R, CL_SNORM_INT8 },
    { 0x822A /* GL_R16 */, CL_R, CL_UNORM_INT16 },
    { 0x8F98 /* GL_R16_SNORM */, CL_R, CL_SNORM_INT16 },
    { 0x822D /* GL_R16F */, CL_R, CL_HALF_FLOAT },
    { 0x822E /* GL_R32F */, CL_R, CL_FLOAT },
    { 0x8231 /* GL_R8I */, CL_R, CL_SIGNED_INT8 },
    { 0x8233 /* GL_R16I */, CL_R, CL_SIGNED_INT16 },
    { 0x8235 /* GL_R32I */, CL_R, CL_SIGNED_INT32 },
    { 0x8232 /* GL_R8UI */, CL_R, CL_UNSIGNED_INT8 },
    { 0x8234 /* GL_R16UI */, CL_R, CL_UNSIGNED_INT16 },
    { 0x8236 /* GL_R32UI */, CL_R, CL_UNSIGNED_INT32 },
    { 0x822B /* GL_RG8 */, CL_RG, CL_UNORM_INT8 },
    { 0x8F95 /* GL_RG8_SNORM */, CL_RG, CL_SNORM_INT8 },
    { 0x822C /* GL_RG16 */, CL_RG, CL_UNORM_INT16 },
    { 0x8F99 /* GL_RG16_SNORM */, CL_RG, CL_SNORM_INT16 },
    { 0x822F /* GL_RG16F */, CL_RG, CL_HALF_FLOAT },
    { 0x8230 /* GL_RG32F */, CL_RG, CL_FLOAT },
    { 0x8237 /* GL_RG8I */, CL_RG, CL_SIGNED_INT8 },
    { 0x8239 /* GL_RG16I */, CL_RG, CL_SIGNED_INT16 },
    { 0x823B /* GL_RG32I */, CL_RG, CL_SIGNED_INT32 },
    { 0x8238 /* GL_RG8UI */, CL_RG, CL_UNSIGNED_INT8 },
    { 0x823A /* GL_RG16UI */, CL_RG, CL_UNSIGNED_INT16 },
    { 0x823C /* GL_RG32UI */, CL_RG, CL_UNSIGNED_INT32 },
  };
  for (size_t i = 0; i < sizeof (table) / sizeof (table[0]); ++i)
    if (table[i].gl == internal_format)
      {
        fmt->image_channel_order = table[i].order;
        fmt->image_channel_data_type = table[i].type;
        return 1;
      }
  return 0;
}

static int
is_2d_target (unsigned target)
{
  return target == GL_TEXTURE_2D || target == GL_TEXTURE_RECTANGLE
         || target == GL_RENDERBUFFER;
}

static uint32_t
interop_access (cl_mem_flags flags)
{
  if (flags & CL_MEM_READ_ONLY)
    return MESA_GLINTEROP_ACCESS_READ_ONLY;
  if (flags & CL_MEM_WRITE_ONLY)
    return MESA_GLINTEROP_ACCESS_WRITE_ONLY;
  return MESA_GLINTEROP_ACCESS_READ_WRITE;
}

/* POCL_GEN7_DUMP=<dir>: the GL sharing calls go to the same session.log as
   pocl-gen7's recorder (devices/gen7/gen7-trace.c). */
static int gl_trace_fd = -1;
static pthread_once_t gl_trace_once = PTHREAD_ONCE_INIT;

static double
gl_now_ms (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void
gl_trace_init (void)
{
  const char *d = getenv ("POCL_GEN7_DUMP");
  if (d == NULL || *d == 0)
    return;
  char path[4096];
  snprintf (path, sizeof (path), "%s/session.log", d);
  gl_trace_fd = open (path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

static void gl_trace (const char *fmt, ...)
    __attribute__ ((format (printf, 1, 2)));
static void
gl_trace (const char *fmt, ...)
{
  pthread_once (&gl_trace_once, gl_trace_init);
  if (gl_trace_fd < 0)
    return;
  char line[1024];
  int n = snprintf (line, sizeof (line), "%.3f [%ld] gl ",
                    gl_now_ms () / 1e3, (long)syscall (SYS_gettid));
  va_list ap;
  va_start (ap, fmt);
  n += vsnprintf (line + n, sizeof (line) - n - 1, fmt, ap);
  va_end (ap);
  if (n > (int)sizeof (line) - 2)
    n = sizeof (line) - 2;
  line[n++] = '\n';
  if (write (gl_trace_fd, line, n) < 0)
    return;
}

/* Submits the GL work on the objects and waits for it: the CL work that
   follows reads what GL rendered. POCL_GL_NOWAIT=1 leaves the ordering to
   the kernel's implicit sync on the shared buffer objects instead of
   blocking the host on the fence. */
static cl_int
flush_gl (gl_ctx_t *g, interop_fns_t *f, unsigned count,
          struct mesa_glinterop_export_in *in)
{
  int fence = -1;
  struct mesa_glinterop_flush_out out = { 1, NULL, &fence };
  int err = f->flush (g->dpy, g->gl_ctx, count, in, &out);
  if (err != MESA_GLINTEROP_SUCCESS)
    return interop_error (err);
  static int nowait = -1;
  if (nowait < 0)
    {
      const char *e = getenv ("POCL_GL_NOWAIT");
      nowait = e && *e && *e != '0';
    }
  double t0 = gl_now_ms ();
  if (fence >= 0)
    {
      struct pollfd pfd = { fence, POLLIN, 0 };
      while (!nowait && poll (&pfd, 1, -1) < 0)
        ;
      close (fence);
    }
  gl_trace ("flush %u objects, fence %d, waited %.3f ms", count, fence,
            gl_now_ms () - t0);
  return CL_SUCCESS;
}

cl_mem
pocl_gl_create_mem (cl_context context, cl_mem_flags flags, unsigned target,
                    int miplevel, unsigned object, cl_int *errcode_ret)
{
  cl_int errcode = CL_SUCCESS;
  cl_mem mem = NULL;
  gl_ctx_t g;
  interop_fns_t f;
  struct mesa_glinterop_export_out out;
  memset (&out, 0, sizeof (out));
  out.dmabuf_fd = -1;

  POCL_GOTO_ERROR_COND ((!IS_CL_OBJECT_VALID (context)), CL_INVALID_CONTEXT);
  flags &= CL_MEM_READ_ONLY | CL_MEM_WRITE_ONLY | CL_MEM_READ_WRITE;
  POCL_GOTO_ERROR_COND ((flags == 0), CL_INVALID_VALUE);
  errcode = gl_ctx_from_properties (context->properties, &g);
  POCL_GOTO_ERROR_ON ((errcode != CL_SUCCESS), CL_INVALID_CONTEXT,
                      "context was not created from a GL context\n");
  POCL_GOTO_ERROR_ON ((!interop_fns (g.egl, &f)), CL_INVALID_CONTEXT,
                      "the GL implementation has no MESA_GLinterop\n");
  POCL_GOTO_ERROR_ON ((target != GL_ARRAY_BUFFER && !is_2d_target (target)),
                      CL_INVALID_GL_OBJECT,
                      "GL target %#x can't be shared (only buffers, 2D "
                      "textures and renderbuffers)\n",
                      target);
  POCL_GOTO_ERROR_ON ((miplevel != 0), CL_INVALID_MIP_LEVEL,
                      "only mip level 0 can be shared\n");

  struct mesa_glinterop_export_in in;
  memset (&in, 0, sizeof (in));
  in.version = 2;
  in.target = target;
  in.obj = object;
  in.miplevel = (unsigned)miplevel;
  in.access = interop_access (flags);
  out.version = 2;

  errcode = flush_gl (&g, &f, 1, &in);
  if (errcode != CL_SUCCESS)
    goto ERROR;
  errcode = interop_error (f.export (g.dpy, g.gl_ctx, &in, &out));
  POCL_GOTO_ERROR_ON ((errcode != CL_SUCCESS), errcode,
                      "exporting GL object %u (target %#x) failed\n", object,
                      target);
  POCL_GOTO_ERROR_ON ((out.version < 2 && target != GL_ARRAY_BUFFER),
                      CL_INVALID_OPERATION,
                      "MESA_GLinterop is too old to report texture sizes\n");
  off_t fd_size = lseek (out.dmabuf_fd, 0, SEEK_END);
  POCL_GOTO_ERROR_ON ((fd_size <= 0), CL_INVALID_GL_OBJECT,
                      "GL object %u has no size\n", object);

  size_t offset = 0, row_pitch = 0;
  if (target == GL_ARRAY_BUFFER)
    {
      POCL_GOTO_ERROR_ON ((out.buf_size <= 0), CL_INVALID_GL_OBJECT,
                          "GL buffer %u has no data store\n", object);
      mem = POname (clCreateBuffer) (context, flags, (size_t)out.buf_size,
                                     NULL, &errcode);
      offset = (size_t)out.buf_offset;
    }
  else
    {
      cl_image_format fmt;
      POCL_GOTO_ERROR_ON ((!format_from_gl (out.internal_format, &fmt)),
                          CL_INVALID_IMAGE_FORMAT_DESCRIPTOR,
                          "GL internal format %#x has no CL format\n",
                          out.internal_format);
      cl_image_desc desc;
      memset (&desc, 0, sizeof (desc));
      desc.image_type = CL_MEM_OBJECT_IMAGE2D;
      desc.image_width = out.width;
      desc.image_height = out.height;
      mem = POname (clCreateImage) (context, flags, &fmt, &desc, NULL,
                                    &errcode);
      row_pitch = out.stride;
      POCL_GOTO_ERROR_ON (
          (mem != NULL
           && (row_pitch < mem->image_row_pitch
               || row_pitch * out.height > (size_t)fd_size)),
          CL_INVALID_GL_OBJECT,
          "GL texture %u: stride %zu doesn't fit %ux%u in %zu bytes\n",
          object, row_pitch, out.width, out.height, (size_t)fd_size);
    }
  if (mem == NULL)
    goto ERROR;

  for (cl_uint i = 0; i < context->num_devices; ++i)
    {
      cl_device_id dev = context->devices[i];
      if (!device_shares_gl (dev))
        continue;
      errcode = dev->ops->import_dmabuf (dev, mem, out.dmabuf_fd,
                                         (size_t)fd_size, offset, row_pitch);
      if (errcode != CL_SUCCESS)
        goto ERROR;
    }
  mem->target = target;
  mem->miplevel = miplevel;
  mem->texture = object;
  gl_trace ("create %p target %#x obj %u flags %#lx ifmt %#x %ux%u stride %u "
            "modifier %#llx fd size %lld buf %td+%td",
            (void *)mem, target, object, (unsigned long)flags,
            out.internal_format, out.width, out.height, out.stride,
            (unsigned long long)out.modifier, (long long)fd_size,
            out.buf_offset, out.buf_size);

ERROR:
  /* the imported GEM objects keep the memory alive */
  if (out.dmabuf_fd >= 0)
    close (out.dmabuf_fd);
  if (errcode != CL_SUCCESS && mem != NULL)
    {
      POname (clReleaseMemObject) (mem);
      mem = NULL;
    }
  if (errcode_ret)
    *errcode_ret = errcode;
  return mem;
}

static int
is_gl_mem (cl_mem m)
{
  return IS_CL_OBJECT_VALID (m) && m->target != 0;
}

/* The CL objects are views of the GL objects' memory and i915 orders GPU
   work on shared buffer objects, so acquiring flushes the GL work and both
   directions order this queue with a marker. */
cl_int
pocl_gl_acquire (cl_command_queue command_queue, cl_uint num_objects,
                 const cl_mem *mem_objects, cl_uint num_events_in_wait_list,
                 const cl_event *event_wait_list, cl_event *event)
{
  POCL_RETURN_ERROR_COND ((!IS_CL_OBJECT_VALID (command_queue)),
                          CL_INVALID_COMMAND_QUEUE);
  POCL_RETURN_ERROR_COND ((num_objects > 0 && mem_objects == NULL),
                          CL_INVALID_VALUE);
  gl_ctx_t g;
  interop_fns_t f;
  POCL_RETURN_ERROR_ON (
      (gl_ctx_from_properties (command_queue->context->properties, &g)
           != CL_SUCCESS
       || !interop_fns (g.egl, &f)),
      CL_INVALID_CONTEXT, "context was not created from a GL context\n");

  struct mesa_glinterop_export_in *in
      = calloc (num_objects ? num_objects : 1, sizeof (*in));
  if (in == NULL)
    return CL_OUT_OF_HOST_MEMORY;
  for (cl_uint i = 0; i < num_objects; ++i)
    {
      if (!is_gl_mem (mem_objects[i]))
        {
          free (in);
          POCL_RETURN_ERROR_ON (1, CL_INVALID_GL_OBJECT,
                                "mem object %u is not a GL object\n", i);
        }
      in[i].version = 2;
      in[i].target = mem_objects[i]->target;
      in[i].obj = mem_objects[i]->texture;
      in[i].miplevel = (unsigned)mem_objects[i]->miplevel;
      in[i].access = interop_access (mem_objects[i]->flags);
    }
  cl_int err = num_objects ? flush_gl (&g, &f, num_objects, in) : CL_SUCCESS;
  free (in);
  gl_trace ("acquire %u objects first %p", num_objects,
            num_objects ? (void *)mem_objects[0] : NULL);
  if (err != CL_SUCCESS)
    return err;
  return POname (clEnqueueMarkerWithWaitList) (
      command_queue, num_events_in_wait_list, event_wait_list, event);
}

cl_int
pocl_gl_release (cl_command_queue command_queue, cl_uint num_objects,
                 const cl_mem *mem_objects, cl_uint num_events_in_wait_list,
                 const cl_event *event_wait_list, cl_event *event)
{
  POCL_RETURN_ERROR_COND ((!IS_CL_OBJECT_VALID (command_queue)),
                          CL_INVALID_COMMAND_QUEUE);
  POCL_RETURN_ERROR_COND ((num_objects > 0 && mem_objects == NULL),
                          CL_INVALID_VALUE);
  for (cl_uint i = 0; i < num_objects; ++i)
    POCL_RETURN_ERROR_ON ((!is_gl_mem (mem_objects[i])), CL_INVALID_GL_OBJECT,
                          "mem object %u is not a GL object\n", i);
  cl_int err = POname (clEnqueueMarkerWithWaitList) (
      command_queue, num_events_in_wait_list, event_wait_list, event);
  /* GL must see the CL work submitted */
  if (err == CL_SUCCESS)
    err = POname (clFlush) (command_queue);
  gl_trace ("release %u objects first %p", num_objects,
            num_objects ? (void *)mem_objects[0] : NULL);
  return err;
}

cl_int
pocl_gl_object_info (cl_mem memobj, cl_gl_object_type *gl_object_type,
                     cl_GLuint *gl_object_name)
{
  POCL_RETURN_ERROR_COND ((!IS_CL_OBJECT_VALID (memobj)),
                          CL_INVALID_MEM_OBJECT);
  POCL_RETURN_ERROR_COND ((memobj->target == 0), CL_INVALID_GL_OBJECT);
  if (gl_object_type)
    switch (memobj->target)
      {
      case GL_ARRAY_BUFFER:
        *gl_object_type = CL_GL_OBJECT_BUFFER;
        break;
      case GL_RENDERBUFFER:
        *gl_object_type = CL_GL_OBJECT_RENDERBUFFER;
        break;
      default:
        *gl_object_type = CL_GL_OBJECT_TEXTURE2D;
        break;
      }
  if (gl_object_name)
    *gl_object_name = memobj->texture;
  return CL_SUCCESS;
}

cl_int
pocl_gl_texture_info (cl_mem memobj, cl_gl_texture_info param_name,
                      size_t param_value_size, void *param_value,
                      size_t *param_value_size_ret)
{
  POCL_RETURN_ERROR_COND ((!IS_CL_OBJECT_VALID (memobj)),
                          CL_INVALID_MEM_OBJECT);
  POCL_RETURN_ERROR_COND (
      (memobj->target == 0 || memobj->target == GL_ARRAY_BUFFER
       || memobj->target == GL_RENDERBUFFER),
      CL_INVALID_GL_OBJECT);
  switch (param_name)
    {
    case CL_GL_TEXTURE_TARGET:
      POCL_RETURN_GETINFO (cl_GLenum, memobj->target);
    case CL_GL_MIPMAP_LEVEL:
      POCL_RETURN_GETINFO (cl_GLint, memobj->miplevel);
    case CL_GL_NUM_SAMPLES:
      POCL_RETURN_GETINFO (cl_GLint, 1);
    }
  return CL_INVALID_VALUE;
}
