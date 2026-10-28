// A deterministic text account of everything compile_model produces, for
// comparing two constructions of one model: the log_prob graph, the
// write_array graph, columns and names, the interpreter fallback notes and
// the warning built from them, and the values both graphs and the
// interpreter give at a fixed point. Graph bodies are hashed field by field
// (not through pointers), so two runs that lowered the same program agree
// and any difference in ops, slots, immediates or fills shows in the hash.
#ifndef STANLI_TESTS_LOWERING_DIGEST_HPP
#define STANLI_TESTS_LOWERING_DIGEST_HPP

#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/wa_interp.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

namespace stanli {
namespace lowering_digest {

struct Fnv {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= c[i];
      h *= 1099511628211ull;
    }
  }
  template <typename T>
  void pod(const T& v) {
    bytes(&v, sizeof v);
  }
  void str(const std::string& s) {
    pod(s.size());
    bytes(s.data(), s.size());
  }
};

inline std::string hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

inline uint64_t graph_hash(const Graph& g) {
  Fnv f;
  f.pod(g.slots.size());
  for (const Slot& s : g.slots) {
    f.pod(s.offset);
    f.pod(s.len);
    f.pod(s.is_param);
  }
  f.pod(g.ops.size());
  for (const Op& op : g.ops) {
    f.pod(op.opcode);
    f.pod(op.variant);
    f.pod(op.out);
    f.pod(op.out2);
    f.bytes(op.in, sizeof op.in);
    f.pod(op.n_in);
    f.pod(op.n_idata);
    for (int64_t k = 0; k < op.n_idata; ++k) f.pod(op.idata[k]);
    f.pod(op.dyn_capacity);
    f.pod(op.dyn_extent_in);
    f.pod(op.dyn_lengths);
    f.pod(op.primal_source);
    f.pod(op.udata != nullptr);
  }
  f.pod(g.udata_pool.size());
  f.pod(g.result_slot);
  return f.h;
}

inline uint64_t fills_hash(
    const std::vector<std::pair<int, std::vector<double>>>& fills) {
  Fnv f;
  f.pod(fills.size());
  for (const auto& fill : fills) {
    f.pod(fill.first);
    f.pod(fill.second.size());
    for (double v : fill.second) {
      uint64_t bits;
      std::memcpy(&bits, &v, sizeof bits);
      f.pod(bits);
    }
  }
  return f.h;
}

inline void put_views(std::ostringstream& o, const char* tag,
                      const std::vector<CompiledModel::ParamView>& views) {
  for (const auto& v : views) {
    o << tag << " " << v.name << " slot=" << v.slot << " len=" << v.len
      << " naming=" << static_cast<int>(v.naming) << " rows=" << v.rows
      << " matrix_storage=" << v.matrix_storage << " dims=";
    for (int64_t d : v.dims) o << d << ",";
    o << " order=";
    for (int64_t d : v.storage_order) o << d << ",";
    o << "\n";
  }
}

