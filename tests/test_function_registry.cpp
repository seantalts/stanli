#include "expect.hpp"

#include <stanli/builtin_registry.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/function_registry.hpp>
#include <stanli/mir.hpp>

#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>

// BuiltinSpec metadata, BuiltinLayout lanes, and size-mismatch messages.
static void check_builtin_layouts() {
  using namespace stanli;
  const auto shape =
      [](BuiltinArgumentKind value, BuiltinContainerKind container,
         std::vector<int64_t> dimensions, int64_t size,
         BuiltinContainerKind leaf = BuiltinContainerKind::Scalar) {
        return BuiltinArgumentShape{value, container, leaf,
                                    std::move(dimensions), size};
      };
  const BuiltinSpec* choose_spec = builtin_spec("choose", 2);
  if (choose_spec == nullptr || choose_spec->opcode != OP_CHOOSE ||
      choose_spec->arguments[0] != BuiltinArgumentKind::Integer ||
      choose_spec->arguments[1] != BuiltinArgumentKind::Integer ||
      choose_spec->result != FunctionArgumentKind::Integer ||
      choose_spec->activity_mask != 0) {
    ++failures;
    std::printf("FAIL choose BuiltinSpec metadata\n");
  }
  {
    const BuiltinSpec* atan2_spec = builtin_spec("atan2", 2);
    const BuiltinLayout layout = builtin_layout(
        *atan2_spec,
        {shape(BuiltinArgumentKind::Real, BuiltinContainerKind::Scalar, {}, 1),
         shape(BuiltinArgumentKind::Real, BuiltinContainerKind::Matrix, {2, 3},
               6)},
        "atan2", false);
    if (layout.lanes != 6 || layout.result_argument != 1) {
      ++failures;
      std::printf("FAIL scalar/matrix BuiltinLayout\n");
    }
    std::string rejected;
    try {
      (void)builtin_layout(*atan2_spec,
                           {shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::Vector, {3}, 3),
                            shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::Vector, {4}, 4)},
                           "atan2", false);
    } catch (const std::invalid_argument& error) {
      rejected = builtin_error_message("atan2", error);
    }
    if (rejected !=
        "Binary function: x (3, 1) and y (4, 1) must match in size") {
      ++failures;
      std::printf("FAIL logical-shape mismatch message: %s\n",
                  rejected.c_str());
    }
    rejected.clear();
    try {
      (void)builtin_layout(*builtin_spec("Plus__", 2),
                           {shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::RowVector, {3}, 3),
                            shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::RowVector, {4}, 4)},
                           "Plus__", false);
    } catch (const std::invalid_argument& error) {
      rejected = builtin_error_message("Plus__", error);
    }
    if (rejected != "add: m1 (1, 3) and m2 (1, 4) must match in size") {
      ++failures;
      std::printf("FAIL operator size mismatch message: %s\n",
                  rejected.c_str());
    }
    rejected.clear();
    try {
      (void)builtin_layout(*builtin_spec("Plus__", 2),
                           {shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::RowVector, {3}, 3),
                            shape(BuiltinArgumentKind::Real,
                                  BuiltinContainerKind::RowVector, {4}, 4)},
                           "Plus__", true);
    } catch (const std::invalid_argument& error) {
      rejected = builtin_error_message("Plus__", error);
    }
    if (rejected != "add: a (1, 3) and b (1, 4) must match in size") {
      ++failures;
      std::printf("FAIL var operator size mismatch message: %s\n",
                  rejected.c_str());
    }
  }
  {
    const BuiltinSpec* bessel = builtin_spec("bessel_first_kind", 2);
    const BuiltinLayout layout =
        builtin_layout(*bessel,
                       {shape(BuiltinArgumentKind::Integer,
                              BuiltinContainerKind::Array, {2, 3}, 6),
                        shape(BuiltinArgumentKind::Real,
                              BuiltinContainerKind::Matrix, {2, 3}, 6)},
                       "bessel_first_kind", false);
    if (layout.lanes != 6 || layout.result_argument != 1 ||
        layout.integer_matrix_rows != 2 || layout.integer_matrix_cols != 3) {
      ++failures;
      std::printf("FAIL mixed matrix/array BuiltinLayout\n");
    }
  }
  {
    const BuiltinSpec* softmax = builtin_spec("softmax", 1);
    const BuiltinLayout layout =
        builtin_layout(*softmax,
                       {shape(BuiltinArgumentKind::Real,
                              BuiltinContainerKind::Vector, {4}, 4)},
                       "softmax", false);
    if (layout.lanes != 4) {
      ++failures;
      std::printf("FAIL whole-value BuiltinLayout\n");
    }
  }
}

