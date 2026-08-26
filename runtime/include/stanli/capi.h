/* stanli C ABI: the versioned boundary language bindings speak to.
 * Exception-free; every entry returns an error code or null on failure and
 * writes a message into the caller's buffer. One stanli_model per (model,
 * data) pair; not thread-safe per instance (use one per chain). */
#ifndef STANLI_CAPI_H
#define STANLI_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct stanli_model stanli_model;

/* The layout version of this ABI. Bump it whenever an existing struct
 * gains, loses or reorders a field, or an existing function changes
 * signature. Adding a NEW function does not need a bump: a binding that
 * does not know about it never looks it up, and one that does gets a
 * named dlsym failure rather than a misread.
 *
 * This exists because the Python wheel ships its library inside the
 * package while the R package downloads one -- so on R, the binding and
 * the runtime are separately versioned artifacts and can drift. Reading
 * an opts struct at the wrong offsets is silent: sampling would succeed
 * from the wrong seed with the wrong step size. A binding must compare
 * this against the value it was compiled against before calling
 * anything, and refuse on a mismatch. */
#define STANLI_ABI_VERSION 1
int stanli_abi_version(void);

/* Compile either stanli portable MIR or legacy MIR S-expression text (from
 * `stanc --O1 --debug-optimized-mir`) against JSON data (CmdStan conventions).
 * The unoptimized --debug-transformed-mir S-expression is also accepted.
 * Returns null on failure with a message in err. */
stanli_model* stanli_model_new(const char* mir_text, const char* data_json,
                               char* err, size_t err_len);

/* Like stanli_model_new but takes Stan source code directly, compiled by the
 * embedded stanc3 (in-process; no subprocess). Fails with a message if this
 * build does not embed stanc. */
stanli_model* stanli_model_new_from_stan(const char* stan_code,
                                         const char* data_json, char* err,
                                         size_t err_len);
/* 1 if this build embeds stanc3, else 0. */
int stanli_has_embedded_stanc(void);

/* Compile Stan source to transformed-MIR text WITHOUT building a model,
 * for a caller that wants to keep the MIR: cache it, ship it, or hand it
 * to stanli_model_new later or elsewhere. Returns null on failure with a
 * message in err; on success the caller owns the string and frees it with
 * stanli_string_free. Needs a build that embeds stanc3. */
char* stanli_stan_to_mir(const char* stan_code, char* err, size_t err_len);

/* Frees a string this library returned ownership of. */
void stanli_string_free(char* p);

/* Identifies this runtime binary: source revision plus the build choices
 * that change what that source produces. Callers caching artifacts beside
 * a particular library (a compiled MIR, a manifest) key them on this and
 * refuse a mismatch. Static storage; do not free. */
const char* stanli_build_id(void);

/* 1 if this build reproduces CmdStan's lp__ exactly, 0 if lp__ sits a
 * per-model constant above it. A STANLI_LITE_LP build drops stan-math's
 * propto instantiations to halve the library, which leaves every
 * gradient bitwise identical and shifts only the reported log density.
 * A given seed still draws a different (equally valid) chain, because a
 * shifted lp rounds differently where the Hamiltonian adds it to the
 * kinetic energy. Callers that display or compare lp__, or that pin a
 * seed and expect the same bytes, should check this. */
int stanli_exact_lp(void);

void stanli_model_free(stanli_model* m);

int64_t stanli_n_unconstrained(const stanli_model* m);

/* log_prob (propto=false, jacobian included) and its gradient at
 * unconstrained q[n]. grad may be null for value-only. Returns 0 on success,
 * 1 on a rejected evaluation (domain error; *lp set to -inf). */
int stanli_grad(stanli_model* m, const double* q, double* lp, double* grad);

/* NUTS with diagonal-metric adaptation. draws must hold
 * samples * n_unconstrained doubles (row-major, one draw per row).
 * Returns 0 on success, nonzero with message in err otherwise. */
int stanli_sample(stanli_model* m, uint32_t seed, int warmup, int samples,
                  double delta, double* draws, char* err, size_t err_len);

/* ---- multi-chain sampling ------------------------------------------------
 *
 * Everything below packs draws CHAIN-MAJOR: draw i of chain c, column j
 * lives at draws[(c * n_draws + i) * n_cols + j]. That is the layout
 * numpy reshapes to (chains, draws, cols) for free, and the layout the
 * diagnostics read. */

typedef struct {
  uint32_t seed;
  int chains;   /* number of chains */
  int chain_id; /* id of the FIRST chain; chain c uses chain_id + c,
                 * which is how CmdStan turns one seed into per-chain
                 * streams. Matching it means a matched seed gives a
                 * matched stream per chain. */
  int warmup;
  int samples; /* transitions, not stored rows: with thin > 1 a run
                * stores samples/thin of them, as CmdStan does */
  int thin;
  double delta; /* target acceptance statistic */
  int max_depth;
  int save_warmup;     /* store warmup draws ahead of the sampling draws */
  double init_radius;  /* uniform(-r, r) on the unconstrained scale; 0
                        * starts at the origin (CmdStan's `init=0`) */
  const double* inits; /* chains * n_unconstrained on the UNCONSTRAINED
                        * scale, or null for random inits. Unconstrained
                        * because that is the scale stanli can read: a
                        * constrained init would need the inverse
                        * parameter transforms, which do not exist yet. */
  int num_threads;     /* honoured only when stanli_thread_safe(); see there */
} stanli_sample_opts;

