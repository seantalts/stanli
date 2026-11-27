/* stanli C ABI: the versioned boundary language bindings speak to.
 * Exception-free; every entry returns an error code or null on failure and
 * writes a message into the caller's buffer. One stanli_model per (model,
 * data) pair.
 *
 * Threads. Different stanli_model handles are independent: calls on
 * different handles may run at the same time on different threads,
 * construction and compilation included (the embedded compiler runs one
 * compilation at a time by itself). One handle is NOT safe for concurrent
 * use. A call that evaluates the model (stanli_grad, stanli_grad_err,
 * stanli_hessian*, stanli_constrain, stanli_wa_row, stanli_wa_seed*,
 * stanli_unconstrain_inits, the samplers, optimize, stanli_laplace_whiten
 * and the other algorithms) uses the handle's own executors and its
 * stanli_wa_seed stream, so at most one such call may run on a handle at a
 * time, and none while stanli_model_free runs. Calls that only read what
 * construction fixed (counts, names, variables, layouts, warnings, messages,
 * stanli_resume_size) may run at any time while the handle lives. A call's
 * callbacks (progress, poll, log) run on the calling thread during the call
 * and must not evaluate the same handle. A sampling call runs its chains on
 * threads of its own; that is internal to the call. To evaluate one model
 * on several threads at once, build one handle per thread, or use the
 * BridgeStan facade, whose handles lend each call an executor of its own.
 *
 * Indexed accessors (name i of n, message i of n, ...) answer an index
 * outside [0, n) with null for a string, 0 for a count or dimension, -1 for
 * a size, and STANLI_COMPILER_MESSAGE_NONE for a message kind. */
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
 * signature or is removed. Adding a NEW function does not need a bump: a
 * binding that does not know about it never looks it up, and one that does gets
 * a named dlsym failure rather than a misread.
 *
 * This exists because the Python wheel ships its library inside the
 * package while the R package downloads one -- so on R, the binding and
 * the runtime are separately versioned artifacts and can drift. Reading
 * an opts struct at the wrong offsets is silent: sampling would succeed
 * from the wrong seed with the wrong step size. A binding must compare
 * this against the value it was compiled against before calling
 * anything, and refuse on a mismatch. */
#define STANLI_ABI_VERSION 7
int stanli_abi_version(void);

/* 1 if this build embeds stanc3, else 0. */
int stanli_has_embedded_stanc(void);

/* Model construction options. Call stanli_model_opts_init before setting
 * fields: seed 1, one thread per chain, fast_math 0, allow_interpreter 1,
 * execution_report 0, parallel_write_array STANLI_PARALLEL_WA_AUTO,
 * parallel_blas 0.
 *
 * seed is the model-construction seed: RNG calls in transformed data draw from
 * create_rng(seed, 0) once, when the model is built, exactly as CmdStan's
 * generated constructor does, so the same seed reproduces the same transformed
 * data. Generated-quantities draws are separate; see stanli_wa_seed_chain.
 *
 * threads_per_chain must be positive; >1 requires stanli_thread_safe().
 * Eligible reduce_sum calls compile to partitioned graphs, with a persistent
 * worker team owned by this handle. Sampling clones get their own teams.
 * One thread retains the existing serial graph. Ineligible calls stay serial.
 *
 * fast_math opts this model into numerics that may differ from CmdStan in the
 * last bits in exchange for speed. For a model built from Stan source it also
 * selects the compile-time rewrites that stanc3 applies, such as fused
 * multiply-add; for a model built from MIR text the rewrites are whatever the
 * MIR was compiled with, so compile it with stanli_stan_to_mir_with_opts
 * using the same setting. Default models are unchanged.
 *
 * allow_interpreter 0 makes construction fail, naming the parts, when parts
 * of the model have no compiled path and would run through the MIR
 * interpreter.
 *
 * execution_report 1 emits the execution manifest, then the host's
 * write_array selection, through the diagnostic sink while the model is
 * built.
 *
 * parallel_write_array says where write_array is lowered while the model is
 * built: AUTO on a second thread when the build has threads and the model has
 * enough to lower on both sides, OFF on the calling thread after log_prob,
 * FORCE on a second thread whenever the build has threads. Results are the
 * same either way.
 *
 * parallel_blas 1 lets this model's accelerated dense operations (in a macOS
 * build with STANLI_ACCELERATE, on macOS 15 or later) use Accelerate's own
 * threads while the chains that run in parallel leave cores free: twice the
 * number of parallel chains at most the number of performance cores. A
 * single gradient outside a sampler counts as one chain. Results can then
 * differ from those with it off by rounding. In a stress test on macOS 26.6,
 * threaded calls gave the single-threaded bits, except a symmetric product
 * (up to 6e-15 relative, n = 512 to 2048) and the Cholesky factorization at
 * n = 2048 (5e-16). Whether a call uses threads depends on how many chains
 * run in parallel, so results can also change with that number. 0, the
 * default, runs every such operation on the calling thread alone, with the
 * same result in every run. Accepted everywhere; it does nothing where no
 * operation is accelerated. */
typedef enum {
  STANLI_PARALLEL_WA_AUTO = 0,
  STANLI_PARALLEL_WA_OFF = 1,
  STANLI_PARALLEL_WA_FORCE = 2
} stanli_parallel_wa;
typedef struct {
  uint32_t seed;
  int threads_per_chain;
  int fast_math;
  int allow_interpreter;
  int execution_report;
  int parallel_write_array; /* a stanli_parallel_wa */
  int parallel_blas;
} stanli_model_opts;
void stanli_model_opts_init(stanli_model_opts* o);

/* Compile stanli portable MIR (the STANLI5 text that stanli-compile, the
 * embedded compiler, stanli_compile_stan and the JavaScript compiler write)
 * against JSON data (CmdStan conventions). Any other text, including stock
 * stanc's --debug-optimized-mir S-expressions, fails with a message that
 * names those compilers. Returns null on failure with a message in err. A null
 * opts means the defaults. Construction cannot be interrupted: it takes no poll
 * callback, and a large model can take seconds or more to prepare. */
stanli_model* stanli_model_new_with_opts(const char* mir_text,
                                         const char* data_json,
                                         const stanli_model_opts* opts,
                                         char* err, size_t err_len);

/* Like stanli_model_new_with_opts but takes Stan source code directly,
 * compiled by the embedded stanc3 (in-process; no subprocess). Fails with a
 * message if this build does not embed stanc. */
stanli_model* stanli_model_new_from_stan_with_opts(
    const char* stan_code, const char* data_json, const stanli_model_opts* opts,
    char* err, size_t err_len);

/* Like stanli_model_new_from_stan_with_opts, naming the source file in
 * compiler messages (null filename: the compiler's default label). The
 * compiler's messages are kept on the model; see
 * stanli_model_n_compiler_messages. */
stanli_model* stanli_model_new_from_stan_named(const char* stan_code,
                                               const char* filename,
                                               const char* data_json,
                                               const stanli_model_opts* opts,
                                               char* err, size_t err_len);

/* Compile Stan source to transformed-MIR text WITHOUT building a model,
 * for a caller that wants to keep the MIR: cache it, ship it, or hand it
 * to stanli_model_new_with_opts later or elsewhere. Returns null on failure
 * with a message in err; on success the caller owns the string and frees it
 * with stanli_string_free. Needs a build that embeds stanc3.
 *
 * Directories in include_paths are searched in order for Stan #include
 * directives. No implicit directory is added. Paths and source are UTF-8; the
 * array and its strings are borrowed for this call only. A null array is valid
 * only when include_path_count is zero. Nested includes use the same search
 * path. Stan's preprocessor owns expansion, cycle detection, and diagnostics.
 * fast_math is the setting of stanli_model_opts. */
char* stanli_stan_to_mir_with_opts(const char* stan_code,
                                   const char* const* include_paths,
                                   size_t include_path_count, int fast_math,
                                   char* err, size_t err_len);

