/* cl_khr_gl_sharing through Mesa's MESA_GLinterop (pocl_gl_sharing.c) */

#ifndef POCL_GL_SHARING_H
#define POCL_GL_SHARING_H

#include "pocl_cl.h"

/* whether dev can share objects of the GL context in properties */
int pocl_gl_device_for_context (cl_device_id dev,
                                const cl_context_properties *properties);

/* target is GL_ARRAY_BUFFER, GL_RENDERBUFFER or a texture target */
cl_mem pocl_gl_create_mem (cl_context context, cl_mem_flags flags,
                           unsigned target, int miplevel, unsigned object,
                           cl_int *errcode_ret);

cl_int pocl_gl_acquire (cl_command_queue command_queue, cl_uint num_objects,
                        const cl_mem *mem_objects,
                        cl_uint num_events_in_wait_list,
                        const cl_event *event_wait_list, cl_event *event);

cl_int pocl_gl_release (cl_command_queue command_queue, cl_uint num_objects,
                        const cl_mem *mem_objects,
                        cl_uint num_events_in_wait_list,
                        const cl_event *event_wait_list, cl_event *event);

cl_int pocl_gl_object_info (cl_mem memobj, cl_gl_object_type *gl_object_type,
                            cl_GLuint *gl_object_name);

cl_int pocl_gl_texture_info (cl_mem memobj, cl_gl_texture_info param_name,
                             size_t param_value_size, void *param_value,
                             size_t *param_value_size_ret);

#endif
