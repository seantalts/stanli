#include <stanli/function_registry.hpp>

#include <iostream>

namespace {

const char* kind(stanli::FunctionArgumentKind value) {
  return value == stanli::FunctionArgumentKind::Integer ? "int" : "real";
}

const char* evaluation(const stanli::FunctionSpec& spec) {
  const stanli::DensitySpec* density = spec.density();
  return density == nullptr || density->evaluation ==
                                   stanli::DensityEvaluationPolicy::GraphKernel
             ? "graph_kernel"
             : "special";
}

const char* layout(const stanli::FunctionSpec& spec) {
  if (const stanli::BuiltinSpec* builtin = spec.builtin()) {
    switch (builtin->shape) {
      case stanli::BuiltinShapePolicy::WholeValue:
        return "whole_value";
      case stanli::BuiltinShapePolicy::Reduction:
        return "reduction";
      case stanli::BuiltinShapePolicy::PairedReduction:
        return "paired_reduction";
      case stanli::BuiltinShapePolicy::Constructor:
        return "constructor";
      case stanli::BuiltinShapePolicy::SliceView:
        return "slice_view";
      case stanli::BuiltinShapePolicy::GroupedReduction:
        return "grouped_reduction";
      case stanli::BuiltinShapePolicy::MatrixOp:
        return "matrix_op";
      case stanli::BuiltinShapePolicy::ShapeQuery:
        return "shape_query";
      case stanli::BuiltinShapePolicy::Predicate:
        return "predicate";
      case stanli::BuiltinShapePolicy::Rng:
        return "rng";
      case stanli::BuiltinShapePolicy::Product:
        return "product";
      case stanli::BuiltinShapePolicy::Solve:
        return "solve";
      case stanli::BuiltinShapePolicy::Elementwise:
        return "elementwise";
    }
    return "elementwise";
  }
  const stanli::DensitySpec& density = *spec.density();
  if (density.glm_layout) return "density_glm";
  switch (density.shape) {
    case stanli::DensityShape::Plain:
      return "density_plain";
    case stanli::DensityShape::Categorical:
      return "density_categorical";
    case stanli::DensityShape::FirstMatrixRows:
      return "density_first_matrix_rows";
    case stanli::DensityShape::FirstMatrixDimensions:
      return "density_first_matrix_dimensions";
    case stanli::DensityShape::VectorizedVectors:
      return "density_vectorized_vectors";
    case stanli::DensityShape::LastMatrixRowsAndRepetitions:
      return "density_last_matrix_rows_and_repetitions";
  }
  return "density_plain";
}

}  // namespace

int main() {
  std::cout << "[\n";
  const auto& specs = stanli::function_specs();
  for (size_t i = 0; i < specs.size(); ++i) {
    const stanli::FunctionSpec& spec = specs[i];
    std::cout << "  {\"name\":\"" << spec.name << "\",\"family\":\""
              << (spec.family() == stanli::FunctionFamily::Builtin ? "builtin"
                                                                   : "density")
              << "\",\"opcode\":" << spec.opcode()
              << ",\"arity\":" << static_cast<unsigned>(spec.arity())
              << ",\"arguments\":[";
    for (size_t k = 0; k < spec.arity(); ++k) {
      if (k != 0) std::cout << ',';
      std::cout << '\"' << kind(spec.argument_kind(k)) << '\"';
    }
    std::cout << "],\"result\":\"" << kind(spec.result())
              << "\",\"activity_mask\":" << spec.activity_mask()
              << ",\"evaluation\":\"" << evaluation(spec) << "\",\"layout\":\""
              << layout(spec) << "\"}"
              << (i + 1 == specs.size() ? "\n" : ",\n");
  }
  std::cout << "]\n";
}
