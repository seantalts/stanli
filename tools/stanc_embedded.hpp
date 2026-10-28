#pragma once

#include "stanc_process.hpp"

#include <stanli/stanc_result.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef STANLI_EMBED_STANC
extern "C" char* stanli_stanc_compile(const char* stan_code,
                                      const char* filename,
                                      const char* const* include_paths,
                                      size_t include_path_count, int flags);
extern "C" void stanli_stanc_free(char* p);

namespace stanli::tooling {

inline std::string embedded_stanc(const std::string& model,
                                  bool fast_math = false) {
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
  char* res = stanli_stanc_compile(src.c_str(), model.c_str(), nullptr, 0,
                                   fast_math ? 3 : 1);
  const stanli::StancResult result = stanli::parse_stanc_result(res);
  if (res) stanli_stanc_free(res);
  if (!result.warnings.empty())
    std::fprintf(stderr, "%s\n", result.warnings.c_str());
  if (!result.ok) throw std::runtime_error("stanc: " + result.body);
  return result.body;
}

}  // namespace stanli::tooling
#endif

namespace stanli::tooling {

// Both native tools use the shipped pipeline by default. Stock stanc is an
// explicit override only; a missing or broken portable compiler must fail.
inline std::string compile_source(const std::string& stanc,
                                  const std::string& compiler,
                                  const std::string& model,
                                  bool fast_math = false) {
  if (!stanc.empty()) {
    if (fast_math)
      throw std::runtime_error(
          "--fast-math needs the stanli compiler; stock stanc is not "
          "supported");
    return run_stanc_process(stanc, model);
  }
  if (!compiler.empty())
    return run_portable_compiler(compiler, model, fast_math);
#ifdef STANLI_EMBED_STANC
  return embedded_stanc(model, fast_math);
#else
  const std::string found = find_portable_compiler(executable_directory());
  if (found.empty())
    throw std::runtime_error(
        "this build does not embed stanc3 and no stanli-compile is beside "
        "the executable or on PATH; pass --stanli-compile PATH or --stanc "
        "PATH");
  return run_portable_compiler(found, model, fast_math);
#endif
}

}  // namespace stanli::tooling
