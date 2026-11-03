#pragma once

#include <stdexcept>
#include <string>

namespace stanli::tooling {

// The compiler rejected the model. what() is the compiler's own message,
// which already names the file and location.
struct CompileError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Run stock stanc with the flags used by the command-line tools and return
// its MIR stdout. A nonzero exit throws CompileError with stanc's stderr;
// otherwise stderr (warnings) is copied to ours. Arguments are passed directly
// to the child process: model and compiler paths are never interpreted by a
// shell.
std::string run_stanc_process(const std::string& stanc,
                              const std::string& model);

// Run stanli-compile, the shipped pipeline as an executable, on one model and
// return the portable MIR it prints. Errors and warnings name the model by
// the path given here, as stanc does; failures throw CompileError.
std::string run_portable_compiler(const std::string& compiler,
                                  const std::string& model,
                                  bool fast_math = false);

// The directory holding the running executable.
std::string executable_directory();

// stanli-compile in `directory`, then on PATH; empty when neither has one.
std::string find_portable_compiler(const std::string& directory);

}  // namespace stanli::tooling
