#pragma once

#include <string>

namespace stanli::tooling {

// Run stock stanc with the flags used by the command-line tools and return
// its MIR stdout. Arguments are passed directly to the child process: model
// and compiler paths are never interpreted by a shell.
std::string run_stanc_process(const std::string& stanc,
                              const std::string& model);

// Run stanli-compile, the shipped pipeline as an executable, on one model and
// return the portable MIR it prints.
std::string run_portable_compiler(const std::string& compiler,
                                  const std::string& model);

// The directory holding the running executable.
std::string executable_directory();

// stanli-compile in `directory`, then on PATH; empty when neither has one.
std::string find_portable_compiler(const std::string& directory);

}  // namespace stanli::tooling