/* What the compiler says besides the MIR: a warning from stanc3. Value 1
 * was a notice that O1 had been skipped; no release after 0.30.1 reports
 * it. */
typedef enum {
  STANLI_COMPILER_MESSAGE_NONE = -1, /* the index was out of range */
  STANLI_COMPILER_WARNING = 0
} stanli_compiler_message_kind;

/* One compilation of Stan source, with the source file named in compiler
 * messages (null: the compiler's default label). The other arguments are
 * those of stanli_stan_to_mir_with_opts. A failure of any kind is reported
 * through stanli_compilation_error; the result is null only when there is
 * not memory to say so. Free with stanli_compilation_free. */
typedef struct stanli_compilation stanli_compilation;
stanli_compilation* stanli_compile_stan(const char* stan_code,
                                        const char* filename,
                                        const char* const* include_paths,
                                        size_t include_path_count,
                                        int fast_math);
/* The MIR, or null when compilation failed. */
const char* stanli_compilation_mir(const stanli_compilation* c);
/* The error, or null when compilation succeeded. */
const char* stanli_compilation_error(const stanli_compilation* c);
/* Message i, for 0 <= i < stanli_compilation_n_messages(c). */
int64_t stanli_compilation_n_messages(const stanli_compilation* c);
stanli_compiler_message_kind stanli_compilation_message_kind(
    const stanli_compilation* c, int64_t i);
const char* stanli_compilation_message(const stanli_compilation* c, int64_t i);
void stanli_compilation_free(stanli_compilation* c);
/* stanli_compile_stan for MIR that only builds a model, as
 * stanli_model_new_from_stan does: the compiler may remove functions that
 * the model cannot reach, which standalone functions would need. */
stanli_compilation* stanli_compile_stan_for_model(
    const char* stan_code, const char* filename,
    const char* const* include_paths, size_t include_path_count, int fast_math);

/* The data block's declarations, read from MIR before any data, so that a
 * host can write each value the way its declaration needs. Variable i has a
 * name and a type, and each type has a kind, its extents and, for a tuple,
 * the types of its parts. A type's extents are those of the arrays around
 * it, then its own (one for a vector, two for a matrix); a tuple's are those
 * of the arrays of tuples around it, and each part's extents start after
 * them. An extent that depends on data is -1, and so is the extent of a
 * complex vector or matrix inside an array, which the MIR does not keep. A
 * constrained type has the kind it is stored as, so simplex[3] is a vector.
 * Returns null with a message in err when the MIR cannot be read. Strings are
 * owned by the result; free it with stanli_data_declarations_free. Out-of-range
 * indices return -1, STANLI_TYPE_NONE, 0 parts or null. */
typedef enum {
  STANLI_TYPE_NONE = -1,
  STANLI_TYPE_INT = 0,
  STANLI_TYPE_REAL = 1,
  STANLI_TYPE_COMPLEX = 2,
  STANLI_TYPE_VECTOR = 3,
  STANLI_TYPE_ROW_VECTOR = 4,
  STANLI_TYPE_MATRIX = 5,
  STANLI_TYPE_COMPLEX_VECTOR = 6,
  STANLI_TYPE_COMPLEX_ROW_VECTOR = 7,
  STANLI_TYPE_COMPLEX_MATRIX = 8,
  STANLI_TYPE_TUPLE = 9
} stanli_type_kind;
typedef struct stanli_data_declarations stanli_data_declarations;
stanli_data_declarations* stanli_data_declarations_new(const char* mir,
                                                       char* err,
                                                       size_t err_len);
/* The parameters block's declarations, read with the same accessors. A
 * parameter's type is the one it is stored as, so cholesky_factor_corr[3]
 * is a 3 x 3 matrix. Transformed parameters and generated quantities are
 * not included. */
stanli_data_declarations* stanli_parameter_declarations_new(const char* mir,
                                                            char* err,
                                                            size_t err_len);
void stanli_data_declarations_free(stanli_data_declarations* d);
int64_t stanli_data_declarations_n(const stanli_data_declarations* d);
const char* stanli_data_declaration_name(const stanli_data_declarations* d,
                                         int64_t i);
int64_t stanli_data_declaration_type(const stanli_data_declarations* d,
                                     int64_t i);
stanli_type_kind stanli_type_kind_of(const stanli_data_declarations* d,
                                     int64_t type);
/* The number of extents that come from arrays: all of a tuple's. */
int64_t stanli_type_n_array_dims(const stanli_data_declarations* d,
                                 int64_t type);
int64_t stanli_type_n_dims(const stanli_data_declarations* d, int64_t type);
int64_t stanli_type_dim(const stanli_data_declarations* d, int64_t type,
                        int64_t k);
int64_t stanli_type_n_parts(const stanli_data_declarations* d, int64_t type);
int64_t stanli_type_part(const stanli_data_declarations* d, int64_t type,
                         int64_t k);

/* Frees a string this library returned ownership of. */
void stanli_string_free(char* p);

/* Where the model's text output goes: print() statements, the sampler's
 * "Chain [n]" notices and warnings. By default each line is written to
 * stdout as it is emitted, from whichever thread emitted it. A host that
 * prints through its own console (R, Python) cannot do that from a sampler
 * thread, so it calls stanli_capture_messages(1) once and then collects the
 * text on its own thread with stanli_take_messages: every line emitted since
 * the last call, each followed by a newline, in emission order, or null when
 * there is none. The result is freed with stanli_string_free. Collect after
 * every call into the library, on error paths too, and periodically from a
 * long call's poll callback; nothing is dropped, but text waits until taken.
 * The setting is process-wide. */
void stanli_capture_messages(int enable);
char* stanli_take_messages(void);

/* Identifies this runtime binary: source revision plus the build choices
 * that change what that source produces. Callers caching artifacts beside
 * a particular library (a compiled MIR, a manifest) key them on this and
 * refuse a mismatch. Static storage; do not free. */
const char* stanli_build_id(void);

/* The data argument a BridgeStan client passes to bs_model_construct to
 * get this model from this runtime: the object in data_json (null means no
 * data) with a "__stanli" member holding stanli_build_id(), mir and name.
 * Returns null with a message in err when data_json is not a JSON object;
 * on success the caller frees the string with stanli_string_free. */
char* stanli_bridgestan_data(const char* data_json, const char* mir,
                             const char* name, char* err, size_t err_len);

/* Always 1: every build reproduces CmdStan's lp__ exactly. Kept so callers
 * written against earlier builds keep linking. */
int stanli_exact_lp(void);

/* Retained native reduction count, and the reasons graph lowering kept
 * reduce_sum calls serial, one per refusal. Diagnostics cover graph
 * lowering, not calls inside opaque runtime regions. Strings are owned by
 * the model. */
int stanli_reduce_sum_count(const stanli_model* m);
int64_t stanli_n_reduce_sum_fallbacks(const stanli_model* m);
const char* stanli_reduce_sum_fallback(const stanli_model* m, int64_t i);

void stanli_model_free(stanli_model* m);

int64_t stanli_n_unconstrained(const stanli_model* m);

/* Starting values on the CONSTRAINED scale, turned into the n_unconstrained
 * free values the samplers and stanli_grad read.
 *
 * `inits_json` is a CmdStan-style JSON object naming every declared
 * parameter, except that a zero-size one may be left out. Each value is
 * checked by CmdStan's rule for JSON data: its dims must be the declared
 * ones, and any empty value, such as [], stands for a zero-size parameter.
 * Unlike CmdStan, extents of 1 may also be added or left out. Writes
 * n_unconstrained doubles to `q`. Returns 0 on success, or nonzero with a
 * message in err: a missing, unknown, wrongly shaped, or out-of-support value
 * names the parameter, and a model whose MIR carries no inverse parameter
 * transforms says so.
 *
 * The sampler entry points keep taking UNCONSTRAINED inits
 * (StanliSampleOpts::inits below). Unconstraining is a host-boundary step,
 * done once per chain, and keeping it out of the sampler leaves one contract
 * for a start rather than two. */
