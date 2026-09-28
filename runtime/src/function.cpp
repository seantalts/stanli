#include <stanli/function.hpp>

#if __has_include(<stanli/capi.h>)
#include <stanli/capi.h>
#define STANLI_FUNCTION_HAS_CAPI 1
#else
// Source-only consumers may deliberately omit the separately exported C API
// while compiling runtime/src/*.cpp as a library. Function::from_mir remains
// usable there; only the convenience constructor from Stan source is absent.
#define STANLI_FUNCTION_HAS_CAPI 0
#endif
#include <stanli/mir_decode.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/message_sink.hpp>

#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void put_err(char* err, size_t err_len, const std::string& what) {
  if (err == nullptr || err_len == 0) return;
  std::strncpy(err, what.c_str(), err_len - 1);
  err[err_len - 1] = '\0';
}

size_t leaf_rank(stanli::mir::UnsizedLeaf leaf) {
  using Leaf = stanli::mir::UnsizedLeaf;
  switch (leaf) {
    case Leaf::Int:
    case Leaf::Real:
      return 0;
    case Leaf::Vector:
    case Leaf::RowVector:
      return 1;
    case Leaf::Matrix:
      return 2;
    case Leaf::Complex:
      throw std::runtime_error("complex function arguments are unsupported");
    case Leaf::Unknown:
      throw std::runtime_error("function has unknown argument type");
  }
  throw std::runtime_error("function has invalid argument type");
}

int64_t checked_elements(const std::vector<int64_t>& dims,
                         const std::string& name) {
  int64_t n = 1;
  for (int64_t dim : dims) {
    if (dim < 0)
      throw std::runtime_error("argument '" + name +
                               "' has a negative dimension");
    if (dim != 0 && n > std::numeric_limits<int64_t>::max() / dim)
      throw std::runtime_error("argument '" + name + "' is too large");
    n *= dim;
  }
  return n;
}

void validate_argument(const std::string& name,
                       const stanli::mir::UnsizedView& view,
                       const stanli::DataMap::Entry& value) {
  using Leaf = stanli::mir::UnsizedLeaf;
  const size_t rank = static_cast<size_t>(view.depth) + leaf_rank(view.leaf);
  if (value.dims.size() != rank)
    throw std::runtime_error("argument '" + name + "' has rank " +
                             std::to_string(value.dims.size()) + ", expected " +
                             std::to_string(rank));

  const int64_t expected = checked_elements(value.dims, name);
  if (value.r.size() != static_cast<size_t>(expected))
    throw std::runtime_error("argument '" + name +
                             "' storage does not match its dimensions");
  if (view.leaf == Leaf::Int) {
    if (!value.is_int)
      throw std::runtime_error("argument '" + name + "' must be integer");
    if (value.i.size() != static_cast<size_t>(expected))
      throw std::runtime_error("integer argument '" + name +
                               "' has no matching integer storage");
  }
}

bool named(const stanli::mir::FunDef& f, std::string_view requested) {
  if (f.name == requested) return true;
  return f.name.size() > requested.size() &&
         f.name.compare(0, requested.size(), requested) == 0 &&
         f.name[requested.size()] == '(';
}

const stanli::mir::FunDef* select_function(
    const std::vector<const stanli::mir::FunDef*>& candidates,
    const std::string& requested, const stanli::DataMap* arguments) {
  // An explicitly resolved signature, e.g. foo(real,vector), selects one
  // definition without inspecting values.
  for (const auto* f : candidates)
    if (f->name == requested) return f;
  if (arguments == nullptr) {
    if (candidates.size() == 1) return candidates.front();
    throw std::runtime_error("Stan function '" + requested +
                             "' is overloaded; use a resolved signature");
  }

  std::vector<const stanli::mir::FunDef*> matches;
  int best_promotions = std::numeric_limits<int>::max();
  for (const auto* f : candidates) {
    try {
      int promotions = 0;
      for (size_t i = 0; i < f->arg_names.size(); ++i) {
        const auto& value = arguments->at(f->arg_names[i]);
        validate_argument(f->arg_names[i], f->arg_views[i], value);
        // Match stanc's ordinary numeric promotion preference: an integer
        // argument selects an integer overload ahead of a real overload.
        if (value.is_int &&
            f->arg_views[i].leaf == stanli::mir::UnsizedLeaf::Real)
          ++promotions;
      }
      if (promotions < best_promotions) {
        best_promotions = promotions;
        matches.clear();
      }
      if (promotions == best_promotions) matches.push_back(f);
    } catch (const std::exception&) {
    }
  }
  if (matches.size() == 1) return matches.front();
  if (matches.empty())
    throw std::runtime_error("no overload of Stan function '" + requested +
                             "' matches the arguments");
  throw std::runtime_error(
      "arguments ambiguously match overloaded Stan function '" + requested +
      "'; use a resolved signature");
}

}  // namespace

struct stanli_function {
  // Select diagnostics at handle construction, not inside the repeated call.
  bool report_execution = stanli::execution_reporting_enabled();
  std::shared_ptr<const stanli::mir::Program> program;
  std::string requested_name;
  // Built once, then read-only. The immutable Program owns every pointed-to
  // definition and outlives these tables. Overload winners are deliberately
  // NOT cached: type/rank/promotion validation still runs on each call.
  std::map<std::string, const stanli::mir::FunDef*> functions;
  std::vector<const stanli::mir::FunDef*> candidates;
};

