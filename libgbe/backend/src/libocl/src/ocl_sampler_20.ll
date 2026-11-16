target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024"
target triple = "spir64"
; Clang 17+ gives sampler_t the target extension type spirv.Sampler (it was a
; pointer to %opencl.sampler_t); this definition must match its calls.
declare target("spirv.Sampler") @__gen_ocl_int_to_sampler(i32)

define target("spirv.Sampler") @__translate_sampler_initializer(i32 %s) {
  %call = call target("spirv.Sampler") @__gen_ocl_int_to_sampler(i32 %s)
  ret target("spirv.Sampler") %call
}