int stanli_unconstrain_inits(stanli_model* m, const char* inits_json, double* q,
                             char* err, size_t err_len);

/* log_prob (propto=true, jacobian included) and its gradient at
 * unconstrained q[n]. grad may be null for value-only. Returns 0 on success,
 * 1 on a rejected evaluation (domain error; *lp set to -inf). */
int stanli_grad(stanli_model* m, const double* q, double* lp, double* grad);

/* Why an evaluation failed. REJECTED is a std::domain_error, which is how
 * Stan's reject() and its argument checks refuse a point: CmdStan's samplers
 * reject the proposal and its initialization tries another point. FAILED is
 * any other error in evaluating the model, such as an index out of range;
 * CmdStan stops initialization at once on these. A FAILED evaluation from
 * fatal_error(), or from a construct that stanli cannot evaluate at run time
 * (a zero-size local that a branch on parameters left empty), also ends
 * sampling, optimization and the other algorithms with its message, as
 * fatal_error() does in Stan. INTERNAL_ERROR is a failed
 * check inside stanli itself, which is a bug worth reporting. */
typedef enum {
  STANLI_EVAL_OK = 0,
  STANLI_EVAL_REJECTED = 1,
  STANLI_EVAL_FAILED = 2,
  STANLI_EVAL_INTERNAL_ERROR = 3
} stanli_eval_status;

/* stanli_grad with the reason for a failure: returns a stanli_eval_status
 * and, unless it is STANLI_EVAL_OK, writes the evaluation's message into err
 * and sets *lp to -inf. err is not touched on success. */
int stanli_grad_err(stanli_model* m, const double* q, double* lp, double* grad,
                    char* err, size_t err_len);

/* The log density of stanli_grad, its gradient, and the n_unconstrained by
 * n_unconstrained Hessian at unconstrained q[n]. The Hessian is symmetric, so
 * row- and column-major agree. `grad` may be null.
 *
 * The Hessian is Stan Math's finite_diff_hessian_auto, the routine
 * laplace_sample uses. It takes central differences of the gradient with a
 * step chosen per coordinate, so it costs 1 + 2n gradient evaluations and its
 * error is that of a finite difference.
 *
 * Returns 0 on success. Returns nonzero with a message in `err` on null
 * arguments or when any evaluation is rejected (domain error); the outputs are
 * then unspecified. */
int stanli_hessian(stanli_model* m, const double* q, double* lp, double* grad,
                   double* hessian, char* err, size_t err_len);

/* How a long call learns that its caller wants it to stop (Ctrl-C, say).
 * Asked on the calling thread; a nonzero answer stops the call. Sampling
 * asks about every 100 ms while chains run and stops every chain after its
 * current transition. */
typedef int (*stanli_sample_poll_cb)(void* user);

/* stanli_hessian, asking `poll` on the calling thread about every 100 ms, and
 * once before the first gradient. A nonzero answer abandons the call, which
 * returns 0 with *interrupted set to 1 and nothing usable in the outputs.
 * `poll` and `interrupted` may be null. */
int stanli_hessian_with_poll(stanli_model* m, const double* q, double* lp,
                             double* grad, double* hessian,
                             stanli_sample_poll_cb poll, void* poll_user,
                             int* interrupted, char* err, size_t err_len);

/* EXPERIMENTAL. A new model over whitened coordinates z, with
 *     q = q_hat + P^T L^{-T} z,   (P v)[i] = v[perm[i]],
 * where q is `inner`'s unconstrained vector and L is a lower triangular
 * Cholesky factor with P Q P^T = L L^T for some precision Q (typically the
 * negative Hessian at q_hat). The new model's log density is the inner one at
 * q(z) with the constant log|det L| dropped, so every entry point that takes
 * the unconstrained vector (stanli_grad, stanli_hessian, the samplers,
 * stanli_optimize, ...) works on z unchanged.
 *
 * What comes OUT stays in the inner model's terms: constrained names, output
 * rows (stanli_constrain, stanli_wa_row, sampler draws) are the inner model's
 * parameters, transformed parameters and generated quantities at q(z).
 * stanli_unconstrain_inits returns the z that corresponds to the given
 * constrained values. Unconstrained inits, modes and Hessians the caller
 * passes in or reads back are in z.
 *
 * L is given in compressed sparse column form: col_ptr[n + 1], and row_idx and
 * values of length nnz. Each column must begin with its (nonzero) diagonal
 * entry, followed by strictly increasing row indices below it. perm[n] must be
 * a permutation of 0..n-1. n must equal stanli_n_unconstrained(inner).
 *
 * The arrays are copied. The result does not refer to `inner`, which can be
 * freed independently. Whitening an already-whitened model is refused.
 * Returns null with a message in `err` on invalid input. Free the result with
 * stanli_model_free. */
stanli_model* stanli_model_whiten(const stanli_model* inner,
                                  const double* q_hat, int64_t n,
                                  const int64_t* perm, const int64_t* col_ptr,
                                  const int64_t* row_idx, const double* values,
                                  int64_t nnz, char* err, size_t err_len);

/* EXPERIMENTAL. Like stanli_model_whiten, with the factor
 * C = diag(c) + tril(W B^T, -1) of a diagonal-plus-low-rank matrix inserted:
 *     q = q_hat + P^T L^{-T} C^{-T} z.
 * With L unit lower triangular and C C^T = D + W0 W0^T this whitens a
 * precision Q' = P^T L (D + W0 W0^T) L^T P, as stanli_sparse_factor_lowrank
 * computes. c has n entries; b and w are n by k, row-major. Per-gradient cost
 * is O(nnz(L) + n k). With k = 0, b and w may be null. */
stanli_model* stanli_model_whiten_lowrank(
    const stanli_model* inner, const double* q_hat, int64_t n,
    const int64_t* perm, const int64_t* col_ptr, const int64_t* row_idx,
    const double* values, int64_t nnz, int64_t k, const double* c,
    const double* b, const double* w, char* err, size_t err_len);

/* EXPERIMENTAL. Sparse LDL^T of a symmetric matrix A, given as the lower
 * triangle in compressed sparse column form (entries above the diagonal are
 * ignored). With perm null the ordering is approximate minimum degree;
 * otherwise perm[n] is used as given. With (P v)[i] = v[perm[i]]:
 *     P A P^T = L diag(d) L^T,
 * L unit lower triangular with each column's diagonal entry first. d may have
 * negative entries when A is indefinite; there is no pivoting. Returns null
 * with a message in `err` on invalid input or a zero pivot. Free with
 * stanli_sparse_factor_free. */
typedef struct stanli_sparse_factor stanli_sparse_factor;
stanli_sparse_factor* stanli_sparse_factor_new(
    int64_t n, const int64_t* col_ptr, const int64_t* row_idx,
    const double* values, const int64_t* perm, char* err, size_t err_len);
/* Entries stored in L, diagonal included. */
int64_t stanli_sparse_factor_nnz(const stanli_sparse_factor* f);
/* Number of negative d entries and the sum of log|d|, i.e. log|det A|. */
void stanli_sparse_factor_stats(const stanli_sparse_factor* f,
                                int64_t* n_negative, double* log_abs_det);
/* perm[n], col_ptr[n + 1], row_idx[nnz], values[nnz] and d[n]. */
void stanli_sparse_factor_get(const stanli_sparse_factor* f, int64_t* perm,
                              int64_t* col_ptr, int64_t* row_idx,
                              double* values, double* d);
/* For u, an n by k column-major matrix in A's own ordering, writes the
 * arguments of stanli_model_whiten_lowrank (c[n], b[n * k], w[n * k]) for
 * A + u u^T, with W0 = L^{-1} P u. Returns nonzero with a message when
 * A + u u^T is not positive definite. */
