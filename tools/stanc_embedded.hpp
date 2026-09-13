#pragma once

#include "stanc_process.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef STANLI_EMBED_STANC
extern "C" char* stanli_stanc_tmir(const char* stan_code);
extern "C" void stanli_stanc_free(char* p);

namespace stanli::tooling {

inline std::string embedded_stanc(const std::string& model) {
  std::string src;
  {
    std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(model.c_str(), "rb"),
                                            std::fclose);
    if (!f) throw std::runtime_error("cannot read " + model);
    std::array<char, 1 << 16> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f.get())) > 0)
      src.append(buf.data(), n);
  }
  char* res = stanli_stanc_tmir(src.c_str());
  const std::string out(res ? res : "ERRstanc returned nothing");
  if (res) stanli_stanc_free(res);
  if (out.compare(0, 3, "ERR") == 0)
    throw std::runtime_error("stanc: " + out.substr(3));
  return out.substr(2);
}

}  // namespace stanli::tooling
#endif

namespace stanli::tooling {

// Both native tools use the shipped pipeline by default. Stock stanc is an
// explicit override only; a missing or broken portable compiler must fail.
inline std::string compile_source(const std::string& stanc,
                                  const std::string& compiler,
                                  const std::string& model) {
  if (!stanc.empty()) return run_stanc_process(stanc, model);
  if (!compiler.empty()) return run_portable_compiler(compiler, model);
#ifdef STANLI_EMBED_STANC
  return embedded_stanc(model);
#else
  const std::string found = find_portable_compiler(executable_directory());
  if (found.empty())
    throw std::runtime_error(
        "this build does not embed stanc3 and no stanli-compile is beside "
        "the executable or on PATH; pass --stanli-compile PATH or --stanc "
        "PATH");
  return run_portable_compiler(found, model);
#endif
}

}  // namespace stanli::tooling
