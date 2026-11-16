/* cl_intel_va_api_media_sharing on top of the drivers' dma-buf import
   (ops->import_dmabuf): a VA-API surface is exported as dma-bufs with
   vaExportSurfaceHandle() and each NV12 plane becomes a 2D image on the same
   memory, without copying. This is the path ffmpeg's hwcontext_opencl uses
   for "hwmap=derive_device=opencl" from VA-API frames.

   libva is not linked: the application that owns the VADisplay has it
   loaded, and its copy is looked up with dlopen(RTLD_NOLOAD).

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

#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#include "pocl_cl.h"
#include "pocl_intel_fd.h"
#include "pocl_util.h"

/* libva ABI (va/va.h, va/va_drmcommon.h) */
#define VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 0x40000000
#define VA_EXPORT_SURFACE_READ_ONLY 0x0001
#define VA_EXPORT_SURFACE_WRITE_ONLY 0x0002
#define VA_EXPORT_SURFACE_SEPARATE_LAYERS 0x0004
typedef struct
{
  uint32_t fourcc, width, height, num_objects;
  struct
  {
    int fd;
    uint32_t size;
    uint64_t drm_format_modifier;
  } objects[4];
  uint32_t num_layers;
  struct
  {
    uint32_t drm_format, num_planes;
    uint32_t object_index[4], offset[4], pitch[4];
  } layers[4];
} va_drm_prime_desc;
typedef int (*vaExportSurfaceHandle_fn) (void *dpy, unsigned surface,
                                         uint32_t mem_type, uint32_t flags,
                                         void *descriptor);
typedef int (*vaSyncSurface_fn) (void *dpy, unsigned surface);

#define VA_FOURCC_NV12 0x3231564E /* 'NV12' */

static vaExportSurfaceHandle_fn va_export;
static vaSyncSurface_fn va_sync;

static int
load_libva (void)
{
  if (va_export && va_sync)
    return 1;
  void *h = dlopen ("libva.so.2", RTLD_LAZY | RTLD_NOLOAD);
  if (h == NULL)
    h = dlopen ("libva.so.2", RTLD_LAZY);
  if (h == NULL)
    return 0;
  va_export = (vaExportSurfaceHandle_fn)dlsym (h, "vaExportSurfaceHandle");
  va_sync = (vaSyncSurface_fn)dlsym (h, "vaSyncSurface");
  return va_export && va_sync;
}

static void *
context_va_display (cl_context context)
{
  for (unsigned i = 0; context->properties && i < context->num_properties;
       ++i)
    if (context->properties[2 * i] == CL_CONTEXT_VA_API_DISPLAY_INTEL)
      return (void *)context->properties[2 * i + 1];
  return NULL;
}

static int
device_shares_va (cl_device_id dev)
{
  return dev->ops->import_dmabuf != NULL && dev->extensions
         && strstr (dev->extensions, "cl_intel_va_api_media_sharing");
}

CL_API_ENTRY cl_int CL_API_CALL
POname (clGetDeviceIDsFromVA_APIMediaAdapterINTEL) (
    cl_platform_id platform, cl_va_api_device_source_intel media_adapter_type,
    void *media_adapter, cl_va_api_device_set_intel media_adapter_set,
    cl_uint num_entries, cl_device_id *devices, cl_uint *num_devices)
{
  POCL_RETURN_ERROR_COND ((media_adapter_type != CL_VA_API_DISPLAY_INTEL),
                          CL_INVALID_VALUE);
  POCL_RETURN_ERROR_COND ((media_adapter == NULL), CL_INVALID_VALUE);
  POCL_RETURN_ERROR_COND (
      (media_adapter_set != CL_PREFERRED_DEVICES_FOR_VA_API_INTEL
       && media_adapter_set != CL_ALL_DEVICES_FOR_VA_API_INTEL),
      CL_INVALID_VALUE);
  POCL_RETURN_ERROR_COND ((devices == NULL && num_devices == NULL),
                          CL_INVALID_VALUE);
  POCL_RETURN_ERROR_COND ((devices != NULL && num_entries == 0),
                          CL_INVALID_VALUE);

  cl_uint n = 0;
  POname (clGetDeviceIDs) (platform, CL_DEVICE_TYPE_ALL, 0, NULL, &n);
  cl_device_id *all = calloc (n ? n : 1, sizeof (cl_device_id));
  if (all == NULL)
    return CL_OUT_OF_HOST_MEMORY;
  if (n)
    POname (clGetDeviceIDs) (platform, CL_DEVICE_TYPE_ALL, n, all, NULL);
  cl_uint found = 0;
  for (cl_uint i = 0; i < n; ++i)
    if (device_shares_va (all[i]))
      {
        if (devices && found < num_entries)
          devices[found] = all[i];
        ++found;
      }
  free (all);
  if (num_devices)
    *num_devices = found;
  return found ? CL_SUCCESS : CL_DEVICE_NOT_FOUND;
}
POsym (clGetDeviceIDsFromVA_APIMediaAdapterINTEL)

