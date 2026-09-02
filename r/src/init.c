/* Routine registration. R CMD check requires it, and it is what makes
 * .Call("stanli_r_sample", ...) resolve without a symbol search. */
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

extern SEXP stanli_bridge_load(SEXP);
extern SEXP stanli_bridge_loaded(void);
extern SEXP stanli_r_model_new(SEXP, SEXP, SEXP);
extern SEXP stanli_r_has_embedded_stanc(void);
extern SEXP stanli_r_exact_lp(void);
extern SEXP stanli_r_thread_safe(void);
extern SEXP stanli_r_n_unconstrained(SEXP);
extern SEXP stanli_r_column_names(SEXP);
extern SEXP stanli_r_grad(SEXP, SEXP);
extern SEXP stanli_r_unconstrain_inits(SEXP, SEXP);
extern SEXP stanli_r_pathfinder_inits(SEXP, SEXP, SEXP, SEXP);
extern SEXP stanli_r_sample(SEXP, SEXP, SEXP, SEXP);
extern SEXP stanli_r_sampler_columns(void);
extern SEXP stanli_r_summary(SEXP, SEXP);
extern SEXP stanli_r_diagnose(SEXP, SEXP, SEXP, SEXP, SEXP);
extern SEXP stanli_r_optimize(SEXP, SEXP, SEXP);

static const R_CallMethodDef CallEntries[] = {
    {"stanli_bridge_load", (DL_FUNC)&stanli_bridge_load, 1},
    {"stanli_bridge_loaded", (DL_FUNC)&stanli_bridge_loaded, 0},
    {"stanli_r_model_new", (DL_FUNC)&stanli_r_model_new, 3},
    {"stanli_r_has_embedded_stanc", (DL_FUNC)&stanli_r_has_embedded_stanc, 0},
    {"stanli_r_exact_lp", (DL_FUNC)&stanli_r_exact_lp, 0},
    {"stanli_r_thread_safe", (DL_FUNC)&stanli_r_thread_safe, 0},
    {"stanli_r_n_unconstrained", (DL_FUNC)&stanli_r_n_unconstrained, 1},
    {"stanli_r_column_names", (DL_FUNC)&stanli_r_column_names, 1},
    {"stanli_r_grad", (DL_FUNC)&stanli_r_grad, 2},
    {"stanli_r_unconstrain_inits", (DL_FUNC)&stanli_r_unconstrain_inits, 2},
    {"stanli_r_pathfinder_inits", (DL_FUNC)&stanli_r_pathfinder_inits, 4},
    {"stanli_r_sample", (DL_FUNC)&stanli_r_sample, 4},
    {"stanli_r_sampler_columns", (DL_FUNC)&stanli_r_sampler_columns, 0},
    {"stanli_r_summary", (DL_FUNC)&stanli_r_summary, 2},
    {"stanli_r_diagnose", (DL_FUNC)&stanli_r_diagnose, 5},
    {"stanli_r_optimize", (DL_FUNC)&stanli_r_optimize, 3},
    {NULL, NULL, 0}};

void R_init_stanli(DllInfo* dll) {
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
}
