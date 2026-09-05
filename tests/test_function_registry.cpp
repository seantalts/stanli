#include <stanli/function_registry.hpp>
#include <stanli/mir.hpp>

#include <cstdio>
#include <set>
#include <string>

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
  std::printf("OK\n");
}
