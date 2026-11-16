// Direct, value-only invocation of a Stan user-defined function.
//
// This is a C++ convenience surface over exported stanli_* symbols.  It uses
// DataMap for named arguments and DataMap::Entry for the result, preserving
// integer identity, logical dimensions, and Stan's column-major storage.
#ifndef STANLI_FUNCTION_HPP
#define STANLI_FUNCTION_HPP

#include <stanli/data.hpp>

#include <cstddef>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

struct stanli_function;

extern "C" {

stanli_function* stanli_function_new_from_stan(const char* stan_code,
                                               const char* function_name,
                                               char* err, size_t err_len);
stanli_function* stanli_function_new_from_mir(const char* mir_text,
                                              const char* function_name,
                                              char* err, size_t err_len);
void stanli_function_free(stanli_function* function);
// Nonzero makes each later call emit its function selection and interpreter
// trace through the diagnostic sink. Handles start with it off, unless the
// developer switch STANLI_EXECUTION_REPORT is set. Set it before calling,
// not while another thread calls the handle.
void stanli_function_set_execution_report(stanli_function* function,
                                          int enabled);

// Results are borrowed only for the duration of this callback. Keeping the
// copy on the caller side avoids transferring STL-owned storage across the
// shared-library boundary, where the two sides may use different C++ runtimes.
typedef int (*stanli_function_result_writer)(
    void* context, int is_int, const double* reals, size_t real_size,
    const int* ints, size_t int_size, const int64_t* dims, size_t dim_size);

int stanli_function_call(const stanli_function* function,
                         const stanli::DataMap* arguments,
                         stanli_function_result_writer write_result,
                         void* result_context, char* err, size_t err_len);

// Plain-C input buffers for language bindings that cannot construct DataMap.
// Exactly one of reals/ints is used, according to is_int (0 or 1). Values are
// first-index-fastest, just like DataMap; no dimensions means one scalar.
// All storage is borrowed for the call and may be null only when its size is
// zero. The runtime copies inputs before evaluation and never mutates them.
struct stanli_function_argument {
  const char* name;
  int is_int;
  const double* reals;
  const int* ints;
  size_t size;
  const int64_t* dims;
  size_t dim_size;
};

int stanli_function_call_values(const stanli_function* function,
                                const stanli_function_argument* arguments,
                                size_t argument_size,
                                stanli_function_result_writer write_result,
                                void* result_context, char* err,
                                size_t err_len);

}  // extern "C"

namespace stanli {

class Function {
 public:
  Function(const std::string& stan_code, const std::string& function_name)
      : handle_(construct(stan_code, function_name, false)) {}

  static Function from_mir(const std::string& mir_text,
                           const std::string& function_name) {
    return Function(construct(mir_text, function_name, true));
  }

  ~Function() { stanli_function_free(handle_); }

  Function(const Function&) = delete;
  Function& operator=(const Function&) = delete;

  Function(Function&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  Function& operator=(Function&& other) noexcept {
    if (this != &other) {
      stanli_function_free(handle_);
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  void set_execution_report(bool enabled) {
    stanli_function_set_execution_report(handle_, enabled ? 1 : 0);
  }

  DataMap::Entry operator()(const DataMap& arguments) const {
    char err[8192] = {};
    DataMap::Entry result;
    struct ResultContext {
      DataMap::Entry* result;
      std::exception_ptr error;
    } context{&result, nullptr};

    const auto write_result = [](void* opaque, int is_int, const double* reals,
                                 size_t real_size, const int* ints,
                                 size_t int_size, const int64_t* dims,
                                 size_t dim_size) -> int {
      auto& context = *static_cast<ResultContext*>(opaque);
      try {
        context.result->is_int = is_int != 0;
        context.result->r.clear();
        context.result->i.clear();
        context.result->dims.clear();
        if (real_size != 0) context.result->r.assign(reals, reals + real_size);
        if (int_size != 0) context.result->i.assign(ints, ints + int_size);
        if (dim_size != 0) context.result->dims.assign(dims, dims + dim_size);
        return 0;
      } catch (...) {
        context.error = std::current_exception();
        return 1;
      }
    };

    if (stanli_function_call(handle_, &arguments, write_result, &context, err,
                             sizeof(err))) {
      if (context.error) std::rethrow_exception(context.error);
      throw std::runtime_error(err[0] ? err
                                      : "Stan function evaluation failed");
    }
    return result;
  }

 private:
  explicit Function(stanli_function* handle) : handle_(handle) {}

  static stanli_function* construct(const std::string& text,
                                    const std::string& name, bool mir) {
    char err[8192] = {};
    stanli_function* result =
        mir ? stanli_function_new_from_mir(text.c_str(), name.c_str(), err,
                                           sizeof(err))
            : stanli_function_new_from_stan(text.c_str(), name.c_str(), err,
                                            sizeof(err));
    if (!result)
      throw std::runtime_error(err[0] ? err
                                      : "Stan function compilation failed");
    return result;
  }

  stanli_function* handle_ = nullptr;
};

inline DataMap::Entry evaluate_function(const std::string& stan_code,
                                        const std::string& function_name,
                                        const DataMap& arguments) {
  return Function(stan_code, function_name)(arguments);
}

}  // namespace stanli

#endif
