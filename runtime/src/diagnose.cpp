#include <stanli/diagnose.hpp>

#include <stan/analyze/mcmc/mcse.hpp>
#include <stan/analyze/mcmc/split_rank_normalized_ess.hpp>
#include <stan/analyze/mcmc/split_rank_normalized_rhat.hpp>
#include <stan/math/prim/fun/quantile.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace stanli {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// stan's analyze functions all take (n_draws x n_chains) column-major
// Eigen matrices, one parameter at a time. Ours is one chain-major
// packed buffer, so every estimator starts here.
Eigen::MatrixXd column_of(const DrawSet& d, int64_t col) {
  Eigen::MatrixXd m(d.n_draws, d.n_chains);
  for (int64_t c = 0; c < d.n_chains; ++c)
    for (int64_t i = 0; i < d.n_draws; ++i) m(i, c) = d.at(c, i)[col];
  return m;
}

std::string fmt(double v, int prec = 4) {
  if (std::isnan(v)) return "nan";
  char buf[64];
  // Large counts (n_leapfrog, ESS on a long run) read better without a
  // decimal point; small ones need the precision.
  if (std::fabs(v) >= 1e5 || (v != 0 && std::fabs(v) < 1e-3))
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
  else
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
  return buf;
}

}  // namespace

double ebfmi(const double* energy, int64_t n, int64_t stride) {
  if (n < 2) return kNaN;
  double mean = 0;
  for (int64_t i = 0; i < n; ++i) mean += energy[i * stride];
  mean /= (double)n;
  double num = 0, den = 0;
  for (int64_t i = 0; i < n; ++i) {
    const double e = energy[i * stride];
    if (i > 0) {
      const double d = e - energy[(i - 1) * stride];
      num += d * d;
    }
    const double c = e - mean;
    den += c * c;
  }
  // A constant energy sequence is not "perfectly mixed", it is a chain
  // that never moved; reporting 0/0 as a number would read as a pass.
  if (den == 0) return kNaN;
  return num / den;
}

std::vector<ParamSummary> summarize(const DrawSet& d,
                                    const std::vector<std::string>& names) {
  std::vector<ParamSummary> out;
  out.reserve((size_t)d.n_cols);
  const int64_t n_total = d.n_chains * d.n_draws;
  for (int64_t j = 0; j < d.n_cols; ++j) {
    ParamSummary s;
    if (j < (int64_t)names.size()) s.name = names[(size_t)j];

    const Eigen::MatrixXd m = column_of(d, j);
    const Eigen::VectorXd flat = m.reshaped();

    s.mean = flat.mean();
    // Sample sd (n-1), matching stansummary.
    s.sd = n_total > 1 ? std::sqrt((flat.array() - s.mean).square().sum() /
                                   (double)(n_total - 1))
                       : 0.0;
    s.q5 = stan::math::quantile(flat, 0.05);
    s.q50 = stan::math::quantile(flat, 0.50);
    s.q95 = stan::math::quantile(flat, 0.95);

    // The rank-normalized estimators need a finite, varying column; they
    // return NaN otherwise, and a constant column (a fixed transformed
    // parameter, lp__ of a degenerate model) is common enough that
    // letting the NaN through is the honest answer rather than a bug.
    const auto rh = stan::analyze::split_rank_normalized_rhat(m);
    s.rhat_bulk = rh.first;
    s.rhat_tail = rh.second;
    s.rhat = (std::isnan(rh.first) || std::isnan(rh.second))
                 ? kNaN
                 : std::max(rh.first, rh.second);
    const auto es = stan::analyze::split_rank_normalized_ess(m);
    s.ess_bulk = es.first;
    s.ess_tail = es.second;
    s.mcse_mean = stan::analyze::mcse_mean(m);
    s.mcse_sd = stan::analyze::mcse_sd(m);
    out.push_back(std::move(s));
  }
  return out;
}