/* Fill with CmdStan's defaults: seed 1, 4 chains from id 1, 1000 warmup,
 * 1000 samples, thin 1, delta 0.8, max_depth 10, no saved warmup, init
 * radius 2, random inits, one thread.
 *
 * ALWAYS call this before setting fields. A zeroed struct is not the
 * defaults: it asks for an init radius of 0, which is a different and
 * perfectly valid request, so the mistake would sample successfully from
 * the wrong starting point rather than fail. */
void stanli_sample_opts_init(stanli_sample_opts* o);

/* Stored rows per chain under these options. This is what `draws` and
 * `stats` must be sized against, and it is not `samples` whenever thin
 * or save_warmup is set. */
int64_t stanli_n_stored_draws(const stanli_sample_opts* o);

/* 1 when this build can run chains in real threads. stan-math's autodiff
 * stack is a plain static unless STAN_THREADS is defined, in which case
 * it is thread_local; the legacy kernels and tape islands build NESTED
 * var tapes on it, so without STAN_THREADS two chains in two threads
 * would quietly corrupt each other. When this is 0, num_threads is
 * clamped to 1 rather than honoured -- a slow answer instead of a wrong
 * one. */
int stanli_thread_safe(void);

/* Run opts->chains chains. draws holds
 * chains * stanli_n_stored_draws(opts) * n_unconstrained doubles; stats,
 * when non-null, holds chains * stanli_n_stored_draws(opts) * 7 (the
 * CmdStan sampler columns, see stanli_sampler_column_name).
 *
 * Returns 0 when every chain succeeded. Returns the NUMBER OF FAILED
 * CHAINS otherwise, with the first failure's message in err: a run where
 * three of four chains sampled is a partial result the caller may want,
 * and the failed chains' rows are left untouched. */
int stanli_sample_multi(stanli_model* m, const stanli_sample_opts* opts,
                        double* draws, double* stats, char* err,
                        size_t err_len);

/* The seven sampler columns, in order: lp__, accept_stat__, stepsize__,
 * treedepth__, n_leapfrog__, divergent__, energy__. */
#define STANLI_N_SAMPLER_COLS 7
const char* stanli_sampler_column_name(int i);

/* ---- summaries and convergence diagnostics -------------------------------
 *
 * These operate on a caller-supplied draw buffer rather than on the
 * model, because the draws worth summarizing are usually the CSV columns
 * (constrained parameters, transformed parameters, generated quantities)
 * that stanli_wa_row produces, not the unconstrained ones the sampler
 * returns. */

/* Per-column summary statistics, in this order. */
enum {
  STANLI_STAT_MEAN = 0,
  STANLI_STAT_MCSE_MEAN,
  STANLI_STAT_SD,
  STANLI_STAT_MCSE_SD,
  STANLI_STAT_Q5,
  STANLI_STAT_Q50,
  STANLI_STAT_Q95,
  STANLI_STAT_ESS_BULK,
  STANLI_STAT_ESS_TAIL,
  STANLI_STAT_RHAT,
  STANLI_N_SUMMARY_STATS
};

/* Writes n_cols * STANLI_N_SUMMARY_STATS doubles into out: for each
 * column, the statistics above in that order. R-hat is rank-normalized
 * split-R-hat and ESS is the bulk/tail pair, both from Vehtari et al.
 * 2021 via stan's own estimators -- the numbers stansummary prints.
 * A constant column yields NaN for R-hat and ESS, which is the honest
 * answer rather than a pass. Returns 0 on success. */
int stanli_summary_stats(const double* draws, int64_t n_chains, int64_t n_draws,
                         int64_t n_cols, double* out);

/* Convergence diagnostics as prose: divergences, treedepth saturation,
 * E-BFMI, R-hat, and bulk/tail ESS, each either confirmed or reported
 * with the number that failed and what to do about it. `stats` is the
 * sampler columns in the same chain-major packing, STANLI_N_SAMPLER_COLS
 * wide, or null to run only the R-hat/ESS half. `names`, when non-null,
 * must hold n_cols entries and is what lets a complaint name the
 * parameter that failed. Writes at most out_len bytes including the
 * terminator; returns the number of bytes the full text needs, so a
 * caller that gets back more than out_len can retry with a bigger
 * buffer. */
int64_t stanli_diagnose_text(const double* draws, int64_t n_chains,
                             int64_t n_draws, int64_t n_cols,
                             const char* const* names, const double* stats,
                             int max_depth, char* out, size_t out_len);

/* ---- optimization --------------------------------------------------------
 *
 * L-BFGS, the same one CmdStan's `optimize` runs, over the same gradient
 * the sampler uses. */

