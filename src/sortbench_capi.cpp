#include "sortbench/core.hpp"
#include "sortbench/capi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

using namespace sortbench;

static char* dup_cstr(const std::string& s) {
  char* p = (char*)std::malloc(s.size() + 1);
  if (!p) return nullptr;
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

extern "C" char* sb_run_json(const sb_core_config* c, int include_speedup, int pretty, char** err_out) {
  if (err_out) *err_out = nullptr;
  try {
    CoreConfig cfg;
    cfg.N = (std::size_t)c->N;
    cfg.dist = static_cast<Dist>(c->dist);
    cfg.type = static_cast<ElemType>(c->elem_type);
    cfg.repeats = c->repeats;
    cfg.warmup = c->warmup;
    if (c->has_seed) cfg.seed = (std::uint64_t)c->seed;
    cfg.algos.clear();
    for (int i = 0; i < c->algos_len; ++i) if (c->algos && c->algos[i]) cfg.algos.emplace_back(c->algos[i]);
    cfg.threads = c->threads;
    cfg.assert_sorted = (c->assert_sorted != 0);
    cfg.verify = (c->verify != 0);
    if (c->has_baseline && c->baseline) cfg.baseline = std::string(c->baseline);
    cfg.partial_shuffle_pct = c->partial_shuffle_pct;
    cfg.dup_values = c->dup_values;
    cfg.plugin_paths.clear();
    for (int i = 0; i < c->plugin_len; ++i) if (c->plugin_paths && c->plugin_paths[i]) cfg.plugin_paths.emplace_back(c->plugin_paths[i]);

    RunResult r = run_benchmark(cfg);
    std::string js = to_json(r, include_speedup != 0, pretty != 0);
    return dup_cstr(js);
  } catch (const std::exception& e) {
    if (err_out) *err_out = dup_cstr(std::string("error: ") + e.what());
    return nullptr;
  }
}

extern "C" char* sb_list_algos_json(int elem_type, const char* const* plugins, int plugins_len, char** err_out) {
  if (err_out) *err_out = nullptr;
  try {
    ElemType t = static_cast<ElemType>(elem_type);
    std::vector<std::string> pp;
    for (int i = 0; i < plugins_len; ++i) if (plugins && plugins[i]) pp.emplace_back(plugins[i]);
    std::vector<std::string> names = (pp.empty() ? list_algorithms(t) : list_algorithms(t, pp));
    std::string js = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i) js += ",";
      js += "\"";
      // simple JSON escape for names
      for (char c : names[i]) {
        switch (c) {
          case '"': js += "\\\""; break;
          case '\\': js += "\\\\"; break;
          default: js += c; break;
        }
      }
      js += "\"";
    }
    js += "]";
    return dup_cstr(js);
  } catch (const std::exception& e) {
    if (err_out) *err_out = dup_cstr(std::string("error: ") + e.what());
    return nullptr;
  }
}

extern "C" void sb_free(char* p) {
  if (p) std::free(p);
}

