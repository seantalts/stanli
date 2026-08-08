#include <stanli/capi.h>


#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/nuts.hpp>
#include <stanli/wa_interp.hpp>

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
  stanli::CompiledModel cm;   // graph moved out into ex
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
};

namespace {

// One interpreted write_array row: the main executor's forward pass
// supplies the constrained parameter values by name.
std::vector<double> interp_wa_row(stanli_model& m, const double* q) {
  stanli::Executor& ex = *m.ex;
  std::memcpy(ex.params_data(), q, sizeof(double) * ex.n_params());
  ex.run_forward_only();
  std::map<std::string, stanli::DataMap::Entry> params;
  for (const auto& v : m.cm.views) {
    stanli::DataMap::Entry en;
    const double* p = ex.value_ptr(v.slot);
    en.r.assign(p, p + v.len);
    if (v.rows > 0)
      en.dims = {v.rows, v.len / v.rows};
    else if (v.len > 1)
      en.dims = {v.len};
    params[v.name] = std::move(en);
  }
  return m.wa_interp->eval(params);
}

// Same deterministic probe points as stanli_check: column discovery for
// the interpreted path needs one evaluation, and a model can be out of
// support at one point and fine at the next.
double probe_point(int64_t i, int variant) {
  switch (variant) {
    case 1: return 0.02 * static_cast<double>((i % 5) - 2);
    case 2: return 0.0;
    default: return 0.1 + 0.05 * static_cast<double>(i % 7) -
                    0.15 * static_cast<double>(i % 3);
  }
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
    for (const auto& v : m->cm.views) {
      m->n_con += v.len;
      for (int64_t i = 0; i < v.len; ++i)
        m->flat_names.push_back(
            v.len == 1 ? v.name : v.name + "." + std::to_string(i + 1));
    }
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
            q[i] = probe_point((int64_t)i, variant);
          try {
            const auto row = interp_wa_row(*m, q.data());
            m->wa_n = (int64_t)row.size();
            m->wa_names = stanli::CompiledModel::csv_names(
                m->wa_interp->columns());
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
  return stanli_sample_stream(m, seed, warmup, samples, delta, draws,
                              nullptr, nullptr, err, err_len);
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

void stanli_wa_seed(stanli_model* m, uint32_t seed) {
  if (m->wa_interp) m->wa_interp->seed(seed);
}

int stanli_wa_row(stanli_model* m, const double* q, double* out) {
  try {
    if (m->wa_interp) {
      const auto row = interp_wa_row(*m, q);
      if ((int64_t)row.size() != m->wa_n) return 1;
      std::memcpy(out, row.data(), sizeof(double) * (size_t)m->wa_n);
      return 0;
    }
    if (m->wa_ex) {
      std::memcpy(m->wa_ex->params_data(), q,
                  sizeof(double) * m->wa_ex->n_params());
      m->wa_ex->run_forward_only();
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
    for (int64_t i = 0; i < v.len; ++i) out[k++] = p[i];
  }
  return 0;
}

}  // extern "C"
