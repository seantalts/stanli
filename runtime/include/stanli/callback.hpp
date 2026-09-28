// Shared state and argument packing for user callbacks retained by runtime
// algorithms (ODEs, algebra solvers, quadrature, and future map/DAE kernels).
#ifndef STANLI_CALLBACK_HPP
#define STANLI_CALLBACK_HPP

#include <stanli/mir.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/container_shape.hpp>

#include <map>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace stanli {

inline std::vector<int64_t> callback_matrix_dimensions(const RhsArg& arg) {
  if (arg.is_int || arg.rows < 0 || arg.cols < 0)
    throw std::invalid_argument(
        "matrix callback argument has no logical dimensions");
  const std::vector<int64_t> dims{arg.rows, arg.cols};
  if (arg.rows > std::numeric_limits<int>::max() ||
      arg.cols > std::numeric_limits<int>::max() ||
      checked_container_size(dims, "matrix callback argument") != arg.len)
    throw std::invalid_argument(
        "matrix callback argument dimensions do not match its values");
  return dims;
}

template <typename T>
class MirInterp;

// Plain matrices have the same column-major order in both engines, but their
// dimensions must survive the retained-call boundary. Keep the established
// positional path for callbacks without matrices; nested arrays need a
// separate, explicit storage adapter before they can join this path.
template <typename T>
std::vector<T> interpret_retained_callback(
    MirInterp<T>& interpreter, const mir::FunDef& function,
    const std::vector<std::vector<T>>& reals,
    const std::vector<std::vector<int>>& ints,
    const std::vector<RhsArg>& bindings) {
  bool matrix = false;
  for (const auto& view : function.arg_views)
    matrix |= view.depth == 0 && view.leaf == mir::UnsizedLeaf::Matrix;
  if (!matrix) return interpreter.call(function, reals, ints);
  if (function.arg_views.size() != function.arg_names.size() ||
      bindings.size() > function.arg_names.size())
    throw std::invalid_argument("callback argument metadata is incomplete");
  const size_t prefix = function.arg_names.size() - bindings.size();
  std::vector<typename MirInterp<T>::Value> values;
  values.reserve(function.arg_names.size());
  size_t ri = 0, ii = 0;
  for (size_t k = 0; k < function.arg_names.size(); ++k) {
    const auto& view = function.arg_views[k];
    if (view.depth > 1 ||
        (view.depth == 1 && view.leaf != mir::UnsizedLeaf::Real &&
         view.leaf != mir::UnsizedLeaf::Int))
      throw std::invalid_argument(
          "matrix callback has an unsupported array view");
    typename MirInterp<T>::Value value;
    value.is_int = view.leaf == mir::UnsizedLeaf::Int;
    if (value.is_int) {
      if (ii >= ints.size())
        throw std::invalid_argument("missing callback integer argument");
      value.i = ints[ii++];
      value.r.assign(value.i.begin(), value.i.end());
    } else {
      if (ri >= reals.size())
        throw std::invalid_argument("missing callback real argument");
      value.r = reals[ri++];
    }
    if (view.depth == 0 && view.leaf == mir::UnsizedLeaf::Matrix) {
      if (k < prefix)
        throw std::invalid_argument(
            "matrix callback state has no logical dimensions");
      value.dims = callback_matrix_dimensions(bindings[k - prefix]);
      if (value.r.size() != static_cast<size_t>(bindings[k - prefix].len))
        throw std::invalid_argument("matrix callback value count mismatch");
    } else if (view.depth != 0 || (view.leaf != mir::UnsizedLeaf::Real &&
                                   view.leaf != mir::UnsizedLeaf::Int)) {
      value.dims = {static_cast<int64_t>(value.r.size())};
    }
    values.push_back(std::move(value));
  }
  if (ri != reals.size() || ii != ints.size())
    throw std::invalid_argument("unexpected callback arguments");
  return interpreter.call(function, values).r;
}

struct RetainedCallback {
  std::map<std::string, mir::FunDef> owned;
  std::map<std::string, const mir::FunDef*> funs_map;
  std::string callback_name;

  // Real callback arguments are packed in source order: runtime values into
  // the kernel's theta input and preparation constants into x_r. Scalar AD
  // activity is selected separately by the kernel variant. Integer values
  // remain in each RhsArg. The register compiler uses the same bindings to
  // reconstruct the callback's original positional signature.
  std::vector<RhsArg> args;
  std::vector<double> x_r;
  std::vector<int> x_i;
  RhsProgram prog;

  void adopt(const std::map<std::string, const mir::FunDef*>& src) {
    for (const auto& [name, def] : src) owned[name] = *def;
    for (const auto& [name, def] : owned) funs_map[name] = &def;
  }
  const mir::FunDef* callback() const {
    auto it = owned.find(callback_name);
    return it == owned.end() ? nullptr : &it->second;
  }
  const mir::FunDef* callback(const std::string& name) const {
    auto it = owned.find(name);
    return it == owned.end() ? nullptr : &it->second;
  }
  const std::map<std::string, const mir::FunDef*>* funs() const {
    return &funs_map;
  }
};

// Backend-neutral classification and packing of retained callback arguments.
// A backend supplies only value acquisition: graph lowering returns Val
// parts, Program lowering returns Range parts, and both receive identical
// RhsArg/x_r ordering and validation.
template <typename Active, typename GetActive, typename GetReals,
          typename GetInts, typename Fail>
std::vector<Active> pack_callback_arguments(
    RetainedCallback& retained, const std::vector<mir::Expr>& exprs,
    size_t begin, size_t end, GetActive&& get_active, GetReals&& get_reals,
    GetInts&& get_ints, Fail&& fail, bool runtime_reals = false) {
  std::vector<Active> active;
  for (size_t i = begin; i < end; ++i) {
    const mir::Expr& arg = exprs[i];
    RhsArg binding;
    if (arg.unsized.leaf == mir::UnsizedLeaf::Int) {
      if (!arg.data_only) {
        fail("integer callback argument " + std::to_string(i - begin + 1) +
             " is not data");
        continue;
      }
      binding.is_int = true;
      binding.ints = get_ints(i);
    } else if (arg.data_only && !runtime_reals) {
      std::vector<double> values = get_reals(i, binding);
      if (values.size() >
          static_cast<size_t>(std::numeric_limits<int>::max())) {
        fail("callback argument " + std::to_string(i - begin + 1) +
             " is too large");
        continue;
      }
      binding.len = static_cast<int>(values.size());
      retained.x_r.insert(retained.x_r.end(), values.begin(), values.end());
    } else {
      auto value = get_active(i, binding);
      binding.is_param = true;
      binding.len = value.second;
      active.push_back(std::move(value.first));
    }
    retained.args.push_back(std::move(binding));
  }
  return active;
}

}  // namespace stanli

#endif
