/* R <-> stanli C ABI.
 *
 * The stanli runtime is a ~16 MB shared library. CRAN builds its own
 * binaries from source and will not carry one that size, so the library
 * is not linked here -- it is dlopen'd at runtime from wherever
 * stanli_install() put it, and every entry point is resolved by name.
 * That is the same shape torch's CRAN package uses for libtorch, and it
 * is why this file has no stan-math in it and compiles in seconds.
 *
 * One consequence worth stating: nothing here is callable until
 * stanli_bridge_load() has succeeded, so every wrapper checks and errors
 * with a message naming the missing piece rather than dereferencing null.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include <R.h>
#include <Rinternals.h>

/* Mirrors stanli_sample_opts in runtime/include/stanli/capi.h. Field
 * order and types must match exactly; R never sees this struct, but a
 * mismatch would be a silent misread rather than an error. */
typedef struct {
  uint32_t seed;
  int chains;
  int chain_id;
  int warmup;
  int samples;
  int thin;
  double delta;
  int max_depth;
  int save_warmup;
  double init_radius;
  const double *inits;
  int num_threads;
} stanli_sample_opts;

typedef struct {
  uint32_t seed;
  int chain_id;
  int iter;
  int jacobian;
  double init_alpha;
  double tol_obj;
  double tol_rel_obj;
  double tol_grad;
  double tol_rel_grad;
  double tol_param;
  int history_size;
  double init_radius;
  const double *init;
} stanli_optimize_opts;

static void *g_lib = NULL;

/* Every entry point the R side uses. */
static void *(*p_model_new_from_stan)(const char *, const char *, char *,
                                      size_t);
static void *(*p_model_new)(const char *, const char *, char *, size_t);
static void (*p_model_free)(void *);
static int (*p_has_embedded_stanc)(void);
static int (*p_exact_lp)(void);
static int (*p_thread_safe)(void);
static int64_t (*p_n_unconstrained)(const void *);
static int (*p_grad)(void *, const double *, double *, double *);
static int64_t (*p_n_constrained)(const void *);
static const char *(*p_constrained_name)(const void *, int64_t);
static int (*p_constrain)(void *, const double *, double *);
static void (*p_sample_opts_init)(stanli_sample_opts *);
static int64_t (*p_n_stored_draws)(const stanli_sample_opts *);
static int (*p_sample_multi)(void *, const stanli_sample_opts *, double *,
                             double *, char *, size_t);
static const char *(*p_sampler_column_name)(int);
static int (*p_summary_stats)(const double *, int64_t, int64_t, int64_t,
                              double *);
static int64_t (*p_diagnose_text)(const double *, int64_t, int64_t, int64_t,
                                  const char *const *, const double *, int,
                                  char *, size_t);
static int64_t (*p_wa_n_columns)(const void *);
static const char *(*p_wa_column_name)(const void *, int64_t);
static void (*p_wa_seed)(void *, uint32_t);
static int (*p_wa_row)(void *, const double *, double *);
static void (*p_optimize_opts_init)(stanli_optimize_opts *);
static int (*p_optimize)(void *, const stanli_optimize_opts *, double *,
                         double *, double *, char *, size_t);

#define BIND(fn, var)                                            \
  do {                                                           \
    *(void **)(&var) = dlsym(g_lib, fn);                         \
    if (var == NULL) {                                           \
      dlclose(g_lib);                                            \
      g_lib = NULL;                                              \
      return mkString("missing symbol in stanli library: " fn);   \
    }                                                            \
  } while (0)

