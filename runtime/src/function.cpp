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
#include <stanli/mir_prog.hpp>
#include <stanli/message_sink.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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

// Values remain runtime inputs. Only integer formals and complete dimensions
// specialize a program; promotion has already happened before this boundary.
struct FunctionKey {
  const stanli::mir::FunDef* definition = nullptr;
  std::vector<std::vector<int64_t>> dims;
  std::vector<std::vector<int>> integers;
  size_t fingerprint() const {
    size_t hash = std::hash<const stanli::mir::FunDef*>{}(definition);
    const auto mix = [&](size_t value) {
      hash ^= value + (size_t)0x9e3779b9 + (hash << 6) + (hash >> 2);
    };
    for (const auto& shape : dims) {
      mix(shape.size());
      for (auto extent : shape) mix(std::hash<int64_t>{}(extent));
    }
    for (const auto& values : integers) {
      mix(values.size());
      for (auto value : values) mix(std::hash<int>{}(value));
    }
    return hash;
  }
  bool operator==(const FunctionKey& other) const {
    return definition == other.definition && dims == other.dims &&
           integers == other.integers;
  }
};

struct FunctionPlan {
  stanli::Program program;
  std::vector<std::pair<int, int>> inputs;
  std::vector<int64_t> result_dims;
  bool integer_result = false;
  bool ok = false;
  std::string refusal;
  size_t storage = 0;
};

bool direct_function_view(const stanli::mir::UnsizedView& view) {
  using Leaf = stanli::mir::UnsizedLeaf;
  if (view.depth == 1) return view.leaf == Leaf::Real || view.leaf == Leaf::Int;
  if (view.depth != 0) return false;
  return view.leaf == Leaf::Real || view.leaf == Leaf::Int ||
         view.leaf == Leaf::Vector || view.leaf == Leaf::RowVector ||
         view.leaf == Leaf::Matrix;
}

void return_view(const std::vector<stanli::mir::Stmt>& body,
                 std::optional<stanli::mir::UnsizedView>& view) {
  using namespace stanli;
  for (const auto& statement : body) {
    if (statement.kind == mir::Stmt::Return) {
      const auto next = statement.rhs.unsized;
      if (!statement.has_init || !direct_function_view(next))
        throw Bail{
            "standalone return has unsupported or missing type metadata"};
      if (view && (view->depth != next.depth || view->leaf != next.leaf))
        throw Bail{"standalone return types disagree"};
      view = next;
    }
    return_view(statement.body, view);
  }
}

