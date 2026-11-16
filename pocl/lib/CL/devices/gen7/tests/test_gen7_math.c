/* Unit tests of gen7-math.c; no GPU needed. Run: ./gen7-math-test */

#include "gen7-math.h"

#include <CL/cl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond)                                                           \
  do                                                                          \
    {                                                                         \
      if (!(cond))                                                            \
        {                                                                     \
          fprintf (stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                   #cond);                                                    \
          failures++;                                                         \
        }                                                                     \
    }                                                                         \
  while (0)

static void
test_local_size (void)
{
  size_t l[3];
  size_t g1[3] = { 1024, 1, 1 };
  gen7_pick_local_size (1, g1, l);
  CHECK (l[0] == 64 && l[1] == 1 && l[2] == 1);

  size_t g2[3] = { 1000, 1, 1 }; /* 1000 = 8*125: largest divisor <= 64 is 50 */
  gen7_pick_local_size (1, g2, l);
  CHECK (l[0] == 50);

  size_t g3[3] = { 64, 64, 1 }; /* 64 then 256/64 = 4 */
  gen7_pick_local_size (2, g3, l);
  CHECK (l[0] == 64 && l[1] == 4 && l[2] == 1);

  size_t g4[3] = { 7, 13, 1 }; /* primes > 1 pick themselves */
  gen7_pick_local_size (2, g4, l);
  CHECK (l[0] == 7 && l[1] == 13);

  size_t g5[3] = { 67, 1, 1 }; /* prime > 64: no divisor in 2..64 -> 1 */
  gen7_pick_local_size (1, g5, l);
  CHECK (l[0] == 1);

  size_t g6[3] = { 16, 16, 16 }; /* 16, 16, then 256/256 = 1 */
  gen7_pick_local_size (3, g6, l);
  CHECK (l[0] == 16 && l[1] == 16 && l[2] == 1);
}

static gen7_curbe_layout_t
layout_none (void)
{
  gen7_curbe_layout_t l;
  memset (&l, 0xff, sizeof (l)); /* all -1 */
  return l;
}

static void
test_curbe_system (void)
{
  char curbe[256];
  memset (curbe, 0, sizeof (curbe));
  gen7_curbe_layout_t l = layout_none ();
  l.local_size[0] = 0;
  l.global_size[0] = 4;
  l.global_offset[1] = 8;
  l.group_num[0] = 12;
  l.group_num[1] = 16;
  l.thread_num = 20;
  l.work_dim = 24;
  size_t off[3] = { 0, 5, 0 }, g[3] = { 100, 1, 1 }, loc[3] = { 16, 1, 1 };
  gen7_curbe_fill_system (curbe, &l, 1, off, g, loc, loc, 2);
  CHECK (*(uint32_t *)(curbe + 0) == 16);
  CHECK (*(uint32_t *)(curbe + 4) == 100);
  CHECK (*(uint32_t *)(curbe + 8) == 5);
  CHECK (*(uint32_t *)(curbe + 12) == 7); /* ceil(100/16) */
  CHECK (*(uint32_t *)(curbe + 16) == 1);
  CHECK (*(uint32_t *)(curbe + 20) == 2);
  CHECK (*(uint32_t *)(curbe + 24) == 1);
  /* nothing written past the used offsets */
  for (int i = 28; i < 256; i++)
    CHECK (curbe[i] == 0);
}

static void
test_varying (void)
{
  /* SIMD16, local 3x2 = 6 items -> 1 thread, 10 inactive lanes */
  const size_t simd = 16, cst = 256, threads = 1;
  char *data = calloc (threads, cst);
  gen7_curbe_layout_t l = layout_none ();
  l.local_id[0] = 0;
  l.local_id[1] = 64;
  l.block_ip = 128;
  l.thread_id = 160;
  size_t loc[3] = { 3, 2, 1 };
  CHECK (gen7_thread_count (loc, simd) == 1);
  gen7_fill_varying (data, &l, loc, simd, cst, threads);
  uint32_t *x = (uint32_t *)(data + 0), *y = (uint32_t *)(data + 64);
  uint16_t *ip = (uint16_t *)(data + 128);
  CHECK (x[0] == 0 && x[1] == 1 && x[2] == 2 && x[3] == 0 && x[5] == 2);
  CHECK (y[0] == 0 && y[2] == 0 && y[3] == 1 && y[5] == 1);
  CHECK (ip[5] == 0 && ip[6] == 0xffff && ip[15] == 0xffff);
  CHECK (*(uint32_t *)(data + 160) == 0);
  free (data);

  /* SIMD8, 20 items -> 3 threads; second thread starts at item 8 */
  const size_t cst2 = 128;
  data = calloc (3, cst2);
  l = layout_none ();
  l.local_id[0] = 0;
  l.dw_block_ip = 32;
  l.thread_id = 64;
  size_t loc2[3] = { 20, 1, 1 };
  CHECK (gen7_thread_count (loc2, 8) == 3);
  gen7_fill_varying (data, &l, loc2, 8, cst2, 3);
  CHECK (((uint32_t *)(data + cst2))[0] == 8);
  CHECK (((uint32_t *)(data + 2 * cst2))[3] == 19);
  CHECK (((uint32_t *)(data + 2 * cst2 + 32))[3] == 0);
  CHECK (((uint32_t *)(data + 2 * cst2 + 32))[4] == 0xffff);
  CHECK (*(uint32_t *)(data + 2 * cst2 + 64) == 2);
  free (data);
}

static void
test_image_layout (void)
{
  gen7_image_layout_t o;
  /* 2D RGBA8 100x30, tile X: pitch 512, height 32 */
  CHECK (gen7_image_layout (CL_MEM_OBJECT_IMAGE2D, 100, 30, 0, 4, GEN7_TILE_X,
                            8192, &o)
         == 0);
  CHECK (o.tiling == GEN7_TILE_X && o.row_pitch == 512 && o.slice_pitch == 0);
  CHECK (o.size == 512 * 32 && o.depth == 1);

  /* 1D: linear, height aligned to 2 */
  gen7_image_layout (CL_MEM_OBJECT_IMAGE1D, 100, 0, 0, 4, GEN7_TILE_X, 8192,
                     &o);
  CHECK (o.tiling == GEN7_NO_TILE && o.row_pitch == 400 && o.size == 800);

  /* 1D buffer wider than the 2D limit becomes 2D */
  gen7_image_layout (CL_MEM_OBJECT_IMAGE1D_BUFFER, 10000, 0, 0, 1,
                     GEN7_TILE_X, 8192, &o);
  CHECK (o.w == 8192 && o.h == 2 && o.row_pitch == 8192);

  /* 3D tile X: slice pitch uses the 2-row alignment, size the 8-row one */
  gen7_image_layout (CL_MEM_OBJECT_IMAGE3D, 64, 5, 3, 4, GEN7_TILE_X, 8192,
                     &o);
  CHECK (o.row_pitch == 512 && o.slice_pitch == 512 * 6);
  CHECK (o.size == 512 * 8 * 3);

  /* 1D array: h = 1, linear, slice pitch = 2 rows */
  gen7_image_layout (CL_MEM_OBJECT_IMAGE1D_ARRAY, 64, 0, 4, 4, GEN7_TILE_X,
                     8192, &o);
  CHECK (o.tiling == GEN7_NO_TILE && o.slice_pitch == 2 * 256);

  /* > 128 MB tiled -> linear */
  gen7_image_layout (CL_MEM_OBJECT_IMAGE2D, 8192, 8192, 0, 4, GEN7_TILE_X,
                     8192, &o);
  CHECK (o.tiling == GEN7_NO_TILE && o.row_pitch == 8192 * 4);

  CHECK (gen7_image_layout (CL_MEM_OBJECT_BUFFER, 1, 1, 1, 1, 0, 8192, &o)
         == -1);
}

int
main (void)
{
  test_local_size ();
  test_curbe_system ();
  test_varying ();
  test_image_layout ();
  if (failures)
    {
      printf ("gen7-math-test: %d failure(s)\n", failures);
      return 1;
    }
  printf ("gen7-math-test: OK\n");
  return 0;
}
