/* beignet's dma-buf sharing entry points (include/CL/cl_intel.h there):
   clCreateBufferFromFdINTEL, clCreateImageFromFdINTEL, clGetMemObjectFdIntel.
   ffmpeg maps VA-API surfaces to OpenCL images through them
   (hwcontext_opencl.c, "Beignet DRM to OpenCL mapping"): zero copy, the
   decoded surface's GEM object becomes the image's device memory.

   The memory object is created as usual and a device that implements
   ops->import_dmabuf / export_dmabuf binds it to the dma-buf.

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

#include "pocl_cl.h"
#include "pocl_intel_fd.h"
#include "pocl_util.h"

/* bind mem (just created) to the dma-buf on every device that can */
static cl_int
import_on_devices (cl_context context, cl_mem mem, int fd, size_t size,
                   size_t offset, size_t row_pitch)
{
  unsigned imported = 0;
  for (unsigned i = 0; i < context->num_devices; ++i)
    {
      cl_device_id dev = context->devices[i];
      if (dev->ops->import_dmabuf == NULL)
        continue;
      cl_int errcode
          = dev->ops->import_dmabuf (dev, mem, fd, size, offset, row_pitch);
      if (errcode != CL_SUCCESS)
        return errcode;
      ++imported;
    }
  POCL_RETURN_ERROR_ON ((imported == 0), CL_INVALID_OPERATION,
                        "no device in the context imports dma-bufs\n");
  return CL_SUCCESS;
}

CL_API_ENTRY cl_mem CL_API_CALL
POname (clCreateBufferFromFdINTEL) (cl_context context,
                                    const cl_import_buffer_info_intel *info,
                                    cl_int *errcode_ret)
{
  cl_int errcode = CL_SUCCESS;
  cl_mem mem = NULL;

  POCL_GOTO_ERROR_COND ((!IS_CL_OBJECT_VALID (context)), CL_INVALID_CONTEXT);
  POCL_GOTO_ERROR_COND ((info == NULL || info->fd < 0 || info->size <= 0),
                        CL_INVALID_VALUE);

  mem = POname (clCreateBuffer) (context, CL_MEM_READ_WRITE,
                                 (size_t)info->size, NULL, &errcode);
  if (mem == NULL)
    goto ERROR;
  errcode = import_on_devices (context, mem, info->fd, (size_t)info->size, 0, 0);
  if (errcode != CL_SUCCESS)
    {
      POname (clReleaseMemObject) (mem);
      mem = NULL;
    }

ERROR:
  if (errcode_ret)
    *errcode_ret = errcode;
  return mem;
}
POsym (clCreateBufferFromFdINTEL)

CL_API_ENTRY cl_mem CL_API_CALL
POname (clCreateImageFromFdINTEL) (cl_context context,
                                   const cl_import_image_info_intel *info,
                                   cl_int *errcode_ret)
{
  cl_int errcode = CL_SUCCESS;
  cl_mem mem = NULL;

  POCL_GOTO_ERROR_COND ((!IS_CL_OBJECT_VALID (context)), CL_INVALID_CONTEXT);
  POCL_GOTO_ERROR_COND ((info == NULL || info->fd < 0 || info->size <= 0),
                        CL_INVALID_VALUE);
  POCL_GOTO_ERROR_ON ((info->type != CL_MEM_OBJECT_IMAGE2D), CL_INVALID_VALUE,
                      "only 2D images can be imported from an fd\n");
  POCL_GOTO_ERROR_COND (
      (info->width == 0 || info->height == 0 || info->row_pitch == 0
       || (size_t)info->offset + (size_t)info->row_pitch * info->height
              > (size_t)info->size),
      CL_INVALID_VALUE);

  cl_image_desc desc = { 0 };
  desc.image_type = CL_MEM_OBJECT_IMAGE2D;
  desc.image_width = info->width;
  desc.image_height = info->height;
  mem = POname (clCreateImage) (context, CL_MEM_READ_WRITE, &info->fmt, &desc,
                                NULL, &errcode);
  if (mem == NULL)
    goto ERROR;
  POCL_GOTO_ERROR_ON (
      ((size_t)info->row_pitch < mem->image_row_pitch), CL_INVALID_VALUE,
      "row_pitch %u is smaller than a row of the image (%zu)\n",
      info->row_pitch, mem->image_row_pitch);
  errcode = import_on_devices (context, mem, info->fd, (size_t)info->size,
                           info->offset, info->row_pitch);

ERROR:
  if (errcode != CL_SUCCESS && mem != NULL)
    {
      POname (clReleaseMemObject) (mem);
      mem = NULL;
    }
  if (errcode_ret)
    *errcode_ret = errcode;
  return mem;
}
POsym (clCreateImageFromFdINTEL)

CL_API_ENTRY cl_int CL_API_CALL
POname (clGetMemObjectFdIntel) (cl_context context, cl_mem mem, int *fd)
{
  POCL_RETURN_ERROR_COND ((!IS_CL_OBJECT_VALID (context)), CL_INVALID_CONTEXT);
  POCL_RETURN_ERROR_COND ((!IS_CL_OBJECT_VALID (mem)), CL_INVALID_MEM_OBJECT);
  POCL_RETURN_ERROR_COND ((fd == NULL), CL_INVALID_VALUE);

  for (unsigned i = 0; i < context->num_devices; ++i)
    {
      cl_device_id dev = context->devices[i];
      if (dev->ops->export_dmabuf == NULL)
        continue;
      /* the device memory must exist before it can be shared */
      if (mem->device_ptrs[dev->global_mem_id].mem_ptr == NULL)
        {
          cl_int errcode = dev->ops->alloc_mem_obj (dev, mem, NULL);
          if (errcode != CL_SUCCESS)
            return errcode;
        }
      return dev->ops->export_dmabuf (dev, mem, fd);
    }
  POCL_RETURN_ERROR_ON (1, CL_INVALID_OPERATION,
                        "no device in the context exports dma-bufs\n");
}
POsym (clGetMemObjectFdIntel)