FitDiagnostics diagnose(const DrawSet& d,
                        const std::vector<ParamSummary>& summary,
                        const double* stats, int max_depth) {
  FitDiagnostics fd;
  fd.n_chains = d.n_chains;
  fd.n_draws = d.n_draws;
  fd.max_depth = max_depth;

  if (stats != nullptr) {
    fd.divergent_by_chain.assign((size_t)d.n_chains, 0);
    fd.max_treedepth_by_chain.assign((size_t)d.n_chains, 0);
    fd.ebfmi_by_chain.assign((size_t)d.n_chains, kNaN);
    fd.stepsize_by_chain.assign((size_t)d.n_chains, kNaN);
    fd.accept_stat_by_chain.assign((size_t)d.n_chains, kNaN);
    for (int64_t c = 0; c < d.n_chains; ++c) {
      const double* base = stats + c * d.n_draws * N_SAMPLER_COLS;
      double acc = 0;
      for (int64_t i = 0; i < d.n_draws; ++i) {
        const double* row = base + i * N_SAMPLER_COLS;
        if (row[COL_DIVERGENT] != 0) fd.divergent_by_chain[(size_t)c]++;
        if ((int)row[COL_TREEDEPTH] >= max_depth)
          fd.max_treedepth_by_chain[(size_t)c]++;
        acc += row[COL_ACCEPT_STAT];
      }
      if (d.n_draws > 0) {
        fd.accept_stat_by_chain[(size_t)c] = acc / (double)d.n_draws;
        // The adapted stepsize is constant post-warmup; the last row
        // reports it, and taking the last rather than the first says so.
        fd.stepsize_by_chain[(size_t)c] =
            base[(d.n_draws - 1) * N_SAMPLER_COLS + COL_STEPSIZE];
      }
      fd.ebfmi_by_chain[(size_t)c] =
          ebfmi(base + COL_ENERGY, d.n_draws, N_SAMPLER_COLS);
      fd.n_divergent += fd.divergent_by_chain[(size_t)c];
      fd.n_max_treedepth += fd.max_treedepth_by_chain[(size_t)c];
    }
  }

  // Worst-over-parameters, skipping NaN (a constant column carries no
  // information about convergence and must not become the reported worst).
  bool first_rhat = true, first_bulk = true, first_tail = true;
  for (const auto& s : summary) {
    if (!std::isnan(s.rhat) && (first_rhat || s.rhat > fd.max_rhat)) {
      fd.max_rhat = s.rhat;
      fd.max_rhat_param = s.name;
      first_rhat = false;
    }
    if (!std::isnan(s.ess_bulk) &&
        (first_bulk || s.ess_bulk < fd.min_ess_bulk)) {
      fd.min_ess_bulk = s.ess_bulk;
      fd.min_ess_bulk_param = s.name;
      first_bulk = false;
    }
    if (!std::isnan(s.ess_tail) &&
        (first_tail || s.ess_tail < fd.min_ess_tail)) {
      fd.min_ess_tail = s.ess_tail;
      fd.min_ess_tail_param = s.name;
      first_tail = false;
    }
  }
  if (first_rhat) fd.max_rhat = kNaN;
  if (first_bulk) fd.min_ess_bulk = kNaN;
  if (first_tail) fd.min_ess_tail = kNaN;
  return fd;
}

std::string format_summary(const std::vector<ParamSummary>& s) {
  size_t w = 8;
  for (const auto& r : s) w = std::max(w, r.name.size());
  const auto pad = [](std::string v, size_t n, bool left) {
    if (v.size() >= n) return v;
    const std::string sp(n - v.size(), ' ');
    return left ? v + sp : sp + v;
  };
  static constexpr int kNum = 11;
  std::string out = pad("name", w, true);
  for (const char* h : {"Mean", "MCSE", "StdDev", "5%", "50%", "95%",
                        "ESS_bulk", "ESS_tail", "R_hat"})
    out += pad(h, kNum, false);
  out += "\n";
  for (const auto& r : s) {
    out += pad(r.name, w, true);
    out += pad(fmt(r.mean), kNum, false);
    out += pad(fmt(r.mcse_mean), kNum, false);
    out += pad(fmt(r.sd), kNum, false);
    out += pad(fmt(r.q5), kNum, false);
    out += pad(fmt(r.q50), kNum, false);
    out += pad(fmt(r.q95), kNum, false);
    out += pad(fmt(r.ess_bulk, 0), kNum, false);
    out += pad(fmt(r.ess_tail, 0), kNum, false);
    out += pad(fmt(r.rhat, 3), kNum, false);
    out += "\n";
  }
  return out;
}

