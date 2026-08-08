// Multi-chain summaries and Betancourt's diagnostic checks.
//
// The estimators themselves are stan's (rank-normalized split-Rhat,
// bulk/tail ESS, MCSE), so what is tested here is the plumbing that can
// actually be wrong: the chain-major packing being read in the right
// order, E-BFMI (the one estimator written here), NaN handling on
// degenerate columns, and the worst-parameter attribution.
#include <stanli/diagnose.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static int failures = 0;

static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

static void expect_near(const std::string& what, double got, double want,
                        double tol) {
  if (!(std::fabs(got - want) <= tol)) {
    ++failures;
    std::printf("FAIL %-30s got %.10g want %.10g (tol %g)\n", what.c_str(),
                got, want, tol);
  }
}

static void expect_in(const std::string& what, double got, double lo,
                      double hi) {
  if (!(got >= lo && got <= hi)) {
    ++failures;
    std::printf("FAIL %-30s got %.6g want in [%g, %g]\n", what.c_str(), got,
                lo, hi);
  }
}

int main() {
  using namespace stanli;

  // ---- packing: each chain/draw/column must land where at() says ---------
  // A column whose value encodes (chain, draw, col) catches a transposed
  // read, which is the failure mode that produces plausible-looking
  // numbers rather than an obvious crash.
  {
    const int64_t C = 3, N = 5, P = 2;
    std::vector<double> d((size_t)(C * N * P));
    for (int64_t c = 0; c < C; ++c)
      for (int64_t i = 0; i < N; ++i)
        for (int64_t j = 0; j < P; ++j)
          d[(size_t)((c * N + i) * P + j)] = 100 * c + 10 * i + j;
    DrawSet ds{d.data(), C, N, P};
    bool ok = true;
    for (int64_t c = 0; c < C; ++c)
      for (int64_t i = 0; i < N; ++i)
        for (int64_t j = 0; j < P; ++j)
          ok = ok && ds.at(c, i)[j] == 100 * c + 10 * i + j;
    expect("packing round-trips", ok);
  }

  // ---- E-BFMI against a hand-computed case ------------------------------
  {
    // energy = {1, 2, 3, 4}: successive differences all 1, so numerator
    // is 3; mean 2.5, so denominator is 2*(1.5^2) + 2*(0.5^2) = 5.
    const double e[] = {1, 2, 3, 4};
    expect_near("ebfmi hand case", ebfmi(e, 4, 1), 3.0 / 5.0, 1e-15);

    // Strided, as the real caller reads it out of the seven-wide stats
    // block: same numbers, one column of a wider row.
    double wide[4 * N_SAMPLER_COLS] = {0};
    for (int i = 0; i < 4; ++i) wide[i * N_SAMPLER_COLS + COL_ENERGY] = e[i];
    expect_near("ebfmi strided", ebfmi(wide + COL_ENERGY, 4, N_SAMPLER_COLS),
                3.0 / 5.0, 1e-15);

    // A chain that never moved has zero energy variance. 0/0 must not
    // read as a pass, so it comes back NaN.
    const double flat[] = {2, 2, 2, 2};
    expect("ebfmi constant energy is nan", std::isnan(ebfmi(flat, 4, 1)));
    expect("ebfmi n<2 is nan", std::isnan(ebfmi(e, 1, 1)));

    // Independent draws from a fixed energy distribution give E-BFMI near
    // 2: successive differences of independent draws have twice the
    // marginal variance. Well above the 0.3 threshold, which is what the
    // check is for.
    std::mt19937 rng(20260808);
    std::normal_distribution<double> nd(0, 1);
    std::vector<double> indep(4000);
    for (auto& v : indep) v = nd(rng);
    expect_in("ebfmi independent ~ 2", ebfmi(indep.data(), 4000, 1), 1.8, 2.2);
  }

  // ---- summaries on independent normal draws ----------------------------
  // Four chains of iid N(0,1) is the case every estimator should call
  // clean: Rhat at 1, ESS near the draw count, mean near 0, sd near 1.
  {
    const int64_t C = 4, N = 1000, P = 2;
    std::vector<double> d((size_t)(C * N * P));
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0, 1);
    for (int64_t c = 0; c < C; ++c)
      for (int64_t i = 0; i < N; ++i) {
        d[(size_t)((c * N + i) * P + 0)] = nd(rng);
        d[(size_t)((c * N + i) * P + 1)] = 5.0;  // constant column
      }
    DrawSet ds{d.data(), C, N, P};
    auto s = summarize(ds, {"x", "fixed"});
    expect("two rows", s.size() == 2);
    expect_near("mean ~ 0", s[0].mean, 0.0, 0.05);
    expect_near("sd ~ 1", s[0].sd, 1.0, 0.05);
    expect_in("rhat ~ 1", s[0].rhat, 0.99, 1.02);
    expect_in("ess_bulk large", s[0].ess_bulk, 2000, 5000);
    expect_in("ess_tail large", s[0].ess_tail, 1500, 5000);
    expect_in("q5", s[0].q5, -1.8, -1.5);
    expect_in("q95", s[0].q95, 1.5, 1.8);
    expect("mcse_mean positive", s[0].mcse_mean > 0 && s[0].mcse_mean < 0.05);

    // A constant column carries no convergence information. NaN is the
    // honest answer and it must not become the reported worst parameter.
    expect("constant rhat is nan", std::isnan(s[1].rhat));
    auto fd = diagnose(ds, s, nullptr, 10);
    expect("worst rhat skips constant", fd.max_rhat_param == "x");
    expect("worst ess skips constant", fd.min_ess_bulk_param == "x");
    expect("clean fit has no complaints",
           format_diagnostics(fd).find("No problems detected") !=
               std::string::npos);
  }

  // ---- summaries on chains that disagree --------------------------------
  // Two chains offset by 10 sd: Rhat must notice. This is the check that
  // fails if the packing is read transposed, since a transposed read
  // mixes the offset uniformly across "chains" and Rhat comes back clean.
  {
    const int64_t C = 2, N = 500, P = 1;
    std::vector<double> d((size_t)(C * N * P));
    std::mt19937 rng(11);
    std::normal_distribution<double> nd(0, 1);
    for (int64_t c = 0; c < C; ++c)
      for (int64_t i = 0; i < N; ++i)
        d[(size_t)((c * N + i) * P)] = nd(rng) + (c == 0 ? 0.0 : 10.0);
    DrawSet ds{d.data(), C, N, P};
    auto s = summarize(ds, {"x"});
    expect("split chains detected", s[0].rhat > 1.5);
    auto fd = diagnose(ds, s, nullptr, 10);
    const std::string txt = format_diagnostics(fd);
    expect("rhat complaint names the parameter",
           txt.find("R-hat reaches") != std::string::npos &&
               txt.find("(x)") != std::string::npos);
  }

  // ---- sampler-column diagnostics ---------------------------------------
  {
    const int64_t C = 2, N = 100, P = 1;
    std::vector<double> d((size_t)(C * N * P), 0.0);
    std::mt19937 rng(3);
    std::normal_distribution<double> nd(0, 1);
    for (auto& v : d) v = nd(rng);
    std::vector<double> st((size_t)(C * N * N_SAMPLER_COLS), 0.0);
    for (int64_t c = 0; c < C; ++c)
      for (int64_t i = 0; i < N; ++i) {
        double* r = &st[(size_t)((c * N + i) * N_SAMPLER_COLS)];
        r[COL_ACCEPT_STAT] = 0.9;
        r[COL_STEPSIZE] = 0.25 + 0.1 * (double)c;
        r[COL_TREEDEPTH] = 3;
        r[COL_ENERGY] = nd(rng);
        // Chain 0 gets three divergences and two depth saturations.
        if (c == 0 && i < 3) r[COL_DIVERGENT] = 1;
        if (c == 0 && i < 2) r[COL_TREEDEPTH] = 10;
      }
    DrawSet ds{d.data(), C, N, P};
    auto fd = diagnose(ds, summarize(ds, {"x"}), st.data(), 10);
    expect("divergences counted", fd.n_divergent == 3);
    expect("divergences attributed", fd.divergent_by_chain[0] == 3 &&
                                         fd.divergent_by_chain[1] == 0);
    expect("treedepth counted", fd.n_max_treedepth == 2);
    expect_near("stepsize chain 0", fd.stepsize_by_chain[0], 0.25, 1e-12);
    expect_near("stepsize chain 1", fd.stepsize_by_chain[1], 0.35, 1e-12);
    expect_near("accept stat", fd.accept_stat_by_chain[0], 0.9, 1e-12);
    const std::string txt = format_diagnostics(fd);
    expect("reports divergences",
           txt.find("3 of 200 transitions") != std::string::npos);
    expect("reports treedepth",
           txt.find("2 of 200 transitions saturated") != std::string::npos);
    expect("counts the failures",
           txt.find("diagnostic checks failed") != std::string::npos);
  }

  // ---- E-BFMI failure is reported ---------------------------------------
  // A slowly drifting energy sequence has tiny successive differences
  // against a large marginal variance, which is exactly what a badly
  // explored heavy tail looks like.
  {
    const int64_t C = 1, N = 500, P = 1;
    std::vector<double> d((size_t)N, 0.0);
    for (int64_t i = 0; i < N; ++i) d[(size_t)i] = (double)i;
    std::vector<double> st((size_t)(N * N_SAMPLER_COLS), 0.0);
    for (int64_t i = 0; i < N; ++i)
      st[(size_t)(i * N_SAMPLER_COLS + COL_ENERGY)] = (double)i;
    DrawSet ds{d.data(), C, N, P};
    auto fd = diagnose(ds, summarize(ds, {"x"}), st.data(), 10);
    expect("drifting energy has low ebfmi",
           fd.ebfmi_by_chain[0] < kEbfmiThreshold);
    expect("reports ebfmi",
           format_diagnostics(fd).find("E-BFMI is below") != std::string::npos);
  }

  // ---- the summary table renders ----------------------------------------
  {
    std::vector<ParamSummary> s(1);
    s[0].name = "mu";
    s[0].mean = 1.5;
    s[0].sd = 0.25;
    s[0].ess_bulk = 1234;
    s[0].rhat = 1.001;
    const std::string t = format_summary(s);
    expect("header present", t.find("ESS_bulk") != std::string::npos &&
                                 t.find("R_hat") != std::string::npos);
    expect("row present", t.find("mu") != std::string::npos &&
                              t.find("1234") != std::string::npos);
  }

  if (failures == 0) std::printf("test_diagnose: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