std::shared_ptr<const FunctionPlan> compile_function(
    const stanli::mir::FunDef& definition,
    const std::map<std::string, const stanli::mir::FunDef*>& functions,
    const std::vector<stanli::DataMap::Entry>& values) {
  using namespace stanli;
  auto out = std::make_shared<FunctionPlan>();
  try {
    std::optional<mir::UnsizedView> result_type;
    return_view(definition.body, result_type);
    if (!result_type) throw Bail{"standalone function has no typed return"};
    out->integer_result = result_type->leaf == mir::UnsizedLeaf::Int;
    std::set<const mir::FunDef*> checked_functions;
    check_program_integer_contract(definition.body, functions,
                                   checked_functions);
    // No interpreter, target, higher-order or external-folding hooks. Failed
    // compilation cannot execute user code or expose partial side effects.
    ProgramCompiler compiler{out->program, functions};
    std::vector<InlineArg> args;
    for (size_t k = 0; k < values.size(); ++k) {
      const auto& value = values[k];
      const auto view = definition.arg_views[k];
      if (!direct_function_view(view))
        throw Bail{"standalone argument requires a storage-order adapter"};
      InlineArg arg;
      if (view.leaf == mir::UnsizedLeaf::Int) {
        arg.is_const_int = true;
        arg.ints.assign(value.i.begin(), value.i.end());
        arg.int_dims = value.dims;
        out->inputs.emplace_back(-1, 0);
      } else {
        if (value.r.size() > (size_t)ProgramCompiler::kMaxRegs)
          throw Bail{"standalone argument exceeds the register limit"};
        auto& range = arg.real;
        range.len = (int)value.r.size();
        range.reg = compiler.alloc(range.len);
        if (view.depth == 1) {
          range.kind = ViewKind::Array;
          range.dims = value.dims;
        } else if (view.leaf == mir::UnsizedLeaf::Vector) {
          range.kind = ViewKind::Vector;
        } else if (view.leaf == mir::UnsizedLeaf::RowVector) {
          range.kind = ViewKind::RowVector;
        } else if (view.leaf == mir::UnsizedLeaf::Matrix) {
          range.kind = ViewKind::Matrix;
          range.rows = value.dims[0];
          range.cols = value.dims[1];
        }
        out->inputs.emplace_back(range.reg, range.len);
      }
      args.push_back(std::move(arg));
    }
    const Range result = compiler.inline_call(definition, args);
    if (!ProgramCompiler::unsized_accepts(*result_type, result))
      throw Bail{"standalone result disagrees with its typed logical view"};
    if (result.kind == ViewKind::Array)
      out->result_dims =
          result.dims.empty() ? std::vector<int64_t>{result.len} : result.dims;
    else if (result.kind == ViewKind::Matrix)
      out->result_dims = {result.rows, result.cols};
    else if (result.kind == ViewKind::Vector ||
             result.kind == ViewKind::RowVector)
      out->result_dims = {result.len};
    for (int i = 0; i < result.len; ++i)
      out->program.out_regs.push_back(result.reg + i);
    compiler.finish();
    compact_program(out->program, out->inputs);
    out->storage = sizeof(FunctionPlan) +
                   out->program.code.size() * sizeof(Program::Instr) +
                   out->program.pool.size() * sizeof(double) +
                   out->program.n_regs * sizeof(double) +
                   out->program.out_regs.size() * sizeof(int) +
                   out->program.calls.size() * sizeof(Program::Call);
    for (const auto& call : out->program.calls)
      out->storage += call.idata.size() * sizeof(int);
    out->ok = true;
  } catch (const Bail& failure) {
    // Nothing from a refused compilation survives in the cache.
    *out = FunctionPlan{};
    out->refusal = failure.why;
    out->storage = sizeof(FunctionPlan) + out->refusal.size();
  } catch (const std::bad_alloc&) {
    throw;  // Resource exhaustion is not a reusable semantic refusal.
  } catch (const std::exception& failure) {
    // Constant constructors and shape validators may report Stan domain
    // errors while compiling an arm that this call will never execute.
    // No hooks executed user code, so defer that error to the established
    // evaluator, at the original point in the function's control flow.
    *out = FunctionPlan{};
    out->refusal =
        "standalone compilation refused: " + std::string(failure.what());
    out->storage = sizeof(FunctionPlan) + out->refusal.size();
  }
  return out;
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
  std::set<const stanli::mir::FunDef*> real_returns;
  struct CachedPlan {
    FunctionKey key;
    std::shared_ptr<const FunctionPlan> plan;
    size_t storage;
  };
  mutable std::mutex cache_mutex;
  mutable std::vector<CachedPlan> cache;
  mutable size_t cache_storage = 0;
  mutable bool cache_saturated = false;
  mutable std::optional<size_t> pending_fingerprint;
};