inline std::string describe(const CompiledModel& cm) {
  std::ostringstream o;
  o << "n_unconstrained " << cm.n_unconstrained << "\n";
  for (const auto& p : cm.param_names) o << "param " << p << "\n";
  for (const auto& u : cm.unc_params) {
    o << "unc " << u.name << " len=" << u.len
      << " transform=" << static_cast<int>(u.transform) << " dims=";
    for (int64_t d : u.dims) o << d << ",";
    o << "\n";
  }
  put_views(o, "view", cm.views);
  o << "log_prob graph " << graph_hash(cm.graph)
    << " ops=" << cm.graph.ops.size() << " slots=" << cm.graph.slots.size()
    << " fills=" << fills_hash(cm.fills) << "\n";
  o << "fallbacks";
  for (const auto& n : cm.interpreter_fallbacks) o << " [" << n << "]";
  o << "\nreduce_sum_fallbacks";
  for (const auto& n : cm.reduce_sum_fallbacks) o << " [" << n << "]";
  o << "\nreduce_sum_notice [" << cm.reduce_sum_notice << "]\n";
  o << "transformed_data_draws " << cm.transformed_data_draws << "\n";
  o << "warning [" << interpreter_warning(cm) << "]\n";
  if (cm.transform_inits)
    o << "transform_inits interp=" << (cm.transform_inits->interp != nullptr)
      << " truncated=[" << cm.transform_inits->truncated << "]\n";
  else
    o << "transform_inits none\n";
  if (!cm.write_array) {
    o << "write_array none\n";
    return o.str();
  }
  const auto& w = *cm.write_array;
  o << "write_array graph " << graph_hash(w.graph)
    << " ops=" << w.graph.ops.size() << " slots=" << w.graph.slots.size()
    << " fills=" << fills_hash(w.fills) << "\n";
  o << "wa n_unconstrained " << w.n_unconstrained << " n_tp_start "
    << w.n_tp_start << " n_gq_start " << w.n_gq_start << " truncated ["
    << w.truncated << "] interp=" << (w.interp != nullptr) << "\n";
  put_views(o, "col", w.columns);
  o << "csv";
  for (const auto& n : CompiledModel::csv_names(w.columns)) o << " " << n;
  o << "\n";
  return o.str();
}

inline double point(int64_t i, int variant = 0) {
  switch (variant) {
    case 1:
      return 0.02 * static_cast<double>((i % 5) - 2);
    case 2:
      return 0.0;
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

inline void put_bits(std::ostringstream& o, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, sizeof bits);
  o << " " << hex(bits);
}

inline std::string describe_values_at(const CompiledModel& cm, int variant) {
  std::ostringstream o;
  o << "point " << variant << "\n";
  try {
    Executor ex(Graph(cm.graph));
    cm.bind(ex);
    const int64_t n = ex.n_params();
    for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = point(i, variant);
    std::vector<double> grad(static_cast<size_t>(n), 0.0);
    const double lp = ex.gradient(grad.data());
    o << "lp";
    put_bits(o, lp);
    o << "\ngrad";
    for (double g : grad) put_bits(o, g);
    o << "\n";
    if (cm.write_array && cm.write_array->interp) {
      WaRng rng(1234);
      const std::vector<double> row =
          cm.write_array->interp->eval(cm.constrained_env(ex), rng);
      o << "wa_interp";
      for (double v : row) put_bits(o, v);
      o << "\n";
    } else if (cm.write_array && !cm.write_array->columns.empty()) {
      Executor wex(Graph(cm.write_array->graph));
      cm.write_array->bind(wex);
      for (int64_t i = 0; i < wex.n_params(); ++i)
        wex.params_data()[i] = point(i, variant);
      WaRng rng(1234);
      wex.run_forward_only(EvalState{&rng});
      o << "wa_graph";
      for (const auto& c : cm.write_array->columns) {
        const double* p = wex.value_ptr(c.slot);
        for (int64_t i = 0; i < c.len; ++i) put_bits(o, p[i]);
      }
      o << "\n";
    }
  } catch (const std::exception& e) {
    o << "eval_throws " << typeid(e).name() << ": " << e.what() << "\n";
  }
  return o.str();
}

// Evaluation at three fixed points. A graph or interpreter that throws is
// part of the account, not a reason to stop: both runs must throw the same
// thing.
inline std::string describe_values(const CompiledModel& cm) {
  return describe_values_at(cm, 0) + describe_values_at(cm, 1) +
         describe_values_at(cm, 2);
}

inline std::string describe_exception(const std::exception& e) {
  return std::string("THROWS ") + typeid(e).name() + ": " + e.what() + "\n";
}

inline std::string digest(const std::string& mir, const DataMap& data,
                          unsigned seed = 1) {
  try {
    CompiledModel cm = compile_model(mir, data, seed);
    return describe(cm) + describe_values(cm);
  } catch (const std::exception& e) {
    return describe_exception(e);
  }
}

}  // namespace lowering_digest
}  // namespace stanli

#endif
