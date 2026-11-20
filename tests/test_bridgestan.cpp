// The BridgeStan C ABI over stanli.
//
// Linked directly rather than dlopen'd: the manifest arrives embedded in
// the data argument, so the whole of bs_model_construct is plain argument
// passing and tests in-process. bs_model_from_mir is the seam beneath it,
// and the manifest reader is tested as the small function it is.
//
// Two properties are worth more than the rest. First, the numbers: a
// density, a gradient and a constrained row through the facade must be
// BITWISE what the same model computes through compile_model + Executor,
// because a facade that quietly recomputes is a facade that quietly
// disagrees. Second, the refusals: every call stanli cannot honor must
// return exactly -1 with a message, never a plausible number for flags it
// did not implement.
#include "../runtime/ext/bridgestan.h"
#include "categorical_check_mir.hpp"

#include "../runtime/src/build_id.hpp"
#include "../runtime/ext/nlohmann_json.hpp"

#include <stanli/bridgestan_internal.hpp>
#include <stanli/compile.hpp>
#include <stanli/executor_pool.hpp>
#include <stanli/nuts.hpp>
#include <stanli/wa_interp.hpp>
#include <stanli/whiten.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void fail(const std::string& what) {
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

void expect_eq_str(const std::string& what, const std::string& got,
                   const std::string& want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s\n  got  %s\n  want %s\n", what.c_str(), got.c_str(),
                want.c_str());
  }
}

void expect_eq_int(const std::string& what, long long got, long long want) {
  if (got != want) {
    ++failures;
    std::printf("FAIL %s: got %lld want %lld\n", what.c_str(), got, want);
  }
}

