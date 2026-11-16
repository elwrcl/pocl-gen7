/* pocl-gen7 recorder, see gen7-trace.h */

#include "gen7-trace.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static const char *trace_dir;
static int trace_fd = -1;

static double
now_ms (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void
trace_init (void)
{
  const char *d = getenv ("POCL_GEN7_DUMP");
  if (d == NULL || *d == 0)
    return;
  mkdir (d, 0755);
  char path[4096];
  snprintf (path, sizeof (path), "%s/session.log", d);
  trace_fd = open (path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (trace_fd < 0)
    return;
  trace_dir = d;
  time_t wall = time (NULL);
  char line[128];
  int n = snprintf (line, sizeof (line), "=== session pid %d %s", getpid (),
                    ctime (&wall));
  if (write (trace_fd, line, n) < 0)
    return;
}

const char *
gen7_trace_dir (void)
{
  pthread_once (&trace_once, trace_init);
  return trace_dir;
}

double
gen7_trace_ms (void)
{
  return now_ms ();
}

void
gen7_trace (const char *fmt, ...)
{
  if (!gen7_trace_on ())
    return;
  char line[1024];
  int n = snprintf (line, sizeof (line), "%.3f [%ld] ", now_ms () / 1e3,
                    (long)syscall (SYS_gettid));
  va_list ap;
  va_start (ap, fmt);
  n += vsnprintf (line + n, sizeof (line) - n - 1, fmt, ap);
  va_end (ap);
  if (n > (int)sizeof (line) - 2)
    n = sizeof (line) - 2;
  line[n++] = '\n';
  /* one write per line: O_APPEND keeps lines from threads whole */
  if (write (trace_fd, line, n) < 0)
    return;
}

static unsigned long long
fnv1a (unsigned long long h, const char *s, size_t n)
{
  for (size_t i = 0; i < n; i++)
    h = (h ^ (unsigned char)s[i]) * 0x100000001b3ULL;
  return h;
}

static void
write_new_file (const char *path, const char *data)
{
  int fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
    return; /* already saved */
  size_t n = strlen (data);
  while (n > 0)
    {
      ssize_t w = write (fd, data, n);
      if (w <= 0)
        break;
      data += w;
      n -= w;
    }
  close (fd);
}

unsigned long long
gen7_trace_program (const char *source, const char *options, double ms, int ok)
{
  if (!gen7_trace_on () || source == NULL)
    return 0;
  /* ms < 0: before the build, so a crash in it still leaves the source */
  if (options == NULL)
    options = "";
  unsigned long long h = 0xcbf29ce484222325ULL;
  h = fnv1a (h, source, strlen (source) + 1);
  h = fnv1a (h, options, strlen (options));
  char path[4096];
  snprintf (path, sizeof (path), "%s/%016llx.cl", trace_dir, h);
  write_new_file (path, source);
  snprintf (path, sizeof (path), "%s/%016llx.opts", trace_dir, h);
  write_new_file (path, options);
  if (ms < 0)
    gen7_trace ("build start %016llx %zu bytes, options: %.200s", h,
                strlen (source), options);
  else
    gen7_trace ("build %016llx %zu bytes %.0f ms %s", h, strlen (source), ms,
                ok ? "ok" : "FAILED");
  return h;
}
