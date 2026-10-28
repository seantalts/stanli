#pragma once

#include <cstdlib>
#include <string>

namespace stanli {

struct StancResult {
  std::string warnings;
  bool ok = false;
  std::string body;
};

inline StancResult parse_stanc_result(const char* raw) {
  StancResult out;
  if (raw == nullptr) {
    out.body = "stanc returned nothing";
    return out;
  }
  const char* p = raw;
  if (*p == 'W') {
    char* end = nullptr;
    const unsigned long n = std::strtoul(p + 1, &end, 10);
    if (end != nullptr && *end == ':') {
      out.warnings.assign(end + 1, n);
      p = end + 1 + n;
    }
  }
  if (p[0] == 'O' && p[1] == 'K') {
    out.ok = true;
    out.body = p + 2;
  } else {
    out.body = p[0] == 'E' && p[1] == 'R' && p[2] == 'R' ? p + 3 : p;
  }
  return out;
}

}  // namespace stanli