void expect_bitwise(const std::string& what, double got, double want) {
  if (!(got == want)) {
    ++failures;
    std::printf("FAIL %s: got %.17g want %.17g\n", what.c_str(), got, want);
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Every unsupported entry has the same contract: exactly -1, a non-null
// message, and the message frees cleanly. `mentions` pins WHICH refusal it
// was, so a call that failed for an unrelated reason cannot pass for a
// call that refused on purpose.
// `err` is a POINTER to the caller's variable, not its value: C++ leaves
// the order of argument evaluation unspecified, so passing `err` next to
// the call that sets it read the old null on gcc and the new message on
// clang. Reading it in the body is sequenced after every argument.
void expect_refused(const std::string& what, int rc, char** err,
                    const std::string& mentions) {
  if (rc != -1) {
    ++failures;
    std::printf("FAIL %s: return code %d, want exactly -1\n", what.c_str(), rc);
  }
  char* msg = *err;
  *err = nullptr;
  if (msg == nullptr) {
    ++failures;
    std::printf("FAIL %s: no error message\n", what.c_str());
  } else if (msg[0] == '\0') {
    ++failures;
    std::printf("FAIL %s: empty error message\n", what.c_str());
  } else if (std::string(msg).find(mentions) == std::string::npos) {
    ++failures;
    std::printf("FAIL %s: message does not mention \"%s\"\n  got %s\n",
                what.c_str(), mentions.c_str(), msg);
  }
  bs_free_error_msg(msg);
}

// A point that depends on the index, so a mixed-up parameter shows up as a
// wrong number rather than as the same number everywhere.
double point(int64_t i) { return 0.3 - 0.17 * (double)(i % 5); }

// ---------------------------------------------------------------------------

// conj: two parameters, one transformed parameter, three generated
// quantities, and a write_array the graph can express end to end.
void test_density_matches_executor() {
  using namespace stanli;
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");

  // The reference: the same model the ordinary way.
  DataMap data = DataMap::from_json_file("tests/fixtures/conj.json");
  CompiledModel cm = compile_model(mir, data);
  Executor ex(std::move(cm.graph));
  cm.bind(ex);
  const int64_t n = ex.n_params();

  // The facade, given the data as a PATH (one of the four `data` forms).
  char* err = nullptr;
  bs_model* m =
      bs_model_from_mir(mir.c_str(), "tests/fixtures/conj.json", 1234, &err);
  if (m == nullptr) {
    fail(std::string("conj construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_int("conj bs_param_unc_num", bs_param_unc_num(m), n);

  std::vector<double> q((size_t)n);
  for (int64_t i = 0; i < n; ++i) q[(size_t)i] = point(i);

  for (int64_t i = 0; i < n; ++i) ex.params_data()[i] = q[(size_t)i];
  const double want_lp = ex.forward();
  std::vector<double> want_g((size_t)n);
  const double want_lp_g = ex.gradient(want_g.data());

  double lp = 0;
  expect_eq_int("bs_log_density rc",
                bs_log_density(m, true, true, q.data(), &lp, &err), 0);
  expect_bitwise("bs_log_density value", lp, want_lp);

  double val = 0;
  std::vector<double> g((size_t)n, 0.0);
  expect_eq_int(
      "bs_log_density_gradient rc",
      bs_log_density_gradient(m, true, true, q.data(), &val, g.data(), &err),
      0);
  expect_bitwise("bs_log_density_gradient value", val, want_lp_g);
  for (int64_t i = 0; i < n; ++i)
    expect_bitwise("bs_log_density_gradient grad[" + std::to_string(i) + "]",
                   g[(size_t)i], want_g[(size_t)i]);

  // Model identity: the name defaults, the info string names the runtime
  // and the ABI it implements.
  expect_eq_str("bs_name default", bs_name(m), "stanli_model");
  const std::string info = bs_model_info(m);
  if (info.find(stanli::runtime_build_id()) == std::string::npos)
    fail("bs_model_info does not carry the build id: " + info);
  if (info.find("2.9.0") == std::string::npos)
    fail("bs_model_info does not name the BridgeStan ABI version: " + info);
  // Clients grep for BridgeStan's make-args spelling, not a bare word:
  // walnutpie refuses any model whose info lacks "STAN_THREADS=true".
  if (stanli::thread_safe_build() &&
      info.find("STAN_THREADS=true") == std::string::npos)
    fail("bs_model_info does not carry STAN_THREADS=true: " + info);

  bs_model_destruct(m);
}

// All four flag combinations, against the column layout the fixture
// declares: mu_c, sigma | prec | mu, sd_from_prec, resid.1..resid.50.
void test_param_num_and_names() {
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");
  char* err = nullptr;
  bs_model* m =
      bs_model_from_mir(mir.c_str(), "tests/fixtures/conj.json", 1, &err);
  if (m == nullptr) {
    fail(std::string("conj construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }

  std::string resid;
  for (int i = 1; i <= 50; ++i) resid += ",resid." + std::to_string(i);

  expect_eq_int("param_num(F,F)", bs_param_num(m, false, false), 2);
  expect_eq_int("param_num(T,F)", bs_param_num(m, true, false), 3);
  expect_eq_int("param_num(F,T)", bs_param_num(m, false, true), 54);
  expect_eq_int("param_num(T,T)", bs_param_num(m, true, true), 55);

  expect_eq_str("param_names(F,F)", bs_param_names(m, false, false),
                "mu_c,sigma");
  expect_eq_str("param_names(T,F)", bs_param_names(m, true, false),
                "mu_c,sigma,prec");
  // The interesting one: parameters plus generated quantities, with the
  // transformed parameter cut out of the middle.
  expect_eq_str("param_names(F,T)", bs_param_names(m, false, true),
                "mu_c,sigma,mu,sd_from_prec" + resid);
  expect_eq_str("param_names(T,T)", bs_param_names(m, true, true),
                "mu_c,sigma,prec,mu,sd_from_prec" + resid);

  // The strings belong to the model, so the same call twice is the same
  // pointer rather than a leak per call.
  if (bs_param_names(m, true, true) != bs_param_names(m, true, true))
    fail("bs_param_names is not cached on the model");

  bs_model_destruct(m);
}

void test_unc_names() {
  char* err = nullptr;

  // Eight schools: a scalar, a bounded scalar, and a vector. The
  // unconstrained count matches the declared shape throughout.
  const std::string es = slurp("tests/fixtures/es.tmir.sexp");
  bs_model* m = bs_model_from_mir(es.c_str(),
                                  "tests/fixtures/eight_schools.json", 1, &err);
  if (m == nullptr) {
    fail(std::string("es construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
  } else {
    std::string want = "mu,tau";
    for (int i = 1; i <= 8; ++i) want += ",theta_tilde." + std::to_string(i);
    expect_eq_str("es bs_param_unc_names", bs_param_unc_names(m), want);
    expect_eq_int("es bs_param_unc_num", bs_param_unc_num(m), 10);
    bs_model_destruct(m);
  }

  // A simplex[3] is THREE constrained values and TWO unconstrained ones,
  // so the declared dims cannot index the unconstrained vector; the names
  // fall back to flat 1..len.
  const std::string simp = slurp("tests/fixtures/simp.tmir.sexp");
  bs_model* s = bs_model_from_mir(simp.c_str(), "{\"K\": 3}", 1, &err);
  if (s == nullptr) {
    fail(std::string("simp construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_int("simp bs_param_unc_num", bs_param_unc_num(s), 2);
  expect_eq_str("simp bs_param_unc_names", bs_param_unc_names(s),
                "theta.1,theta.2");
  expect_eq_int("simp bs_param_num(F,F)", bs_param_num(s, false, false), 3);
  expect_eq_str("simp bs_param_names(F,F)", bs_param_names(s, false, false),
                "theta.1,theta.2,theta.3");
  bs_model_destruct(s);
}

// Generated Stan deserializes array[2,3] real outer-first from q. Its CSV
// surface is a separate contract: the first logical index is written fastest.
// Literal expectations keep this test independent of stanli's Executor and
// ParamView implementations.
// The graph write_array path, checked against the same graph run directly.
void test_constrain_graph() {
  using namespace stanli;
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");
  DataMap data = DataMap::from_json_file("tests/fixtures/conj.json");
  CompiledModel cm = compile_model(mir, data);
  if (!cm.write_array || cm.write_array->columns.empty()) {
    fail("conj: no graph write_array");
    return;
  }
  Executor wex(std::move(cm.write_array->graph));
  cm.write_array->bind(wex);
  const int64_t n = wex.n_params();
  std::vector<double> q((size_t)n);
  for (int64_t i = 0; i < n; ++i) q[(size_t)i] = point(i);
  for (int64_t i = 0; i < n; ++i) wex.params_data()[i] = q[(size_t)i];
  wex.run_forward_only();
  std::vector<double> want;
  for (const auto& c : cm.write_array->columns) {
    const double* p = wex.value_ptr(c.slot);
    for (int64_t i = 0; i < c.len; ++i) want.push_back(p[i]);
  }

  char* err = nullptr;
  bs_model* m =
      bs_model_from_mir(mir.c_str(), "tests/fixtures/conj.json", 1, &err);
  if (m == nullptr) {
    fail(std::string("conj construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }

  // Each flag combination is a slice of the same row, so the expectation
  // is built from the reference row rather than restated.
  struct Case {
    bool tp, gq;
    std::vector<size_t> idx;
  };
  std::vector<size_t> params = {0, 1}, tp = {2}, gq;
  for (size_t i = 3; i < want.size(); ++i) gq.push_back(i);
  auto cat = [](std::vector<size_t> a, const std::vector<size_t>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };
  const std::vector<Case> cases = {{false, false, params},
                                   {true, false, cat(params, tp)},
                                   {false, true, cat(params, gq)},
                                   {true, true, cat(cat(params, tp), gq)}};
  for (const auto& c : cases) {
    const std::string tag = std::string("conj constrain(") +
                            (c.tp ? "T" : "F") + "," + (c.gq ? "T" : "F") + ")";
    expect_eq_int(tag + " param_num", bs_param_num(m, c.tp, c.gq),
                  (long long)c.idx.size());
    std::vector<double> got(c.idx.size(), 0.0);
    // rng may be null whenever generated quantities are not asked for.
    expect_eq_int(
        tag + " rc",
        bs_param_constrain(m, c.tp, c.gq, q.data(), got.data(), nullptr, &err),
        c.gq ? -1 : 0);
    if (c.gq) {
      // No RNG handle and generated quantities requested: a refusal, not a
      // guess. Re-run with one.
      bs_free_error_msg(err);
      err = nullptr;
      bs_rng* rng = bs_rng_construct(7, &err);
      if (rng == nullptr) {
        fail("bs_rng_construct returned null");
        continue;
      }
      expect_eq_int(
          tag + " rc (with rng)",
          bs_param_constrain(m, c.tp, c.gq, q.data(), got.data(), rng, &err),
          0);
      bs_rng_destruct(rng);
    }
    for (size_t k = 0; k < c.idx.size(); ++k)
      expect_bitwise(tag + "[" + std::to_string(k) + "]", got[k],
                     want[c.idx[k]]);
  }

  // And one value pinned outright, so a reference row that was wrong in
  // the same way twice would still be caught: sigma is exp of its
  // unconstrained value.
  std::vector<double> two(2, 0.0);
  bs_param_constrain(m, false, false, q.data(), two.data(), nullptr, &err);
  expect_bitwise("conj sigma = exp(q1)", two[1], std::exp(q[1]));

  bs_model_destruct(m);
}

// The interpreted write_array path: per-draw control flow, with the stream
// owned by the caller's bs_rng.
void test_constrain_interp() {
  const std::string mir = slurp("tests/fixtures/gqrng.tmir.sexp");
  char* err = nullptr;
  // Data as a JSON LITERAL (another of the four `data` forms).
  bs_model* m = bs_model_from_mir(mir.c_str(), "{\"N\": 5}", 1, &err);
  if (m == nullptr) {
    fail(std::string("gqrng construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_int("gqrng param_num(F,F)", bs_param_num(m, false, false), 1);
  expect_eq_int("gqrng param_num(T,T)", bs_param_num(m, true, true), 5);
  expect_eq_str("gqrng param_names(T,T)", bs_param_names(m, true, true),
                "sigma,yrep,crep,branchy,p");

  const double q[1] = {0.53};  // sigma = exp(0.53) > 1, so branchy is 1
  bs_rng* a = bs_rng_construct(42, &err);
  bs_rng* b = bs_rng_construct(42, &err);
  if (a == nullptr || b == nullptr) {
    fail("bs_rng_construct returned null");
    return;
  }
  std::vector<double> ra(5, 0.0), rb(5, 0.0);
  expect_eq_int("gqrng constrain rc",
                bs_param_constrain(m, true, true, q, ra.data(), a, &err), 0);
  expect_eq_int("gqrng constrain rc (b)",
                bs_param_constrain(m, true, true, q, rb.data(), b, &err), 0);
  expect_bitwise("gqrng sigma", ra[0], std::exp(0.53));
  expect_bitwise("gqrng branchy", ra[3], 1.0);
  expect_bitwise("gqrng prod", ra[4], 6.0);
  if (ra[2] != std::floor(ra[2]) || ra[2] < 0.0 || ra[2] > 5.0)
    fail("gqrng crep is not an integer draw in [0, 5]");
  if (ra != rb) fail("two bs_rng handles at seed 42 drew different rows");

  // Independent advance: each handle carries its own stream, so the
  // second draw agrees between them and differs from the first.
  std::vector<double> ra2(5, 0.0), rb2(5, 0.0);
  bs_param_constrain(m, true, true, q, ra2.data(), a, &err);
  bs_param_constrain(m, true, true, q, rb2.data(), b, &err);
  if (ra2 != rb2) fail("bs_rng streams diverged on the second draw");
  if (ra2[1] == ra[1]) fail("the bs_rng stream did not advance");

  // The header allows a null rng whenever generated quantities are not
  // asked for -- including on a model whose write_array is the interpreter
  // and therefore always runs the RNG-bearing section.
  std::vector<double> only_params(1, 0.0);
  expect_eq_int(
      "gqrng constrain(F,F) with no rng",
      bs_param_constrain(m, false, false, q, only_params.data(), nullptr, &err),
      0);
  expect_bitwise("gqrng constrain(F,F) sigma", only_params[0], std::exp(0.53));

  bs_rng_destruct(a);
  bs_rng_destruct(b);
  bs_model_destruct(m);

  // A runtime RNG outcome cannot become categorical's compile-time integer
  // payload, so the whole section uses WaInterp; the categorical call must
  // remain available through BridgeStan.
  const std::string categorical = categorical_write_array_mir(
      slurp("tests/fixtures/cat.tmir.sexp"), "categorical_logit_lpmf", false,
      false, false, true);
  err = nullptr;
  m = bs_model_from_mir(categorical.c_str(), R"({"K":3,"y":2,"ys":[3,1,3]})",
                        1234, &err);
  if (m == nullptr) {
    fail(std::string("interpreted categorical construct: ") +
         (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_str("interpreted categorical names", bs_param_names(m, true, true),
                "theta.1,theta.2,theta.3,categorical_value");
  const double categorical_q[2] = {0.0, 0.0};
  std::vector<double> categorical_row(4);
  bs_rng* categorical_rng = bs_rng_construct(1234, &err);
  if (categorical_rng == nullptr) {
    fail("interpreted categorical rng construction");
  } else {
    expect_eq_int(
        "interpreted categorical constrain rc",
        bs_param_constrain(m, true, true, categorical_q, categorical_row.data(),
                           categorical_rng, &err),
        0);
    expect_bitwise("interpreted categorical value", categorical_row[3],
                   -std::log(3.0));
    bs_rng_destruct(categorical_rng);
  }
  bs_model_destruct(m);
}

// Transformed data draws come from the construction seed, once, when the
// model is built: the same seed builds the same model, a different seed
// different draws, and every draw reaches write_array like any other
// generated quantity. The fixture spans interpreter-only, graph-native,
// container, broadcast and user-function RNG spellings.
void test_transformed_data_rng() {
  const std::string mir = slurp("tests/fixtures/tdrng.tmir.sexp");
  char* err = nullptr;
  bs_model* a = bs_model_from_mir(mir.c_str(), "{}", 7, &err);
  bs_model* b = bs_model_from_mir(mir.c_str(), "{}", 7, &err);
  bs_model* c = bs_model_from_mir(mir.c_str(), "{}", 8, &err);
  bs_rng* rng = bs_rng_construct(1, &err);
  if (a == nullptr || b == nullptr || c == nullptr || rng == nullptr) {
    fail(std::string("transformed-data RNG construct: ") +
         (err ? err : "(no message)"));
    bs_free_error_msg(err);
  } else {
    expect_eq_str("transformed-data RNG names", bs_param_names(a, true, true),
                  "mu,td_z,td_k,td_d.1,td_d.2,td_d.3,td_mv.1,td_mv.2,td_g.1,"
                  "td_g.2,td_u");
    const double q[1] = {0.25};
    std::vector<double> ra(11), rb(11), rc(11);
    const int arc = bs_param_constrain(a, true, true, q, ra.data(), rng, &err);
    const int brc = bs_param_constrain(b, true, true, q, rb.data(), rng, &err);
    const int crc = bs_param_constrain(c, true, true, q, rc.data(), rng, &err);
    if (arc != 0 || brc != 0 || crc != 0)
      fail(std::string("transformed-data RNG constrain: ") +
           (err ? err : "(no message)"));
    if (ra != rb)
      fail("same construction seed built different transformed data");
    if (ra == rc)
      fail("different construction seeds built the same transformed data");
    if (ra[2] != std::floor(ra[2]) || ra[2] < 0.0)
      fail("transformed-data poisson draw is not a count");
    if (std::fabs(ra[3] + ra[4] + ra[5] - 1.0) > 1e-12)
      fail("transformed-data dirichlet draw is not a simplex");
  }
  bs_rng_destruct(rng);
  bs_model_destruct(a);
  bs_model_destruct(b);
  bs_model_destruct(c);
}

void test_constrain_compiled_rng() {
  const std::string mir = slurp("tests/fixtures/gq_scalar_rng.tmir.sexp");
  char* err = nullptr;
  bs_model* m = bs_model_from_mir(mir.c_str(), "{}", 1, &err);
  if (m == nullptr) {
    fail(std::string("compiled scalar RNG construct: ") +
         (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_str("compiled scalar RNG names", bs_param_names(m, true, true),
                "x,p,u,b,n,l,k,gm,ex");
  if (bs_param_num(m, true, true) != 9) {
    fail("compiled scalar RNG column count is not nine");
    bs_model_destruct(m);
    return;
  }
  const double q[1] = {0.25};
  bs_rng* a = bs_rng_construct(42, &err);
  bs_rng* b = bs_rng_construct(42, &err);
  if (a == nullptr || b == nullptr) {
    fail("compiled scalar RNG handle construction");
    bs_rng_destruct(a);
    bs_rng_destruct(b);
    bs_model_destruct(m);
    return;
  }
  std::vector<double> a1(9), b1(9), a2(9), b2(9);
  const int a1rc = bs_param_constrain(m, true, true, q, a1.data(), a, &err);
  const int b1rc = bs_param_constrain(m, true, true, q, b1.data(), b, &err);
  const int a2rc = bs_param_constrain(m, true, true, q, a2.data(), a, &err);
  const int b2rc = bs_param_constrain(m, true, true, q, b2.data(), b, &err);
  if (a1rc != 0 || b1rc != 0 || a2rc != 0 || b2rc != 0 || a1 != b1 ||
      a2 != b2 || a1 == a2)
    fail("compiled BridgeStan RNG streams are shared, stalled, or divergent");

  // No generated quantities requested: null is legal and the graph's
  // discarded effects run on an internal scratch stream.
  double x = 0.0;
  expect_eq_int("compiled scalar RNG no-gq null stream",
                bs_param_constrain(m, false, false, q, &x, nullptr, &err), 0);
  expect_bitwise("compiled scalar RNG parameter-only value", x, q[0]);
  bs_rng_destruct(a);
  bs_rng_destruct(b);
  bs_model_destruct(m);
}

// Generated quantities built out of operators rather than function calls:
// integer `%` and `%/%`, and the two matrix solves. This pins that the
// columns are all there and carry the right values.
void test_constrain_operators() {
  const std::string mir = slurp("tests/fixtures/gqops.tmir.sexp");
  char* err = nullptr;
  bs_model* m =
      bs_model_from_mir(mir.c_str(), "tests/fixtures/gqops.json", 1, &err);
  if (m == nullptr) {
    fail(std::string("gqops construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_int("gqops param_num(F,F)", bs_param_num(m, false, false), 8);
  expect_eq_int("gqops param_num(T,T)", bs_param_num(m, true, true), 14);
  expect_eq_str("gqops param_names(T,T)", bs_param_names(m, true, true),
                "v.1,v.2,rv.1,rv.2,A.1.1,A.2.1,A.1.2,A.2.2,i,q,dv.1,dv.2,"
                "drv.1,drv.2");

  // v = (1,2), rv = (3,4), A = diag(2,4) column-major. Every generated
  // value is exact in binary: 7 % 2 = 1, 7 %/% 2 = 3, A \ v = (0.5, 0.5),
  // rv / A = (1.5, 1).
  const double q[8] = {1, 2, 3, 4, 2, 0, 0, 4};
  std::vector<double> row(14, 0.0);
  bs_rng* rng = bs_rng_construct(1, &err);
  if (rng == nullptr) {
    fail("gqops rng construction");
    bs_model_destruct(m);
    return;
  }
  expect_eq_int("gqops constrain rc",
                bs_param_constrain(m, true, true, q, row.data(), rng, &err), 0);
  const double want[14] = {1, 2, 3, 4, 2, 0, 0, 4, 1, 3, 0.5, 0.5, 1.5, 1};
  for (size_t k = 0; k < row.size(); ++k)
    expect_bitwise("gqops row[" + std::to_string(k) + "]", row[k], want[k]);
  bs_rng_destruct(rng);
  bs_model_destruct(m);
}

void test_unsupported() {
  const std::string mir = slurp("tests/fixtures/es.tmir.sexp");
  char* err = nullptr;
  bs_model* m = bs_model_from_mir(mir.c_str(),
                                  "tests/fixtures/eight_schools.json", 1, &err);
  if (m == nullptr) {
    fail(std::string("es construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  const int64_t n = bs_param_unc_num(m);
  std::vector<double> q((size_t)n, 0.1), out((size_t)n * (size_t)n, 0.0),
      grad((size_t)n, 0.0), vec((size_t)n, 1.0);
  double val = 0;

  err = nullptr;
  expect_refused("bs_log_density_hessian",
                 bs_log_density_hessian(m, true, true, q.data(), &val,
                                        grad.data(), out.data(), &err),
                 &err, "first derivatives only");
  err = nullptr;
  expect_refused(
      "bs_log_density_hessian_vector_product",
      bs_log_density_hessian_vector_product(m, true, true, q.data(), vec.data(),
                                            &val, grad.data(), &err),
      &err, "first derivatives only");
  // Both unconstrain entry points answer now. The round trip is the check
  // that matters: constrain a free point and require it back, which fails on
  // any drift between the two directions or any layout slip between the
  // serial order theta uses and the arena order q uses.
  err = nullptr;
  std::vector<double> theta((size_t)bs_param_num(m, false, false), 0.0);
  expect_eq_int("bs_param_constrain for the unconstrain round trip",
                bs_param_constrain(m, false, false, q.data(), theta.data(),
                                   nullptr, &err),
                0);
  std::vector<double> back((size_t)n, 0.0);
  expect_eq_int("bs_param_unconstrain rc",
                bs_param_unconstrain(m, theta.data(), back.data(), &err), 0);
  for (int64_t i = 0; i < n; ++i)
    if (!(std::abs(back[(size_t)i] - q[(size_t)i]) < 1e-9))
      fail("bs_param_unconstrain round trip at " + std::to_string(i) +
           ": got " + std::to_string(back[(size_t)i]) + ", want " +
           std::to_string(q[(size_t)i]));
  // A document that leaves a parameter out is refused by that parameter's
  // name, not by an index into a buffer the caller never sees.
  err = nullptr;
  expect_refused("bs_param_unconstrain_json missing a parameter",
                 bs_param_unconstrain_json(m, "{\"mu\": 0}", back.data(), &err),
                 &err, "tau");
  err = nullptr;
  expect_refused(
      "bs_param_unconstrain_json unknown parameter",
      bs_param_unconstrain_json(
          m,
          "{\"mu\": 0, \"tau\": 1, \"theta_tilde\": [0,0,0,0,0,0,0,0], "
          "\"not_a_parameter\": 1}",
          back.data(), &err),
      &err, "not_a_parameter");

  // Density flags: only (propto=true, jacobian=true) is the quantity a
  // stanli graph computes, so the other three refuse rather than serve a
  // different number.
  const std::string flags = "propto=true, jacobian=true";
  err = nullptr;
  expect_refused("bs_log_density propto=false",
                 bs_log_density(m, false, true, q.data(), &val, &err), &err,
                 flags);
  err = nullptr;
  expect_refused("bs_log_density jacobian=false",
                 bs_log_density(m, true, false, q.data(), &val, &err), &err,
                 flags);
  err = nullptr;
  expect_refused("bs_log_density_gradient propto=false",
                 bs_log_density_gradient(m, false, true, q.data(), &val,
                                         grad.data(), &err),
                 &err, flags);
  err = nullptr;
  expect_refused("bs_log_density_gradient jacobian=false",
                 bs_log_density_gradient(m, true, false, q.data(), &val,
                                         grad.data(), &err),
                 &err, flags);

  err = nullptr;
  bs_rng* rng = bs_rng_construct(1, &err);
  err = nullptr;
  expect_refused("bs_param_initialize with an incomplete json point",
                 bs_param_initialize(m, "{\"mu\": 0}", rng, 2.0, 100, true,
                                     q.data(), &err),
                 &err, "tau");
  err = nullptr;
  expect_refused(
      "bs_param_initialize jacobian=false",
      bs_param_initialize(m, nullptr, rng, 2.0, 100, false, q.data(), &err),
      &err, flags);
  bs_rng_destruct(rng);
  bs_model_destruct(m);
}

void test_initialize() {
  const std::string mir = slurp("tests/fixtures/es.tmir.sexp");
  char* err = nullptr;
  bs_model* m = bs_model_from_mir(mir.c_str(),
                                  "tests/fixtures/eight_schools.json", 1, &err);
  if (m == nullptr) {
    fail(std::string("es construct: ") + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  const int64_t n = bs_param_unc_num(m);
  bs_rng* rng = bs_rng_construct(99, &err);
  std::vector<double> q((size_t)n, 0.0);
  expect_eq_int(
      "bs_param_initialize rc",
      bs_param_initialize(m, nullptr, rng, 2.0, 100, true, q.data(), &err), 0);
  for (int64_t i = 0; i < n; ++i)
    if (!(q[(size_t)i] >= -2.0 && q[(size_t)i] < 2.0))
      fail("bs_param_initialize left q[" + std::to_string(i) +
           "] outside [-2, 2): " + std::to_string(q[(size_t)i]));
  double lp = 0;
  expect_eq_int("initialized point evaluates",
                bs_log_density(m, true, true, q.data(), &lp, &err), 0);
  if (!std::isfinite(lp))
    fail("bs_param_initialize returned a point with non-finite log density");

  // An explicit constrained point is taken as given rather than replaced by a
  // random draw: build one by constraining a known free point, hand it back
  // as JSON, and require exactly that free point out.
  const auto number = [](double x) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.17g", x);
    return std::string(buf);
  };
  std::vector<double> chosen((size_t)n, 0.0);
  for (int64_t i = 0; i < n; ++i)
    chosen[(size_t)i] = 0.05 * (double)((i % 5) - 2);
  std::vector<double> theta((size_t)bs_param_num(m, false, false), 0.0);
  expect_eq_int("constrain the chosen point",
                bs_param_constrain(m, false, false, chosen.data(), theta.data(),
                                   nullptr, &err),
                0);
  // eight_schools: mu, tau, then theta_tilde's J values, in CSV order.
  std::string json = "{\"mu\": " + number(theta[0]) +
                     ", \"tau\": " + number(theta[1]) + ", \"theta_tilde\": [";
  for (size_t i = 2; i < theta.size(); ++i)
    json += (i > 2 ? ", " : "") + number(theta[i]);
  json += "]}";

  std::vector<double> from_json((size_t)n, 0.0);
  expect_eq_int("bs_param_initialize from an explicit point",
                bs_param_initialize(m, json.c_str(), rng, 2.0, 100, true,
                                    from_json.data(), &err),
                0);
  for (int64_t i = 0; i < n; ++i)
    if (!(std::abs(from_json[(size_t)i] - chosen[(size_t)i]) < 1e-9))
      fail("bs_param_initialize moved the explicit point at " +
           std::to_string(i) + ": got " + std::to_string(from_json[(size_t)i]) +
           ", want " + std::to_string(chosen[(size_t)i]));

  // Genuine exhaustion, not a contrived one: at a radius this wide every
  // draw puts mu at a magnitude whose normal(0, 5) term is -inf and tau at
  // exp(huge) = inf, so no attempt can produce a finite log density.
  std::vector<double> qq((size_t)n, 0.0);
  err = nullptr;
  expect_refused(
      "bs_param_initialize exhausts",
      bs_param_initialize(m, nullptr, rng, 1e300, 5, true, qq.data(), &err),
      &err, "initialization failed to find a point");

  bs_rng_destruct(rng);
  bs_model_destruct(m);
}

// ---------------------------------------------------------------------------

std::string g_printed;
void collect_print(const char* data, size_t size) {
  g_printed.append(data, size);
}

void test_print_callback() {
  char* err = nullptr;
  expect_eq_int("bs_set_print_callback rc",
                bs_set_print_callback(&collect_print, &err), 0);

  // rejectprint prints from transformed data (at construction, through the
  // MIR interpreter) and from the model block (at evaluation, through the
  // OP_PRINT kernel). Both have to arrive.
  const std::string mir = slurp("tests/fixtures/rejectprint.tmir.sexp");
  g_printed.clear();
  bs_model* m =
      bs_model_from_mir(mir.c_str(), "{\"N\": 3, \"lim\": 1.0}", 1, &err);
  if (m == nullptr) {
    fail(std::string("rejectprint construct: ") + (err ? err : "(none)"));
    bs_free_error_msg(err);
    bs_set_print_callback(nullptr, &err);
    return;
  }
  if (g_printed.find("compiled with N = 3") == std::string::npos)
    fail("transformed-data print did not reach the callback: [" + g_printed +
         "]");

  g_printed.clear();
  std::vector<double> q((size_t)bs_param_unc_num(m), 0.25);
  double lp = 0;
  bs_log_density(m, true, true, q.data(), &lp, &err);
  if (g_printed.find("drawing at x = ") == std::string::npos)
    fail("model-block print did not reach the callback: [" + g_printed + "]");
  // Reference BridgeStan hands the callback the bytes Stan wrote to the
  // stream, and Stan's print() ends its line; a client concatenating
  // chunks has to get the same text.
  if (g_printed.empty() || g_printed.back() != '\n')
    fail("print callback text is not newline-terminated: [" + g_printed + "]");

  // Null restores the default, so nothing further reaches the callback.
  expect_eq_int("bs_set_print_callback(null) rc",
                bs_set_print_callback(nullptr, &err), 0);
  g_printed.clear();
  bs_log_density(m, true, true, q.data(), &lp, &err);
  if (!g_printed.empty())
    fail("the callback still received output after being cleared: [" +
         g_printed + "]");

  bs_model_destruct(m);
}

void test_necessity_effects() {
  const std::string mir = slurp("tests/fixtures/necessity_effects.tmir.sexp");
  char* err = nullptr;
  expect_eq_int("necessity callback install",
                bs_set_print_callback(&collect_print, &err), 0);

  nlohmann::json root = {{"mode", 1}};
  root["__stanli"] = {{"build_id", stanli::runtime_build_id()},
                      {"mir", mir},
                      {"name", "necessity_effects"}};
  g_printed.clear();
  bs_model* m = bs_model_construct(root.dump().c_str(), 1, &err);
  if (m == nullptr) {
    fail("BridgeStan refused necessity print: " +
         std::string(err != nullptr ? err : "(no message)"));
    bs_free_error_msg(err);
  } else {
    const double q = 0.1;
    double lp = 0, grad = 0;
    g_printed.clear();
    expect_eq_int("BridgeStan necessity print gradient",
                  bs_log_density_gradient(m, true, true, &q, &lp, &grad, &err),
                  0);
    if (g_printed != "positive x=0.1\n")
      fail("BridgeStan necessity print did not execute exactly once: [" +
           g_printed + "]");
    bs_model_destruct(m);
  }

  root["mode"] = 2;
  g_printed.clear();
  err = nullptr;
  m = bs_model_construct(root.dump().c_str(), 1, &err);
  if (m == nullptr) {
    fail("BridgeStan refused dynamic necessity reject: " +
         std::string(err != nullptr ? err : "(no message)"));
    bs_free_error_msg(err);
  } else {
    double lp = 0.0;
    const double accepted = 0.1;
    expect_eq_int("BridgeStan untaken dynamic reject",
                  bs_log_density(m, true, true, &accepted, &lp, &err), 0);
    const double rejected = -0.1;
    expect_refused("BridgeStan taken dynamic reject",
                   bs_log_density(m, true, true, &rejected, &lp, &err), &err,
                   "negative x=-0.1");
    bs_model_destruct(m);
  }
  if (!g_printed.empty())
    fail("BridgeStan dynamic reject unexpectedly printed: [" + g_printed + "]");

  err = nullptr;
  expect_eq_int("necessity callback clear",
                bs_set_print_callback(nullptr, &err), 0);
}

// No parameters: theta_unc is null and the parameter buffer is empty, so
// the copy into it must be skipped rather than made from a null pointer.
void test_parameterless() {
  const std::string mir = slurp("tests/fixtures/view_gq_data_matrix.tmir.sexp");
  char* err = nullptr;
  bs_model* m =
      bs_model_from_mir(mir.c_str(), R"({"M":[[1,2,3],[4,5,6]]})", 1234, &err);
  if (m == nullptr) {
    fail(std::string("parameterless construct: ") +
         (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_int("parameterless bs_param_unc_num", bs_param_unc_num(m), 0);
  double lp = 1.0;
  expect_eq_int("parameterless bs_log_density",
                bs_log_density(m, true, true, nullptr, &lp, &err), 0);
  expect_bitwise("parameterless log density", lp, 0.0);
  lp = 1.0;
  expect_eq_int(
      "parameterless bs_log_density_gradient",
      bs_log_density_gradient(m, true, true, nullptr, &lp, nullptr, &err), 0);
  expect_bitwise("parameterless gradient log density", lp, 0.0);
  bs_rng* rng = bs_rng_construct(1, &err);
  std::vector<double> row((size_t)bs_param_num(m, true, true), -1.0);
  expect_eq_int(
      "parameterless bs_param_constrain",
      bs_param_constrain(m, true, true, nullptr, row.data(), rng, &err), 0);
  if (row.size() != 2 || row[0] != 6.0)
    fail("parameterless bs_param_constrain row is wrong");
  bs_rng_destruct(rng);
  bs_model_destruct(m);
}

// The manifest reader on its own: the manifest is read and its build id
// checked before anything is compiled.
void test_manifest() {
  const std::string id = stanli::runtime_build_id();
  stanli::BsManifest man;
  std::string err;

  const std::string good = "{\"build_id\": \"" + id +
                           "\", \"name\": \"mymodel\", \"mir\": \"(prog)\"}";
  if (!stanli::bs_read_manifest(good, &man, &err)) {
    fail("a matching manifest was rejected: " + err);
  } else {
    expect_eq_str("manifest name", man.name, "mymodel");
    expect_eq_str("manifest mir", man.mir, "(prog)");
  }

  const std::string stale =
      "{\"build_id\": \"abi1-deadbeef-Linux-x86_64\", \"name\": \"m\", "
      "\"mir\": \"(prog)\"}";
  err.clear();
  if (stanli::bs_read_manifest(stale, &man, &err)) {
    fail("a manifest from another build was accepted");
  } else {
    if (err.find("abi1-deadbeef-Linux-x86_64") == std::string::npos ||
        err.find(id) == std::string::npos)
      fail("the build-id mismatch message names neither id: " + err);
  }

  err.clear();
  if (stanli::bs_read_manifest("not json at all", &man, &err))
    fail("a malformed manifest was accepted");
  else if (err.empty())
    fail("a malformed manifest was rejected without a message");

  err.clear();
  if (stanli::bs_read_manifest("{\"build_id\": \"" + id + "\"}", &man, &err))
    fail("a manifest with no mir was accepted");
  else if (err.empty())
    fail("a manifest with no mir was rejected without a message");
}

// The manifest transport: the data JSON carries the model under
// "__stanli", so the full bs_model_construct path is exercised
// in-process here.
void test_embedded_manifest() {
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");
  nlohmann::json root =
      nlohmann::json::parse(slurp("tests/fixtures/conj.json"));
  root["__stanli"] = {{"build_id", stanli::runtime_build_id()},
                      {"mir", mir},
                      {"name", "conj_embedded"}};
  const std::string data = root.dump();

  char* err = nullptr;
  bs_model* m = bs_model_construct(data.c_str(), 1234, &err);
  if (m == nullptr) {
    fail(std::string("bs_model_construct rejected an embedded manifest: ") +
         (err != nullptr ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }
  expect_eq_str("embedded name reaches bs_name", bs_name(m), "conj_embedded");

  // The same model through the bs_model_from_mir seam; the key must have
  // been stripped, so the two see identical data and agree bitwise.
  bs_model* w =
      bs_model_from_mir(mir.c_str(), "tests/fixtures/conj.json", 1234, &err);
  if (w == nullptr) {
    fail("bs_model_from_mir failed on the reference construction");
    bs_model_destruct(m);
    return;
  }
  const int n = bs_param_unc_num(m);
  expect_eq_int("embedded param count", n, bs_param_unc_num(w));
  std::vector<double> q((size_t)n);
  for (int i = 0; i < n; ++i) q[(size_t)i] = 0.1 * (double)(i + 1);
  double lp = 0, want_lp = 0;
  std::vector<double> g((size_t)n), want_g((size_t)n);
  expect_eq_int(
      "embedded gradient rc",
      bs_log_density_gradient(m, true, true, q.data(), &lp, g.data(), &err), 0);
  expect_eq_int("reference gradient rc",
                bs_log_density_gradient(w, true, true, q.data(), &want_lp,
                                        want_g.data(), &err),
                0);
  expect_bitwise("embedded lp", lp, want_lp);
  for (int i = 0; i < n; ++i)
    expect_bitwise("embedded grad[" + std::to_string(i) + "]", g[(size_t)i],
                   want_g[(size_t)i]);
  bs_model_destruct(w);
  bs_model_destruct(m);

  // Stan's data reader accepts bare NaN and Infinity, and Python's
  // json.dumps writes them, so the payload around the manifest must too.
  const std::string non_finite =
      "{\"spare\": [NaN, Infinity, -Infinity], " + data.substr(1);
  err = nullptr;
  m = bs_model_construct(non_finite.c_str(), 1234, &err);
  if (m == nullptr)
    fail(std::string("NaN and Infinity in the data were rejected: ") +
         (err != nullptr ? err : "(no message)"));
  bs_free_error_msg(err);
  bs_model_destruct(m);

  // A wrong build id is a loud staleness error: the MIR dialect moves
  // with the runtime that lowers it.
  root["__stanli"]["build_id"] = "abi1-deadbeef-Linux-x86_64";
  err = nullptr;
  m = bs_model_construct(root.dump().c_str(), 1234, &err);
  if (m != nullptr) {
    fail("an embedded manifest from another build was accepted");
    bs_model_destruct(m);
  } else if (err == nullptr ||
             std::string(err).find("abi1-deadbeef") == std::string::npos) {
    fail(std::string("the embedded staleness message names no build id: ") +
         (err != nullptr ? err : "(no message)"));
  }
  bs_free_error_msg(err);

  // Mentioning the key and getting it wrong is loud, never a silent
  // slide into the plain-data "no manifest" refusal.
  err = nullptr;
  m = bs_model_construct("{\"__stanli\": 7}", 1234, &err);
  if (m != nullptr) {
    fail("a non-object __stanli was accepted");
    bs_model_destruct(m);
  } else if (err == nullptr) {
    fail("a non-object __stanli failed without a message");
  }
  bs_free_error_msg(err);
}

std::string base64_of(const void* data, size_t bytes) {
  static const char* digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const unsigned char* p = static_cast<const unsigned char*>(data);
  std::string out;
  for (size_t i = 0; i < bytes; i += 3) {
    const uint32_t a = p[i], b = i + 1 < bytes ? p[i + 1] : 0,
                   c = i + 2 < bytes ? p[i + 2] : 0;
    const uint32_t word = a << 16 | b << 8 | c;
    out.push_back(digits[word >> 18 & 63]);
    out.push_back(digits[word >> 12 & 63]);
    out.push_back(i + 1 < bytes ? digits[word >> 6 & 63] : '=');
    out.push_back(i + 2 < bytes ? digits[word & 63] : '=');
  }
  return out;
}

template <class T>
std::string base64_of(const std::vector<T>& v) {
  return base64_of(v.data(), v.size() * sizeof(T));
}

void check_whitened_bridge(const std::string& label, int64_t k) {
  using namespace stanli;
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");
  const std::string json = slurp("tests/fixtures/conj.json");
  const int64_t n = 2;
  const std::vector<double> q_hat = {5.9, -0.31};
  const std::vector<int64_t> perm = {1, 0};
  const std::vector<int64_t> col_ptr = {0, 2, 3};
  const std::vector<int64_t> row_idx = {0, 1, 1};
  const std::vector<double> values = {1.3, 0.4, 0.9};
  const std::vector<double> c = {1.1, 0.8}, b = {0.3, 0.2}, w = {0.5, -0.7};
  const auto spec = make_whiten_spec(
      q_hat.data(), n, perm.data(), col_ptr.data(), row_idx.data(),
      values.data(), 3, k, k > 0 ? c.data() : nullptr,
      k > 0 ? b.data() : nullptr, k > 0 ? w.data() : nullptr);

  DataMap data = DataMap::from_json_file("tests/fixtures/conj.json");
  CompiledModel cm = compile_model(mir, data);
  Executor inner(std::move(cm.graph));
  cm.bind(inner);
  Executor ref(whiten_graph(inner.graph(), spec), inner);

  char* err = nullptr;
  bs_model* plain =
      bs_model_from_mir(mir.c_str(), "tests/fixtures/conj.json", 1234, &err);
  if (plain == nullptr) {
    fail(label + ": plain construction: " + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    return;
  }

  nlohmann::json root = nlohmann::json::parse(json);
  nlohmann::json whiten = {{"q_hat", base64_of(q_hat)},
                           {"perm", base64_of(perm)},
                           {"col_ptr", base64_of(col_ptr)},
                           {"row_idx", base64_of(row_idx)},
                           {"values", base64_of(values)}};
  if (k > 0) {
    whiten["k"] = k;
    whiten["c"] = base64_of(c);
    whiten["b"] = base64_of(b);
    whiten["w"] = base64_of(w);
  }
  root["__stanli"] = {{"build_id", stanli::runtime_build_id()},
                      {"mir", mir},
                      {"name", "conj_whitened"},
                      {"whiten", whiten}};
  const std::string data_text = root.dump();
  bs_model* m = bs_model_construct(data_text.c_str(), 1234, &err);
  if (m == nullptr) {
    fail(label + ": bs_model_construct: " + (err ? err : "(no message)"));
    bs_free_error_msg(err);
    bs_model_destruct(plain);
    return;
  }
  expect_eq_int(label + " unconstrained count", bs_param_unc_num(m), n);
  expect_eq_str(label + " unconstrained names", bs_param_unc_names(m),
                "z.1,z.2");

  const std::vector<std::vector<double>> points = {
      {0.0, 0.0}, {0.3, -0.17}, {-1.2, 0.8}, {2.5, -2.1}};
  for (size_t t = 0; t < points.size(); ++t) {
    const std::string at = label + " point " + std::to_string(t);
    const std::vector<double>& z = points[t];

    for (int64_t i = 0; i < n; ++i) ref.params_data()[i] = z[(size_t)i];
    std::vector<double> want_g((size_t)n);
    const double want_lp = ref.gradient(want_g.data());
    double lp = 0;
    std::vector<double> g((size_t)n);
    expect_eq_int(
        at + " gradient rc",
        bs_log_density_gradient(m, true, true, z.data(), &lp, g.data(), &err),
        0);
    expect_bitwise(at + " lp", lp, want_lp);
    for (int64_t i = 0; i < n; ++i)
      expect_bitwise(at + " grad[" + std::to_string(i) + "]", g[(size_t)i],
                     want_g[(size_t)i]);

    std::vector<double> q((size_t)n);
    spec->to_q(z.data(), q.data());
    const int cols = bs_param_num(plain, true, true);
    std::vector<double> want_row((size_t)cols), row((size_t)cols);
    bs_rng* rng = bs_rng_construct(1, &err);
    expect_eq_int(at + " inner constrain rc",
                  bs_param_constrain(plain, true, true, q.data(),
                                     want_row.data(), rng, &err),
                  0);
    expect_eq_int(
        at + " constrain rc",
        bs_param_constrain(m, true, true, z.data(), row.data(), rng, &err), 0);
    bs_rng_destruct(rng);
    expect_eq_int(at + " constrained count", bs_param_num(m, true, true), cols);
    for (int i = 0; i < cols; ++i)
      expect_bitwise(at + " row[" + std::to_string(i) + "]", row[(size_t)i],
                     want_row[(size_t)i]);

    std::vector<double> inner_q((size_t)n), want_back((size_t)n);
    const std::vector<double> flat = {want_row[0], want_row[1]};
    expect_eq_int(
        at + " inner unconstrain rc",
        bs_param_unconstrain(plain, flat.data(), inner_q.data(), &err), 0);
    spec->to_z(inner_q.data(), want_back.data());
    std::vector<double> back((size_t)n);
    expect_eq_int(at + " unconstrain rc",
                  bs_param_unconstrain(m, flat.data(), back.data(), &err), 0);
    for (int64_t i = 0; i < n; ++i)
      expect_bitwise(at + " unconstrain[" + std::to_string(i) + "]",
                     back[(size_t)i], want_back[(size_t)i]);

    const std::string inits =
        "{\"mu_c\": " + nlohmann::json(want_row[0]).dump() +
        ", \"sigma\": " + nlohmann::json(want_row[1]).dump() + "}";
    std::vector<double> back_json((size_t)n);
    expect_eq_int(
        at + " unconstrain_json rc",
        bs_param_unconstrain_json(m, inits.c_str(), back_json.data(), &err), 0);
    std::vector<double> inner_json((size_t)n), want_json((size_t)n);
    expect_eq_int(at + " inner unconstrain_json rc",
                  bs_param_unconstrain_json(plain, inits.c_str(),
                                            inner_json.data(), &err),
                  0);
    spec->to_z(inner_json.data(), want_json.data());
    for (int64_t i = 0; i < n; ++i)
      expect_bitwise(at + " unconstrain_json[" + std::to_string(i) + "]",
                     back_json[(size_t)i], want_json[(size_t)i]);
  }

  const std::string start = "{\"mu_c\": 5.9, \"sigma\": 0.7}";
  std::vector<double> inner_start((size_t)n), want((size_t)n), got((size_t)n);
  bs_param_unconstrain_json(plain, start.c_str(), inner_start.data(), &err);
  spec->to_z(inner_start.data(), want.data());
  bs_rng* rng = bs_rng_construct(7, &err);
  expect_eq_int(label + " initialize rc",
                bs_param_initialize(m, start.c_str(), rng, 2.0, 10, true,
                                    got.data(), &err),
                0);
  bs_rng_destruct(rng);
  for (int64_t i = 0; i < n; ++i)
    expect_bitwise(label + " initialize[" + std::to_string(i) + "]",
                   got[(size_t)i], want[(size_t)i]);

  bs_model_destruct(m);
  bs_model_destruct(plain);
}

void test_whitened_manifest() {
  check_whitened_bridge("whitened", 0);
  check_whitened_bridge("whitened lowrank", 1);

  stanli::BsManifest man;
  std::string err;
  const std::string base = std::string("{\"build_id\": \"") +
                           stanli::runtime_build_id() + "\", \"mir\": \"x\"";
  if (!stanli::bs_read_manifest(base + "}", &man, &err) || man.whiten)
    fail("a manifest with no whiten member reads as unwhitened");
  if (stanli::bs_read_manifest(base + ", \"whiten\": 3}", &man, &err))
    fail("a non-object whiten member was accepted");
  if (stanli::bs_read_manifest(base + ", \"whiten\": {}}", &man, &err) ||
      err.find("q_hat") == std::string::npos)
    fail("an empty whiten member did not name the missing q_hat: " + err);
  const std::string bad = base +
                          ", \"whiten\": {\"q_hat\": \"!!!!\", \"perm\": \"\", "
                          "\"col_ptr\": \"\", \"row_idx\": \"\", \"values\": "
                          "\"\"}}";
  if (stanli::bs_read_manifest(bad, &man, &err) ||
      err.find("base64") == std::string::npos)
    fail("invalid base64 in whiten was not refused as base64: " + err);
  const std::vector<double> one = {1.0};
  const std::vector<int64_t> p0 = {0}, cp = {0, 1}, ri = {0};
  const std::string tail = ", \"whiten\": {\"q_hat\": \"" + base64_of(one) +
                           "\", \"perm\": \"" + base64_of(p0) +
                           "\", \"col_ptr\": \"" + base64_of(cp) +
                           "\", \"row_idx\": \"" + base64_of(ri) +
                           "\", \"values\": \"" + base64_of(one) + "\"}}";
  if (!stanli::bs_read_manifest(base + tail, &man, &err) || !man.whiten ||
      man.whiten->n != 1)
    fail("a one-parameter whiten member was not read: " + err);
  const std::vector<double> zero = {0.0};
  const std::string singular = ", \"whiten\": {\"q_hat\": \"" + base64_of(one) +
                               "\", \"perm\": \"" + base64_of(p0) +
                               "\", \"col_ptr\": \"" + base64_of(cp) +
                               "\", \"row_idx\": \"" + base64_of(ri) +
                               "\", \"values\": \"" + base64_of(zero) + "\"}}";
  if (stanli::bs_read_manifest(base + singular, &man, &err))
    fail("a whitening with a zero diagonal was accepted");
}

void test_construct_errors() {
  char* err = nullptr;
  // A model that cannot be compiled fails as a null return with a message,
  // never as an escaping exception.
  bs_model* m = bs_model_from_mir("(not a program)", nullptr, 1, &err);
  if (m != nullptr) {
    fail("bs_model_from_mir accepted nonsense MIR");
    bs_model_destruct(m);
  } else if (err == nullptr) {
    fail("bs_model_from_mir failed without a message");
  }
  bs_free_error_msg(err);

  // Data that is neither a path nor parseable JSON is the same story.
  err = nullptr;
  const std::string mir = slurp("tests/fixtures/conj.tmir.sexp");
  m = bs_model_from_mir(mir.c_str(), "{not json", 1, &err);
  if (m != nullptr) {
    fail("bs_model_from_mir accepted malformed data JSON");
    bs_model_destruct(m);
  } else if (err == nullptr) {
    fail("malformed data JSON failed without a message");
  }
  bs_free_error_msg(err);

  // A constrained transformed parameter is checked per draw on both the
  // density and write_array surfaces. Construction itself still succeeds.
  err = nullptr;
  const std::string runtime_mir =
      slurp("tests/fixtures/data_and_tp_checks.tmir.sexp");
  m = bs_model_from_mir(
      runtime_mir.c_str(),
      R"({"d":0,"raw":0,"N":1,"M":1,"lo":[-10],"R":1,"C":1,"BR":1,"BC":1,"matrix_lo":[[-10]]})",
      1, &err);
  if (m == nullptr) {
    fail(std::string("runtime-bound model construction: ") +
         (err != nullptr ? err : "(no message)"));
    bs_free_error_msg(err);
  } else {
    const double q = -1.0;
    double lp = 0.0;
    err = nullptr;
    const int density_rc = bs_log_density(m, true, true, &q, &lp, &err);
    expect_refused("runtime-bound density", density_rc, &err, "z");

    std::vector<double> row((size_t)bs_param_num(m, true, false));
    const int constrain_rc =
        bs_param_constrain(m, true, false, &q, row.data(), nullptr, &err);
    expect_refused("runtime-bound write_array", constrain_rc, &err, "z");
    bs_model_destruct(m);
  }

  // Structured declaration checks use the same phase and error contract,
  // including arrays whose JSON storage interleaves their leaves.
  const std::string structured_mir =
      slurp("tests/fixtures/structured_checks.tmir.sexp");
  const std::string structured_good =
      slurp("tests/fixtures/structured_checks.json");
  std::string structured_bad = structured_good;
  const std::string valid_simplex = "[0.2, 0.3, 0.5]";
  const size_t simplex_at = structured_bad.find(valid_simplex);
  if (simplex_at == std::string::npos) {
    fail("BridgeStan structured fixture replacement");
  } else {
    structured_bad.replace(simplex_at, valid_simplex.size(), "[0.2, 0.3, 0.6]");
    err = nullptr;
    m = bs_model_from_mir(structured_mir.c_str(), structured_bad.c_str(), 1,
                          &err);
    if (m != nullptr) {
      fail("BridgeStan accepted invalid simplex data");
      bs_model_destruct(m);
    } else if (err == nullptr ||
               std::string(err).find("d_simplex") == std::string::npos) {
      fail(std::string("BridgeStan invalid simplex error: ") +
           (err != nullptr ? err : "(no message)"));
    }
    bs_free_error_msg(err);
  }

  err = nullptr;
  m = bs_model_from_mir(structured_mir.c_str(), structured_good.c_str(), 1,
                        &err);
  if (m == nullptr) {
    fail(std::string("BridgeStan structured-check construction: ") +
         (err != nullptr ? err : "(no message)"));
    bs_free_error_msg(err);
  } else {
    double q[2] = {0.25, 0.0};
    double lp = 0.0;
    int rc = bs_log_density(m, true, true, q, &lp, &err);
    expect_refused("structured transformed parameter", rc, &err, "tp_simplex");

    q[0] = 0.0;
    q[1] = 1.0;
    std::vector<double> row((size_t)bs_param_num(m, true, true));
    bs_rng* rng = bs_rng_construct(1, &err);
    if (rng == nullptr) {
      fail(std::string("structured-check rng construction: ") +
           (err != nullptr ? err : "(no message)"));
      bs_free_error_msg(err);
      err = nullptr;
    } else {
      rc = bs_param_constrain(m, true, true, q, row.data(), rng, &err);
      expect_refused("structured generated quantity", rc, &err,
                     "gq_sum_to_zero");
      bs_rng_destruct(rng);
    }
    bs_model_destruct(m);
  }

  // All-data propto categorical calls remain runtime checks. The facade must
  // propagate the same Stan Math error instead of reporting a finite density.
  err = nullptr;
  const std::string categorical_mir =
      categorical_check_mir("categorical_logit_lpmf", false, 1, 3);
  m = bs_model_from_mir(categorical_mir.c_str(),
                        R"({"outcome":[4],"arg":[-1,0,1]})", 1, &err);
  if (m == nullptr) {
    fail(std::string("categorical-check model construction: ") +
         (err != nullptr ? err : "(no message)"));
    bs_free_error_msg(err);
  } else {
    const double q = 0.0;
    double lp = 0.0;
    const int rc = bs_log_density(m, true, true, &q, &lp, &err);
    expect_refused("categorical-check density", rc, &err,
                   "categorical outcome out of support");
    bs_model_destruct(m);
  }

  // The version globals name the ABI this file implements.
  expect_eq_int("bs_major_version", bs_major_version, 2);
  expect_eq_int("bs_minor_version", bs_minor_version, 9);
  expect_eq_int("bs_patch_version", bs_patch_version, 0);
}

}  // namespace

int main() {
  test_density_matches_executor();
  test_param_num_and_names();
  test_unc_names();
  test_constrain_graph();
  test_constrain_interp();
  test_constrain_compiled_rng();
  test_transformed_data_rng();
  test_constrain_operators();
  test_unsupported();
  test_initialize();
  test_print_callback();
  test_necessity_effects();
  test_parameterless();
  test_manifest();
  test_embedded_manifest();
  test_whitened_manifest();
  test_construct_errors();
  if (failures == 0) std::printf("test_bridgestan OK\n");
  return failures == 0 ? 0 : 1;
}
