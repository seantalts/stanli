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
// a seeded stream owned here. Columns are discovered on the first
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

class WaInterp {
 public:
  WaInterp(std::shared_ptr<const mir::Program> prog,
           std::map<std::string, DataMap::Entry> base_env);

  void seed(unsigned s) { rng_.seed(s); }

  // One CSV row: constrained parameter values by name in, every column of
  // the generate_quantities section out.
  std::vector<double> eval(const std::map<std::string, DataMap::Entry>& params);

  // Valid after the first eval.
  const std::vector<CompiledModel::ParamView>& columns() const { return cols_; }

 private:
  bool read_param(MirInterp<double>& in, const mir::Stmt& s,
                  const std::map<std::string, DataMap::Entry>& params);
  bool write_param(MirInterp<double>& in, const mir::Stmt& s,
                   std::vector<double>& row);
  bool rng_fun(MirInterp<double>& in, const mir::Expr& e, DataMap::Entry* out);
  bool ode_fun(MirInterp<double>& in, const mir::Expr& e, DataMap::Entry* out);

  std::shared_ptr<const mir::Program> prog_;
  std::map<std::string, const mir::FunDef*> funs_;
  std::map<std::string, DataMap::Entry> base_env_;
  boost::ecuyer1988 rng_;
  std::vector<CompiledModel::ParamView> cols_;
  bool have_cols_ = false;
};

}  // namespace stanli

#endif