int stanli_sparse_factor_lowrank(const stanli_sparse_factor* f, int64_t k,
                                 const double* u, double* c, double* b,
                                 double* w, char* err, size_t err_len);
void stanli_sparse_factor_free(stanli_sparse_factor* f);

/* EXPERIMENTAL. The Laplace whitening builder: everything that
 * stanli_model_whiten_lowrank needs to whiten a model with random effects u
 * (the `latent` block of the unconstrained vector) and hyperparameters theta
 * (the rest).
 *
 * 1. From q_start, the sparsity pattern of the Hessian block over u: entries
 *    above pattern_tolerance relative to sqrt(|H_ii| |H_jj|). By default it
 *    comes from gradient differences along random groups of coordinates,
 *    checked against three random central differences; if the check fails
 *    the builder warns and uses the dense Hessian (stanli_hessian), which is
 *    also what dense_pattern asks for.
 * 2. The theta that maximizes log p(theta, u_hat(theta)) - log det(-H_uu) / 2,
 *    with u_hat from an inner L-BFGS (inner_iter iterations), H_uu from
 *    central differences along a colouring of the pattern, and theta from an
 *    outer L-BFGS (max_outer iterations) on forward differences.
 * 3. Q, the negative Hessian there (coloured, or dense with dense_hessian),
 *    without the entries below threshold relative to sqrt(Q_ii Q_jj).
 * 4. A sparse LDL^T of Q under an approximate minimum degree order. When Q
 *    is not positive definite: REPAIR_ABS replaces each eigenvalue below clip
 *    times the largest absolute eigenvalue by its absolute value, floored at
 *    that level, as a low-rank correction found by a Lanczos search;
 *    REPAIR_ABS_DENSE does the same densely; REPAIR_CLIP raises each
 *    eigenvalue below clip times the largest to that level, densely. Either
 *    way the result carries a warning.
 *
 * The random numbers of step 1 come from `seed`. */
typedef enum {
  STANLI_WHITEN_REPAIR_ABS = 0,
  STANLI_WHITEN_REPAIR_ABS_DENSE = 1,
  STANLI_WHITEN_REPAIR_CLIP = 2
} stanli_whiten_repair;
typedef struct {
  uint32_t seed;
  int max_outer;
  int inner_iter;
  double pattern_tolerance;
  double threshold;
  int repair; /* a stanli_whiten_repair */
  double clip;
  int dense_pattern;
  int dense_hessian;
  int keep_precision; /* keep Q (repaired) for stanli_whitening_get_precision */
} stanli_laplace_whiten_opts;
/* seed 1, max_outer 60, inner_iter 2000, tolerances 1e-6, REPAIR_ABS, clip
 * 1e-8 (the conventional clip for REPAIR_CLIP is 1e-3), the rest 0. */
void stanli_laplace_whiten_opts_init(stanli_laplace_whiten_opts* o);

/* Runs the builder from the unconstrained q_start[n], with latent[n_latent]
 * the increasing 0-based index of u. A null opts means the defaults.
 * Returns null with a message in `err` on invalid input or failure, and null
 * with *interrupted set to 1 when `poll` (asked as stanli_hessian_with_poll
 * asks it) stopped the call. `poll` and `interrupted` may be null. Free the
 * result with stanli_whitening_free. */
typedef struct stanli_whitening stanli_laplace;
stanli_whitening* stanli_laplace_whiten(stanli_model* m, const double* q_start,
                                        int64_t n_latent, const int64_t* latent,
                                        const stanli_laplace_whiten_opts* opts,
                                        stanli_sample_poll_cb poll,
                                        void* poll_user, int* interrupted,
                                        char* err, size_t err_len);

typedef struct {
  int64_t n;
  int64_t n_latent;
  int64_t n_theta;
  int64_t nnz;  /* entries of L */
  int64_t rank; /* columns of the low-rank correction; 0 for none */
  int64_t n_clipped;
  /* min_eigenvalue and max_eigenvalue hold the extreme eigenvalues of Q when
   * it was not positive definite, and are 0 otherwise. */
  int has_spectrum;
  double min_eigenvalue;
  double max_eigenvalue;
  int64_t n_colors; /* of the final Hessian's colouring; n when dense */
  int64_t pattern_gradients;
  int64_t marginal_gradients;
  int64_t final_gradients;
  int64_t outer_iterations;
  int64_t outer_evaluations;
  int pattern_fallback; /* the probe failed; the pattern is the dense one */
  int has_precision;
} stanli_whitening_report;
void stanli_whitening_get_report(const stanli_whitening* r,
                                 stanli_whitening_report* out);
/* The arguments of stanli_model_whiten_lowrank: q_hat[n], perm[n],
 * col_ptr[n + 1], row_idx[nnz], values[nnz], and c[n], b[n * rank] and
 * w[n * rank] (row-major). When rank is 0, c, b and w are not written and
 * may be null, and the whitening is stanli_model_whiten's. */
void stanli_whitening_get_spec(const stanli_whitening* r, double* q_hat,
                               int64_t* perm, int64_t* col_ptr,
                               int64_t* row_idx, double* values, double* c,
                               double* b, double* w);
/* theta_start[n_theta] and theta_hat[n_theta]. */
void stanli_whitening_get_theta(const stanli_whitening* r, double* theta_start,
                                double* theta_hat);
/* How the outer L-BFGS stopped; empty when there is no theta. */
const char* stanli_whitening_outer_message(const stanli_whitening* r);
/* Warnings for the caller to show: the probe fell back, or eigenvalues were
 * repaired. */
int64_t stanli_whitening_n_warnings(const stanli_whitening* r);
const char* stanli_whitening_warning(const stanli_whitening* r, int64_t i);
/* The repaired Q as n * n doubles, when keep_precision was set. Returns
 * nonzero, writing nothing, otherwise. */
int stanli_whitening_get_precision(const stanli_whitening* r, double* out);
void stanli_whitening_free(stanli_whitening* r);

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
                        * scale, or null for random inits. Start from
                        * constrained values by running them through
                        * stanli_unconstrain_inits first: it is a per-chain
                        * host-boundary step, and one scale here means one
                        * contract for what a start is. */
  int num_threads;     /* honoured only when stanli_thread_safe(); see there */
  /* Continue earlier chains instead of starting new ones: one resume record
   * per chain (resume_in[c], resume_in_len[c] bytes), as an earlier run wrote
   * them to resume_out. Null starts fresh. See stanli_resume_size. */
  const uint8_t* const* resume_in;
  const size_t* resume_in_len;
  /* chains * stanli_resume_size(m) bytes receiving each chain's record after
   * the run, chain c at c * stanli_resume_size(m); null writes none. */
  uint8_t* resume_out;
} stanli_sample_opts;

/* Fill with CmdStan's defaults: seed 1, 4 chains from id 1, 1000 warmup,
 * 1000 samples, thin 1, delta 0.8, max_depth 10, no saved warmup, init
 * radius 2, random inits, one thread, no resume records.
 *
 * ALWAYS call this before setting fields. A zeroed struct is not the
 * defaults: it asks for an init radius of 0, which is a different and
 * perfectly valid request, so the mistake would sample successfully from
 * the wrong starting point rather than fail. */
void stanli_sample_opts_init(stanli_sample_opts* o);

