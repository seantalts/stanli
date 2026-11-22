#pragma once

#include <stanli/stanc_result.hpp>

#include <stdexcept>
#include <string>

namespace stanli::tooling {

// The compiler rejected the model. what() is the compiler's own message,
// which already names the file and location.
struct CompileError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Run `stanli_check --emit-mir model --passes passes` and return the
// portable MIR it prints, for tests that compile a model with a chosen pass
// list. A nonzero exit throws CompileError with its stderr; otherwise stderr
// (warnings) is copied to ours. Arguments are passed directly to the child
// process: model and executable paths are never interpreted by a shell.
std::string run_emit_mir(const std::string& stanli_check,
                         const std::string& model, const std::string& passes);

// Run stanli-compile, the shipped pipeline as an executable, on one model.
// The result holds the portable MIR it prints and the warnings it writes to
// its --messages-json file. Errors and warnings
// name the model by the path given here, as stanc does; failures throw
// CompileError with the compiler's stderr. A nonempty `passes` is
// stanli-compile's --passes spec. `model_only` drops what only C++ needs,
// such as unused functions.
stanli::StancResult run_portable_compiler(const std::string& compiler,
                                          const std::string& model,
                                          bool fast_math = false,
                                          const std::string& passes = "",
                                          bool model_only = true);

// The directory holding the running executable.
std::string executable_directory();

// stanli-compile in `directory`, then on PATH; empty when neither has one.
std::string find_portable_compiler(const std::string& directory);

}  // namespace stanli::tooling