namespace {
std::shared_ptr<const FunctionPlan> function_plan(
    const stanli_function& function, const stanli::mir::FunDef& definition,
    const std::vector<stanli::DataMap::Entry>& values) {
  // Bound retained programs and their potential per-call register buffers.
  // Kernel metadata/string overhead is additional; this is not a process RSS
  // cap. In-flight calls may retain an evicted immutable plan until completion.
  constexpr size_t capacity = 8;
  constexpr size_t storage_budget = 4 * 1024 * 1024;
  FunctionKey key;
  key.definition = &definition;
  size_t key_storage = sizeof(FunctionKey);
  for (const auto& value : values) {
    // DataMap::set_int accepts long and keeps that value in the real mirror,
    // even if narrowing to Stan's int changed the integer mirror. Preserve
    // that legacy API behavior without allowing two distinct mirrors to
    // collide on the integer-only specialization key.
    if (value.is_int)
      for (size_t i = 0; i < value.i.size(); ++i)
        if (value.r[i] != (double)value.i[i] ||
            (value.r[i] == 0 && std::signbit(value.r[i]))) {
          auto refused = std::make_shared<FunctionPlan>();
          refused->refusal =
              "standalone integer input mirrors require the MIR interpreter";
          return refused;
        }
    key_storage +=
        value.dims.size() * sizeof(int64_t) + value.i.size() * sizeof(int);
    if (key_storage > storage_budget) {
      auto refused = std::make_shared<FunctionPlan>();
      refused->refusal =
          "standalone specialization key exceeds the cache budget";
      return refused;
    }
    key.dims.push_back(value.dims);
    key.integers.push_back(value.i);
  }
  {
    std::lock_guard<std::mutex> lock(function.cache_mutex);
    for (const auto& cached : function.cache)
      if (cached.key == key) return cached.plan;
    if (function.cache.size() >= capacity || function.cache_saturated) {
      // One-off signatures must not turn bounded storage into unbounded
      // recompilation work. A stable new signature promotes on its second
      // miss. Fingerprint collisions only cause earlier compilation: actual
      // plan lookup always compares the complete key above.
      const size_t fingerprint = key.fingerprint();
      if (function.pending_fingerprint != fingerprint) {
        function.pending_fingerprint = fingerprint;
        auto refused = std::make_shared<FunctionPlan>();
        refused->refusal = "standalone cache awaiting a repeated signature";
        return refused;
      }
      function.pending_fingerprint.reset();
    }
  }
  auto plan = compile_function(definition, function.functions, values);
  if (plan->storage + key_storage > storage_budget) {
    auto refused = std::make_shared<FunctionPlan>();
    refused->refusal = "standalone compiled program exceeds the cache budget";
    refused->storage = sizeof(FunctionPlan) + refused->refusal.size();
    plan = std::move(refused);
  }
  const size_t storage = plan->storage + key_storage;
  std::lock_guard<std::mutex> lock(function.cache_mutex);
  for (const auto& cached : function.cache)
    if (cached.key == key) return cached.plan;
  while (!function.cache.empty() &&
         (function.cache.size() >= capacity ||
          function.cache_storage + storage > storage_budget)) {
    function.cache_saturated = true;
    function.cache_storage -= function.cache.front().storage;
    function.cache.erase(function.cache.begin());
  }
  function.cache.push_back({std::move(key), plan, storage});
  function.cache_storage += storage;
  return plan;
}

stanli::DataMap::Entry run_function(
    const FunctionPlan& plan,
    const std::vector<stanli::DataMap::Entry>& values) {
  std::vector<double> registers((size_t)plan.program.n_regs);
  for (size_t k = 0; k < values.size(); ++k) {
    const auto [reg, len] = plan.inputs[k];
    if (len > 0) std::copy_n(values[k].r.begin(), len, registers.begin() + reg);
  }
  stanli::run_program(plan.program, registers);
  stanli::DataMap::Entry result;
  result.dims = plan.result_dims;
  result.r.reserve(plan.program.out_regs.size());
  for (int reg : plan.program.out_regs)
    result.r.push_back(registers[(size_t)reg]);
  if (plan.integer_result) {
    result.is_int = true;
    result.i.reserve(result.r.size());
    for (double value : result.r) result.i.push_back(static_cast<int>(value));
  }
  return result;
}
}  // namespace

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
      if (named(f, function_name)) {
        out->candidates.push_back(&f);
        // The public result follows the typed return, just as promoted input
        // formals do. The interpreter can otherwise leave integer mirrors on
        // a real return, making its type depend on cache admission.
        try {
          std::optional<stanli::mir::UnsizedView> view;
          return_view(f.body, view);
          if (view && view->leaf != stanli::mir::UnsizedLeaf::Int)
            out->real_returns.insert(&f);
        } catch (const stanli::Bail&) {
        }
      }
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

    const auto plan = function_plan(*function, *selected, values);
    if (function->report_execution)
      stanli::report_execution_function(selected->name, plan->ok, plan->refusal,
                                        plan->program.n_regs);
    stanli::DataMap::Entry result;
    if (plan->ok) {
      // Execution errors propagate. Never repeat effects through a fallback.
      result = run_function(*plan, values);
    } else {
      stanli::MirInterp<double> interpreter(
          function->functions, "function " + function->requested_name);
      result = interpreter.call(*selected, values);
    }
    if (function->real_returns.count(selected)) {
      result.is_int = false;
      result.i.clear();
    }
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
