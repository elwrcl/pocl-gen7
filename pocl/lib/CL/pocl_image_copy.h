/* clEnqueueCopyImage as a kernel on the device (pocl_image_copy.c) */

#ifndef POCL_IMAGE_COPY_H
#define POCL_IMAGE_COPY_H

#include "pocl_cl.h"

/* Enqueues the copy as an NDRange of an internal kernel when the device
   copies images faster that way. Returns CL_SUCCESS when it did, or
   CL_INVALID_OPERATION when the caller must enqueue the copy command. */
cl_int pocl_image_copy_by_kernel (cl_command_queue command_queue,
                                  cl_mem src_image, cl_mem dst_image,
                                  const size_t *src_origin,
                                  const size_t *dst_origin,
                                  const size_t *region,
                                  cl_uint num_events_in_wait_list,
                                  const cl_event *event_wait_list,
                                  cl_event *event);

/* clEnqueueReadImage of a 2D image through a linear copy: the kernel writes
   the region into a temporary buffer, which the host reads cached instead of
   through an uncached GTT map of the tiled image. Returns CL_SUCCESS when it
   enqueued the read, CL_INVALID_OPERATION when the caller must. */
cl_int pocl_image_read_by_kernel (cl_command_queue command_queue,
                                  cl_mem image, cl_bool blocking_read,
                                  const size_t *origin, const size_t *region,
                                  size_t row_pitch, void *ptr,
                                  cl_uint num_events_in_wait_list,
                                  const cl_event *event_wait_list,
                                  cl_event *event);

/* frees the context's copy program; the context is being freed */
void pocl_image_copy_release (cl_context context);

#endif
