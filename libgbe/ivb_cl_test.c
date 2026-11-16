/*
 * ivb_cl_test.c
 *
 * Beignet ve crocus/rusticl arasinda IVB (Gen7) OpenCL davranisini
 * karsilastirmak icin kucuk bir test harness'i.
 *
 * Derleme:
 *   gcc -o ivb_cl_test ivb_cl_test.c -lOpenCL
 *
 * Kullanim:
 *   ./ivb_cl_test                 -> tum platform/device'lari listeler
 *   ./ivb_cl_test <platform_idx> <device_idx>
 *                                 -> secilen device uzerinde tum testleri kosar
 *
 * Testler:
 *   1) vecadd_sanity        - temel dogrulama
 *   2) local_mem_reduction  - local memory + barrier
 *   3) global_atomics       - cl_khr_global_int32_base_atomics
 *   4) multi_buffer_global  - IKI ayri global buffer'a ayni kernel'den yazma
 *                             (senin BTI collision bug'ina benzer senaryo)
 *   5) byte_scattered_rw    - char/uchar granularity global store
 *                             (elk_dp_byte_scattered_rw_desc crash'ine yakin)
 *   6) max_wg_stress        - CL_DEVICE_MAX_WORK_GROUP_SIZE ile launch
 *
 * Her test PASS/FAIL/CRASH-benzeri hata koduyla raporlanir; segfault olursa
 * dislari process crash eder, bu da baslibasina bilgi verir (hangi testte
 * durdugunu stdout'tan gorursun, flush ediyoruz).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#define CHK(err, msg) do { \
    if ((err) != CL_SUCCESS) { \
        printf("  [FAIL] %s (err=%d)\n", msg, (int)(err)); \
        return 0; \
    } \
} while (0)

static void flush_now(void) { fflush(stdout); }

static void list_platforms(void) {
    cl_uint nplat = 0;
    clGetPlatformIDs(0, NULL, &nplat);
    cl_platform_id *plats = malloc(sizeof(cl_platform_id) * nplat);
    clGetPlatformIDs(nplat, plats, NULL);

    for (cl_uint p = 0; p < nplat; p++) {
        char name[256];
        clGetPlatformInfo(plats[p], CL_PLATFORM_NAME, sizeof(name), name, NULL);
        printf("Platform %u: %s\n", p, name);

        cl_uint ndev = 0;
        clGetDeviceIDs(plats[p], CL_DEVICE_TYPE_ALL, 0, NULL, &ndev);
        cl_device_id *devs = malloc(sizeof(cl_device_id) * ndev);
        clGetDeviceIDs(plats[p], CL_DEVICE_TYPE_ALL, ndev, devs, NULL);

        for (cl_uint d = 0; d < ndev; d++) {
            char dname[256];
            clGetDeviceInfo(devs[d], CL_DEVICE_NAME, sizeof(dname), dname, NULL);
            printf("  Device %u: %s\n", d, dname);
        }
        free(devs);
    }
    free(plats);
    printf("\nKullanim: %s <platform_idx> <device_idx>\n", "ivb_cl_test");
}

static cl_device_id get_device(int p_idx, int d_idx) {
    cl_uint nplat = 0;
    clGetPlatformIDs(0, NULL, &nplat);
    cl_platform_id *plats = malloc(sizeof(cl_platform_id) * nplat);
    clGetPlatformIDs(nplat, plats, NULL);
    if (p_idx >= (int)nplat) { fprintf(stderr, "platform index gecersiz\n"); exit(1); }

    cl_uint ndev = 0;
    clGetDeviceIDs(plats[p_idx], CL_DEVICE_TYPE_ALL, 0, NULL, &ndev);
    cl_device_id *devs = malloc(sizeof(cl_device_id) * ndev);
    clGetDeviceIDs(plats[p_idx], CL_DEVICE_TYPE_ALL, ndev, devs, NULL);
    if (d_idx >= (int)ndev) { fprintf(stderr, "device index gecersiz\n"); exit(1); }

    cl_device_id dev = devs[d_idx];
    free(devs);
    free(plats);
    return dev;
}

static cl_program build_program(cl_context ctx, cl_device_id dev, const char *src, int *ok) {
    cl_int err;
    cl_program prog = clCreateProgramWithSource(ctx, 1, &src, NULL, &err);
    if (err != CL_SUCCESS) { *ok = 0; return NULL; }

    err = clBuildProgram(prog, 1, &dev, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        size_t log_size;
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size);
        char *log = malloc(log_size + 1);
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, log_size, log, NULL);
        log[log_size] = 0;
        printf("  build log:\n%s\n", log);
        free(log);
        *ok = 0;
        return NULL;
    }
    *ok = 1;
    return prog;
}

/* ---------- Test 1: vecadd sanity ---------- */
static const char *src_vecadd =
"__kernel void vecadd(__global const float *a, __global const float *b, __global float *c) {\n"
"    int i = get_global_id(0);\n"
"    c[i] = a[i] + b[i];\n"
"}\n";

