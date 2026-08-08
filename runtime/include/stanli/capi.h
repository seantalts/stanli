/* stanli C ABI: the stable boundary language bindings speak to.
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

/* Compile transformed-MIR sexp text (from `stanc --debug-transformed-mir`)
 * against JSON data (CmdStan conventions). Returns null on failure with a
 * message in err. */
stanli_model* stanli_model_new(const char* tmir_sexp, const char* data_json,
                               char* err, size_t err_len);

/* Like stanli_model_new but takes Stan source code directly, compiled by the
 * embedded stanc3 (in-process; no subprocess). Fails with a message if this
 * build does not embed stanc. */
stanli_model* stanli_model_new_from_stan(const char* stan_code,
                                         const char* data_json, char* err,
                                         size_t err_len);
/* 1 if this build embeds stanc3, else 0. */
int stanli_has_embedded_stanc(void);

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
                         stanli_draw_cb cb, void* user,
                         char* err, size_t err_len);

/* write_array: every CSV column CmdStan would emit for one draw --
 * constrained parameters, transformed parameters, generated quantities,
 * in CmdStan's column order. n_columns is 0 when the model has no
 * generate_quantities section (use stanli_constrain then). RNG calls in
 * generated quantities draw from one stream; seed it per chain with
 * stanli_wa_seed before the first row. wa_row evaluates at unconstrained
 * q and writes n_columns doubles; returns 0 on success. */
int64_t stanli_wa_n_columns(const stanli_model* m);
const char* stanli_wa_column_name(const stanli_model* m, int64_t i);
void stanli_wa_seed(stanli_model* m, uint32_t seed);
int stanli_wa_row(stanli_model* m, const double* q, double* out);

#ifdef __cplusplus
}
#endif

#endif