/* ARGUMENT CHECKS. Each options struct has a _check function, and its entry
 * point applies it first and fails with the same message, so a binding that
 * sizes buffers from the options can call the check before allocating and
 * need not repeat the rules. The rules are CmdStan's: chain_id >= 0 (CmdStan
 * accepts id 0), counts that CmdStan requires to be positive >= 1, its
 * tolerances and step sizes finite and > 0. CmdStan accepts a negative init
 * radius and then hangs drawing from it; stanli requires a finite radius
 * >= 0. A check returns 0 when every rule holds, else 1 with one message
 * naming the first field that breaks one, in one of these forms:
 *   "<field> must be an integer >= <n>"
 *   "<field> must be a finite number > 0"
 *   "init_radius must be a finite number >= 0"
 *   "chain_id + chains - 1 must not exceed 2147483647"
 * or a sentence for a rule of its own (delta's range, an enum's values).
 *
 * stanli_sample_opts_check: chains >= 1, warmup >= 0, samples >= 0,
 * thin >= 1, delta >= 0.1 and < 1, max_depth >= 1, init_radius, chain_id. */
int stanli_sample_opts_check(const stanli_sample_opts* o, char* err,
                             size_t err_len);

/* Stored rows per chain under these options. This is what `draws` and
 * `stats` must be sized against, and it is not `samples` whenever thin
 * or save_warmup is set, or when resume_in continues a thinned run (rows
 * are kept where the whole run's transition count is a multiple of thin). */
int64_t stanli_n_stored_draws(const stanli_sample_opts* o);

/* 1 when this build can run chains in real threads. stan-math's autodiff
 * stack is a plain static unless STAN_THREADS is defined, in which case
 * it is thread_local; the legacy kernels and tape islands build NESTED
 * var tapes on it, so without STAN_THREADS two chains in two threads
 * would quietly corrupt each other. When this is 0, num_threads is
 * clamped to 1 rather than honoured -- a slow answer instead of a wrong
 * one. */
int stanli_thread_safe(void);

/* Per-chain facts that are not properties of one stored draw. Counts cover
 * every post-warmup transition even when thinning drops rows. */
typedef struct {
  double warmup_seconds;
  double sampling_seconds;
  int64_t n_divergent;
  int64_t n_max_treedepth;
} stanli_sample_report;

/* A completed transition selected by `refresh`. The iteration is one-based
 * across warmup and sampling together, `total` is warmup + samples, and
 * warmup is nonzero for the warmup phase. `n_divergent` and `n_max_treedepth`
 * count that chain's post-warmup transitions so far and end at the values in
 * its stanli_sample_report; both are zero during warmup. Callbacks are
 * serialized on the thread that called stanli_sample_multi_metric, never a
 * sampler worker, so language bindings may safely write to their own
 * consoles. */
typedef void (*stanli_sample_progress_cb)(int32_t chain_id, int64_t iteration,
                                          int64_t total, int32_t warmup,
                                          int64_t n_divergent,
                                          int64_t n_max_treedepth, void* user);

/* Metric kinds for stanli_metric_opts::metric, as CmdStan's unit_e, diag_e and
 * dense_e. */
enum {
  STANLI_METRIC_UNIT = 0,
  STANLI_METRIC_DIAG = 1,
  STANLI_METRIC_DENSE = 2
};

/* Metric and step size controls for stanli_sample_multi_metric.
 *
 * `inv_metric` is the starting inverse metric: null for the identity, else
 * chain-major with n_unconstrained values per chain for DIAG or
 * n_unconstrained^2 for DENSE; UNIT takes none. `step_size` is CmdStan's
 * `stepsize`. With `adapt_engaged` zero no adaptation runs and the metric and
 * step size stay as given.
 *
 * With `adapt_engaged` nonzero and `adapt_metric` zero, warmup adapts only the
 * step size, by dual averaging as Stan does, and the inverse metric stays as
 * supplied (the identity when `inv_metric` is null), so `adapted_inv_metric`
 * equals the input bitwise. `adapt_metric` has no effect with
 * STANLI_METRIC_UNIT, which never adapts a metric, or with `adapt_engaged`
 * zero.
 *
 * `adapted_step_size`, when non-null, receives one step size per chain and
 * `adapted_inv_metric`, when non-null, the metric each chain holds after
 * warmup in the layout of `inv_metric` (nothing for UNIT). Both are filled
 * only for chains that ran. */
typedef struct {
  int metric;
  const double* inv_metric;
  double step_size;
  int adapt_engaged;
  int adapt_metric;
  double* adapted_step_size;
  double* adapted_inv_metric;
} stanli_metric_opts;

/* Fill with diag, identity, step size 1, adaptation engaged for the step size
 * and the metric, no outputs. */
void stanli_metric_opts_init(stanli_metric_opts* o);

/* Everything stanli_sample_multi_metric checks before it starts, apart from
 * the values in the inverse metric and resume records:
 * stanli_sample_opts_check, a known metric kind, and, as in CmdStan, warmup >=
 * 1 whenever adaptation is engaged on a run that does not continue from resume
 * records. A null metric means the defaults. */
int stanli_sample_check(const stanli_sample_opts* o,
                        const stanli_metric_opts* metric, char* err,
                        size_t err_len);

/* Run opts->chains chains. draws, when non-null, holds
 * chains * stanli_n_stored_draws(opts) * n_unconstrained doubles; stats,
 * when non-null, holds chains * stanli_n_stored_draws(opts) * 7 (the
 * CmdStan sampler columns, see stanli_sampler_column_name). Each stored
 * draw is written into these buffers as the chain stores it; the library
 * keeps no copy of the run.
 *
 * Returns 0 when every chain succeeded. Returns the NUMBER OF FAILED
 * CHAINS otherwise, with the first failure's message in err: a run where
 * three of four chains sampled is a partial result the caller may want.
 * A failed chain's rows hold what it stored before it failed, and the rest
 * of its rows are left untouched. Returns -1 with a message in err when the
 * run could not start: bad options, metric or resume records.
 *
 * `refresh` follows CmdStan's convention: in each phase, report the first
 * transition, every refresh-th transition, and the phase's final transition.
 * Zero disables callbacks; negative values are rejected. `reports`, when
 * non-null, holds opts->chains entries.
 *
 * The run sends its own notices to the message sink (the one print() uses),
 * each line prefixed "Chain [id]": a proposal rejected because the model
 * threw, and the adaptation-window warnings. As in CmdStan they do not
 * depend on `refresh`, which controls only the progress callbacks. A
 * positive `refresh` also adds a note when threads_per_chain exceeds 1 on a
 * model that runs no reduce_sum call in parallel, with the reasons from
 * stanli_reduce_sum_fallback.
 *
 * `poll` is asked on the calling thread about every 100 ms. A stopped run
 * returns 0 with `*interrupted` set to 1 and only the rows stored before the
 * stop written; the binding raises its language's interrupt from that. `poll`,
 * `progress` and `interrupted` may be null.
 *
 * Each chain also writes its stored draws through write_array on its own
 * thread as they are stored, so the work runs in parallel across chains,
 * inside the sampling progress. `values` receives
 * chains * stanli_n_stored_draws(opts) rows of W doubles, chain-major exactly
 * like `draws`, where W is stanli_n_output_columns(m). Generated quantities
 * advance the live sampler RNG between stored transitions, as in CmdStan. Thus
 * RNG-bearing models have different seeded trajectories when `values` is null,
 * since generated quantities then do not run. The model's own stanli_wa_seed
 * stream is untouched.
 *
 * A domain error while producing a row fills that output row with NaNs and
 * sampling continues, retaining any consumed randomness. Other exceptions
 * fail the chain and err names its chain and draw.
 *
 * `metric` holds the metric controls (null means the defaults of
 * stanli_metric_opts_init). An invalid metric, inverse metric or step size
 * fails before any chain starts, returns -1, and puts Stan's message in
 * err. */
int stanli_sample_multi_metric(stanli_model* m, const stanli_sample_opts* opts,
                               const stanli_metric_opts* metric, int refresh,
                               double* draws, double* stats, double* values,
                               stanli_sample_progress_cb progress,
                               void* progress_user, stanli_sample_poll_cb poll,
                               void* poll_user, int* interrupted,
                               stanli_sample_report* reports, char* err,
                               size_t err_len);