static int test_vecadd(cl_context ctx, cl_command_queue q, cl_device_id dev) {
    printf("[1] vecadd_sanity... "); flush_now();
    int ok;
    cl_program prog = build_program(ctx, dev, src_vecadd, &ok);
    if (!ok) return 0;
    cl_int err;
    cl_kernel k = clCreateKernel(prog, "vecadd", &err);
    CHK(err, "clCreateKernel");

    const int N = 1024;
    float *ha = malloc(sizeof(float) * N);
    float *hb = malloc(sizeof(float) * N);
    float *hc = malloc(sizeof(float) * N);
    for (int i = 0; i < N; i++) { ha[i] = (float)i; hb[i] = (float)(2 * i); }

    cl_mem ba = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float)*N, ha, &err);
    CHK(err, "buf a");
    cl_mem bb = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float)*N, hb, &err);
    CHK(err, "buf b");
    cl_mem bc = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(float)*N, NULL, &err);
    CHK(err, "buf c");

    clSetKernelArg(k, 0, sizeof(cl_mem), &ba);
    clSetKernelArg(k, 1, sizeof(cl_mem), &bb);
    clSetKernelArg(k, 2, sizeof(cl_mem), &bc);

    size_t gws = N;
    err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, NULL, 0, NULL, NULL);
    CHK(err, "enqueue");
    clFinish(q);

    err = clEnqueueReadBuffer(q, bc, CL_TRUE, 0, sizeof(float)*N, hc, 0, NULL, NULL);
    CHK(err, "read back");

    int good = 1;
    for (int i = 0; i < N; i++) if (hc[i] != ha[i] + hb[i]) { good = 0; break; }

    clReleaseMemObject(ba); clReleaseMemObject(bb); clReleaseMemObject(bc);
    clReleaseKernel(k); clReleaseProgram(prog);
    free(ha); free(hb); free(hc);

    printf(good ? "PASS\n" : "FAIL (yanlis sonuc)\n");
    return good;
}

/* ---------- Test 2: local memory reduction ---------- */
static const char *src_local_reduce =
"__kernel void reduce(__global const int *in, __global int *out, __local int *scratch) {\n"
"    int lid = get_local_id(0);\n"
"    int lsize = get_local_size(0);\n"
"    scratch[lid] = in[get_global_id(0)];\n"
"    barrier(CLK_LOCAL_MEM_FENCE);\n"
"    for (int off = lsize / 2; off > 0; off >>= 1) {\n"
"        if (lid < off) scratch[lid] += scratch[lid + off];\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"    }\n"
"    if (lid == 0) out[get_group_id(0)] = scratch[0];\n"
"}\n";

