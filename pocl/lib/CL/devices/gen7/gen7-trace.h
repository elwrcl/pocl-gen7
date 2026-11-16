/* pocl-gen7 recorder: POCL_GEN7_DUMP=<dir> saves every program built from
   source (<hash>.cl, <hash>.opts) and logs builds, image layouts, dma-buf
   imports, host transfers and kernel runs with timestamps to
   <dir>/session.log, to debug applications such as DaVinci Resolve offline. */

#ifndef POCL_GEN7_TRACE_H
#define POCL_GEN7_TRACE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the dump directory, or NULL when recording is off */
const char *gen7_trace_dir (void);

static inline int
gen7_trace_on (void)
{
  return gen7_trace_dir () != NULL;
}

double gen7_trace_ms (void);

/* one line to session.log, prefixed with the time since start and thread */
void gen7_trace (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

/* saves the program (once per content) and logs the build; ms < 0 logs its
   start. Returns the content hash. */
unsigned long long gen7_trace_program (const char *source, const char *options,
                                       double ms, int ok);

#ifdef __cplusplus
}
#endif

#endif
