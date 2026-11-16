
#include "pocl_cl.h"
#include "pocl_gl_sharing.h"

CL_API_ENTRY cl_mem CL_API_CALL
POname(clCreateFromGLBuffer)(cl_context      context,
                        cl_mem_flags    flags,
                        cl_GLuint       bufobj,
                        cl_int *        errcode_ret)
CL_API_SUFFIX__VERSION_1_0
{
  return pocl_gl_create_mem (context, flags, 0x8892 /* GL_ARRAY_BUFFER */, 0,
                             bufobj, errcode_ret);
}
POsym(clCreateFromGLBuffer)



CL_API_ENTRY cl_mem CL_API_CALL
POname(clCreateFromGLRenderbuffer)(cl_context      context,
                        cl_mem_flags    flags,
                        cl_GLuint       renderbuffer,
                        cl_int *        errcode_ret)
CL_API_SUFFIX__VERSION_1_0
{
  return pocl_gl_create_mem (context, flags, 0x8D41 /* GL_RENDERBUFFER */, 0,
                             renderbuffer, errcode_ret);
}
POsym(clCreateFromGLRenderbuffer)



CL_API_ENTRY cl_int CL_API_CALL
POname(clGetGLObjectInfo)(cl_mem        memobj,
                          cl_gl_object_type *gl_object_type,
                          cl_GLuint       *gl_object_name)
CL_API_SUFFIX__VERSION_1_0
{
  return pocl_gl_object_info (memobj, gl_object_type, gl_object_name);
}
POsym(clGetGLObjectInfo)




CL_API_ENTRY cl_int CL_API_CALL
POname(clGetGLTextureInfo) (cl_mem        memobj,
                            cl_gl_texture_info param_name,
                            size_t  param_value_size,
                            void  *param_value,
                            size_t  *param_value_size_ret)
CL_API_SUFFIX__VERSION_1_0
{
  return pocl_gl_texture_info (memobj, param_name, param_value_size,
                               param_value, param_value_size_ret);
}
POsym(clGetGLTextureInfo)