/* Bytes in one chain's resume record for this model.
 *
 * Every NUTS run of stanli_sample_multi_metric can write one record per chain
 * (stanli_sample_opts::resume_out). A record is opaque and versioned. It holds
 * what continuing the chain needs: the position after its last transition,
 * the RNG state after that transition's generated quantities, the step size,
 * the number of post-warmup transitions so far, and a hash of the model and
 * its data. Passing the records back as resume_in, with warmup 0 and each
 * chain's inverse metric (the adapted_inv_metric of the earlier run) as
 * stanli_metric_opts::inv_metric, continues every chain exactly: 1000 samples
 * then 1000 more give the draws, sampler columns and generated quantities of
 * one run of 2000.
 *
 * A continued run skips initialization and adaptation, ignores seed, inits
 * radius, step_size and adapt_engaged, and numbers its progress from the
 * earlier transitions ("1001 / 2000"). It fails with a message when warmup is
 * not 0, inits is set, a record has the wrong length or version, the records
 * disagree on their transition count, or a record comes from a different
 * model or data. */
size_t stanli_resume_size(const stanli_model* m);

/* The seven sampler columns, in order: lp__, accept_stat__, stepsize__,
 * treedepth__, n_leapfrog__, divergent__, energy__. */
#define STANLI_N_SAMPLER_COLS 7
const char* stanli_sampler_column_name(int i);
int stanli_n_sampler_columns(void);

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

/* "mean", "mcse_mean", "sd", "mcse_sd", "q5", "q50", "q95", "ess_bulk",
 * "ess_tail", "r_hat": the name of STANLI_STAT_* value i, for bindings that
 * cannot read this enum. */
const char* stanli_summary_stat_name(int i);
int stanli_n_summary_stats(void);

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

/* The warnings to print after a stanli_sample_multi_metric run: divergent
 * transitions, transitions that hit the maximum treedepth, and chains whose
 * E-BFMI is below 0.3, in CmdStanR's wording. `opts` is the options the run
 * used, `reports` its per-chain reports and `stats` its sampler columns
 * (STANLI_N_SAMPLER_COLS wide, chain-major, as filled by the run). E-BFMI is
 * taken over the stored post-warmup rows. Writes at most out_len bytes
 * including the terminator and returns the number of bytes the full text
 * needs, 1 when nothing is wrong, 0 on bad arguments. */
int64_t stanli_sampling_warnings(const stanli_sample_opts* opts,
                                 const stanli_sample_report* reports,
                                 const double* stats, char* out,
                                 size_t out_len);

/* The verdict stanli_diagnose_text ends with, as numbers: how many checks
 * failed and how many could not be computed (too few draws, say). Takes
 * the same draws, stats and max_depth. Returns 0 on success. */
int stanli_diagnose_counts(const double* draws, int64_t n_chains,
                           int64_t n_draws, int64_t n_cols, const double* stats,
                           int max_depth, int* n_failed, int* n_unavailable);

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
/* chain_id, iter >= 1, history_size >= 1, init_alpha and the five tolerances
 * > 0, init_radius. jacobian = 0 passes the check and is refused by the run,
 * because stanli cannot compute it. */
int stanli_optimize_opts_check(const stanli_optimize_opts* o, char* err,
                               size_t err_len);

/* Writes the mode to `unconstrained` (n_unconstrained doubles) and, when
 * `values` is non-null, every CSV column there (stanli_n_output_columns). *lp
 * receives the log density at the mode. The start and the generated quantities
 * at the mode draw from the stream of (seed, chain_id), in that order, as in
 * CmdStan; the model's stanli_wa_seed stream is not touched.
 *
 * Returns 0 when the optimizer converged, nonzero otherwise with a
 * message in err -- and in that case the buffers still hold the last
 * point it reached, which is usually what a user wants to look at. */
int stanli_optimize(stanli_model* m, const stanli_optimize_opts* opts,
                    double* unconstrained, double* values, double* lp,
                    char* err, size_t err_len);

/* stanli_optimize, asking `poll` on the calling thread between iterations,
 * about every 100 ms and once before the first. A nonzero answer abandons the
 * run, which returns 0 with *interrupted set to 1 and nothing usable in the
 * outputs. `poll` and `interrupted` may be null. */
int stanli_optimize_with_poll(stanli_model* m, const stanli_optimize_opts* opts,
                              double* unconstrained, double* values, double* lp,
                              stanli_sample_poll_cb poll, void* poll_user,
                              int* interrupted, char* err, size_t err_len);

/* Constrained view: flattened parameter values for one unconstrained q.
 * n_constrained gives the output length; names are "mu", "theta.1", ... */
int64_t stanli_n_constrained(const stanli_model* m);
const char* stanli_constrained_name(const stanli_model* m, int64_t i);

/* The unconstrained vector as declared parameters, in declaration order:
 * block i is stanli_param_block_size(m, i) consecutive entries and belongs to
 * the parameter stanli_param_block_name(m, i). A simplex of K entries is one
 * block of K - 1. A whitened model reports no blocks, because its vector is
 * not in the declared parameters' terms. */
int64_t stanli_n_param_blocks(const stanli_model* m);
const char* stanli_param_block_name(const stanli_model* m, int64_t i);
int64_t stanli_param_block_size(const stanli_model* m, int64_t i);
int stanli_constrain(stanli_model* m, const double* q, double* out);

/* Single-chain NUTS with diagonal-metric adaptation. `cb`, when non-null,
 * fires after every transition. During warmup (`warmup` nonzero) the buffer is
 * untouched; after it, draw `i` is written to `draws` before its callback, so
 * rows [0, i] are readable mid-run. draws must hold samples * n_unconstrained
 * doubles (row-major, one draw per row). `init`, when non-null, is one
 * n_unconstrained vector on the unconstrained scale. On success, `stats`, when
 * non-null, receives samples * STANLI_N_SAMPLER_COLS doubles in draw-major
 * order, excluding warmup; stats are available after return, not during
 * callbacks. The maximum treedepth is 10. Returns 0 on success, nonzero with
 * message in err otherwise. */
typedef void (*stanli_draw_cb)(int32_t i, int32_t warmup, void* user);
int stanli_sample_stream_stats_init(stanli_model* m, uint32_t seed, int warmup,
                                    int samples, double delta,
                                    const double* init, double* draws,
                                    double* stats, stanli_draw_cb cb,
                                    void* user, char* err, size_t err_len);

/* WALNUTS (within-orbit adaptive step-length NUTS, arXiv:2506.18746),
 * same streaming contract as stanli_sample_stream_stats_init. `max_error` is
 * the sampler's tunable in place of NUTS's `delta`: the largest drift in
 * the joint log density allowed across one macro step before the step
 * is halved within the trajectory; pass 0 for the default (0.5). */
int stanli_sample_walnuts_stream(stanli_model* m, uint32_t seed, int warmup,
                                 int samples, double max_error, double* draws,
                                 stanli_draw_cb cb, void* user, char* err,
                                 size_t err_len);

/* WALNUTS with the chain's stream and start chosen as for NUTS: the sampler
 * and the starting point use (seed, chain_id), chain_id >= 0, and `init`,
 * when non-null, is one n_unconstrained start on the unconstrained scale;
 * otherwise the start is drawn uniformly in (-init_radius, init_radius).
 * stanli_sample_walnuts_stream is this call with chain_id 1, init_radius 2,
 * no init and no stats. `stats`, when non-null, receives samples *
 * STANLI_N_SAMPLER_COLS doubles in the NUTS column order after return:
 * lp__ and stepsize__ (the adapted macro step) are set, the other columns
 * are NaN because WALNUTS has no counterpart for them. */
int stanli_sample_walnuts_stats_init(stanli_model* m, uint32_t seed,
                                     int chain_id, int warmup, int samples,
                                     double max_error, double init_radius,
                                     const double* init, double* draws,
                                     double* stats, stanli_draw_cb cb,
                                     void* user, char* err, size_t err_len);

