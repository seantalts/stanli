#ifndef STANLI_INIT_INTERP_HPP
#define STANLI_INIT_INTERP_HPP

#include <stanli/data.hpp>
#include <stanli/mir.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Starting values on the CONSTRAINED scale, turned into the free vector the
// sampler reads.
//
// This runs stanc3's `transform_inits` section through the MIR interpreter,
// the same way WaInterp runs `generate_quantities`. Interpreting rather than
// lowering is deliberate: it happens once per chain, and it is the only way
// to serve a bound that depends on an earlier parameter
// (`vector<lower=alpha>`), whose value is not known until that parameter has
// been read.
//
// Inside the section, `FnReadData` names a PARAMETER read from the caller's
// init context, while data a bound refers to is an ordinary variable resolved
// from the model environment -- so the two sources never overlap.
namespace stanli {

// One declared parameter, as the section will ask for it.
struct InitParam {
  std::string name;
  // Constrained dimensions in declaration order, empty for a scalar.
  std::vector<int64_t> dims;
  // Constrained value count, and the free count it unconstrains to.
  int64_t constrained_len = 1;
  int64_t free_len = 0;
  // How many trailing dims the transform's leaf occupies: 2 for a matrix
  // transform, 1 for a vector one, 0 when the transform is elementwise.
  int leaf_rank = 0;
};

class InitInterp {
 public:
  InitInterp(
      std::shared_ptr<const mir::Program> prog,
      std::shared_ptr<const std::map<std::string, DataMap::Entry>> base_env,
      std::vector<InitParam> params);

  // The parameters this model expects on the constrained scale, in
  // declaration order.
  const std::vector<InitParam>& params() const { return params_; }

  // Constrained starting values by name in; the free vector out, in the
  // layout the sampler reads. Throws CompileError naming the parameter for a
  // missing, unknown, wrong-length, or out-of-support value.
  std::vector<double> eval(
      const std::map<std::string, DataMap::Entry>& inits) const;

 private:
  std::shared_ptr<const mir::Program> prog_;
  std::map<std::string, const mir::FunDef*> funs_;
  std::shared_ptr<const std::map<std::string, DataMap::Entry>> base_env_;
  std::vector<InitParam> params_;
};

}  // namespace stanli

#endif
