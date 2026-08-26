// Compile-time description of a legacy algebra_solver call.  The algebraic
// system remains callable after lowering because the root finder chooses the
// unknown vector values at runtime.  This owns the MIR function table, data,
// tolerances, and (when possible) a compiled register program for the system.
#ifndef STANLI_ALGEBRA_HPP
#define STANLI_ALGEBRA_HPP

#include <stanli/mir.hpp>
#include <stanli/ode_prog.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace stanli {

struct AlgebraSpec {
  std::map<std::string, mir::FunDef> owned;
  std::map<std::string, const mir::FunDef*> funs_map;
  std::string system_name;

  void adopt(const std::map<std::string, const mir::FunDef*>& src) {
    for (const auto& [name, def] : src) owned[name] = *def;
    for (const auto& [name, def] : owned) funs_map[name] = &def;
  }
  const mir::FunDef* system() const {
    auto it = owned.find(system_name);
    return it == owned.end() ? nullptr : &it->second;
  }
  const std::map<std::string, const mir::FunDef*>* funs() const {
    return &funs_map;
  }

  std::vector<double> x_r;
  std::vector<int> x_i;
  double relative_tolerance = 1e-10;
  double function_tolerance = 1e-6;
  int64_t max_num_steps = 1000;

  // Algebra systems have (unknown, parameters, real data, integer data),
  // while RhsProgram's seed convention has an extra leading scalar time.
  // Lowering compiles a synthetic, unused time formal so the mature ODE
  // register machine can be shared without changing either Stan signature.
  RhsProgram prog;
};

}  // namespace stanli

#endif