static int test_local_reduce(cl_context ctx, cl_command_queue q, cl_device_id dev) {
    printf("[2] local_mem_reduction... "); flush_now();
    int ok;
    cl_program prog = build_program(ctx, dev, src_local_reduce, &ok);
    if (!ok) return 0;
    cl_int err;
    cl_kernel k = clCreateKernel(prog, "reduce", &err);
    CHK(err, "clCreateKernel");

    const int LSIZE = 64;
    const int NGROUPS = 8;
    const int N = LSIZE * NGROUPS;

    int *hin = malloc(sizeof(int) * N);
    int *hout = malloc(sizeof(int) * NGROUPS);
    for (int i = 0; i < N; i++) hin[i] = 1;

    cl_mem bin = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(int)*N, hin, &err);
    CHK(err, "buf in");
    cl_mem bout = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(int)*NGROUPS, NULL, &err);
    CHK(err, "buf out");

    clSetKernelArg(k, 0, sizeof(cl_mem), &bin);
    clSetKernelArg(k, 1, sizeof(cl_mem), &bout);
    clSetKernelArg(k, 2, sizeof(int) * LSIZE, NULL);

    size_t gws = N, lws = LSIZE;
    err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, &lws, 0, NULL, NULL);
    CHK(err, "enqueue");
    clFinish(q);

    err = clEnqueueReadBuffer(q, bout, CL_TRUE, 0, sizeof(int)*NGROUPS, hout, 0, NULL, NULL);
    CHK(err, "read back");

    int good = 1;
    for (int i = 0; i < NGROUPS; i++) if (hout[i] != LSIZE) { good = 0; break; }

    clReleaseMemObject(bin); clReleaseMemObject(bout);
    clReleaseKernel(k); clReleaseProgram(prog);
    free(hin); free(hout);

    printf(good ? "PASS\n" : "FAIL (yanlis toplam)\n");
    return good;
}

/* ---------- Test 3: global atomics ---------- */
static const char *src_atomics =
"__kernel void atom_inc_test(__global int *counter) {\n"
"    atomic_inc(counter);\n"
"}\n";

static int test_atomics(cl_context ctx, cl_command_queue q, cl_device_id dev) {
    printf("[3] global_atomics... "); flush_now();
    int ok;
    cl_program prog = build_program(ctx, dev, src_atomics, &ok);
    if (!ok) return 0;
    cl_int err;
    cl_kernel k = clCreateKernel(prog, "atom_inc_test", &err);
    CHK(err, "clCreateKernel");

    int zero = 0;
    cl_mem bc = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof(int), &zero, &err);
    CHK(err, "buf counter");

    clSetKernelArg(k, 0, sizeof(cl_mem), &bc);

    const int N = 2048;
    size_t gws = N;
    err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, NULL, 0, NULL, NULL);
    CHK(err, "enqueue");
    clFinish(q);

    int result;
    err = clEnqueueReadBuffer(q, bc, CL_TRUE, 0, sizeof(int), &result, 0, NULL, NULL);
    CHK(err, "read back");

    clReleaseMemObject(bc);
    clReleaseKernel(k); clReleaseProgram(prog);

    int good = (result == N);
    printf(good ? "PASS\n" : "FAIL (beklenen %d, gelen %d)\n", N, result);
    if (!good) printf("         (bu FAIL atomic race/kayip anlamina gelir)\n");
    return good;
}

/* ---------- Test 4: iki ayri global buffer'a yazma (multi-buffer BTI senaryosu) ---------- */
static const char *src_multi_buf =
"__kernel void write_two(__global int *bufA, __global int *bufB) {\n"
"    int i = get_global_id(0);\n"
"    bufA[i] = i;\n"
"    bufB[i] = i * 2;\n"
"}\n";

static int test_multi_buffer(cl_context ctx, cl_command_queue q, cl_device_id dev) {
    printf("[4] multi_buffer_global... "); flush_now();
    int ok;
    cl_program prog = build_program(ctx, dev, src_multi_buf, &ok);
    if (!ok) return 0;
    cl_int err;
    cl_kernel k = clCreateKernel(prog, "write_two", &err);
    CHK(err, "clCreateKernel");

    const int N = 512;
    cl_mem ba = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(int)*N, NULL, &err);
    CHK(err, "buf A");
    cl_mem bb = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(int)*N, NULL, &err);
    CHK(err, "buf B");

    clSetKernelArg(k, 0, sizeof(cl_mem), &ba);
    clSetKernelArg(k, 1, sizeof(cl_mem), &bb);

    size_t gws = N;
    err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, NULL, 0, NULL, NULL);
    CHK(err, "enqueue");
    clFinish(q);

    int *ha = malloc(sizeof(int)*N);
    int *hb = malloc(sizeof(int)*N);
    err = clEnqueueReadBuffer(q, ba, CL_TRUE, 0, sizeof(int)*N, ha, 0, NULL, NULL);
    CHK(err, "read A");
    err = clEnqueueReadBuffer(q, bb, CL_TRUE, 0, sizeof(int)*N, hb, 0, NULL, NULL);
    CHK(err, "read B");

    int good = 1;
    for (int i = 0; i < N; i++) {
        if (ha[i] != i || hb[i] != i * 2) { good = 0; break; }
    }

    clReleaseMemObject(ba); clReleaseMemObject(bb);
    clReleaseKernel(k); clReleaseProgram(prog);
    free(ha); free(hb);

    printf(good ? "PASS\n" : "FAIL (buffer'lar carpismis olabilir - BTI collision)\n");
    return good;
}

