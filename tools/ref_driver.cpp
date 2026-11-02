// Reference driver: compiled per model against CmdStan's generated .hpp
// (passed via -include). Evaluates log_prob_propto_jacobian (the sampling
// semantics; stanli lowers ~ statements propto with matched activity) and
// its gradient at the deterministic stanli_check point.
// Output: OK <lp> <g0> <g1> ...
//         EVAL_FAIL <what>             log_prob or its gradient threw
//         DATA_FAIL <what>             the model constructor threw; exit 1
//         WANAMES <n0,n1,...>          write_array columns (tparams + gqs)
//         WAVALS <v0> <v1> ...         their values at the same point
// With `--points N,N,...` each listed point prints `POINT <k>` followed by
// the lines above, errors on one line with newlines replaced by " | ".
// The write_array lines carry FAIL <what> instead when the model throws
// there; the recorder then skips the model's wa reference.
#include <stan/io/json/json_data.hpp>
#include <stan/model/model_base.hpp>
#include <stan/services/util/create_rng.hpp>
#include <stan/math.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// Provided by the generated model translation unit.
stan::model::model_base& new_model(stan::io::var_context& data_context,
                                   unsigned int seed, std::ostream* msg_stream);

// Same points as tools/stanli_check.cpp; see the note there on why more
// than one exists.
static double eval_point(int64_t i, int variant) {
  switch (variant) {
    case 1:
      return 0.02 * static_cast<double>((i % 5) - 2);
    case 2:
      return 0.0;
    case 3: {
      uint64_t z = static_cast<uint64_t>(i) + 0x9E3779B97F4A7C15ull;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      z ^= z >> 31;
      const double u = static_cast<double>(z >> 11) * 0x1.0p-53;
      return -1.5 + 3.0 * u;
    }
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

static std::string one_line(std::string s) {
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
    if (s[i] == '\n' || s[i] == '\r')
      out += " | ";
    else
      out += s[i];
  }
  return out;
}

static bool parse_points(const std::string& list, std::vector<int>* out) {
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t comma = list.find(',', pos);
    if (comma == std::string::npos) comma = list.size();
    const std::string tok = list.substr(pos, comma - pos);
    char* end = nullptr;
    const long v = std::strtol(tok.c_str(), &end, 10);
    if (tok.empty() || *end != '\0' || v < 0) return false;
    out->push_back(static_cast<int>(v));
    pos = comma + 1;
  }
  return true;
}

static int eval_at(stan::model::model_base& model, int variant, bool multi) {
  const auto msg = [multi](const std::string& s) {
    return multi ? one_line(s) : s;
  };
  const int64_t n = static_cast<int64_t>(model.num_params_r());
  try {
    Eigen::Matrix<stan::math::var, -1, 1> q(n);
    for (int64_t i = 0; i < n; ++i) q(i) = eval_point(i, variant);

    stan::math::var lp = model.log_prob_propto_jacobian(q, &std::cerr);
    lp.grad();
    std::printf("OK %.17g", lp.val());
    for (int64_t i = 0; i < n; ++i) std::printf(" %.17g", q(i).adj());
    std::printf("\n");
  } catch (const std::exception& e) {
    stan::math::recover_memory();
    std::printf("EVAL_FAIL %s\n", msg(e.what()).c_str());
    return 1;
  }
  stan::math::recover_memory();

  // write_array at the same point: every CSV column (constrained params,
  // transformed parameters, generated quantities). The RNG stream matches
  // stanli_check --wa-values and BridgeStan's public RNG contract exactly:
  // Stan's current engine, seeded through create_rng(seed, chain=0).
  try {
    std::vector<std::string> names;
    model.constrained_param_names(names, true, true);
    Eigen::VectorXd qd(n);
    for (int64_t i = 0; i < n; ++i) qd(i) = eval_point(i, variant);
    stan::rng_t rng = stan::services::util::create_rng(1234, 0);
    Eigen::VectorXd vars;
    model.write_array(rng, qd, vars, true, true, &std::cerr);
    std::string joined;
    for (const auto& s : names) {
      if (!joined.empty()) joined += ',';
      joined += s;
    }
    std::printf("WANAMES %s\n", joined.c_str());
    std::printf("WAVALS");
    for (int64_t i = 0; i < vars.size(); ++i) std::printf(" %.17g", vars(i));
    std::printf("\n");
  } catch (const std::exception& e) {
    std::printf("WANAMES FAIL %s\n", msg(e.what()).c_str());
    std::printf("WAVALS FAIL\n");
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: ref_driver data.json [point | --points N,N,...]\n");
    return 2;
  }
  int variant = 0;
  std::vector<int> points;
  if (argc > 2 && std::string(argv[2]) == "--points") {
    if (argc < 4 || !parse_points(argv[3], &points)) {
      std::fprintf(stderr, "ref_driver: bad --points list\n");
      return 2;
    }
  } else if (argc > 2) {
    variant = std::atoi(argv[2]);
  }
  std::ifstream f(argv[1]);
  stan::model::model_base* model = nullptr;
  try {
    stan::json::json_data data(f);
    model = &new_model(data, 1, &std::cerr);
  } catch (const std::exception& e) {
    std::printf("DATA_FAIL %s\n", one_line(e.what()).c_str());
    return 1;
  }

  if (points.empty()) return eval_at(*model, variant, false);
  for (int k : points) {
    std::printf("POINT %d\n", k);
    eval_at(*model, k, true);
    std::fflush(stdout);
  }
  return 0;
}