extern "C" {

stanli_function* stanli_function_new_from_mir(const char* mir_text,
                                              const char* function_name,
                                              char* err, size_t err_len) {
  try {
    if (mir_text == nullptr || function_name == nullptr)
      throw std::runtime_error("function MIR and name must not be null");
    auto out = std::make_unique<stanli_function>();
    out->program = std::make_shared<stanli::mir::Program>(
        stanli::decode_program(mir_text));
    out->requested_name = function_name;
    // Fail at construction for a missing name. Overloads are intentionally
    // selected later, when their argument values are present.
    for (const auto& f : out->program->fun_defs) {
      out->functions.emplace(f.name, &f);
      if (named(f, function_name)) out->candidates.push_back(&f);
    }
    if (out->candidates.empty())
      throw std::runtime_error("Stan function not found: " +
                               std::string(function_name));
    return out.release();
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return nullptr;
  }
}

stanli_function* stanli_function_new_from_stan(const char* stan_code,
                                               const char* function_name,
                                               char* err, size_t err_len) {
  if (stan_code == nullptr || function_name == nullptr) {
    put_err(err, err_len, "function source and name must not be null");
    return nullptr;
  }
#if STANLI_FUNCTION_HAS_CAPI
  char* mir = stanli_stan_to_mir(stan_code, err, err_len);
  if (mir == nullptr) return nullptr;
  stanli_function* out =
      stanli_function_new_from_mir(mir, function_name, err, err_len);
  stanli_string_free(mir);
  return out;
#else
  put_err(err, err_len,
          "Stan source compilation is unavailable without the Stanli C API; "
          "construct the function from MIR instead");
  return nullptr;
#endif
}

void stanli_function_free(stanli_function* function) { delete function; }

int stanli_function_call(const stanli_function* function,
                         const stanli::DataMap* arguments,
                         stanli_function_result_writer write_result,
                         void* result_context, char* err, size_t err_len) {
  stanli::ExecutionTrace trace;
  std::optional<stanli::ExecutionTraceScope> trace_scope;
  if (function && function->report_execution) trace_scope.emplace(trace);
  try {
    if (function == nullptr || arguments == nullptr || write_result == nullptr)
      throw std::runtime_error(
          "function, arguments, and result writer must not be null");
    const stanli::mir::FunDef* selected = select_function(
        function->candidates, function->requested_name, arguments);

    std::vector<stanli::DataMap::Entry> values;
    values.reserve(selected->arg_names.size());
    for (size_t i = 0; i < selected->arg_names.size(); ++i) {
      auto value = arguments->at(selected->arg_names[i]);
      validate_argument(selected->arg_names[i], selected->arg_views[i], value);
      // DataMap keeps an integer mirror so integer data can be promoted to a
      // real formal. Once selected, the formal owns the type: leaving the
      // mirror set would make a real identity function return an integer and
      // could make later interpreter operations choose integer semantics.
      if (selected->arg_views[i].leaf != stanli::mir::UnsizedLeaf::Int) {
        value.is_int = false;
        value.i.clear();
      }
      values.push_back(std::move(value));
    }

    stanli::MirInterp<double> interpreter(
        function->functions, "function " + function->requested_name);
    const auto result = interpreter.call(*selected, values);
    if (write_result(result_context, result.is_int ? 1 : 0, result.r.data(),
                     result.r.size(), result.i.data(), result.i.size(),
                     result.dims.data(), result.dims.size()) != 0)
      throw std::runtime_error("function result writer failed");
    if (trace_scope)
      stanli::emit_diagnostic(stanli::execution_trace_report(trace));
    return 0;
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    if (trace_scope) {
      try {
        stanli::emit_diagnostic(stanli::execution_trace_report(trace));
      } catch (...) {
      }  // Preserve the original C-API error even if a sink fails.
    }
    return 1;
  }
}

int stanli_function_call_values(const stanli_function* function,
                                const stanli_function_argument* arguments,
                                size_t argument_size,
                                stanli_function_result_writer write_result,
                                void* result_context, char* err,
                                size_t err_len) {
  try {
    if (argument_size != 0 && arguments == nullptr)
      throw std::runtime_error("function arguments must not be null");
    stanli::DataMap values;
    for (size_t i = 0; i < argument_size; ++i) {
      const auto& arg = arguments[i];
      if (arg.name == nullptr || arg.name[0] == '\0')
        throw std::runtime_error("function argument name must not be empty");
      const std::string name = arg.name;
      if (values.has(name))
        throw std::runtime_error("duplicate function argument: " + name);
      if (arg.dim_size != 0 && arg.dims == nullptr)
        throw std::runtime_error("argument '" + name + "' dimensions are null");
      std::vector<int64_t> dims;
      if (arg.dim_size != 0) dims.assign(arg.dims, arg.dims + arg.dim_size);
      if (static_cast<uint64_t>(checked_elements(dims, name)) != arg.size)
        throw std::runtime_error("argument '" + name +
                                 "' storage does not match its dimensions");
      if (arg.is_int == 1) {
        if (arg.size != 0 && arg.ints == nullptr)
          throw std::runtime_error("integer argument '" + name + "' is null");
        if (dims.empty()) {
          values.set_int(name, arg.ints[0]);
        } else {
          std::vector<int> data;
          if (arg.size != 0) data.assign(arg.ints, arg.ints + arg.size);
          values.set_int_array(name, std::move(data), std::move(dims));
        }
      } else if (arg.is_int == 0) {
        if (arg.size != 0 && arg.reals == nullptr)
          throw std::runtime_error("real argument '" + name + "' is null");
        if (dims.empty()) {
          values.set_real(name, arg.reals[0]);
        } else {
          std::vector<double> data;
          if (arg.size != 0) data.assign(arg.reals, arg.reals + arg.size);
          values.set_real_array(name, std::move(data), std::move(dims));
        }
      } else {
        throw std::runtime_error("argument '" + name + "' has invalid is_int");
      }
    }
    return stanli_function_call(function, &values, write_result, result_context,
                                err, err_len);
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return 1;
  }
}

}  // extern "C"