/* Single-path Pathfinder: a normal approximation fitted along an L-BFGS
 * path, drawn from in milliseconds where a sampler takes seconds. Draws
 * come back UNCONSTRAINED, num_draws rows of stanli_n_unconstrained
 * doubles, the same layout stanli_sample_stream_stats_init writes, so the
 * same stanli_wa_row call constrains them.
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

/* Generate one unconstrained NUTS starting point per chain from a single-path
 * Pathfinder approximation. `inits` receives chains * n_unconstrained
 * doubles in chain-major order, ready for stanli_sample_opts::inits. The
 * sampling seed owns reproducibility: Pathfinder uses (seed, chain_id), then
 * returns `chains` draws from the selected approximation. Single-path
 * Pathfinder performs no PSIS resampling.
 *
 * The four tunables are the deliberately small initialization surface. Pass
 * CmdStan defaults (1000, 25, 5, 2) unless the host's dedicated Pathfinder
 * options object overrides them. `cb`, when non-null, observes the L-BFGS
 * path and can provide initialization progress in an interactive caller. */
int stanli_pathfinder_inits(stanli_model* m, uint32_t seed, int chain_id,
                            int chains, int num_iterations, int num_elbo_draws,
                            int history_size, double init_radius, double* inits,
                            stanli_path_cb cb, void* user, char* err,
                            size_t err_len);

/* stanli_pathfinder_inits, asking `poll` as stanli_optimize_with_poll does. */
int stanli_pathfinder_inits_with_poll(stanli_model* m, uint32_t seed,
                                      int chain_id, int chains,
                                      int num_iterations, int num_elbo_draws,
                                      int history_size, double init_radius,
                                      double* inits, stanli_path_cb cb,
                                      void* user, stanli_sample_poll_cb poll,
                                      void* poll_user, int* interrupted,
                                      char* err, size_t err_len);

/* ---- fixed_param, Laplace, Pathfinder, ADVI -------------------------------
 *
 * Stan's own services for its other inference algorithms. They share a
 * contract that differs from the entry points above in one way that matters:
 *
 * DRAWS COME BACK CONSTRAINED. Each row is every CSV column of the model, in
 * CmdStan's order: stanli_n_output_columns(m) doubles, named by
 * stanli_output_column_name. The services draw generated quantities from their
 * own generator between their other draws, so the rows are written as the
 * run goes; that is what makes a seed name the run it names in CmdStan. The
 * model's stanli_wa_seed stream is not touched.
 *
 * Starting points are UNCONSTRAINED, as everywhere else, and a null pointer
 * means a uniform draw within init_radius.
 *
 * `log`, when non-null, receives each line the service logs, on the calling
 * thread: level 0 is information (ADVI's convergence table, progress), 1 a
 * warning (Pathfinder's Pareto k, ADVI stopping at its iteration limit), 2
 * an error. `poll`, when non-null, is
 * asked on the calling thread about every 100 ms; a nonzero answer abandons
 * the run, which then returns 0 with *interrupted set to 1 and nothing
 * usable in the output buffers. `interrupted` may be null.
 *
 * Each returns 0 on success and nonzero with a message in err otherwise.
 * Call the _init function before setting fields: a zeroed struct is not the
 * defaults. */
typedef void (*stanli_log_cb)(int32_t level, const char* text, void* user);

/* The parameters stay at each chain's starting point and only generated
 * quantities are drawn: stan::services::sample::fixed_param, once per chain,
 * chain c on the stream of (seed, chain_id + c). */
typedef struct {
  uint32_t seed;
  int chains;
  int chain_id;
  int samples; /* iterations; ceil(samples / thin) rows are stored */
  int thin;
  double init_radius;
  const double* inits; /* chains * n_unconstrained, or null */
  int refresh;         /* log progress every `refresh` iterations; 0 = never */
} stanli_fixed_param_opts;

/* seed 1, 4 chains from id 1, 1000 samples, thin 1, radius 2, no progress. */
void stanli_fixed_param_opts_init(stanli_fixed_param_opts* o);
/* chains >= 1, chain_id, samples >= 0, thin >= 1, init_radius,
 * refresh >= 0. */
int stanli_fixed_param_opts_check(const stanli_fixed_param_opts* o, char* err,
                                  size_t err_len);
/* Stored rows per chain under these options. */
int64_t stanli_fixed_param_n_draws(const stanli_fixed_param_opts* o);
/* `values` holds chains * stanli_fixed_param_n_draws(opts) rows, chain-major.
 * There are no per-draw diagnostics: lp__ is zero by definition. A row whose
 * generated quantities are rejected is NaN, as in CmdStan. `seconds`, when
 * non-null, receives each chain's wall time, opts->chains doubles. */
int stanli_fixed_param(stanli_model* m, const stanli_fixed_param_opts* opts,
                       double* values, double* seconds, stanli_log_cb log,
                       void* log_user, stanli_sample_poll_cb poll,
                       void* poll_user, int* interrupted, char* err,
                       size_t err_len);

/* Draws from the normal approximation at a mode, whose covariance is the
 * inverse of the negative Hessian there: stan::services::laplace_sample. */
typedef struct {
  uint32_t seed;
  int draws;
  int jacobian;     /* must be 1, for the reason stanli_optimize's must */
  int calculate_lp; /* 0 leaves lp as NaN and saves an evaluation per draw */
  int refresh;
} stanli_laplace_opts;

/* seed 1, 1000 draws, jacobian 1, calculate_lp 1, no progress. */
void stanli_laplace_opts_init(stanli_laplace_opts* o);
/* draws >= 1, refresh >= 0. */
int stanli_laplace_opts_check(const stanli_laplace_opts* o, char* err,
                              size_t err_len);
/* `mode` is n_unconstrained doubles, normally stanli_optimize's
 * `unconstrained` from a run with jacobian = 1. `values` holds opts->draws
 * rows. `lp` and `lp_approx`, each opts->draws doubles or null, receive the
 * model's log density at each draw and the approximation's unnormalized log
 * density (CmdStan's log_p__ and log_q__). */
int stanli_laplace_sample(stanli_model* m, const stanli_laplace_opts* opts,
                          const double* mode, double* values, double* lp,
                          double* lp_approx, stanli_log_cb log, void* log_user,
                          stanli_sample_poll_cb poll, void* poll_user,
                          int* interrupted, char* err, size_t err_len);

/* Single-path Pathfinder with constrained rows:
 * stan::services::pathfinder::pathfinder_lbfgs_single. For unconstrained
 * draws, the L-BFGS path and k-hat, see stanli_run_pathfinder above.
 *
 * MULTI-PATH PATHFINDER IS NOT AVAILABLE in this version. Stan 2.40's
 * multi-path service reads past the end of an array while resampling, so
 * num_paths must be 1 and any other value is refused with a message saying
 * so. num_psis_draws and psis_resample belong to multi-path only; they are
 * not read, and stay in the struct so its layout does not change when
 * multi-path returns. */
typedef struct {
  uint32_t seed;
  int chain_id;
  int num_paths;      /* must be 1 */
  int num_draws;      /* draws taken from the approximation */
  int num_psis_draws; /* unused: multi-path only */
  int num_elbo_draws;
  int max_lbfgs_iters;
  int history_size;
  double init_alpha;
  double tol_obj;
  double tol_rel_obj;
  double tol_grad;
  double tol_rel_grad;
  double tol_param;
  double init_radius;
  const double* inits; /* n_unconstrained, or null */
  int psis_resample;   /* unused: multi-path only */
  int calculate_lp;    /* 0 leaves lp as NaN beyond the first
                        * num_elbo_draws draws, which Stan has scored */
  int refresh;
} stanli_pathfinder_opts;

