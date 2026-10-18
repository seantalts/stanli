#include <stanli/init_interp.hpp>

#include <stanli/compile.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/unconstrain.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace stanli {
namespace {

[[noreturn]] void refuse(const std::string& what) {
  throw CompileError("stanli init: " + what);
}

int64_t product(const std::vector<int64_t>& dims) {
  int64_t n = 1;
  for (const int64_t d : dims) n *= d;
  return n;
}

// The two layouts a parameter has, and the only place stanli has to convert
// between them.
//
// SERIAL is what a user's init, a JSON file, and every CSV column use: the
// first logical index fastest. ARENA is what the free vector uses: array
// dimensions outer-major, with an innermost matrix leaf column-major inside
// its batch. `ParamView::set_serial_layout` documents the same pair from the
// constrained side.
//
// Returns, for arena batch `b` and position `j` within that batch's
// constrained leaf, the serial position of that value.
int64_t serial_index(const std::vector<int64_t>& dims, int leaf_rank, int64_t b,
                     int64_t j) {
  const size_t n = dims.size();
  const size_t outer = n - (size_t)leaf_rank;
  std::vector<int64_t> index(n, 0);

  // Outer dims are enumerated outer-major, so the LAST outer dimension moves
  // fastest as `b` counts up.
  int64_t rest = b;
  for (size_t d = outer; d-- > 0;) {
    index[d] = rest % dims[d];
    rest /= dims[d];
  }
  // The leaf is column-major, so its FIRST dimension moves fastest.
  int64_t leaf = j;
  for (size_t d = outer; d < n; ++d) {
    index[d] = leaf % dims[d];
    leaf /= dims[d];
  }

  // Serial: first logical index fastest.
  int64_t serial = 0, stride = 1;
  for (size_t d = 0; d < n; ++d) {
    serial += index[d] * stride;
    stride *= dims[d];
  }
  return serial;
}

// A container bound's value at one serial position of the value it bounds.
double bound_at(const DataMap::Entry& e, int64_t serial,
                const std::string& name) {
  if (serial >= (int64_t)e.r.size())
    refuse("a bound of " + name + " is shorter than the value it bounds");
  return e.r[(size_t)serial];
}

// A scalar bound, which broadcasts across the whole leaf.
TransformArg scalar_bound(const DataMap::Entry& e, const std::string& name) {
  if (e.r.empty()) refuse("a bound of " + name + " evaluated to nothing");
  return TransformArg{e.r.data(), 1};
}

}  // namespace

InitInterp::InitInterp(std::shared_ptr<const mir::Program> prog,
                       std::map<std::string, DataMap::Entry> base_env,
                       std::vector<InitParam> params)
    : prog_(std::move(prog)),
      base_env_(std::move(base_env)),
      params_(std::move(params)) {
  for (const auto& f : prog_->fun_defs) funs_[f.name] = &f;
}

