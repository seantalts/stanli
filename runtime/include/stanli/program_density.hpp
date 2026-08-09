// The scalar continuous densities, once, for everything that is not a
// graph kernel.
//
// There used to be two lists. The graph had all of them
// (STANLI_SCALAR_DENSITY_LIST, optable.hpp); the register machine carried
// a hand-picked twelve of its own, and the MIR interpreter generated from
// that twelve and then hand-added `student_t_lpdf` back because it did not
// fit. The subset was not a design -- it was whatever the corpus needed
// when the register machine was written -- and it was load-bearing in the
// worst way: a region whose control flow depends on a parameter has to
// compile to the register machine or not at all, so a density outside the
// twelve was a hard compile error for a model the runtime handles
// everywhere else. `target += chi_square_lpdf(y | nu)` inside an
// `if (theta > 0)` did not compile; the same line outside the `if` did.
//
// So there is one list now, and this is the one place that switches on it.
// Three callers share these definitions rather than instantiating 27
// densities apiece: the register machine's interpreter (program.hpp), its
// generated adjoint (adjoint.cpp), and the MIR interpreter
// (mir_interp.hpp).
#ifndef STANLI_PROGRAM_DENSITY_HPP
#define STANLI_PROGRAM_DENSITY_HPP

#include <stan/math/rev/core.hpp>

#include <cstdint>
#include <string>

namespace stanli {

// The widest Stan gives a scalar continuous density: student_t,
// skew_normal, exp_mod_normal, pareto_type_2, skew_double_exponential.
constexpr int kMaxDensityArgs = 4;

// How many densities there are; ids run over [0, count).
int program_density_count();

// How many arguments density `id` takes, or 0 if `id` names none. The
// caller does not need to know which density it holds to lay out its
// arguments.
int program_density_arity(int id);

// Density id for a Stan function name (`"normal_lpdf"`) or an opcode
// (`OP_NORMAL_LPDF`), or -1. The name form is what the MIR front ends
// look up; the opcode form is what the island carver translates.
int program_density_id_by_name(const std::string& name);
int program_density_id_by_opcode(uint16_t opcode);

// Its name, for diagnostics.
const char* program_density_name(int id);

// Evaluate density `id` over `arity` contiguous arguments. propto-OFF:
// with no term-dropping the value does not depend on which arguments are
// autodiff, so one call serves the double and var passes alike.
template <typename T>
T program_density(int id, const T* args);

extern template double program_density<double>(int, const double*);
extern template stan::math::var program_density<stan::math::var>(
    int, const stan::math::var*);

// The same densities differentiated: stan-math computes value and partials
// in doubles through the recorder scalar (recorder.hpp), with no tape.
// `partials` receives one double per argument. Nothing here differentiates
// a density by hand.
void program_density_partials(int id, const double* args, double* partials);

}  // namespace stanli

#endif
