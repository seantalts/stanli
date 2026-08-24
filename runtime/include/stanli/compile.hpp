// The graph compiler: transformed-MIR sexp text -> executable graph, sized
// against a concrete dataset. Scope: straight-line log_prob; unsupported
// constructs raise CompileError naming the construct.
//
// Generated lower/upper checks keep their source phase: prepare_data checks
// run at construction, while log_prob/write_array checks stay ordered graph
// effects. Structural value checks remain an explicit compatibility seam.
#ifndef STANLI_COMPILE_HPP
#define STANLI_COMPILE_HPP

#include <stanli/data.hpp>
#include <stanli/graph.hpp>
#include <stanli/mir.hpp>

#include <optional>
#include <map>
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
    // A declaration-level view carries its full logical shape. Stan writes
    // the first logical index fastest, while the arena keeps arrays
    // outer-major and an innermost matrix column-major. Write-array views are
    // already peeled to one emitted value and leave this empty.
    std::vector<int64_t> dims;
    bool matrix_storage = false;  // the final two dims form a matrix
    // Empty means identity. Nonidentity permutations are prepared once while
    // compiling the concrete model, so serialization performs one indexed
    // copy per value with no per-draw division or allocation.
    std::vector<int64_t> storage_order;

    void set_serial_layout(std::vector<int64_t> logical_dims,
                           bool innermost_matrix) {
      dims = std::move(logical_dims);
      matrix_storage = innermost_matrix;
      storage_order.resize((size_t)len);
      bool identity = true;
      for (int64_t serial = 0; serial < len; ++serial) {
        int64_t q = serial;
        const size_t outer = matrix_storage ? dims.size() - 2 : dims.size();
        int64_t stride =
            matrix_storage ? len / (dims[outer] * dims[outer + 1]) : len;
        int64_t at = 0;
        for (size_t d = 0; d < outer; ++d) {
          stride /= dims[d];
          at += (q % dims[d]) * stride;
          q /= dims[d];
        }
        if (matrix_storage) at = at * dims[outer] * dims[outer + 1] + q;
        storage_order[(size_t)serial] = at;
        identity &= at == serial;
      }
      if (identity) storage_order.clear();
    }

    int64_t storage_index(int64_t serial) const {
      return storage_order.empty() ? serial : storage_order[(size_t)serial];
    }

    void append_names(std::vector<std::string>& out) const {
      if (!dims.empty()) {
        for (int64_t f = 0; f < len; ++f) {
          std::string column = name;
          int64_t q = f;
          for (int64_t d : dims) {
            column += "." + std::to_string(q % d + 1);
            q /= d;
          }
          out.push_back(std::move(column));
        }
      } else if (naming == Naming::Matrix && rows > 0) {
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

  // Materialize constrained declarations in the MIR interpreter's logical
  // first-index-fast layout. Hosts must use this boundary instead of handing
  // arena storage directly to WaInterp: arrays use different physical order
  // on the two sides.
  std::map<std::string, DataMap::Entry> constrained_env(Executor& ex) const {
    std::map<std::string, DataMap::Entry> env;
    for (const auto& view : views) {
      DataMap::Entry value;
      value.dims = view.dims;
      value.r.resize((size_t)view.len);
      const double* stored = ex.value_ptr(view.slot);
      for (int64_t i = 0; i < view.len; ++i)
        value.r[(size_t)i] = stored[view.storage_index(i)];
      env.emplace(view.name, std::move(value));
    }
    return env;
  }

  int64_t n_unconstrained = 0;
  // Each declared parameter, in declaration order: what it is called, how
  // many UNCONSTRAINED values it contributes, its logical free dimensions,
  // and which constraint transform produced it. The unconstrained vector is
  // these lengths concatenated in declaration order.
  struct UncParam {
    std::string name;
    int64_t len = 0;  // unconstrained values
    // Exact public naming shape of the free values. Structured matrix leaves
    // are flat here; sum-to-zero matrices retain their two free dimensions.
    // Outer array dimensions are always preserved.
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
    // Non-empty when lowering stopped early (an unsupported RNG family or
    // draw-dependent branch, say). `columns` then holds the prefix that did
    // lower, and this says what stopped it -- silently short CSV rows would
    // be worse.
    std::string truncated;
    // Set alongside `truncated`: the per-draw interpreter that runs the
    // whole section, unsupported RNG draws and draw-dependent branches
    // included.
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
