// Simple C plugin using the v2 multi-type ABI. Implements wrappers around
// the C standard library qsort for several element types.

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../sortbench_plugin.h"

static int cmp_i32(const void* a, const void* b){
    int aa = *(const int*)a, bb = *(const int*)b;
    return (aa>bb) - (aa<bb);
}
static int cmp_u32(const void* a, const void* b){
    unsigned int aa = *(const unsigned int*)a, bb = *(const unsigned int*)b;
    return (aa>bb) - (aa<bb);
}
static int cmp_i64(const void* a, const void* b){
    long long aa = *(const long long*)a, bb = *(const long long*)b;
    return (aa>bb) - (aa<bb);
}
static int cmp_u64(const void* a, const void* b){
    unsigned long long aa = *(const unsigned long long*)a, bb = *(const unsigned long long*)b;
    return (aa>bb) - (aa<bb);
}
static int cmp_f32(const void* a, const void* b){
    float aa = *(const float*)a, bb = *(const float*)b;
    if (isnan(aa) && isnan(bb)) return 0; if (isnan(aa)) return -1; if (isnan(bb)) return 1;
    return (aa>bb) - (aa<bb);
}
static int cmp_f64(const void* a, const void* b){
    double aa = *(const double*)a, bb = *(const double*)b;
    if (isnan(aa) && isnan(bb)) return 0; if (isnan(aa)) return -1; if (isnan(bb)) return 1;
    return (aa>bb) - (aa<bb);
}

static void run_i32(int* data, int n){ qsort(data, (size_t)n, sizeof(int), cmp_i32); }
static void run_u32(unsigned int* data, int n){ qsort(data, (size_t)n, sizeof(unsigned int), cmp_u32); }
static void run_i64(long long* data, int n){ qsort(data, (size_t)n, sizeof(long long), cmp_i64); }
static void run_u64(unsigned long long* data, int n){ qsort(data, (size_t)n, sizeof(unsigned long long), cmp_u64); }
static void run_f32(float* data, int n){ qsort(data, (size_t)n, sizeof(float), cmp_f32); }
static void run_f64(double* data, int n){ qsort(data, (size_t)n, sizeof(double), cmp_f64); }

static const sortbench_algo_v2 ALGOS[] = {
    {
        .name = "c_qsort_libc",
        .run_i32 = run_i32,
        .run_u32 = run_u32,
        .run_i64 = run_i64,
        .run_u64 = run_u64,
        .run_f32 = run_f32,
        .run_f64 = run_f64,
    },
};

int sortbench_get_algorithms_v2(const sortbench_algo_v2** out_algos, int* out_count){
    if (!out_algos || !out_count) return 0;
    *out_algos = ALGOS;
    *out_count = (int)(sizeof(ALGOS)/sizeof(ALGOS[0]));
    return 1;
}

