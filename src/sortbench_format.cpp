// Pure formatting helpers for RunResult
#include "sortbench/core.hpp"

#include <iomanip>
#include <sstream>
#include <string>

namespace sortbench {

static inline std::string esc_json(const std::string &s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"': o += "\\\""; break;
    case '\\': o += "\\\\"; break;
    case '\n': o += "\\n"; break;
    case '\r': o += "\\r"; break;
    case '\t': o += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[7];
        std::snprintf(buf, sizeof(buf), "\\u%04x", (int)(unsigned char)c);
        o += buf;
      } else {
        o += c;
      }
    }
  }
  return o;
}

std::string to_csv(const RunResult &r, bool with_header, bool include_speedup) {
  std::ostringstream os;
  if (with_header) {
    os << "algo,N,dist,median_ms,mean_ms,min_ms,max_ms,stddev_ms";
    if (include_speedup)
      os << ",speedup_vs_baseline";
    os << '\n';
  }
  os.setf(std::ios::fixed);
  os << std::setprecision(3);
  for (const auto &row : r.rows) {
    os << row.algo << ',' << row.N << ',' << row.dist << ','
       << row.stats.median_ms << ',' << row.stats.mean_ms << ','
       << row.stats.min_ms << ',' << row.stats.max_ms << ','
       << row.stats.stddev_ms;
    if (include_speedup)
      os << ',' << row.speedup_vs_baseline;
    os << '\n';
  }
  return os.str();
}

std::string to_json(const RunResult &r, bool include_speedup, bool pretty) {
  std::ostringstream os;
  const char *nl = pretty ? "\n" : "";
  const char *sp = pretty ? "  " : "";
  os << "[" << nl;
  for (std::size_t i = 0; i < r.rows.size(); ++i) {
    const auto &row = r.rows[i];
    os << (pretty ? "  {" : "{");
    os << "\"algo\":\"" << esc_json(row.algo) << "\",";
    os << "\"N\":" << row.N << ",";
    os << "\"dist\":\"" << esc_json(row.dist) << "\",";
    os.setf(std::ios::fixed);
    os << std::setprecision(3);
    os << "\"median_ms\":" << row.stats.median_ms << ",";
    os << "\"mean_ms\":" << row.stats.mean_ms << ",";
    os << "\"min_ms\":" << row.stats.min_ms << ",";
    os << "\"max_ms\":" << row.stats.max_ms << ",";
    os << "\"stddev_ms\":" << row.stats.stddev_ms;
    if (include_speedup)
      os << ",\"speedup_vs_baseline\":" << row.speedup_vs_baseline;
    os << "}";
    if (i + 1 != r.rows.size())
      os << ",";
    os << nl;
  }
  os << "]" << nl;
  return os.str();
}

std::string to_jsonl(const RunResult &r, bool include_speedup) {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os << std::setprecision(3);
  for (const auto &row : r.rows) {
    os << '{' << "\"algo\":\"" << esc_json(row.algo) << "\",";
    os << "\"N\":" << row.N << ",";
    os << "\"dist\":\"" << esc_json(row.dist) << "\",";
    os << "\"median_ms\":" << row.stats.median_ms << ",";
    os << "\"mean_ms\":" << row.stats.mean_ms << ",";
    os << "\"min_ms\":" << row.stats.min_ms << ",";
    os << "\"max_ms\":" << row.stats.max_ms << ",";
    os << "\"stddev_ms\":" << row.stats.stddev_ms;
    if (include_speedup)
      os << ",\"speedup_vs_baseline\":" << row.speedup_vs_baseline;
    os << "}" << '\n';
  }
  return os.str();
}

} // namespace sortbench

