// The graph compiler: transformed-MIR sexp text -> executable graph, sized
// against a concrete dataset. Scope: straight-line log_prob; unsupported
// constructs raise CompileError naming the construct.
//
// Deliberate simplification: FnCheck data validations are skipped (sizes
// are still enforced when binding data slots); the data is assumed valid.
#ifndef STANLI_COMPILE_HPP
#define STANLI_COMPILE_HPP

#include <stanli/data.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <memory>

namespace stanli {

class WaInterp;

struct CompileError : std::runtime_error {
  explicit CompileError(const std::string& what) : std::runtime_error(what) {}
};

struct CompiledModel {
  Graph graph;
  std::vector<std::string> param_names;  // declaration order (flat)
  // Constrained value of each parameter, readable from the executor after a
  // forward pass (slot of the post-transform value).
  struct ParamView {
    // How CmdStan spells this variable's CSV columns. A scalar is written
    // bare; a container is always indexed, even at length one (`vector[1] v`
    // is v.1, not v); a matrix carries row and column indices in the
    // column-major order its storage already has -- m.1.1, m.2.1, m.1.2.
    enum class Naming { Auto, Scalar, Container, Matrix };

    std::string name;
    int slot;
    int64_t len;
    Naming naming = Naming::Auto;  // Auto: bare at length 1, indexed above
    int64_t rows = 0;              // Matrix only

    void append_names(std::vector<std::string>& out) const {
      if (naming == Naming::Matrix && rows > 0) {
        for (int64_t f = 0; f < len; ++f)
          out.push_back(name + "." + std::to_string(f % rows + 1) + "." +
                        std::to_string(f / rows + 1));
      } else if (naming == Naming::Scalar ||
                 (naming == Naming::Auto && len == 1)) {
        out.push_back(name);
      } else {
        for (int64_t i = 0; i < len; ++i)
          out.push_back(name + "." + std::to_string(i + 1));
      }
    }
  };

  static std::vector<std::string> csv_names(
      const std::vector<ParamView>& cols) {
    std::vector<std::string> out;
    for (const auto& c : cols) c.append_names(out);
    return out;
  }
  std::vector<ParamView> views;
  int64_t n_unconstrained = 0;
  // Each declared parameter, in declaration order: what it is called, how
  // many UNCONSTRAINED values it contributes, its declared dimensions, and
  // which constraint transform produced it. The unconstrained vector is
  // these lengths concatenated in this order.
  struct UncParam {
    std::string name;
    int64_t len = 0;  // unconstrained values
    // The dimensions as declared, which are not always the constrained
    // shape: cholesky_factor_corr[K] declares K and holds K x K.
    std::vector<int64_t> dims;
    mir::Transform::Kind transform = mir::Transform::Identity;
  };
  std::vector<UncParam> unc_params;
  // Slot fills for data + constants, applied after Executor construction.
  std::vector<std::pair<int, std::vector<double>>> fills;

  void bind(Executor& ex) const {
    for (const auto& f : fills) {
      double* p = ex.value_ptr(f.first);
      for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
    }
  }

  // CmdStan's write_array: a second, forward-only graph over the same
  // unconstrained draw that produces every CSV column -- constrained
  // parameters, transformed parameters, generated quantities -- in CmdStan's
  // column order. The log_prob graph deliberately does not compute these:
  // nothing in the target depends on a transformed parameter that the target
  // does not already read, and generated quantities never reach it at all.
  struct WriteArray {
    Graph graph;
    std::vector<ParamView> columns;  // CSV order
    // Index into `columns` where each section starts. Everything before
    // n_tp_start is a constrained parameter, [n_tp_start, n_gq_start) is a
    // transformed parameter, the rest is a generated quantity.
    size_t n_tp_start = 0;
    size_t n_gq_start = 0;
    std::vector<std::pair<int, std::vector<double>>> fills;
    int64_t n_unconstrained = 0;  // must agree with the log_prob graph's
    // Non-empty when lowering stopped early (an RNG in generated quantities,
    // say). `columns` then holds the prefix that did lower, and this says
    // what stopped it -- silently short CSV rows would be worse.
    std::string truncated;
    // Set alongside `truncated`: the per-draw interpreter that runs the
    // whole section, RNG draws and draw-dependent branches included.
    // Drivers prefer it over the truncated graph (see wa_interp.hpp);
    // the graph remains the fast path whenever lowering completes.
    std::shared_ptr<WaInterp> interp;

    void bind(Executor& ex) const {
      for (const auto& f : fills) {
        double* p = ex.value_ptr(f.first);
        for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
      }
    }
  };
  std::optional<WriteArray> write_array;
};

CompiledModel compile_model(const std::string& tmir_text, const DataMap& data);

}  // namespace stanli

#endif
