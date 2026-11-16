/* clEnqueueCopyImage as a kernel on the device's own queue.

   pocl-gen7 copies images on the host, through GTT maps of the tiled
   images, which are uncached: a 1280x720 RGBA half copy takes 50-300 ms
   (DaVinci Resolve's Fusion page copies one into a GL texture every
   frame). A kernel reading the source and writing the destination does it
   on the GPU, on the same ring as everything else, so it is ordered with
   the GL and CL work on those images like any kernel (copying on the BLT
   ring instead hung the render ring under Resolve).

   The kernel moves raw pixels (the device binds both images with an
   integer format of their pixel size), so the copy is bit exact for every
   format. 3D images, arrays and copies within one image take the copy
   command. The program is built once per context and doesn't retain it.

   POCL_KERNEL_IMAGE_COPY=0 disables it.

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

#include <stdlib.h>
#include <string.h>

#include "pocl_image_copy.h"
#include "pocl_util.h"

/* reads and writes the images as unsigned integers of their pixel size:
   the device binds them with such formats for this kernel (pocl-gen7:
   gen7-launch.c), which copies any format bit for bit */
static const char *const copy_kernel_name = "pocl_copy_image2d_raw";

static const char *const copy_source
    = "kernel void pocl_copy_image2d_raw (read_only image2d_t src,\n"
      "                                   write_only image2d_t dst,\n"
      "                                   int sx, int sy, int dx, int dy)\n"
      "{\n"
      "  const sampler_t s = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_NONE\n"
      "                      | CLK_FILTER_NEAREST;\n"
      "  int x = get_global_id (0), y = get_global_id (1);\n"
      "  write_imageui (dst, (int2)(dx + x, dy + y),\n"
      "                 read_imageui (src, s, (int2)(sx + x, sy + y)));\n"
      "}\n";

struct pocl_image_copy
{
  cl_device_id device;
  cl_program program;
  cl_kernel kernel;
  /* the kernels' arguments are set and the NDRange enqueued under it */
  pocl_lock_t lock;
  int failed; /* the program didn't build: always take the copy command */
};

/* devices whose image copy command is slower than a kernel */
static int
device_copies_by_kernel (cl_device_id dev)
{
  static int enabled = -1;
  if (enabled < 0)
    {
      const char *e = getenv ("POCL_KERNEL_IMAGE_COPY");
      enabled = !(e && *e == '0');
    }
  return enabled && dev->short_name && strcmp (dev->short_name, "gen7") == 0;
}

/* builds the context's copy kernels for dev on first use */
static struct pocl_image_copy *
get_image_copy (cl_context context, cl_device_id dev)
{
  POCL_LOCK_OBJ (context);
  struct pocl_image_copy *ic = context->image_copy;
  if (ic != NULL)
    {
      POCL_UNLOCK_OBJ (context);
      return (ic->device == dev && !ic->failed) ? ic : NULL;
    }
  ic = calloc (1, sizeof (*ic));
  if (ic == NULL)
    {
      POCL_UNLOCK_OBJ (context);
      return NULL;
    }
  ic->device = dev;
  POCL_INIT_LOCK (ic->lock);
  context->image_copy = ic;
  POCL_UNLOCK_OBJ (context);

  /* the build happens outside the context lock; a copy racing with it takes
     the copy command (failed is set until it's done) */
  ic->failed = 1;
  cl_int err;
  cl_program p = POname (clCreateProgramWithSource) (context, 1, &copy_source,
                                                     NULL, &err);
  if (p == NULL)
    return NULL;
  /* hidden: the context owns it and frees it, it doesn't retain it */
  p->hidden = CL_TRUE;
  POname (clReleaseContext) (context);
  ic->program = p;
  if (POname (clBuildProgram) (p, 1, &dev, "", NULL, NULL) != CL_SUCCESS)
    {
      POCL_MSG_WARN ("image copy kernels didn't build, copying on the host\n");
      return NULL;
    }
  ic->kernel = POname (clCreateKernel) (p, copy_kernel_name, &err);
  if (ic->kernel == NULL)
    return NULL;
  ic->failed = 0;
  return ic;
}

void
pocl_image_copy_release (cl_context context)
{
  struct pocl_image_copy *ic = context->image_copy;
  if (ic == NULL)
    return;
  if (ic->kernel)
    POname (clReleaseKernel) (ic->kernel);
  if (ic->program)
    POname (clReleaseProgram) (ic->program);
  POCL_DESTROY_LOCK (ic->lock);
  free (ic);
  context->image_copy = NULL;
}

