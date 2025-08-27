#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Sortbench plugin ABI v1 (backward compatible)
// Exported symbol:
//   int sortbench_get_algorithms_v1(const sortbench_algo_v1** out_algos, int* out_count);
// Return 1 on success, 0 on failure. out_algos points to a static array owned by the plugin.

typedef void (*sortbench_run_int_fn)(int* data, int n);

typedef struct sortbench_algo_v1 {
    const char* name;              // unique algorithm name to display
    sortbench_run_int_fn run_int;  // sorts int data in-place: ascending order
} sortbench_algo_v1;

int sortbench_get_algorithms_v1(const sortbench_algo_v1** out_algos, int* out_count);

// Sortbench plugin ABI v2 (multi-type support). All function pointers are optional;
// set to NULL when the algorithm does not support that element type.
typedef void (*sortbench_run_u32_fn)(unsigned int* data, int n);
typedef void (*sortbench_run_i64_fn)(long long* data, int n);
typedef void (*sortbench_run_u64_fn)(unsigned long long* data, int n);
typedef void (*sortbench_run_f32_fn)(float* data, int n);
typedef void (*sortbench_run_f64_fn)(double* data, int n);

typedef struct sortbench_algo_v2 {
    const char* name;
    // Optional entry points for different types
    sortbench_run_int_fn  run_i32;  // int
    sortbench_run_u32_fn  run_u32;  // unsigned int
    sortbench_run_i64_fn  run_i64;  // long long
    sortbench_run_u64_fn  run_u64;  // unsigned long long
    sortbench_run_f32_fn  run_f32;  // float
    sortbench_run_f64_fn  run_f64;  // double
} sortbench_algo_v2;

// New entry point; preferred when available
int sortbench_get_algorithms_v2(const sortbench_algo_v2** out_algos, int* out_count);

#ifdef __cplusplus
}
#endif