typedef struct {
  uint32_t seed;
  int chain_id;
  int iter;
  int jacobian; /* include the change-of-variables Jacobian, making
                 * this the posterior MODE rather than the penalized
                 * maximum likelihood. CmdStan defaults it off. */
  double init_alpha;
  double tol_obj;
  double tol_rel_obj;
  double tol_grad;
  double tol_rel_grad;
  double tol_param;
  int history_size;
  double init_radius;
  const double* init; /* unconstrained, or null for a random start */
} stanli_optimize_opts;

/* CmdStan's defaults. Call before setting fields, for the same reason
 * stanli_sample_opts_init exists. */
void stanli_optimize_opts_init(stanli_optimize_opts* o);

/* Writes the mode to `unconstrained` (n_unconstrained doubles) and, when
 * `values` is non-null, every CSV column there (stanli_wa_n_columns, or
 * n_constrained when the model has no generated quantities). *lp receives
 * the log density at the mode.
 *
 * Returns 0 when the optimizer converged, nonzero otherwise with a
 * message in err -- and in that case the buffers still hold the last
 * point it reached, which is usually what a user wants to look at. */
int stanli_optimize(stanli_model* m, const stanli_optimize_opts* opts,
                    double* unconstrained, double* values, double* lp,
                    char* err, size_t err_len);

/* Constrained view: flattened parameter values for one unconstrained q.
 * n_constrained gives the output length; names are "mu", "theta.1", ... */
int64_t stanli_n_constrained(const stanli_model* m);
const char* stanli_constrained_name(const stanli_model* m, int64_t i);
int stanli_constrain(stanli_model* m, const double* q, double* out);

/* Streaming variant of stanli_sample: `cb`, when non-null, fires after
 * every transition. During warmup (`warmup` nonzero) the buffer is
 * untouched; after it, draw `i` is written to `draws` before its
 * callback, so rows [0, i] are readable mid-run. */
typedef void (*stanli_draw_cb)(int32_t i, int32_t warmup, void* user);
int stanli_sample_stream(stanli_model* m, uint32_t seed, int warmup,
                         int samples, double delta, double* draws,
                         stanli_draw_cb cb, void* user, char* err,
                         size_t err_len);

/* WALNUTS (within-orbit adaptive step-length NUTS, arXiv:2506.18746),
 * same streaming contract as stanli_sample_stream. `max_error` is the
 * sampler's tunable in place of NUTS's `delta`: the largest drift in
 * the joint log density allowed across one macro step before the step
 * is halved within the trajectory; pass 0 for the default (0.5). */
int stanli_sample_walnuts_stream(stanli_model* m, uint32_t seed, int warmup,
                                 int samples, double max_error, double* draws,
                                 stanli_draw_cb cb, void* user, char* err,
                                 size_t err_len);

/* Single-path Pathfinder: a normal approximation fitted along an L-BFGS
 * path, drawn from in milliseconds where a sampler takes seconds. Draws
 * come back UNCONSTRAINED, num_draws rows of stanli_n_unconstrained
 * doubles, the same layout stanli_sample writes, so the same
 * stanli_wa_row call constrains them.
 *
 * `lp` and `lp_approx` each receive num_draws doubles: the model's log
 * density at the draw, and the approximation's. Their difference is the
 * log importance ratio k-hat is fitted to. Either may be null.
 *
 * `cb`, when non-null, fires once per L-BFGS iterate as the optimizer
 * climbs, which is the whole path -- there is no other way to get it.
 *
 * `summary` receives STANLI_N_PATHFINDER_SUMMARY doubles. */
#define STANLI_PATHFINDER_KHAT 0
#define STANLI_PATHFINDER_SELECTED_ITER 1
#define STANLI_PATHFINDER_SELECTED_ELBO 2
#define STANLI_PATHFINDER_ELAPSED_MS 3
#define STANLI_N_PATHFINDER_SUMMARY 4
typedef void (*stanli_path_cb)(int32_t iter, double lp, void* user);
int stanli_run_pathfinder(stanli_model* m, uint32_t seed, int chain_id,
                          int num_draws, double* draws, double* lp,
                          double* lp_approx, double* summary, stanli_path_cb cb,
                          void* user, char* err, size_t err_len);

/* write_array: every CSV column CmdStan would emit for one draw --
 * constrained parameters, transformed parameters, generated quantities,
 * in CmdStan's column order. n_columns is 0 when the model has no
 * generate_quantities section (use stanli_constrain then). RNG calls in
 * generated quantities draw from one stream; seed it per chain with
 * stanli_wa_seed_chain before the first row. stanli_wa_seed is the
 * backward-compatible direct-write-array convention with chain 0. wa_row
 * evaluates at unconstrained q and writes n_columns doubles; returns 0 on
 * success. */
int64_t stanli_wa_n_columns(const stanli_model* m);
const char* stanli_wa_column_name(const stanli_model* m, int64_t i);
void stanli_wa_seed(stanli_model* m, uint32_t seed);
void stanli_wa_seed_chain(stanli_model* m, uint32_t seed, uint32_t chain);
int stanli_wa_row(stanli_model* m, const double* q, double* out);

#ifdef __cplusplus
}
#endif

#endif