cl_int
pocl_image_copy_by_kernel (cl_command_queue command_queue, cl_mem src_image,
                           cl_mem dst_image, const size_t *src_origin,
                           const size_t *dst_origin, const size_t *region,
                           cl_uint num_events_in_wait_list,
                           const cl_event *event_wait_list, cl_event *event)
{
  if (!IS_CL_OBJECT_VALID (command_queue)
      || !device_copies_by_kernel (command_queue->device)
      || !IS_CL_OBJECT_VALID (src_image) || !IS_CL_OBJECT_VALID (dst_image)
      || src_image == dst_image || src_origin == NULL || dst_origin == NULL
      || region == NULL)
    return CL_INVALID_OPERATION;
  if (src_image->type != CL_MEM_OBJECT_IMAGE2D
      || dst_image->type != CL_MEM_OBJECT_IMAGE2D || region[2] != 1
      || src_origin[2] != 0 || dst_origin[2] != 0)
    return CL_INVALID_OPERATION;
  if (src_image->image_channel_order != dst_image->image_channel_order
      || src_image->image_channel_data_type
             != dst_image->image_channel_data_type)
    return CL_INVALID_OPERATION; /* the copy command reports the error */
  /* the kernel reads src as read_only and writes dst as write_only */
  if ((src_image->flags & CL_MEM_WRITE_ONLY)
      || (dst_image->flags & CL_MEM_READ_ONLY))
    return CL_INVALID_OPERATION;
  if (src_origin[0] + region[0] > src_image->image_width
      || src_origin[1] + region[1] > src_image->image_height
      || dst_origin[0] + region[0] > dst_image->image_width
      || dst_origin[1] + region[1] > dst_image->image_height
      || region[0] == 0 || region[1] == 0)
    return CL_INVALID_OPERATION;
  struct pocl_image_copy *ic
      = get_image_copy (command_queue->context, command_queue->device);
  if (ic == NULL)
    return CL_INVALID_OPERATION;

  cl_kernel k = ic->kernel;
  cl_int o[4] = { (cl_int)src_origin[0], (cl_int)src_origin[1],
                  (cl_int)dst_origin[0], (cl_int)dst_origin[1] };
  size_t global[2] = { region[0], region[1] };
  cl_int err;
  POCL_LOCK (ic->lock);
  err = POname (clSetKernelArg) (k, 0, sizeof (cl_mem), &src_image);
  if (err == CL_SUCCESS)
    err = POname (clSetKernelArg) (k, 1, sizeof (cl_mem), &dst_image);
  for (int i = 0; err == CL_SUCCESS && i < 4; ++i)
    err = POname (clSetKernelArg) (k, 2 + i, sizeof (cl_int), &o[i]);
  if (err == CL_SUCCESS)
    err = POname (clEnqueueNDRangeKernel) (command_queue, k, 2, NULL, global,
                                           NULL, num_events_in_wait_list,
                                           event_wait_list, event);
  POCL_UNLOCK (ic->lock);
  if (err != CL_SUCCESS)
    {
      POCL_MSG_WARN ("image copy kernel failed (%d), copying on the host\n",
                     err);
      return CL_INVALID_OPERATION;
    }
  return CL_SUCCESS;
}

cl_int
pocl_image_read_by_kernel (cl_command_queue command_queue, cl_mem image,
                           cl_bool blocking_read, const size_t *origin,
                           const size_t *region, size_t row_pitch, void *ptr,
                           cl_uint num_events_in_wait_list,
                           const cl_event *event_wait_list, cl_event *event)
{
  if (!IS_CL_OBJECT_VALID (command_queue) || !IS_CL_OBJECT_VALID (image)
      || !device_copies_by_kernel (command_queue->device) || origin == NULL
      || region == NULL || ptr == NULL)
    return CL_INVALID_OPERATION;
  /* an image on a buffer is linear already */
  if (image->type != CL_MEM_OBJECT_IMAGE2D || image->buffer != NULL
      || region[2] != 1 || origin[2] != 0 || region[0] == 0 || region[1] == 0
      || origin[0] + region[0] > image->image_width
      || origin[1] + region[1] > image->image_height
      || (image->flags & CL_MEM_WRITE_ONLY))
    return CL_INVALID_OPERATION;

  cl_context ctx = command_queue->context;
  size_t px = image->image_elem_size * image->image_channels;
  size_t pitch = region[0] * px;
  if (row_pitch == 0)
    row_pitch = pitch;
  if (px == 0 || row_pitch < pitch)
    return CL_INVALID_OPERATION;

  cl_int err;
  cl_mem buf = POname (clCreateBuffer) (ctx, CL_MEM_READ_WRITE,
                                        pitch * region[1], NULL, &err);
  if (buf == NULL)
    return CL_INVALID_OPERATION;
  cl_image_format fmt
      = { image->image_channel_order, image->image_channel_data_type };
  cl_image_desc desc;
  memset (&desc, 0, sizeof (desc));
  desc.image_type = CL_MEM_OBJECT_IMAGE2D;
  desc.image_width = region[0];
  desc.image_height = region[1];
  desc.image_row_pitch = pitch;
  desc.buffer = buf;
  cl_mem lin = POname (clCreateImage) (ctx, CL_MEM_READ_WRITE, &fmt, &desc,
                                       NULL, &err);
  cl_event copied = NULL;
  const size_t zero[3] = { 0, 0, 0 };
  cl_int ret = CL_INVALID_OPERATION;
  if (lin != NULL
      && pocl_image_copy_by_kernel (command_queue, image, lin, origin, zero,
                                    region, num_events_in_wait_list,
                                    event_wait_list, &copied)
             == CL_SUCCESS)
    {
      const size_t bytes[3] = { pitch, region[1], 1 };
      ret = POname (clEnqueueReadBufferRect) (command_queue, buf,
                                              blocking_read, zero, zero, bytes,
                                              pitch, 0, row_pitch, 0, ptr, 1,
                                              &copied, event);
      if (ret != CL_SUCCESS)
        {
          /* the copy ran for nothing; the read command does it again */
          POname (clWaitForEvents) (1, &copied);
          ret = CL_INVALID_OPERATION;
        }
    }
  /* the commands keep what they use alive */
  if (copied)
    POname (clReleaseEvent) (copied);
  if (lin)
    POname (clReleaseMemObject) (lin);
  POname (clReleaseMemObject) (buf);
  return ret;
}