SEXP stanli_bridge_load(SEXP path) {
  if (g_lib != NULL) return mkString("");
  g_lib = dlopen(CHAR(STRING_ELT(path, 0)), RTLD_NOW | RTLD_LOCAL);
  if (g_lib == NULL) return mkString(dlerror());

  BIND("stanli_model_new_from_stan", p_model_new_from_stan);
  BIND("stanli_model_new", p_model_new);
  BIND("stanli_model_free", p_model_free);
  BIND("stanli_has_embedded_stanc", p_has_embedded_stanc);
  BIND("stanli_exact_lp", p_exact_lp);
  BIND("stanli_thread_safe", p_thread_safe);
  BIND("stanli_n_unconstrained", p_n_unconstrained);
  BIND("stanli_grad", p_grad);
  BIND("stanli_n_constrained", p_n_constrained);
  BIND("stanli_constrained_name", p_constrained_name);
  BIND("stanli_constrain", p_constrain);
  BIND("stanli_sample_opts_init", p_sample_opts_init);
  BIND("stanli_n_stored_draws", p_n_stored_draws);
  BIND("stanli_sample_multi", p_sample_multi);
  BIND("stanli_sampler_column_name", p_sampler_column_name);
  BIND("stanli_summary_stats", p_summary_stats);
  BIND("stanli_diagnose_text", p_diagnose_text);
  BIND("stanli_wa_n_columns", p_wa_n_columns);
  BIND("stanli_wa_column_name", p_wa_column_name);
  BIND("stanli_wa_seed", p_wa_seed);
  BIND("stanli_wa_row", p_wa_row);
  BIND("stanli_optimize_opts_init", p_optimize_opts_init);
  BIND("stanli_optimize", p_optimize);
  return mkString("");
}

SEXP stanli_bridge_loaded(void) { return ScalarLogical(g_lib != NULL); }

static void require_loaded(void) {
  if (g_lib == NULL)
    error("the stanli runtime is not loaded; call stanli_install() once, "
          "then restart or call stanli:::load_runtime()");
}

/* The model handle is an external pointer with a finalizer, so a model
 * that goes out of scope in R frees its arenas rather than leaking them
 * until the session ends. */
static void model_finalizer(SEXP ext) {
  void *m = R_ExternalPtrAddr(ext);
  if (m != NULL && p_model_free != NULL) {
    p_model_free(m);
    R_ClearExternalPtr(ext);
  }
}

static void *model_ptr(SEXP ext) {
  void *m = R_ExternalPtrAddr(ext);
  if (m == NULL) error("stanli model handle is no longer valid");
  return m;
}

