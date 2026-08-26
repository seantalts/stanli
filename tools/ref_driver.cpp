// Reference driver: compiled per model against CmdStan's generated .hpp
// (passed via -include). Evaluates log_prob_propto_jacobian (the sampling
// semantics; stanli lowers ~ statements propto with matched activity) and
// its gradient at the deterministic stanli_check point.
// Output: OK <lp> <g0> <g1> ...
//         WANAMES <n0,n1,...>          write_array columns (tparams + gqs)
//         WAVALS <v0> <v1> ...         their values at the same point
// The write_array lines carry FAIL <what> instead when the model throws
// there; the recorder then skips the model's wa reference.
#include <stan/io/json/json_data.hpp>
#include <stan/model/model_base.hpp>
#include <stan/services/util/create_rng.hpp>
#include <stan/math.hpp>

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
    default:
      return 0.1 + 0.05 * static_cast<double>(i % 7) -
             0.15 * static_cast<double>(i % 3);
  }
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: ref_driver data.json [point]\n");
    return 2;
  }
  const int variant = argc > 2 ? std::atoi(argv[2]) : 0;
  std::ifstream f(argv[1]);
  stan::json::json_data data(f);
  stan::model::model_base& model = new_model(data, 1, &std::cerr);

  const int64_t n = static_cast<int64_t>(model.num_params_r());
  Eigen::Matrix<stan::math::var, -1, 1> q(n);
  for (int64_t i = 0; i < n; ++i) q(i) = eval_point(i, variant);

  stan::math::var lp = model.log_prob_propto_jacobian(q, &std::cerr);
  lp.grad();
  std::printf("OK %.17g", lp.val());
  for (int64_t i = 0; i < n; ++i) std::printf(" %.17g", q(i).adj());
  std::printf("\n");

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
    std::printf("WANAMES FAIL %s\n", e.what());
    std::printf("WAVALS FAIL\n");
  }
  return 0;
}