std::vector<double> InitInterp::eval(
    const std::map<std::string, DataMap::Entry>& inits) const {
  // Validate before interpreting. stanc3 synthesizes these checks in its C++
  // backend rather than in the MIR (Lower_program.ml's `validate_dims`), so
  // the section carries none: an undersized value would fail deep inside the
  // interpreter with an index message, and an oversized one would pass
  // unnoticed, its extra values silently dropped by the sequential read.
  int64_t total_free = 0;
  for (const InitParam& p : params_) {
    const auto it = inits.find(p.name);
    if (it == inits.end()) refuse("no starting value for parameter " + p.name);
    const int64_t got =
        (int64_t)std::max(it->second.r.size(), it->second.i.size());
    if (got != p.constrained_len)
      refuse(p.name + " needs " + std::to_string(p.constrained_len) +
             " starting values, got " + std::to_string(got));
    total_free += p.free_len;
  }
  for (const auto& supplied : inits) {
    const std::string& name = supplied.first;
    const bool declared =
        std::any_of(params_.begin(), params_.end(),
                    [&name](const InitParam& p) { return p.name == name; });
    if (!declared) refuse(name + " is not a parameter of this model");
  }

  // Give every value its declared shape before the section runs. A caller
  // supplies values in serial order and may or may not have nested them --
  // a JSON document usually flattens an array of matrices to one list --
  // and the interpreter reaches a bare parameter declaration through the
  // same lookup as an FnReadData, so an unshaped entry would land in the
  // environment and make the section's own indexed writes fail.
  std::map<std::string, DataMap::Entry> shaped;
  for (const InitParam& p : params_) {
    DataMap::Entry e = inits.at(p.name);
    if (e.r.empty() && !e.i.empty()) e.r.assign(e.i.begin(), e.i.end());
    e.dims = p.dims;
    shaped.emplace(p.name, std::move(e));
  }

  std::vector<double> free_values;
  free_values.reserve((size_t)total_free);

  std::map<std::string, const InitParam*> by_name;
  for (const InitParam& p : params_) by_name[p.name] = &p;

  MirInterp<double>* cur = nullptr;
  MirHooks h;
  // FnReadData names a parameter here, never model data. Bare variables a
  // bound refers to come from env() instead, so these never collide.
  h.data = [&shaped](const std::string& name) -> const DataMap::Entry* {
    const auto it = shaped.find(name);
    return it == shaped.end() ? nullptr : &it->second;
  };
  h.stmt = [&](const mir::Stmt& s) {
    if (s.kind != mir::Stmt::NRFunApp || s.fn_name != "FnWriteParam" ||
        !s.write_transform)
      return false;
    if (s.fn_args.empty() || s.fn_args[0].kind != mir::Expr::Var)
      refuse("a free write does not name its parameter");
    const std::string& name = s.fn_args[0].name;
    const auto found = by_name.find(name);
    if (found == by_name.end())
      refuse("the init section writes unknown parameter " + name);
    const InitParam& p = *found->second;

    const DataMap::Entry* value = cur->find(name);
    if (value == nullptr) refuse("no constrained value was built for " + name);
    if ((int64_t)value->r.size() != p.constrained_len)
      refuse(name + " built " + std::to_string(value->r.size()) +
             " constrained values, expected " +
             std::to_string(p.constrained_len));

    // Bounds are expressions over data and over parameters already read, so
    // they are evaluated here rather than at compile time.
    std::vector<DataMap::Entry> bounds;
    for (const mir::Expr& e : s.write_transform->args)
      bounds.push_back(cur->eval(e));

    const std::vector<int64_t> leaf_dims(p.dims.end() - p.leaf_rank,
                                         p.dims.end());
    const int64_t inner_con = product(leaf_dims);
    const int64_t inner_free =
        free_leaf_size(s.write_transform->kind, leaf_dims);
    const int64_t batches = inner_con == 0 ? 0 : p.constrained_len / inner_con;

    const size_t base = free_values.size();
    free_values.resize(base + (size_t)(batches * inner_free));

    std::vector<double> leaf((size_t)inner_con);
    // A container bound is laid out like the value it bounds, so it is
    // gathered through the same permutation. A scalar bound broadcasts and
    // needs no gathering. Structured transforms have no bounds at all.
    std::vector<std::vector<double>> gathered_bounds(bounds.size());
    for (int64_t b = 0; b < batches; ++b) {
      for (size_t k = 0; k < bounds.size(); ++k)
        if (bounds[k].r.size() > 1)
          gathered_bounds[k].assign((size_t)inner_con, 0.0);
      for (int64_t j = 0; j < inner_con; ++j) {
        const int64_t serial = serial_index(p.dims, p.leaf_rank, b, j);
        leaf[(size_t)j] = value->r[(size_t)serial];
        for (size_t k = 0; k < bounds.size(); ++k)
          if (bounds[k].r.size() > 1)
            gathered_bounds[k][(size_t)j] = bound_at(bounds[k], serial, name);
      }
      std::vector<TransformArg> args;
      for (size_t k = 0; k < bounds.size(); ++k)
        args.push_back(bounds[k].r.size() > 1
                           ? TransformArg{gathered_bounds[k].data(), inner_con}
                           : scalar_bound(bounds[k], name));
      try {
        unconstrain_leaf(s.write_transform->kind, leaf_dims, leaf.data(), args,
                         free_values.data() + base + b * inner_free);
      } catch (const CompileError&) {
        throw;
      } catch (const std::exception& e) {
        // stan-math describes the violation but not whose it is.
        refuse(std::string(e.what()) + " (starting value for " + name + ")");
      }
    }
    return true;
  };

  MirInterp<double> in(funs_, "transform_inits", std::move(h));
  cur = &in;
  in.env() = base_env_;
  in.run(prog_->transform_inits);

  if ((int64_t)free_values.size() != total_free)
    refuse("the init section produced " + std::to_string(free_values.size()) +
           " free values, expected " + std::to_string(total_free));
  return free_values;
}

namespace {

const InitInterp& transform_inits_interp(const CompiledModel& cm) {
  if (!cm.transform_inits)
    throw std::runtime_error(
        "this model has no inverse parameter transforms: its MIR carries no "
        "transform_inits section");
  if (!cm.transform_inits->interp)
    throw std::runtime_error(
        "this model's inverse parameter transforms are unavailable: " +
        cm.transform_inits->truncated);
  return *cm.transform_inits->interp;
}

}  // namespace

void unconstrain_inits(const CompiledModel& cm,
                       const std::map<std::string, DataMap::Entry>& inits,
                       double* out) {
  const std::vector<double> unc = transform_inits_interp(cm).eval(inits);
  if ((int64_t)unc.size() != cm.n_unconstrained)
    throw std::runtime_error("the inverse transforms produced " +
                             std::to_string(unc.size()) +
                             " unconstrained values, expected " +
                             std::to_string(cm.n_unconstrained));
  std::copy(unc.begin(), unc.end(), out);
}

void unconstrain_inits_json(const CompiledModel& cm, const char* json,
                            double* out) {
  transform_inits_interp(cm);
  const DataMap supplied = DataMap::from_json(json == nullptr ? "{}" : json);
  unconstrain_inits(cm,
                    std::map<std::string, DataMap::Entry>(
                        supplied.entries().begin(), supplied.entries().end()),
                    out);
}

}  // namespace stanli