/* ---------- Test 5: byte-granularity global store ---------- */
static const char *src_byte_scattered =
"__kernel void byte_scatter(__global uchar *buf) {\n"
"    int i = get_global_id(0);\n"
"    buf[i] = (uchar)(i & 0xFF);\n"
"}\n";

static int test_byte_scattered(cl_context ctx, cl_command_queue q, cl_device_id dev) {
    printf("[5] byte_scattered_rw... "); flush_now();
    int ok;
    cl_program prog = build_program(ctx, dev, src_byte_scattered, &ok);
    if (!ok) return 0;
    cl_int err;
    cl_kernel k = clCreateKernel(prog, "byte_scatter", &err);
    CHK(err, "clCreateKernel");

    const int N = 4096;
    cl_mem buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(unsigned char)*N, NULL, &err);
    CHK(err, "buf");

    clSetKernelArg(k, 0, sizeof(cl_mem), &buf);

    size_t gws = N;
    err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, NULL, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        printf("FAIL (enqueue err=%d - assert/crash burada olabilirdi)\n", (int)err);
        clReleaseKernel(k); clReleaseProgram(prog); clReleaseMemObject(buf);
        return 0;
    }
    clFinish(q);

    unsigned char *h = malloc(N);
    err = clEnqueueReadBuffer(q, buf, CL_TRUE, 0, N, h, 0, NULL, NULL);
    CHK(err, "read back");

    int good = 1;
    for (int i = 0; i < N; i++) if (h[i] != (unsigned char)(i & 0xFF)) { good = 0; break; }

    clReleaseMemObject(buf);
    clReleaseKernel(k); clReleaseProgram(prog);
    free(h);

    printf(good ? "PASS\n" : "FAIL (yanlis byte degerleri)\n");
    return good;
}

/* ---------- Test 6: raporlanan max work-group size ile stres ---------- */
static const char *src_maxwg =
"__kernel void touch(__global int *out) {\n"
"    out[get_global_id(0)] = get_local_id(0);\n"
"}\n";

static int test_max_wg(cl_context ctx, cl_command_queue q, cl_device_id dev) {
    printf("[6] max_wg_stress... "); flush_now();

    size_t max_wg = 0;
    clGetDeviceInfo(dev, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(max_wg), &max_wg, NULL);
    printf("(raporlanan max=%zu) ", max_wg); flush_now();

    int ok;
    cl_program prog = build_program(ctx, dev, src_maxwg, &ok);
    if (!ok) return 0;
    cl_int err;
    cl_kernel k = clCreateKernel(prog, "touch", &err);
    CHK(err, "clCreateKernel");

    cl_mem buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(int) * max_wg, NULL, &err);
    CHK(err, "buf");
    clSetKernelArg(k, 0, sizeof(cl_mem), &buf);

    size_t gws = max_wg, lws = max_wg;
    err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, &lws, 0, NULL, NULL);
    if (err != CL_SUCCESS) {
        printf("FAIL (enqueue err=%d - donanim bu boyutu kaldirmiyor olabilir)\n", (int)err);
        clReleaseMemObject(buf); clReleaseKernel(k); clReleaseProgram(prog);
        return 0;
    }
    clFinish(q);

    printf("PASS (calisti, cikti dogrulamasi yapilmadi)\n");
    clReleaseMemObject(buf); clReleaseKernel(k); clReleaseProgram(prog);
    return 1;
}

/* ---------- Test 7: gercek max work-group size'i binary search ile bul ---------- */
static int try_launch_wg(cl_context ctx, cl_command_queue q, cl_kernel k, cl_mem buf, size_t wg) {
    (void)ctx; (void)buf;
    size_t gws = wg;
    cl_int err = clEnqueueNDRangeKernel(q, k, 1, NULL, &gws, &wg, 0, NULL, NULL);
    if (err != CL_SUCCESS) return 0;
    err = clFinish(q);
    return err == CL_SUCCESS;
}