CL_API_ENTRY cl_mem CL_API_CALL
POname (clCreateFromVA_APIMediaSurfaceINTEL) (cl_context context,
                                              cl_mem_flags flags,
                                              VASurfaceID *surface,
                                              cl_uint plane,
                                              cl_int *errcode_ret)
{
  cl_int errcode = CL_SUCCESS;
  cl_mem mem = NULL;
  va_drm_prime_desc d;
  memset (&d, 0, sizeof (d));
  d.num_objects = 0;

  POCL_GOTO_ERROR_COND ((!IS_CL_OBJECT_VALID (context)), CL_INVALID_CONTEXT);
  POCL_GOTO_ERROR_COND ((surface == NULL), CL_INVALID_VA_API_MEDIA_SURFACE_INTEL);
  POCL_GOTO_ERROR_COND ((plane > 1), CL_INVALID_VALUE);
  void *dpy = context_va_display (context);
  POCL_GOTO_ERROR_ON ((dpy == NULL), CL_INVALID_CONTEXT,
                      "context was not created with "
                      "CL_CONTEXT_VA_API_DISPLAY_INTEL\n");
  POCL_GOTO_ERROR_ON ((!load_libva ()), CL_INVALID_OPERATION,
                      "libva (vaExportSurfaceHandle) not available\n");

  uint32_t va_flags = VA_EXPORT_SURFACE_SEPARATE_LAYERS;
  if (flags & CL_MEM_READ_ONLY)
    va_flags |= VA_EXPORT_SURFACE_READ_ONLY;
  else if (flags & CL_MEM_WRITE_ONLY)
    va_flags |= VA_EXPORT_SURFACE_WRITE_ONLY;
  else
    va_flags |= VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_WRITE_ONLY;
  POCL_GOTO_ERROR_ON (
      (va_export (dpy, *surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                  va_flags, &d)
       != 0),
      CL_INVALID_VA_API_MEDIA_SURFACE_INTEL,
      "vaExportSurfaceHandle failed for surface %#x\n", *surface);
  POCL_GOTO_ERROR_ON ((d.fourcc != VA_FOURCC_NV12 || d.num_layers != 2),
                      CL_INVALID_VA_API_MEDIA_SURFACE_INTEL,
                      "only NV12 surfaces can be shared (fourcc %#x, %u "
                      "layers)\n",
                      d.fourcc, d.num_layers);

  /* plane 0: Y as R8, plane 1: interleaved UV as RG8 at half size */
  cl_image_format fmt = { plane ? CL_RG : CL_R, CL_UNORM_INT8 };
  cl_image_desc desc;
  memset (&desc, 0, sizeof (desc));
  desc.image_type = CL_MEM_OBJECT_IMAGE2D;
  desc.image_width = plane ? (d.width + 1) / 2 : d.width;
  desc.image_height = plane ? (d.height + 1) / 2 : d.height;
  mem = POname (clCreateImage) (
      context,
      flags & (CL_MEM_READ_ONLY | CL_MEM_WRITE_ONLY | CL_MEM_READ_WRITE),
      &fmt, &desc, NULL, &errcode);
  if (mem == NULL)
    goto ERROR;

  uint32_t obj = d.layers[plane].object_index[0];
  for (cl_uint i = 0; i < context->num_devices; ++i)
    {
      cl_device_id dev = context->devices[i];
      if (!device_shares_va (dev))
        continue;
      errcode = dev->ops->import_dmabuf (dev, mem, d.objects[obj].fd,
                                     d.objects[obj].size,
                                     d.layers[plane].offset[0],
                                     d.layers[plane].pitch[0]);
      if (errcode != CL_SUCCESS)
        goto ERROR;
    }

ERROR:
  /* the imported GEM objects keep the memory alive */
  for (uint32_t i = 0; i < d.num_objects && i < 4; ++i)
    if (d.objects[i].fd >= 0)
      close (d.objects[i].fd);
  if (errcode != CL_SUCCESS && mem != NULL)
    {
      POname (clReleaseMemObject) (mem);
      mem = NULL;
    }
  if (errcode_ret)
    *errcode_ret = errcode;
  return mem;
}
POsym (clCreateFromVA_APIMediaSurfaceINTEL)

/* The images are views of the surface's memory and i915 orders GPU work on
   shared buffer objects, so acquiring and releasing only has to order this
   queue: a marker each. The application waits for the release event before
   handing the surface back to VA-API. */
CL_API_ENTRY cl_int CL_API_CALL
POname (clEnqueueAcquireVA_APIMediaSurfacesINTEL) (
    cl_command_queue command_queue, cl_uint num_objects,
    const cl_mem *mem_objects, cl_uint num_events_in_wait_list,
    const cl_event *event_wait_list, cl_event *event)
{
  POCL_RETURN_ERROR_COND ((num_objects > 0 && mem_objects == NULL),
                          CL_INVALID_VALUE);
  return POname (clEnqueueMarkerWithWaitList) (
      command_queue, num_events_in_wait_list, event_wait_list, event);
}
POsym (clEnqueueAcquireVA_APIMediaSurfacesINTEL)

CL_API_ENTRY cl_int CL_API_CALL
POname (clEnqueueReleaseVA_APIMediaSurfacesINTEL) (
    cl_command_queue command_queue, cl_uint num_objects,
    const cl_mem *mem_objects, cl_uint num_events_in_wait_list,
    const cl_event *event_wait_list, cl_event *event)
{
  POCL_RETURN_ERROR_COND ((num_objects > 0 && mem_objects == NULL),
                          CL_INVALID_VALUE);
  return POname (clEnqueueMarkerWithWaitList) (
      command_queue, num_events_in_wait_list, event_wait_list, event);
}
POsym (clEnqueueReleaseVA_APIMediaSurfacesINTEL)