std::string format_diagnostics(const FitDiagnostics& d) {
  std::string out;
  int problems = 0;
  const auto line = [&out](const std::string& s) { out += s + "\n"; };
  char buf[512];

  const int64_t n_total = d.n_chains * d.n_draws;
  if (!d.divergent_by_chain.empty()) {
    if (d.n_divergent > 0) {
      ++problems;
      std::snprintf(buf, sizeof buf,
                    "%lld of %lld transitions (%.1f%%) diverged. The "
                    "posterior is biased: raise adapt_delta above the "
                    "current target or reparameterize.",
                    (long long)d.n_divergent, (long long)n_total,
                    100.0 * (double)d.n_divergent / (double)n_total);
      line(buf);
    } else {
      line("No divergent transitions.");
    }

    if (d.n_max_treedepth > 0) {
      ++problems;
      std::snprintf(buf, sizeof buf,
                    "%lld of %lld transitions saturated the maximum "
                    "treedepth of %d. This costs efficiency, not "
                    "validity: raise max_depth.",
                    (long long)d.n_max_treedepth, (long long)n_total,
                    d.max_depth);
      line(buf);
    } else {
      std::snprintf(buf, sizeof buf,
                    "No transitions saturated the maximum treedepth of %d.",
                    d.max_depth);
      line(buf);
    }

    int bad_e = 0;
    double worst_e = 0;
    for (size_t c = 0; c < d.ebfmi_by_chain.size(); ++c) {
      const double e = d.ebfmi_by_chain[c];
      if (!std::isnan(e) && e < kEbfmiThreshold) {
        if (bad_e == 0 || e < worst_e) worst_e = e;
        ++bad_e;
      }
    }
    if (bad_e > 0) {
      ++problems;
      std::snprintf(buf, sizeof buf,
                    "E-BFMI is below %.1f in %d of %lld chains (lowest "
                    "%.2f). The momentum resampling is not moving the "
                    "chain across the energy distribution, so the tails "
                    "are explored badly; reparameterize.",
                    kEbfmiThreshold, bad_e, (long long)d.n_chains, worst_e);
      line(buf);
    } else {
      std::snprintf(buf, sizeof buf, "E-BFMI is above %.1f in every chain.",
                    kEbfmiThreshold);
      line(buf);
    }
  }

  if (!std::isnan(d.max_rhat)) {
    if (d.max_rhat > kRhatThreshold) {
      ++problems;
      std::snprintf(buf, sizeof buf,
                    "R-hat reaches %.3f (%s), above %.2f. The chains have "
                    "not mixed; run longer warmup or reparameterize.",
                    d.max_rhat, d.max_rhat_param.c_str(), kRhatThreshold);
      line(buf);
    } else {
      std::snprintf(buf, sizeof buf,
                    "R-hat is below %.2f for every "
                    "parameter (worst %.3f, %s).",
                    kRhatThreshold, d.max_rhat, d.max_rhat_param.c_str());
      line(buf);
    }
  }

  // ESS is judged per chain, which is how the Vehtari et al. threshold is
  // stated: 100 per chain, not 100 overall.
  const double ess_floor = kEssPerChainThreshold * (double)d.n_chains;
  if (!std::isnan(d.min_ess_bulk)) {
    if (d.min_ess_bulk < ess_floor) {
      ++problems;
      std::snprintf(buf, sizeof buf,
                    "Bulk ESS falls to %.0f (%s), below %.0f (%.0f per "
                    "chain). The central estimates are noisy; draw more "
                    "samples.",
                    d.min_ess_bulk, d.min_ess_bulk_param.c_str(), ess_floor,
                    kEssPerChainThreshold);
      line(buf);
    } else {
      std::snprintf(buf, sizeof buf,
                    "Bulk ESS is at least %.0f per chain for every "
                    "parameter (worst %.0f, %s).",
                    kEssPerChainThreshold, d.min_ess_bulk,
                    d.min_ess_bulk_param.c_str());
      line(buf);
    }
  }
  if (!std::isnan(d.min_ess_tail)) {
    if (d.min_ess_tail < ess_floor) {
      ++problems;
      std::snprintf(buf, sizeof buf,
                    "Tail ESS falls to %.0f (%s), below %.0f. The 5%% and "
                    "95%% quantiles are unreliable even where the mean is "
                    "not; draw more samples.",
                    d.min_ess_tail, d.min_ess_tail_param.c_str(), ess_floor);
      line(buf);
    } else {
      std::snprintf(buf, sizeof buf,
                    "Tail ESS is at least %.0f per chain for every "
                    "parameter (worst %.0f, %s).",
                    kEssPerChainThreshold, d.min_ess_tail,
                    d.min_ess_tail_param.c_str());
      line(buf);
    }
  }

  if (problems == 0)
    line("No problems detected.");
  else
    line(problems == 1
             ? "1 diagnostic check failed."
             : std::to_string(problems) + " diagnostic checks failed.");
  return out;
}

}  // namespace stanli