/* seed 1, chain id 1, one path, 1000 draws, 25 ELBO draws, 1000 L-BFGS
 * iterations, history 5, L-BFGS tolerances as in stanli_optimize_opts,
 * radius 2, lp on. */
void stanli_pathfinder_opts_init(stanli_pathfinder_opts* o);
/* chain_id, num_paths, num_draws, num_elbo_draws, max_lbfgs_iters and
 * history_size >= 1, init_alpha and the tolerances > 0, init_radius,
 * refresh >= 0. num_paths other than 1 passes and is refused by the run. */
int stanli_pathfinder_opts_check(const stanli_pathfinder_opts* o, char* err,
                                 size_t err_len);
/* The rows a run under these options returns: num_draws, or 0 for options
 * the run would refuse. */
int64_t stanli_pathfinder_max_draws(const stanli_pathfinder_opts* o);
/* `values` holds stanli_pathfinder_max_draws(opts) rows; `lp`, `lp_approx`
 * and `path` that many doubles each, or null. *n_draws receives the rows
 * written. `path` is CmdStan's path__, which is chain_id on every row. */
int stanli_pathfinder(stanli_model* m, const stanli_pathfinder_opts* opts,
                      double* values, double* lp, double* lp_approx,
                      double* path, int64_t* n_draws, stanli_log_cb log,
                      void* log_user, stanli_sample_poll_cb poll,
                      void* poll_user, int* interrupted, char* err,
                      size_t err_len);

/* ADVI: stan::services::experimental::advi::meanfield or fullrank. */
#define STANLI_ADVI_MEANFIELD 0
#define STANLI_ADVI_FULLRANK 1
typedef struct {
  uint32_t seed;
  int chain_id;
  int algorithm; /* STANLI_ADVI_MEANFIELD or STANLI_ADVI_FULLRANK */
  int iter;      /* maximum iterations */
  int grad_samples;
  int elbo_samples;
  double eta;
  int adapt_engaged;
  int adapt_iter;
  double tol_rel_obj;
  int eval_elbo;
  int output_samples;
  double init_radius;
  const double* init; /* n_unconstrained, or null */
} stanli_variational_opts;

/* CmdStan's defaults: seed 1, chain id 1, meanfield, 10000 iterations, 1
 * gradient sample, 100 ELBO samples, eta 1 with adaptation on for 50
 * iterations, tol_rel_obj 0.01, ELBO every 100 iterations, 1000 output
 * samples, radius 2. */
void stanli_variational_opts_init(stanli_variational_opts* o);
/* chain_id, a known algorithm, iter, grad_samples, elbo_samples, adapt_iter,
 * eval_elbo and output_samples >= 1, eta and tol_rel_obj > 0, init_radius. */
int stanli_variational_opts_check(const stanli_variational_opts* o, char* err,
                                  size_t err_len);
/* `mean`, one row or null, receives the columns at the mean of the
 * approximation, which is the row Stan writes ahead of the draws. `values`
 * holds opts->output_samples rows. `lp` and `lp_approx`, each
 * opts->output_samples doubles or null, receive Stan's log_p__ and log_g__.
 * Running out of iterations is not a failure: Stan says so through `log` and
 * returns the approximation it reached. */
int stanli_variational(stanli_model* m, const stanli_variational_opts* opts,
                       double* mean, double* values, double* lp,
                       double* lp_approx, stanli_log_cb log, void* log_user,
                       stanli_sample_poll_cb poll, void* poll_user,
                       int* interrupted, char* err, size_t err_len);

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
/* The row every entry point writes (`values` of the samplers and optimize,
 * the rows of fixed_param, Laplace, Pathfinder and ADVI): the write_array
 * columns when stanli_wa_n_columns is positive, the constrained parameters
 * otherwise. Use these instead of choosing between the two. */
int64_t stanli_n_output_columns(const stanli_model* m);
const char* stanli_output_column_name(const stanli_model* m, int64_t i);
/* Index of the first generated-quantity column in the write_array output.
 * It equals n_columns when there are no generated quantities in that output.
 * Returns 0 when write_array is unavailable. */
int64_t stanli_wa_n_generated_start(const stanli_model* m);
const char* stanli_wa_column_name(const stanli_model* m, int64_t i);
void stanli_wa_seed(stanli_model* m, uint32_t seed);
void stanli_wa_seed_chain(stanli_model* m, uint32_t seed, uint32_t chain);
int stanli_wa_row(stanli_model* m, const double* q, double* out);
/* Empty unless parts of the model have no compiled path and run through
 * the MIR interpreter. Then a message to show once per model: which parts,
 * why, and where to report it. Owned by the model. */
const char* stanli_warnings(const stanli_model* m);
/* The compiler's messages for a model built from Stan source, as
 * stanli_compilation_message* report them; none for a model built from MIR.
 * i must satisfy 0 <= i < stanli_model_n_compiler_messages(m). Strings are
 * owned by the model. */
int64_t stanli_model_n_compiler_messages(const stanli_model* m);
stanli_compiler_message_kind stanli_model_compiler_message_kind(
    const stanli_model* m, int64_t i);
const char* stanli_model_compiler_message(const stanli_model* m, int64_t i);

/* The declared variables behind a set of output columns, in column order.
 * Variable i covers the product of its dims consecutive columns, first index
 * fastest (CmdStan's order); a scalar has no dims and one column. `set` is
 * STANLI_CONSTRAINED_COLUMNS (stanli_constrained_name),
 * STANLI_WRITE_ARRAY_COLUMNS (stanli_wa_column_name) or STANLI_OUTPUT_COLUMNS
 * (stanli_output_column_name). Strings are owned by the model; out-of-range
 * indices return 0 or null. */
#define STANLI_CONSTRAINED_COLUMNS 0
#define STANLI_WRITE_ARRAY_COLUMNS 1
#define STANLI_OUTPUT_COLUMNS 2
int64_t stanli_n_variables(const stanli_model* m, int set);
const char* stanli_variable_name(const stanli_model* m, int set, int64_t i);
int64_t stanli_variable_n_dims(const stanli_model* m, int set, int64_t i);
int64_t stanli_variable_dim(const stanli_model* m, int set, int64_t i,
                            int64_t k);
/* The declaration annotations on variable i, by name without the `@` and in
 * source order (`@latent` is "latent"). Only parameters appear in
 * STANLI_CONSTRAINED_COLUMNS; STANLI_WRITE_ARRAY_COLUMNS also lists
 * transformed parameters and generated quantities. Strings are owned by the
 * model; out-of-range indices return 0 or null. */
int64_t stanli_variable_n_annotations(const stanli_model* m, int set,
                                      int64_t i);
const char* stanli_variable_annotation(const stanli_model* m, int set,
                                       int64_t i, int64_t k);

/* The layout of every struct this header declares, for bindings that mirror
 * them and cannot include it (R's C glue copies them, Python declares ctypes
 * structures). Entry i names a struct and one of its fields; a binding
 * compares each field it mirrors, and each struct's size, at load and refuses
 * to run on any difference. Static storage. */
typedef struct {
  const char* type;
  const char* field;
  size_t offset;
  size_t size;
  size_t type_size;
} stanli_field_layout;
int64_t stanli_n_field_layouts(void);
const stanli_field_layout* stanli_field_layouts(void);
/* 1 if transformed data drew from the construction seed, so this model is a
 * function of that seed: a binding that runs everything under one seed the
 * way CmdStan does rebuilds with the run seed when this is set, and never
 * otherwise. Generated-quantities draws do not count. */
int stanli_transformed_data_rng(const stanli_model* m);
/* The parts of the model stanli_warnings is about, one entry each: what
 * runs through the MIR interpreter and why. Owned by the model. */
int64_t stanli_n_interpreter_fallbacks(const stanli_model* m);
const char* stanli_interpreter_fallback(const stanli_model* m, int64_t i);

#ifdef __cplusplus
}
#endif

#endif
