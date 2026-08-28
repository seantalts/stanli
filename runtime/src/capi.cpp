#include <stanli/capi.h>

#include <stanli/compile.hpp>
#include <stanli/diagnose.hpp>
#include <stanli/estimate.hpp>
#include <stanli/graph.hpp>
#include <stanli/nuts.hpp>
#include <stanli/optable.hpp>
#include <stanli/walnuts.hpp>
#include <stanli/wa_interp.hpp>

#include "build_id.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

void put_err(char* err, size_t err_len, const char* what) {
  if (err == nullptr || err_len == 0) return;
  std::strncpy(err, what, err_len - 1);
  err[err_len - 1] = '\0';
}

}  // namespace

struct stanli_model {
  stanli::CompiledModel cm;  // graph moved out into ex
  std::unique_ptr<stanli::Executor> ex;
  std::vector<std::string> flat_names;  // constrained, flattened
  int64_t n_con = 0;
  // write_array: either a second executor over the write_array graph, or
  // the per-draw interpreter for the sections the graph cannot express.
  std::unique_ptr<stanli::Executor> wa_ex;
  std::vector<stanli::CompiledModel::ParamView> wa_cols;
  std::shared_ptr<stanli::WaInterp> wa_interp;
  std::vector<std::string> wa_names;  // CSV order, flattened
  int64_t wa_n = 0;
  // The stream stanli_wa_seed sets and stanli_wa_row draws from. This ABI
  // names one stream per model, so the model holds it; callers wanting a
  // stream per thread use WaInterp::eval directly with their own WaRng.
  // Seeded rather than default-constructed so a caller who never calls
  // stanli_wa_seed still gets the same rows every run.
  stanli::WaRng wa_rng{1};
};

namespace {

// One interpreted write_array row: the main executor's forward pass
// supplies the constrained parameter values by name.
std::vector<double> interp_wa_row(stanli_model& m, const double* q,
                                  stanli::WaRng& rng) {
  stanli::Executor& ex = *m.ex;
  std::memcpy(ex.params_data(), q, sizeof(double) * ex.n_params());
  ex.run_forward_only();
  return m.wa_interp->eval(m.cm.constrained_env(ex), rng);
}

}  // namespace