static int test_real_max_wg(cl_context ctx, cl_command_queue q, cl_device_id dev) {
    printf("[7] real_max_wg_binary_search... "); flush_now();

    size_t reported_max = 0;
    clGetDeviceInfo(dev, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(reported_max), &reported_max, NULL);

    int ok;
    cl_program prog = build_program(ctx, dev, src_maxwg, &ok);
    if (!ok) return 0;
    cl_int err;
    cl_kernel k = clCreateKernel(prog, "touch", &err);
    CHK(err, "clCreateKernel");

    cl_mem buf = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(int) * reported_max, NULL, &err);
    CHK(err, "buf");
    clSetKernelArg(k, 0, sizeof(cl_mem), &buf);

    /* 1) reported_max'tan asagi 2'nin katlari ile ilk calisan degeri bul */
    size_t hi = reported_max;
    size_t lo = 1;
    int found_any_pow2 = 0;

    for (size_t cand = reported_max; cand >= 1; cand >>= 1) {
        if (try_launch_wg(ctx, q, k, buf, cand)) {
            found_any_pow2 = 1;
            lo = cand;
            hi = (cand == reported_max) ? reported_max : cand << 1;
            break;
        }
        if (cand == 1) break;
    }

    if (!found_any_pow2) {
        printf("FAIL (1 bile calismadi, cok garip)\n");
        clReleaseMemObject(buf); clReleaseKernel(k); clReleaseProgram(prog);
        return 0;
    }

    /* 2) lo (calisan) ile hi arasinda binary search, en buyuk calisan wg boyutunu bul */
    size_t working = lo;
    size_t low = lo, high = hi;
    while (low < high) {
        size_t mid = low + (high - low + 1) / 2;
        if (mid == low) break;
        if (try_launch_wg(ctx, q, k, buf, mid)) {
            working = mid;
            low = mid;
        } else {
            high = mid - 1;
        }
    }

    printf("\n         raporlanan max         = %zu\n", reported_max);
    printf("         gercekte calisan max    = %zu\n", working);
    if (working != reported_max)
        printf("         [UYUMSUZLUK] cap sorgusu ile gercek HW limiti farkli!\n");
    else
        printf("         raporlanan deger dogrulandi.\n");

    clReleaseMemObject(buf); clReleaseKernel(k); clReleaseProgram(prog);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        list_platforms();
        return 0;
    }

    int p_idx = atoi(argv[1]);
    int d_idx = atoi(argv[2]);
    cl_device_id dev = get_device(p_idx, d_idx);

    char dname[256];
    clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(dname), dname, NULL);
    printf("=== Test ediliyor: %s ===\n\n", dname);

    cl_int err;
    cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
    if (err != CL_SUCCESS) { fprintf(stderr, "context olusturulamadi (%d)\n", err); return 1; }

    cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, &err);
    if (err != CL_SUCCESS) { fprintf(stderr, "queue olusturulamadi (%d)\n", err); return 1; }

    int results[7];
    results[0] = test_vecadd(ctx, q, dev);
    results[1] = test_local_reduce(ctx, q, dev);
    results[2] = test_atomics(ctx, q, dev);
    results[3] = test_multi_buffer(ctx, q, dev);
    results[4] = test_byte_scattered(ctx, q, dev);
    results[5] = test_max_wg(ctx, q, dev);
    results[6] = test_real_max_wg(ctx, q, dev);

    printf("\n=== Ozet ===\n");
    const char *names[7] = {
        "vecadd_sanity", "local_mem_reduction", "global_atomics",
        "multi_buffer_global", "byte_scattered_rw", "max_wg_stress",
        "real_max_wg_binary_search"
    };
    int total_pass = 0;
    for (int i = 0; i < 7; i++) {
        printf("  %-25s %s\n", names[i], results[i] ? "PASS" : "FAIL");
        total_pass += results[i];
    }
    printf("\n%d/7 test gecti.\n", total_pass);

    clReleaseCommandQueue(q);
    clReleaseContext(ctx);
    return 0;
}
