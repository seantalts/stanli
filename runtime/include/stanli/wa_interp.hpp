// Interpreted write_array: the fallback when the write_array graph cannot
// express the whole generate_quantities section.
//
// The graph is the fast path and stays it. What defeats it is anything
// whose VALUE only exists per draw: RNG calls (including int-valued ones
// that then size or index things), and branches on quantities computed
// from the draw (the hmm models' Viterbi recursions). Those are exactly
// what a per-draw interpreter does naturally, and generated quantities run
// once per stored draw on plain doubles, so slow is acceptable: the
// sampler never goes through here.
//
// Per draw the host supplies the CONSTRAINED parameter values by name (the
// log_prob executor already computes them for its views); FnReadParam
// declarations are satisfied from that map instead of re-deriving the
// transforms, FnWriteParam appends to the CSV row, and RNG calls draw from
// the stream the caller passes in. Columns are discovered on the first
// evaluation and fixed from then on.
#ifndef STANLI_WA_INTERP_HPP
#define STANLI_WA_INTERP_HPP

#include <stanli/compile.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>

#include <boost/random/additive_combine.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace stanli {

// The generated-quantities RNG stream. The CALLER owns it, because a
// stream belongs to whoever is drawing rather than to the model: two
// chains sharing one model must not share one stream, and a model-owned
// member cannot express that. It is not thread-safe -- one per drawing
// thread, which is also what BridgeStan's bs_rng asks of its callers.
class WaRng {
 public:
  explicit WaRng(unsigned seed) : gen_(seed) {}
  void seed(unsigned s) { gen_.seed(s); }
  boost::ecuyer1988& gen() { return gen_; }

 private:
  boost::ecuyer1988 gen_;
};

// The columns only exist after one evaluation, so every driver that wants
// them at construction time has to probe. These two are that probe, shared
// so the C ABI and the BridgeStan facade discover the SAME columns: a
// driver with its own probe schedule would find a model in support where
// the other found it out of support, and quietly serve a shorter row.

// Probe point i under variant 0, 1 or 2. A model can be out of support at
// one variant and fine at the next, so callers walk all three.
double wa_probe_point(int64_t i, int variant);

class WaInterp {
 public:
  WaInterp(std::shared_ptr<const mir::Program> prog,
           std::map<std::string, DataMap::Entry> base_env);

  // One CSV row: constrained parameter values by name in, every column of
  // the generate_quantities section out. Any RNG draw advances `rng`.
  std::vector<double> eval(const std::map<std::string, DataMap::Entry>& params,
                           WaRng& rng);

  // Valid after the first eval.
  const std::vector<CompiledModel::ParamView>& columns() const { return cols_; }
  // Where the CSV's three sections meet, in `columns` indices: the same
  // contract as CompiledModel::WriteArray's fields of these names.
  size_t n_tp_start() const { return n_tp_start_; }
  size_t n_gq_start() const { return n_gq_start_; }

 private:
  bool read_param(MirInterp<double>& in, const mir::Stmt& s,
                  const std::map<std::string, DataMap::Entry>& params);
  bool write_param(MirInterp<double>& in, const mir::Stmt& s,
                   std::vector<double>& row);
  bool rng_fun(MirInterp<double>& in, const mir::Expr& e, DataMap::Entry* out,
               WaRng& rng);
  bool ode_fun(MirInterp<double>& in, const mir::Expr& e, DataMap::Entry* out);

  std::shared_ptr<const mir::Program> prog_;
  std::map<std::string, const mir::FunDef*> funs_;
  std::map<std::string, DataMap::Entry> base_env_;
  std::vector<CompiledModel::ParamView> cols_;
  size_t n_tp_start_ = 0;
  size_t n_gq_start_ = 0;
  bool saw_tp_ = false, saw_gq_ = false;
  bool have_cols_ = false;
  // The variable the last FnWriteParam named, while columns are being
  // discovered: the anchor for naming a write whose variable reference
  // the optimizer substituted away (see write_param).
  std::string last_written_;
};

}  // namespace stanli

#endif