extern "C" {

stanli_model* stanli_model_new(const char* tmir_sexp, const char* data_json,
                               char* err, size_t err_len) {
  try {
    auto m = std::make_unique<stanli_model>();
    stanli::DataMap data = stanli::DataMap::from_json(data_json);
    m->cm = stanli::compile_model(tmir_sexp, data);
    m->ex = std::make_unique<stanli::Executor>(std::move(m->cm.graph));
    m->cm.bind(*m->ex);
    for (const auto& v : m->cm.views) m->n_con += v.len;
    m->flat_names = stanli::CompiledModel::csv_names(m->cm.views);
    if (m->cm.write_array) {
      auto& wa = *m->cm.write_array;
      if (wa.interp) {
        // Column discovery needs one evaluation; walk the probe points
        // and disable write_array only if every one fails.
        m->wa_interp = wa.interp;
        std::vector<double> q(m->ex->n_params());
        bool found = false;
        for (int variant = 0; variant < 3 && !found; ++variant) {
          for (size_t i = 0; i < q.size(); ++i)
            q[i] = stanli::wa_probe_point((int64_t)i, variant);
          try {
            // A scratch stream: discovery is the runtime probing the
            // model, not a draw the caller asked for, so it must not
            // advance the stream stanli_wa_seed names.
            stanli::WaRng probe_rng(1);
            const auto row = interp_wa_row(*m, q.data(), probe_rng);
            m->wa_n = (int64_t)row.size();
            m->wa_names =
                stanli::CompiledModel::csv_names(m->wa_interp->columns());
            found = true;
          } catch (const std::exception&) {
          }
        }
        if (!found) m->wa_interp.reset();
      } else if (!wa.columns.empty()) {
        m->wa_ex = std::make_unique<stanli::Executor>(std::move(wa.graph));
        wa.bind(*m->wa_ex);
        m->wa_cols = wa.columns;
        m->wa_names = stanli::CompiledModel::csv_names(wa.columns);
        for (const auto& c : wa.columns) m->wa_n += c.len;
      }
    }
    return m.release();
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return nullptr;
  }
}

#ifdef STANLI_EMBED_STANC
extern "C" char* stanli_stanc_tmir(const char* stan_code);
extern "C" void stanli_stanc_free(char* p);
#endif

stanli_model* stanli_model_new_from_stan(const char* stan_code,
                                         const char* data_json, char* err,
                                         size_t err_len) {
#ifdef STANLI_EMBED_STANC
  char* res = stanli_stanc_tmir(stan_code);
  if (std::strncmp(res, "OK", 2) != 0) {
    put_err(err, err_len, res + (std::strncmp(res, "ERR", 3) == 0 ? 3 : 0));
    stanli_stanc_free(res);
    return nullptr;
  }
  stanli_model* m = stanli_model_new(res + 2, data_json, err, err_len);
  stanli_stanc_free(res);
  return m;
#else
  (void)stan_code;
  (void)data_json;
  put_err(err, err_len, "this build does not embed stanc3");
  return nullptr;
#endif
}

char* stanli_stan_to_mir(const char* stan_code, char* err, size_t err_len) {
#ifdef STANLI_EMBED_STANC
  char* res = stanli_stanc_tmir(stan_code);
  if (std::strncmp(res, "OK", 2) != 0) {
    put_err(err, err_len, res + (std::strncmp(res, "ERR", 3) == 0 ? 3 : 0));
    stanli_stanc_free(res);
    return nullptr;
  }
  // Hand back the MIR alone, on its own allocation: the caller frees this
  // with stanli_string_free and never sees the "OK" the OCaml side
  // prefixes its result with.
  const size_t n = std::strlen(res + 2);
  char* out = (char*)std::malloc(n + 1);
  if (out != nullptr) std::memcpy(out, res + 2, n + 1);
  stanli_stanc_free(res);
  if (out == nullptr) put_err(err, err_len, "out of memory");
  return out;
#else
  (void)stan_code;
  put_err(err, err_len, "this build does not embed stanc3");
  return nullptr;
#endif
}

void stanli_string_free(char* p) { std::free(p); }

const char* stanli_build_id(void) { return stanli::runtime_build_id(); }

int stanli_abi_version(void) { return STANLI_ABI_VERSION; }

int stanli_has_embedded_stanc(void) {
#ifdef STANLI_EMBED_STANC
  return 1;
#else
  return 0;
#endif
}

int stanli_exact_lp(void) { return stanli::exact_lp_build() ? 1 : 0; }

void stanli_model_free(stanli_model* m) { delete m; }

int64_t stanli_n_unconstrained(const stanli_model* m) {
  return m->ex->n_params();
}

int stanli_grad(stanli_model* m, const double* q, double* lp, double* grad) {
  const int64_t n = m->ex->n_params();
  std::memcpy(m->ex->params_data(), q, sizeof(double) * n);
  try {
    if (grad == nullptr) {
      *lp = m->ex->forward();
    } else {
      *lp = m->ex->gradient(grad);
    }
    return 0;
  } catch (const std::exception&) {
    *lp = -std::numeric_limits<double>::infinity();
    return 1;
  }
}

int stanli_sample(stanli_model* m, uint32_t seed, int warmup, int samples,
                  double delta, double* draws, char* err, size_t err_len) {
  return stanli_sample_stream(m, seed, warmup, samples, delta, draws, nullptr,
                              nullptr, err, err_len);
}

int stanli_sample_stream(stanli_model* m, uint32_t seed, int warmup,
                         int samples, double delta, double* draws,
                         stanli_draw_cb cb, void* user, char* err,
                         size_t err_len) {
  try {
    stanli::NutsConfig cfg;
    cfg.seed = seed;
    cfg.warmup = warmup;
    cfg.samples = samples;
    cfg.delta = delta;
    const int64_t n = m->ex->n_params();
    stanli::DrawObserver observe;
    if (cb) {
      // Each post-warmup draw lands in the caller's buffer before its
      // callback fires, so a streaming consumer can read rows [0, i].
      observe = [&](int64_t i, bool wu, const double* q) {
        if (!wu) std::memcpy(draws + i * n, q, sizeof(double) * n);
        cb((int32_t)i, wu ? 1 : 0, user);
      };
    }
    auto out = stanli::run_nuts(*m->ex, cfg, nullptr, observe);
    for (size_t s = 0; s < out.size(); ++s)
      std::memcpy(draws + s * n, out[s].data(), sizeof(double) * n);
    return 0;
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return 1;
  }
}

int stanli_sample_walnuts_stream(stanli_model* m, uint32_t seed, int warmup,
                                 int samples, double max_error, double* draws,
                                 stanli_draw_cb cb, void* user, char* err,
                                 size_t err_len) {
  try {
    stanli::WalnutsConfig cfg;
    cfg.seed = seed;
    cfg.warmup = warmup;
    cfg.samples = samples;
    if (max_error > 0) cfg.max_error = max_error;
    const int64_t n = m->ex->n_params();
    stanli::DrawObserver observe;
    if (cb) {
      observe = [&](int64_t i, bool wu, const double* q) {
        if (!wu) std::memcpy(draws + i * n, q, sizeof(double) * n);
        cb((int32_t)i, wu ? 1 : 0, user);
      };
    }
    auto out = stanli::run_walnuts(*m->ex, cfg, nullptr, observe);
    for (size_t s = 0; s < out.size(); ++s)
      std::memcpy(draws + s * n, out[s].data(), sizeof(double) * n);
    return 0;
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return 1;
  }
}

int stanli_run_pathfinder(stanli_model* m, uint32_t seed, int chain_id,
                          int num_draws, double* draws, double* lp,
                          double* lp_approx, double* summary, stanli_path_cb cb,
                          void* user, char* err, size_t err_len) {
  try {
    stanli::PathfinderConfig cfg;
    cfg.seed = seed;
    cfg.chain_id = chain_id;
    cfg.num_draws = num_draws;
    stanli::PathObserver observe;
    if (cb) {
      observe = [&](const stanli::PathIterate& it) {
        cb((int32_t)it.iter, it.lp, user);
      };
    }
    stanli::PathfinderResult r = stanli::run_pathfinder(*m->ex, cfg, observe);
    if (r.return_code != 0) {
      put_err(err, err_len,
              r.message.empty() ? "pathfinder failed" : r.message.c_str());
      return r.return_code;
    }
    const int64_t n = m->ex->n_params();
    for (size_t s = 0; s < r.draws.size(); ++s) {
      if (draws)
        std::memcpy(draws + s * n, r.draws[s].data(), sizeof(double) * n);
      if (lp) lp[s] = r.lp[s];
      if (lp_approx) lp_approx[s] = r.lp_approx[s];
    }
    if (summary) {
      summary[STANLI_PATHFINDER_KHAT] = r.khat;
      summary[STANLI_PATHFINDER_SELECTED_ITER] = r.selected_iter;
      summary[STANLI_PATHFINDER_SELECTED_ELBO] = r.selected_elbo;
      summary[STANLI_PATHFINDER_ELAPSED_MS] = r.elapsed_ms;
    }
    return 0;
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return 1;
  }
}

void stanli_sample_opts_init(stanli_sample_opts* o) {
  if (o == nullptr) return;
  *o = stanli_sample_opts{};
  o->seed = 1;
  o->chains = 4;
  o->chain_id = 1;
  o->warmup = 1000;
  o->samples = 1000;
  o->thin = 1;
  o->delta = 0.8;
  o->max_depth = 10;
  o->save_warmup = 0;
  o->init_radius = 2.0;
  o->inits = nullptr;
  o->num_threads = 1;
}

int64_t stanli_n_stored_draws(const stanli_sample_opts* o) {
  if (o == nullptr) return 0;
  const int thin = o->thin > 0 ? o->thin : 1;
  // Ceiling division: transition 0 is always stored, so a run of 5
  // transitions thinned by 2 keeps 3 rows (0, 2, 4), not 2.
  const int64_t kept_samples = (o->samples + thin - 1) / thin;
  const int64_t kept_warmup =
      o->save_warmup ? (o->warmup + thin - 1) / thin : 0;
  return kept_samples + kept_warmup;
}

int stanli_thread_safe(void) { return stanli::thread_safe_build() ? 1 : 0; }

const char* stanli_sampler_column_name(int i) {
  static const char* kNames[STANLI_N_SAMPLER_COLS] = {
      "lp__",         "accept_stat__", "stepsize__", "treedepth__",
      "n_leapfrog__", "divergent__",   "energy__"};
  if (i < 0 || i >= STANLI_N_SAMPLER_COLS) return "";
  return kNames[i];
}

int stanli_sample_multi(stanli_model* m, const stanli_sample_opts* opts,
                        double* draws, double* stats, char* err,
                        size_t err_len) {
  return stanli_sample_multi_progress(m, opts, 0, draws, stats, nullptr,
                                      nullptr, nullptr, err, err_len);
}

int stanli_sample_multi_progress(
    stanli_model* m, const stanli_sample_opts* opts, int refresh, double* draws,
    double* stats, stanli_sample_progress_cb progress, void* progress_user,
    stanli_sample_report* reports, char* err, size_t err_len) {
  try {
    if (opts == nullptr) {
      put_err(err, err_len, "null options");
      return 1;
    }
    if (refresh < 0) {
      put_err(err, err_len, "refresh must be nonnegative");
      return 1;
    }
    const int n_chains = opts->chains > 0 ? opts->chains : 1;
    const int64_t n = m->ex->n_params();
    const int64_t n_stored = stanli_n_stored_draws(opts);
    if (reports != nullptr)
      for (int c = 0; c < n_chains; ++c) reports[c] = stanli_sample_report{};

    stanli::NutsConfig cfg;
    cfg.seed = opts->seed;
    cfg.chain_id = opts->chain_id > 0 ? opts->chain_id : 1;
    cfg.warmup = opts->warmup;
    cfg.samples = opts->samples;
    cfg.thin = opts->thin > 0 ? opts->thin : 1;
    cfg.delta = opts->delta;
    cfg.max_depth = opts->max_depth > 0 ? opts->max_depth : 10;
    cfg.save_warmup = opts->save_warmup != 0;
    cfg.init_radius = opts->init_radius;

    // Chain 0 samples on the model's own executor; the rest get clones,
    // so a single-chain run allocates no second arena and behaves exactly
    // as stanli_sample always has.
    auto clones = stanli::clone_executors(*m->ex, n_chains - 1);
    std::vector<stanli::Executor*> execs;
    execs.push_back(m->ex.get());
    for (auto& c : clones) execs.push_back(c.get());

    stanli::ChainProgressObserver progress_observer;
    if (progress != nullptr && refresh > 0) {
      progress_observer = [&](int chain, int64_t i, bool warmup) {
        const int64_t completed = warmup ? i + 1 : (int64_t)cfg.warmup + i + 1;
        progress(cfg.chain_id + chain, completed,
                 (int64_t)cfg.warmup + cfg.samples, warmup ? 1 : 0,
                 progress_user);
      };
    }

    std::vector<stanli::ChainResult> res;
    if (opts->inits == nullptr) {
      res = stanli::run_nuts_chains(execs, cfg, opts->num_threads, {},
                                    progress_observer, refresh);
    } else {
      // Per-chain inits mean per-chain configs, which run_nuts_chains
      // does not take (it varies only the chain id). Run them one at a
      // time; explicit inits are a debugging and Pathfinder-handoff path,
      // not the hot one.
      res.resize((size_t)n_chains);
      for (int c = 0; c < n_chains; ++c) {
        stanli::NutsConfig cc = cfg;
        cc.chain_id = cfg.chain_id + c;
        cc.init = opts->inits + (int64_t)c * n;
        try {
          stanli::ProgressObserver one_progress;
          if (progress_observer)
            one_progress = [&, c, cc](int64_t i, bool warmup) {
              if (stanli::should_report_progress(cc, i, warmup, refresh))
                progress_observer(c, i, warmup);
            };
          res[(size_t)c].draws =
              stanli::run_nuts(*execs[(size_t)c], cc, &res[(size_t)c].stats, {},
                               one_progress, &res[(size_t)c].report);
        } catch (const std::exception& e) {
          res[(size_t)c].error = e.what();
        }
      }
    }

    int failed = 0;
    std::string first_error;
    for (int c = 0; c < n_chains; ++c) {
      const auto& r = res[(size_t)c];
      if (reports != nullptr) {
        reports[c].warmup_seconds = r.report.warmup_seconds;
        reports[c].sampling_seconds = r.report.sampling_seconds;
        reports[c].n_divergent = r.report.n_divergent;
        reports[c].n_max_treedepth = r.report.n_max_treedepth;
      }
      if (!r.error.empty()) {
        if (failed++ == 0)
          first_error =
              "chain " + std::to_string(cfg.chain_id + c) + ": " + r.error;
        continue;
      }
      // A chain that stored fewer rows than asked would leave the tail of
      // its block stale; it cannot happen (the loop is a fixed count) but
      // the buffer is the caller's, so write only what exists.
      const int64_t rows = std::min<int64_t>((int64_t)r.draws.size(), n_stored);
      for (int64_t i = 0; i < rows; ++i) {
        std::memcpy(draws + ((int64_t)c * n_stored + i) * n,
                    r.draws[(size_t)i].data(), sizeof(double) * (size_t)n);
        if (stats != nullptr && i < (int64_t)r.stats.rows.size())
          std::memcpy(
              stats + ((int64_t)c * n_stored + i) * STANLI_N_SAMPLER_COLS,
              r.stats.rows[(size_t)i].data(),
              sizeof(double) * STANLI_N_SAMPLER_COLS);
      }
    }
    if (failed) put_err(err, err_len, first_error.c_str());
    return failed;
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return 1;
  }
}

int stanli_summary_stats(const double* draws, int64_t n_chains, int64_t n_draws,
                         int64_t n_cols, double* out) {
  try {
    stanli::DrawSet d{draws, n_chains, n_draws, n_cols};
    const auto s = stanli::summarize(d, {});
    for (size_t j = 0; j < s.size(); ++j) {
      double* r = out + (int64_t)j * STANLI_N_SUMMARY_STATS;
      r[STANLI_STAT_MEAN] = s[j].mean;
      r[STANLI_STAT_MCSE_MEAN] = s[j].mcse_mean;
      r[STANLI_STAT_SD] = s[j].sd;
      r[STANLI_STAT_MCSE_SD] = s[j].mcse_sd;
      r[STANLI_STAT_Q5] = s[j].q5;
      r[STANLI_STAT_Q50] = s[j].q50;
      r[STANLI_STAT_Q95] = s[j].q95;
      r[STANLI_STAT_ESS_BULK] = s[j].ess_bulk;
      r[STANLI_STAT_ESS_TAIL] = s[j].ess_tail;
      r[STANLI_STAT_RHAT] = s[j].rhat;
    }
    return 0;
  } catch (const std::exception&) {
    return 1;
  }
}

int64_t stanli_diagnose_text(const double* draws, int64_t n_chains,
                             int64_t n_draws, int64_t n_cols,
                             const char* const* names, const double* stats,
                             int max_depth, char* out, size_t out_len) {
  try {
    stanli::DrawSet d{draws, n_chains, n_draws, n_cols};
    std::vector<std::string> nm;
    if (names != nullptr) {
      nm.reserve((size_t)n_cols);
      for (int64_t j = 0; j < n_cols; ++j)
        nm.push_back(names[j] ? names[j] : "");
    }
    const auto summary = stanli::summarize(d, nm);
    const auto fd = stanli::diagnose(d, summary, stats, max_depth);
    const std::string text = stanli::format_diagnostics(fd);
    if (out != nullptr && out_len > 0) {
      const size_t k = std::min(text.size(), out_len - 1);
      std::memcpy(out, text.data(), k);
      out[k] = '\0';
    }
    return (int64_t)text.size() + 1;
  } catch (const std::exception&) {
    return 0;
  }
}

void stanli_optimize_opts_init(stanli_optimize_opts* o) {
  if (o == nullptr) return;
  *o = stanli_optimize_opts{};
  const stanli::OptimizeConfig d;
  o->seed = d.seed;
  o->chain_id = d.chain_id;
  o->iter = d.iter;
  o->jacobian = d.jacobian ? 1 : 0;
  o->init_alpha = d.init_alpha;
  o->tol_obj = d.tol_obj;
  o->tol_rel_obj = d.tol_rel_obj;
  o->tol_grad = d.tol_grad;
  o->tol_rel_grad = d.tol_rel_grad;
  o->tol_param = d.tol_param;
  o->history_size = d.history_size;
  o->init_radius = d.init_radius;
  o->init = nullptr;
}

int stanli_optimize(stanli_model* m, const stanli_optimize_opts* opts,
                    double* unconstrained, double* values, double* lp,
                    char* err, size_t err_len) {
  try {
    if (opts == nullptr) {
      put_err(err, err_len, "null options");
      return 1;
    }
    stanli::OptimizeConfig cfg;
    cfg.seed = opts->seed;
    cfg.chain_id = opts->chain_id > 0 ? opts->chain_id : 1;
    cfg.iter = opts->iter > 0 ? opts->iter : 2000;
    cfg.jacobian = opts->jacobian != 0;
    cfg.init_alpha = opts->init_alpha;
    cfg.tol_obj = opts->tol_obj;
    cfg.tol_rel_obj = opts->tol_rel_obj;
    cfg.tol_grad = opts->tol_grad;
    cfg.tol_rel_grad = opts->tol_rel_grad;
    cfg.tol_param = opts->tol_param;
    cfg.history_size = opts->history_size > 0 ? opts->history_size : 5;
    cfg.init_radius = opts->init_radius;
    cfg.init = opts->init;

    // The CSV side, so the optimizer can report the mode the way a draw is
    // reported: whichever write_array path this model has.
    stanli::WriteArray wa;
    wa.names = m->wa_n > 0 ? m->wa_names : m->flat_names;
    wa.row = [m](const double* q, double* out) {
      if (m->wa_n > 0) {
        stanli_wa_row(m, q, out);
      } else {
        stanli_constrain(m, q, out);
      }
    };

    const stanli::OptimizeResult r = stanli::run_optimize(*m->ex, &wa, cfg);
    for (size_t i = 0; i < r.unconstrained.size(); ++i)
      unconstrained[i] = r.unconstrained[i];
    if (values != nullptr)
      for (size_t i = 0; i < r.values.size(); ++i) values[i] = r.values[i];
    if (lp != nullptr) *lp = r.lp;
    if (r.return_code != 0)
      put_err(
          err, err_len,
          r.message.empty() ? "L-BFGS did not converge" : r.message.c_str());
    return r.return_code;
  } catch (const std::exception& e) {
    put_err(err, err_len, e.what());
    return 1;
  }
}

int64_t stanli_n_constrained(const stanli_model* m) { return m->n_con; }

const char* stanli_constrained_name(const stanli_model* m, int64_t i) {
  if (i < 0 || i >= (int64_t)m->flat_names.size()) return nullptr;
  return m->flat_names[i].c_str();
}

int64_t stanli_wa_n_columns(const stanli_model* m) { return m->wa_n; }

const char* stanli_wa_column_name(const stanli_model* m, int64_t i) {
  if (i < 0 || i >= (int64_t)m->wa_names.size()) return "";
  return m->wa_names[(size_t)i].c_str();
}

void stanli_wa_seed(stanli_model* m, uint32_t seed) { m->wa_rng.seed(seed); }

void stanli_wa_seed_chain(stanli_model* m, uint32_t seed, uint32_t chain) {
  m->wa_rng.seed(seed, chain);
}

int stanli_wa_row(stanli_model* m, const double* q, double* out) {
  try {
    if (m->wa_interp) {
      const auto row = interp_wa_row(*m, q, m->wa_rng);
      if ((int64_t)row.size() != m->wa_n) return 1;
      std::memcpy(out, row.data(), sizeof(double) * (size_t)m->wa_n);
      return 0;
    }
    if (m->wa_ex) {
      std::memcpy(m->wa_ex->params_data(), q,
                  sizeof(double) * m->wa_ex->n_params());
      m->wa_ex->run_forward_only(stanli::EvalState{&m->wa_rng});
      int64_t at = 0;
      for (const auto& c : m->wa_cols) {
        const double* p = m->wa_ex->value_ptr(c.slot);
        for (int64_t i = 0; i < c.len; ++i) out[at++] = p[i];
      }
      return 0;
    }
    return 1;
  } catch (const std::exception&) {
    return 1;
  }
}

int stanli_constrain(stanli_model* m, const double* q, double* out) {
  const int64_t n = m->ex->n_params();
  std::memcpy(m->ex->params_data(), q, sizeof(double) * n);
  try {
    m->ex->run_forward_only();
  } catch (const std::exception&) {
    return 1;
  }
  int64_t k = 0;
  for (const auto& v : m->cm.views) {
    const double* p = m->ex->value_ptr(v.slot);
    for (int64_t i = 0; i < v.len; ++i) out[k++] = p[v.storage_index(i)];
  }
  return 0;
}

}  // extern "C"
