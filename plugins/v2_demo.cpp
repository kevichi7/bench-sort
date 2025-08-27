#include <algorithm>
#include <vector>
#include "../sortbench_plugin.h"

static void v2_sort_i32(int* data, int n) { std::sort(data, data+n); }
static void v2_sort_f32(float* data, int n) { std::sort(data, data+n); }

static const sortbench_algo_v2 k_algos[] = {
    {"v2_std_sort", &v2_sort_i32, nullptr, nullptr, nullptr, &v2_sort_f32},
};

extern "C" int sortbench_get_algorithms_v2(const sortbench_algo_v2** out_algos, int* out_count) {
    if (!out_algos || !out_count) return 0;
    *out_algos = k_algos;
    *out_count = (int)(sizeof(k_algos)/sizeof(k_algos[0]));
    return 1;
}
