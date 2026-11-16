#pragma once

#include "stanc_process.hpp"

#include <stanli/stanc_result.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace stanli::tooling {

// Print a compilation's warnings and O1 fallback notice to stderr and return
// its MIR, or throw CompileError with its error.
inline std::string report_compilation(const stanli::StancResult& result) {
  for (const auto& w : result.warnings) std::fprintf(stderr, "%s\n", w.c_str());
  if (result.o1_fallback)
    std::fprintf(stderr, "%s\n", result.o1_fallback->c_str());
  if (!result.mir) {
    std::string message = result.error;
    while (!message.empty() && message.back() == '\n') message.pop_back();
    throw CompileError(message);
  }
  return *result.mir;
}

}  // namespace stanli::tooling

#ifdef STANLI_EMBED_STANC
namespace stanli::tooling {

inline stanli::StancResult embedded_stanc(const std::string& model,
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
  return stanli::embedded_stanc_compile(
      src.c_str(), model.c_str(), nullptr, 0,
      stanli::kStancModelOnly | (fast_math ? stanli::kStancFastMath : 0));
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
    return report_compilation(
        run_portable_compiler(compiler, model, fast_math));
#ifdef STANLI_EMBED_STANC
  return report_compilation(embedded_stanc(model, fast_math));
#else
  const std::string found = find_portable_compiler(executable_directory());
  if (found.empty())
    throw std::runtime_error(
        "this build does not embed stanc3 and no stanli-compile is beside "
        "the executable or on PATH; pass --stanli-compile PATH or --stanc "
        "PATH");
  return report_compilation(run_portable_compiler(found, model, fast_math));
#endif
}

}  // namespace stanli::tooling
