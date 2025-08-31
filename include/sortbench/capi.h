#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// These enum values must match sortbench::ElemType and sortbench::Dist
enum sb_elem_type {
  SB_ELEM_I32 = 0,
  SB_ELEM_U32 = 1,
  SB_ELEM_I64 = 2,
  SB_ELEM_U64 = 3,
  SB_ELEM_F32 = 4,
  SB_ELEM_F64 = 5,
  SB_ELEM_STR = 6,
};

enum sb_dist {
  SB_DIST_RANDOM = 0,
  SB_DIST_PARTIAL = 1,
  SB_DIST_DUPS = 2,
  SB_DIST_REVERSE = 3,
  SB_DIST_SORTED = 4,
  SB_DIST_SAW = 5,
  SB_DIST_RUNS = 6,
  SB_DIST_GAUSS = 7,
  SB_DIST_EXP = 8,
  SB_DIST_ZIPF = 9,
  // New distributions — keep values in sync with sortbench::Dist
  SB_DIST_ORGANPIPE = 10,
  SB_DIST_STAGGERED = 11,
  SB_DIST_RUNS_HT = 12,
};

typedef struct sb_core_config {
  uint64_t N;
  int dist;            // sb_dist
  int elem_type;       // sb_elem_type
  int repeats;
  int warmup;
  uint64_t seed;
  int has_seed;
  const char** algos;
  int algos_len;
  int threads;
  int assert_sorted;
  int verify;
  const char* baseline;
  int has_baseline;
  int partial_shuffle_pct;
  int dup_values;
  double zipf_s;
  double runs_alpha;
  int stagger_block;
  const char** plugin_paths;
  int plugin_len;
} sb_core_config;

// Returns malloc-allocated JSON string on success; caller must free via sb_free.
// On error, returns NULL and sets *err_out (also needs sb_free).
char* sb_run_json(const sb_core_config* cfg, int include_speedup, int pretty, char** err_out);

// Returns JSON array string of algorithm names. Caller frees via sb_free.
char* sb_list_algos_json(int elem_type, const char* const* plugins, int plugins_len, char** err_out);

void sb_free(char* p);

#ifdef __cplusplus
}
#endif