// The plan choke point must refuse parameter-dependent input outside a
// fixed activity mask rather than let a kernel drop its partial silently.
// Every backend builds its plan here, so this one guard covers them all.
static void check_density_activity_guard() {
  using namespace stanli;
  DensitySpec spec;
  spec.opcode = OP_LKJ_CORR_LPDF;
  spec.arity = 2;
  spec.integer_args = 0;
  spec.shape = DensityShape::FirstMatrixRows;
  spec.activity_mask = 0x1;
  std::vector<DensityCallArgument> args(2);
  args[0].shape.container = FunctionContainerKind::Matrix;
  args[0].shape.dimensions = {2, 2};
  args[0].shape.storage_size = 4;
  args[0].data_only = false;
  args[0].active = true;
  args[1].shape.container = FunctionContainerKind::Scalar;
  args[1].scalar = true;
  args[1].data_only = false;
  args[1].active = true;  // outside the 0x1 mask: must refuse
  bool threw = false;
  try {
    density_call_plan(spec, args, false);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  if (!threw) {
    ++failures;
    std::printf("FAIL activity guard: out-of-mask activity was planned\n");
  }
  args[1].data_only = true;
  args[1].active = false;  // within the mask: must still plan
  try {
    density_call_plan(spec, args, false);
  } catch (const std::exception& error) {
    ++failures;
    std::printf("FAIL activity guard: in-mask plan threw: %s\n", error.what());
  }
}

int main() {
  using namespace stanli;
  std::set<std::string> keys;
  std::set<const FunctionSpec*> canonical_entries;
  size_t builtin_count = 0;
  size_t density_count = 0;
  for (const FunctionSpec& spec : function_specs()) {
    canonical_entries.insert(&spec);
    uint64_t integer_arguments = 0;
    std::string key = std::string(spec.name) + "/";
    for (size_t k = 0; k < spec.arity(); ++k) {
      const FunctionArgumentKind kind = spec.argument_kind(k);
      key += kind == FunctionArgumentKind::Integer ? "i" : "r";
      if (kind == FunctionArgumentKind::Integer)
        integer_arguments |= uint64_t{1} << k;
    }
    key += spec.result() == FunctionArgumentKind::Integer ? "/i" : "/r";
    if (!keys.insert(key).second) {
      std::printf("FAIL duplicate function registry entry %s\n", key.c_str());
      return 1;
    }
    if (function_spec(spec.name, spec.arity(), integer_arguments,
                      spec.result()) != &spec) {
      std::printf("FAIL lookup identity %s\n", key.c_str());
      return 1;
    }
    for (size_t k = 0; k < spec.arity(); ++k) (void)spec.argument_kind(k);
    if (spec.family() == FunctionFamily::Builtin) {
      ++builtin_count;
      if (spec.builtin() == nullptr || spec.density() != nullptr) return 1;
      const BuiltinSpec& builtin = *spec.builtin();
      if (spec.opcode() != builtin.opcode || spec.arity() != builtin.arity ||
          spec.activity_mask() != builtin.activity_mask ||
          spec.result() != builtin.result)
        return 1;
    } else {
      ++density_count;
      if (spec.builtin() != nullptr || spec.density() == nullptr) return 1;
      const DensitySpec& density = *spec.density();
      if (spec.opcode() != density.opcode ||
          spec.arity() != static_cast<size_t>(density.arity) ||
          spec.activity_mask() != density.activity_mask ||
          spec.result() != FunctionArgumentKind::Real)
        return 1;
    }
  }
  if (builtin_count != builtin_specs().size() ||
      density_count != density_specs().size() || builtin_count == 0 ||
      density_count == 0) {
    std::printf("FAIL registry family counts\n");
    return 1;
  }
  std::set<const FunctionSpec*> family_entries;
  for (const FunctionSpec* spec : builtin_specs()) {
    if (spec == nullptr || spec->family() != FunctionFamily::Builtin ||
        spec->builtin() == nullptr || !family_entries.insert(spec).second)
      return 1;
  }
  for (const FunctionSpec* spec : density_specs()) {
    if (spec == nullptr || spec->family() != FunctionFamily::Density ||
        spec->density() == nullptr || !family_entries.insert(spec).second)
      return 1;
  }
  if (family_entries != canonical_entries) {
    std::printf("FAIL family views do not reference canonical storage\n");
    return 1;
  }
  const FunctionSpec* softmax =
      function_spec("softmax", 1, 0, FunctionArgumentKind::Real);
  const FunctionSpec* normal =
      function_spec("normal_lpdf", 3, 0, FunctionArgumentKind::Real);
  if (softmax == nullptr || softmax->family() != FunctionFamily::Builtin ||
      normal == nullptr || normal->family() != FunctionFamily::Density ||
      function_spec("normal_lpdf", 2, 0, FunctionArgumentKind::Real) !=
          nullptr) {
    std::printf("FAIL representative unified lookup\n");
    return 1;
  }
  const FunctionSpec* categorical_name =
      function_spec("categorical_lpmf", 2, 1, FunctionArgumentKind::Real);
  const FunctionSpec* categorical_logit =
      function_spec("categorical_logit_lpmf", 2, 1, FunctionArgumentKind::Real);
  if (!function_registered("abs") || !function_arity_registered("abs", 1) ||
      function_arity_registered("abs", 2) ||
      function_spec("abs", 1, FunctionFamily::Builtin) == nullptr ||
      function_spec("normal_lpdf", FunctionFamily::Density) == nullptr ||
      categorical_name == nullptr || categorical_logit == nullptr ||
      categorical_name == categorical_logit ||
      categorical_name->opcode() != categorical_logit->opcode()) {
    std::printf("FAIL indexed compatibility lookup\n");
    return 1;
  }
  if (function_spec("abs", 1, 1, FunctionArgumentKind::Integer) == nullptr ||
      function_spec("abs", 1, 1, FunctionArgumentKind::Real)->result() !=
          FunctionArgumentKind::Integer ||
      function_spec("choose", 2, 0, FunctionArgumentKind::Integer) != nullptr ||
      function_spec("choose", 2, 3, FunctionArgumentKind::Integer) == nullptr ||
      function_spec("normal_lpdf", 3, 1, FunctionArgumentKind::Real) ==
          nullptr) {
    std::printf("FAIL overload kind matching\n");
    return 1;
  }
  mir::Expr untyped_choose;
  untyped_choose.name = "choose";
  untyped_choose.args.resize(2);
  untyped_choose.args[0].type_ = "UInt";
  untyped_choose.args[1].type_ = "UInt";
  if (function_spec(untyped_choose) == nullptr) {
    std::printf("FAIL missing result metadata lookup\n");
    return 1;
  }
  mir::Expr complex_abs;
  complex_abs.name = "abs";
  complex_abs.unsized.leaf = mir::UnsizedLeaf::Real;
  complex_abs.args.resize(1);
  complex_abs.args[0].unsized.leaf = mir::UnsizedLeaf::Complex;
  if (function_spec(complex_abs) != nullptr) {
    std::printf("FAIL complex overload matched numeric registry\n");
    return 1;
  }
  // stanc synthesizes truncation normalizers with UInt metadata even on
  // real-valued CDFs and log_diff_exp. Their surrounding unary minus must
  // resolve from the registered result, not truncate the normalizer to int.
  const auto call = [](std::string name, std::vector<mir::Expr> args) {
    mir::Expr e;
    e.kind = mir::Expr::FunApp;
    e.name = std::move(name);
    e.args = std::move(args);
    e.type_ = "UInt";
    e.unsized.leaf = mir::UnsizedLeaf::Int;
    return e;
  };
  mir::Expr real;
  real.kind = mir::Expr::Var;
  real.type_ = "UReal";
  real.unsized.leaf = mir::UnsizedLeaf::Real;
  const auto cdf = call("normal_lcdf", {real, real, real});
  const auto normalizer = call("log_diff_exp", {cdf, cdf});
  const auto negated = call("PMinus__", {normalizer});
  if (function_spec(negated)->result() != FunctionArgumentKind::Real ||
      function_spec(call("PMinus__", {negated}))->result() !=
          FunctionArgumentKind::Real) {
    std::printf("FAIL synthesized real normalizer resolved as integer\n");
    return 1;
  }
  mir::Expr integer;
  integer.kind = mir::Expr::LitInt;
  integer.type_ = "UInt";
  integer.unsized.leaf = mir::UnsizedLeaf::Int;
  const auto integer_negated = call("PMinus__", {integer});
  if (function_spec(call("PMinus__", {integer_negated}))->result() !=
      FunctionArgumentKind::Integer) {
    std::printf("FAIL nested integer negation lost its integer overload\n");
    return 1;
  }
  auto integer_array = integer;
  integer_array.kind = mir::Expr::Var;
  integer_array.unsized.depth = 1;
  // Integer sum is lowered separately and has no integer registry entry.
  // Its real registry overload must not override valid integer metadata.
  if (function_spec(call("PMinus__", {call("sum", {integer_array})}))
          ->result() != FunctionArgumentKind::Integer) {
    std::printf("FAIL unregistered integer reduction became real\n");
    return 1;
  }
  auto promoted = integer_negated;
  promoted.promoted = true;
  promoted.type_ = "UReal";
  promoted.unsized.leaf = mir::UnsizedLeaf::Real;
  auto user_call = normalizer;
  user_call.fn_lib = mir::Expr::Lib::UserDefined;
  if (function_spec(call("PMinus__", {promoted}))->result() !=
          FunctionArgumentKind::Real ||
      function_spec(call("PMinus__", {user_call}))->result() !=
          FunctionArgumentKind::Integer) {
    std::printf("FAIL promotion or user-function result metadata ignored\n");
    return 1;
  }
  const auto scalar_shape = [](FunctionArgumentKind kind) {
    return make_function_shape(kind, FunctionContainerKind::Scalar,
                               FunctionContainerKind::Scalar, {}, 1);
  };
  const FunctionSpec* bernoulli = function_spec(
      "bernoulli_lpmf", 2, uint64_t{1}, FunctionArgumentKind::Real);
  if (bernoulli == nullptr || bernoulli->density() == nullptr) return 1;
  const DensityCallPlan bernoulli_plan = density_call_plan(
      *bernoulli->density(),
      {{scalar_shape(FunctionArgumentKind::Integer), {1}, true, true, false},
       {make_function_shape(FunctionArgumentKind::Real,
                            FunctionContainerKind::Vector,
                            FunctionContainerKind::Scalar, {3}, 3),
        {},
        false,
        false,
        true}},
      true);
  if (bernoulli_plan.idata != std::vector<int>({1, 1, 1}) ||
      bernoulli_plan.activity_mask != 1 ||
      bernoulli_plan.variant != uint8_t{0x81}) {
    std::printf("FAIL shared density scalar-outcome plan\n");
    return 1;
  }
  const FunctionSpec* categorical = function_spec(
      "categorical_lpmf", 2, uint64_t{1}, FunctionArgumentKind::Real);
  if (categorical == nullptr || categorical->density() == nullptr) return 1;
  const DensityCallPlan categorical_plan = density_call_plan(
      *categorical->density(),
      {{scalar_shape(FunctionArgumentKind::Integer), {}, true, true, false},
       {make_function_shape(FunctionArgumentKind::Real,
                            FunctionContainerKind::Vector,
                            FunctionContainerKind::Scalar, {2}, 2),
        {},
        false,
        false,
        true}},
      false);
  if (categorical_plan.variant !=
          uint8_t{kCategoricalArgAutodiff | kCategoricalScalarOutcome} ||
      categorical_plan.activity_mask != 2) {
    std::printf("FAIL shared categorical density plan\n");
    return 1;
  }
  bool refused = false;
  try {
    (void)evaluate_integer_binary_builtin(OP_DIV, 1, 0);
  } catch (const std::domain_error&) {
    refused = true;
  }
  if (!refused) {
    std::printf("FAIL integer division by zero was not refused\n");
    return 1;
  }
  const FunctionSpec* head_ints =
      function_spec("head", 2, uint64_t{3}, FunctionArgumentKind::Integer);
  if (head_ints == nullptr ||
      head_ints->argument_kind(0) != FunctionArgumentKind::Integer) {
    std::printf("FAIL exact integer overload lost to a promoted one\n");
    return 1;
  }
  check_builtin_layouts();
  check_density_activity_guard();
  return finish("test_function_registry");
}