SEXP stanli_r_model_new(SEXP code, SEXP data_json, SEXP is_mir) {
  require_loaded();
  char err[8192];
  err[0] = '\0';
  void *m;
  if (asLogical(is_mir))
    m = p_model_new(CHAR(STRING_ELT(code, 0)), CHAR(STRING_ELT(data_json, 0)),
                    err, sizeof err);
  else
    m = p_model_new_from_stan(CHAR(STRING_ELT(code, 0)),
                              CHAR(STRING_ELT(data_json, 0)), err, sizeof err);
  if (m == NULL) error("%s", err[0] ? err : "stanli: model compilation failed");
  SEXP ext = PROTECT(R_MakeExternalPtr(m, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(ext, model_finalizer, TRUE);
  UNPROTECT(1);
  return ext;
}

SEXP stanli_r_has_embedded_stanc(void) {
  require_loaded();
  return ScalarLogical(p_has_embedded_stanc());
}
SEXP stanli_r_exact_lp(void) {
  require_loaded();
  return ScalarLogical(p_exact_lp());
}
SEXP stanli_r_thread_safe(void) {
  require_loaded();
  return ScalarLogical(p_thread_safe());
}

SEXP stanli_r_n_unconstrained(SEXP m) {
  require_loaded();
  return ScalarInteger((int)p_n_unconstrained(model_ptr(m)));
}

SEXP stanli_r_column_names(SEXP m) {
  require_loaded();
  void *mm = model_ptr(m);
  /* write_array columns when the model has generated quantities, the
   * constrained parameters otherwise -- the same rule the Python
   * wrapper follows, so both report the same CSV. */
  int64_t n = p_wa_n_columns(mm);
  int wa = n > 0;
  if (!wa) n = p_n_constrained(mm);
  SEXP out = PROTECT(allocVector(STRSXP, (R_xlen_t)n));
  for (int64_t i = 0; i < n; ++i) {
    const char *s = wa ? p_wa_column_name(mm, i) : p_constrained_name(mm, i);
    SET_STRING_ELT(out, (R_xlen_t)i, mkChar(s ? s : ""));
  }
  UNPROTECT(1);
  return out;
}

SEXP stanli_r_grad(SEXP m, SEXP q) {
  require_loaded();
  void *mm = model_ptr(m);
  const int64_t n = p_n_unconstrained(mm);
  if (XLENGTH(q) != n)
    error("q has %lld elements, model has %lld unconstrained parameters",
          (long long)XLENGTH(q), (long long)n);
  SEXP grad = PROTECT(allocVector(REALSXP, (R_xlen_t)n));
  double lp = 0;
  const int rc = p_grad(mm, REAL(q), &lp, REAL(grad));
  if (rc != 0) {
    UNPROTECT(1);
    error("log density evaluation failed at this point (domain error in a "
          "distribution or function)");
  }
  SEXP out = PROTECT(allocVector(VECSXP, 2));
  SET_VECTOR_ELT(out, 0, ScalarReal(lp));
  SET_VECTOR_ELT(out, 1, grad);
  SEXP nm = PROTECT(allocVector(STRSXP, 2));
  SET_STRING_ELT(nm, 0, mkChar("lp"));
  SET_STRING_ELT(nm, 1, mkChar("grad"));
  setAttrib(out, R_NamesSymbol, nm);
  UNPROTECT(3);
  return out;
}

/* opts arrives from R as a named list; unpacking it here keeps the field
 * order in one place (this file) rather than in R as well. */
static void fill_sample_opts(stanli_sample_opts *o, SEXP l) {
  p_sample_opts_init(o);
  o->seed = (uint32_t)asInteger(VECTOR_ELT(l, 0));
  o->chains = asInteger(VECTOR_ELT(l, 1));
  o->warmup = asInteger(VECTOR_ELT(l, 2));
  o->samples = asInteger(VECTOR_ELT(l, 3));
  o->thin = asInteger(VECTOR_ELT(l, 4));
  o->delta = asReal(VECTOR_ELT(l, 5));
  o->max_depth = asInteger(VECTOR_ELT(l, 6));
  o->save_warmup = asLogical(VECTOR_ELT(l, 7));
  o->init_radius = asReal(VECTOR_ELT(l, 8));
  o->num_threads = asInteger(VECTOR_ELT(l, 9));
}

SEXP stanli_r_sample(SEXP m, SEXP optlist, SEXP inits) {
  require_loaded();
  void *mm = model_ptr(m);
  stanli_sample_opts o;
  fill_sample_opts(&o, optlist);
  if (XLENGTH(inits) > 0) o.inits = REAL(inits);

  const int64_t n = p_n_unconstrained(mm);
  const int64_t rows = p_n_stored_draws(&o);
  const int64_t nchain = o.chains > 0 ? o.chains : 1;

  SEXP raw = PROTECT(allocVector(REALSXP, (R_xlen_t)(nchain * rows * n)));
  SEXP stats = PROTECT(allocVector(REALSXP, (R_xlen_t)(nchain * rows * 7)));
  char err[4096];
  err[0] = '\0';
  const int failed =
      p_sample_multi(mm, &o, REAL(raw), REAL(stats), err, sizeof err);
  if (failed) {
    UNPROTECT(2);
    error("%d of %d chains failed; first: %s", failed, (int)nchain,
          err[0] ? err : "(no message)");
  }

  /* Constrain every stored draw into the CSV columns, per chain, so the
   * RNG stream in generated quantities differs by chain the way CmdStan's
   * does. */
  int64_t ncol = p_wa_n_columns(mm);
  const int wa = ncol > 0;
  if (!wa) ncol = p_n_constrained(mm);
  SEXP vals = PROTECT(allocVector(REALSXP, (R_xlen_t)(nchain * rows * ncol)));
  double *vp = REAL(vals);
  double *rp = REAL(raw);
  double *row = (double *)R_alloc((size_t)ncol, sizeof(double));
  for (int64_t c = 0; c < nchain; ++c) {
    if (wa) p_wa_seed(mm, (uint32_t)(o.seed + c));
    for (int64_t i = 0; i < rows; ++i) {
      const double *q = rp + (c * rows + i) * n;
      const int rc = wa ? p_wa_row(mm, q, row) : p_constrain(mm, q, row);
      if (rc != 0) {
        UNPROTECT(3);
        error("failed to write draw %lld of chain %lld", (long long)i,
              (long long)c);
      }
      memcpy(vp + (c * rows + i) * ncol, row, sizeof(double) * (size_t)ncol);
    }
  }

  SEXP out = PROTECT(allocVector(VECSXP, 5));
  SET_VECTOR_ELT(out, 0, vals);
  SET_VECTOR_ELT(out, 1, stats);
  SET_VECTOR_ELT(out, 2, ScalarInteger((int)nchain));
  SET_VECTOR_ELT(out, 3, ScalarInteger((int)rows));
  SET_VECTOR_ELT(out, 4, raw);
  SEXP nm = PROTECT(allocVector(STRSXP, 5));
  SET_STRING_ELT(nm, 0, mkChar("values"));
  SET_STRING_ELT(nm, 1, mkChar("stats"));
  SET_STRING_ELT(nm, 2, mkChar("chains"));
  SET_STRING_ELT(nm, 3, mkChar("draws"));
  SET_STRING_ELT(nm, 4, mkChar("unconstrained"));
  setAttrib(out, R_NamesSymbol, nm);
  UNPROTECT(5);
  return out;
}

SEXP stanli_r_sampler_columns(void) {
  require_loaded();
  SEXP out = PROTECT(allocVector(STRSXP, 7));
  for (int i = 0; i < 7; ++i)
    SET_STRING_ELT(out, i, mkChar(p_sampler_column_name(i)));
  UNPROTECT(1);
  return out;
}

SEXP stanli_r_summary(SEXP draws, SEXP dims) {
  require_loaded();
  const int64_t nchain = INTEGER(dims)[0];
  const int64_t ndraw = INTEGER(dims)[1];
  const int64_t ncol = INTEGER(dims)[2];
  SEXP out = PROTECT(allocVector(REALSXP, (R_xlen_t)(ncol * 10)));
  if (p_summary_stats(REAL(draws), nchain, ndraw, ncol, REAL(out)) != 0) {
    UNPROTECT(1);
    error("stanli: summary failed");
  }
  UNPROTECT(1);
  return out;
}

SEXP stanli_r_diagnose(SEXP draws, SEXP dims, SEXP names, SEXP stats,
                       SEXP max_depth) {
  require_loaded();
  const int64_t nchain = INTEGER(dims)[0];
  const int64_t ndraw = INTEGER(dims)[1];
  const int64_t ncol = INTEGER(dims)[2];
  const char **nm =
      (const char **)R_alloc((size_t)ncol, sizeof(const char *));
  for (int64_t i = 0; i < ncol; ++i) nm[i] = CHAR(STRING_ELT(names, i));
  const int64_t need = p_diagnose_text(REAL(draws), nchain, ndraw, ncol, nm,
                                       REAL(stats), asInteger(max_depth), NULL,
                                       0);
  char *buf = (char *)R_alloc((size_t)(need > 0 ? need : 1), 1);
  p_diagnose_text(REAL(draws), nchain, ndraw, ncol, nm, REAL(stats),
                  asInteger(max_depth), buf, (size_t)(need > 0 ? need : 1));
  return mkString(buf);
}

SEXP stanli_r_optimize(SEXP m, SEXP optlist, SEXP init) {
  require_loaded();
  void *mm = model_ptr(m);
  stanli_optimize_opts o;
  p_optimize_opts_init(&o);
  o.seed = (uint32_t)asInteger(VECTOR_ELT(optlist, 0));
  o.iter = asInteger(VECTOR_ELT(optlist, 1));
  o.jacobian = asLogical(VECTOR_ELT(optlist, 2));
  o.init_radius = asReal(VECTOR_ELT(optlist, 3));
  if (XLENGTH(init) > 0) o.init = REAL(init);

  const int64_t n = p_n_unconstrained(mm);
  int64_t ncol = p_wa_n_columns(mm);
  if (ncol == 0) ncol = p_n_constrained(mm);
  SEXP q = PROTECT(allocVector(REALSXP, (R_xlen_t)n));
  SEXP vals = PROTECT(allocVector(REALSXP, (R_xlen_t)ncol));
  double lp = 0;
  char err[4096];
  err[0] = '\0';
  if (p_optimize(mm, &o, REAL(q), REAL(vals), &lp, err, sizeof err) != 0) {
    UNPROTECT(2);
    error("optimize failed: %s", err[0] ? err : "(no message)");
  }
  SEXP out = PROTECT(allocVector(VECSXP, 3));
  SET_VECTOR_ELT(out, 0, vals);
  SET_VECTOR_ELT(out, 1, q);
  SET_VECTOR_ELT(out, 2, ScalarReal(lp));
  SEXP nm = PROTECT(allocVector(STRSXP, 3));
  SET_STRING_ELT(nm, 0, mkChar("values"));
  SET_STRING_ELT(nm, 1, mkChar("unconstrained"));
  SET_STRING_ELT(nm, 2, mkChar("lp"));
  setAttrib(out, R_NamesSymbol, nm);
  UNPROTECT(4);
  return out;
}
