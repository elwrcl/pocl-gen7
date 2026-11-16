/* beignet's dma-buf sharing ABI (beignet include/CL/cl_intel.h); the
   structures must stay layout compatible with it. */

#ifndef POCL_INTEL_FD_H
#define POCL_INTEL_FD_H

#include "pocl_cl.h"

typedef struct _cl_import_buffer_info_intel
{
  int fd;
  int size;
} cl_import_buffer_info_intel;

typedef struct _cl_import_image_info_intel
{
  int fd;
  int size;
  cl_mem_object_type type;
  cl_image_format fmt;
  uint32_t offset;
  uint32_t width;
  uint32_t height;
  uint32_t row_pitch;
} cl_import_image_info_intel;

/* cl_intel_va_api_media_sharing (CL/cl_va_api_media_sharing_intel.h, which
   needs libva's headers) */
#ifndef CL_VA_API_DISPLAY_INTEL
typedef cl_uint cl_va_api_device_source_intel;
typedef cl_uint cl_va_api_device_set_intel;
#define CL_INVALID_VA_API_MEDIA_ADAPTER_INTEL -1098
#define CL_INVALID_VA_API_MEDIA_SURFACE_INTEL -1099
#define CL_VA_API_MEDIA_SURFACE_ALREADY_ACQUIRED_INTEL -1100
#define CL_VA_API_MEDIA_SURFACE_NOT_ACQUIRED_INTEL -1101
#define CL_VA_API_DISPLAY_INTEL 0x4094
#define CL_PREFERRED_DEVICES_FOR_VA_API_INTEL 0x4095
#define CL_ALL_DEVICES_FOR_VA_API_INTEL 0x4096
#define CL_CONTEXT_VA_API_DISPLAY_INTEL 0x4097
#define CL_MEM_VA_API_MEDIA_SURFACE_INTEL 0x4098
#define CL_IMAGE_VA_API_PLANE_INTEL 0x4099
#define CL_COMMAND_ACQUIRE_VA_API_MEDIA_SURFACES_INTEL 0x409A
#define CL_COMMAND_RELEASE_VA_API_MEDIA_SURFACES_INTEL 0x409B
#endif
typedef unsigned int VASurfaceID;

POCL_EXPORT CL_API_ENTRY cl_int CL_API_CALL
    POname (clGetDeviceIDsFromVA_APIMediaAdapterINTEL) (
        cl_platform_id platform, cl_va_api_device_source_intel media_adapter_type,
        void *media_adapter, cl_va_api_device_set_intel media_adapter_set,
        cl_uint num_entries, cl_device_id *devices, cl_uint *num_devices);

POCL_EXPORT CL_API_ENTRY cl_mem CL_API_CALL
    POname (clCreateFromVA_APIMediaSurfaceINTEL) (cl_context context,
                                                  cl_mem_flags flags,
                                                  VASurfaceID *surface,
                                                  cl_uint plane,
                                                  cl_int *errcode_ret);

POCL_EXPORT CL_API_ENTRY cl_int CL_API_CALL
    POname (clEnqueueAcquireVA_APIMediaSurfacesINTEL) (
        cl_command_queue command_queue, cl_uint num_objects,
        const cl_mem *mem_objects, cl_uint num_events_in_wait_list,
        const cl_event *event_wait_list, cl_event *event);

POCL_EXPORT CL_API_ENTRY cl_int CL_API_CALL
    POname (clEnqueueReleaseVA_APIMediaSurfacesINTEL) (
        cl_command_queue command_queue, cl_uint num_objects,
        const cl_mem *mem_objects, cl_uint num_events_in_wait_list,
        const cl_event *event_wait_list, cl_event *event);

POCL_EXPORT CL_API_ENTRY cl_mem CL_API_CALL POname (clCreateBufferFromFdINTEL) (
    cl_context context, const cl_import_buffer_info_intel *info,
    cl_int *errcode_ret);

POCL_EXPORT CL_API_ENTRY cl_mem CL_API_CALL POname (clCreateImageFromFdINTEL) (
    cl_context context, const cl_import_image_info_intel *info,
    cl_int *errcode_ret);

POCL_EXPORT CL_API_ENTRY cl_int CL_API_CALL
    POname (clGetMemObjectFdIntel) (cl_context context, cl_mem mem, int *fd);

#endif
