// Literal oracles from the generated Stan model and Reference BridgeStan.
//
// Raw q is declaration-major, then outer-batch-major, then leaf-major. Stan's
// public names and constrained rows use a different order: the first logical
// index varies fastest. Keeping both here makes a test fail if production ever
// conflates sampler storage with serialization again.
#ifndef STANLI_TEST_STRUCTURED_ARRAY_ORACLES_HPP
#define STANLI_TEST_STRUCTURED_ARRAY_ORACLES_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace structured_array_oracle {

inline void check_batch(int B) {
  if (B < 0 || B > 2) throw std::invalid_argument("oracle batch is 0, 1, or 2");
}

inline int64_t n_unc(int B) {
  check_batch(B);
  return 1 + 19 * B;
}

inline int64_t n_con(int B) {
  check_batch(B);
  return 1 + 40 * B;
}

inline std::vector<double> q(int B) {
  check_batch(B);
  const int width[] = {2, 1, 3, 1, 3, 5, 2, 2};
  const int base[] = {-12, -8, -6, 0, 2, 8, 18, 22};
  std::vector<double> out;
  out.reserve((size_t)n_unc(B));
  for (int t = 0; t < 8; ++t)
    for (int b = 0; b < B; ++b)
      for (int j = 0; j < width[t]; ++j)
        out.push_back((base[t] + b * width[t] + j) / 32.0);
  out.push_back(26.0 / 32.0);  // anchor
  return out;
}

inline double lp(int B) {
  check_batch(B);
  const double want[] = {0.8125, 15.789351719352936, 59.262183036924633};
  return want[B];
}

inline std::vector<double> grad(int B) {
  check_batch(B);
  if (B == 0) return {1};
  if (B == 1) {
    return {
        // simp
        0.66356325635519187,
        0.55425374991445153,
        // corr
        5.1899115688393085,
        // cov
        3.4678279595721309,
        5.4907038272628022,
        3.55760156614281,
        // chol_corr
        8,
        // chol_sq
        2.0644944589178591,
        12,
        2.1331484530668261,
        // chol_rect
        2.2840254166877414,
        1,
        2.3668379411737961,
        1,
        14,
        // stz_vec
        -12.020815280171307,
        6.9402209378856723,
        // stz_mat
        9.4999999999999982,
        -5.4848275573014451,
        // anchor
        1,
    };
  }
  return {
      // simp
      0.66356325635519187,
      0.55425374991445153,
      1.0128732363725292,
      0.73668676260971322,
      // corr
      5.1899115688393085,
      9.9669983537929241,
      // cov
      3.4678279595721309,
      5.4907038272628022,
      3.55760156614281,
      5.5194199065140719,
      12.497145059320479,
      5.7576522512539032,
      // chol_corr
      8,
      15.859456336354686,
      // chol_sq
      2.0644944589178591,
      12,
      2.1331484530668261,
      3.3382368923390087,
      24,
      3.4890402155321905,
      // chol_rect
      2.2840254166877414,
      1,
      2.3668379411737961,
      1,
      14,
      4.0023556000002456,
      2,
      4.1959908999012665,
      2,
      28,
      // stz_vec
      -12.020815280171307,
      6.9402209378856723,
      -24.041630560342615,
      13.880441875771345,
      // stz_mat
      9.4999999999999982,
      -5.4848275573014451,
      18.999999999999996,
      -10.96965511460289,
      // anchor
      1,
  };
}

inline std::vector<double> constrained(int B) {
  check_batch(B);
  if (B == 0) return {0.8125};
  if (B == 1) {
    return {
        // simp
        0.21342031450900809,
        0.36270637213303819,
        0.42387331335795364,
        // corr
        1,
        -0.24491866240370913,
        -0.24491866240370913,
        1.0000000000000002,
        // cov
        0.68728927879097224,
        -0.12953579971568757,
        -0.12953579971568757,
        0.80321484557140499,
        // chol_corr
        1,
        0,
        0,
        1,
        // chol_sq
        1.0644944589178593,
        0.09375,
        0,
        1.1331484530668263,
        // chol_rect
        1.2840254166877414,
        0.28125,
        0.34375,
        0,
        1.3668379411737963,
        0.375,
        // stz_vec
        0.64014498688035171,
        -0.15535014195451427,
        -0.48479484492583741,
        // stz_mat
        0.13626474700997815,
        -0.13626474700997815,
        0.41497050598004359,
        -0.41497050598004359,
        -0.55123525299002174,
        0.55123525299002174,
        // zero-free stz row and column leaves
        0,
        0,
        0,
        0,
        0,
        0,
        // anchor
        0.8125,
    };
  }
  return {
      // simp
      0.21342031450900809,
      0.2316997594137947,
      0.36270637213303819,
      0.3604611310396531,
      0.42387331335795364,
      0.40783910954655223,
      // corr
      1,
      1,
      -0.24491866240370913,
      -0.21532633966578324,
      -0.24491866240370913,
      -0.21532633966578324,
      1.0000000000000002,
      1,
      // cov
      0.68728927879097224,
      0.82902911818040037,
      -0.12953579971568757,
      -0.056906897586252135,
      -0.12953579971568757,
      -0.056906897586252135,
      0.80321484557140499,
      0.94331931281347592,
      // chol_corr
      1,
      1,
      0,
      0.031239831446031256,
      0,
      0,
      1,
      0.99951191735327671,
      // chol_sq
      1.0644944589178593,
      1.1691184461695043,
      0.09375,
      0.1875,
      0,
      0,
      1.1331484530668263,
      1.2445201077660952,
      // chol_rect
      1.2840254166877414,
      1.5011778000001228,
      0.28125,
      0.4375,
      0.34375,
      0.5,
      0,
      0,
      1.3668379411737963,
      1.5979954499506333,
      0.375,
      0.53125,
      // stz_vec
      0.64014498688035171,
      0.70985467885850229,
      -0.15535014195451427,
      -0.17402879762468204,
      -0.48479484492583741,
      -0.53582588123382024,
      // stz_mat
      0.13626474700997815,
      0.14947255109780239,
      -0.13626474700997815,
      -0.14947255109780239,
      0.41497050598004359,
      0.45105489780439512,
      -0.41497050598004359,
      -0.45105489780439512,
      -0.55123525299002174,
      -0.60052744890219745,
      0.55123525299002174,
      0.60052744890219745,
      // zero-free stz row leaves, then column leaves
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      // anchor
      0.8125,
  };
}

inline void append_vector_names(std::vector<std::string>& out,
                                const std::string& name, int B, int K) {
  for (int k = 1; k <= K; ++k)
    for (int b = 1; b <= B; ++b)
      out.push_back(name + "." + std::to_string(b) + "." + std::to_string(k));
}

inline void append_matrix_names(std::vector<std::string>& out,
                                const std::string& name, int B, int R, int C) {
  for (int c = 1; c <= C; ++c)
    for (int r = 1; r <= R; ++r)
      for (int b = 1; b <= B; ++b)
        out.push_back(name + "." + std::to_string(b) + "." + std::to_string(r) +
                      "." + std::to_string(c));
}

inline std::vector<std::string> unconstrained_names(int B) {
  check_batch(B);
  std::vector<std::string> out;
  append_vector_names(out, "simp", B, 2);
  append_vector_names(out, "corr", B, 1);
  append_vector_names(out, "cov", B, 3);
  append_vector_names(out, "chol_corr", B, 1);
  append_vector_names(out, "chol_sq", B, 3);
  append_vector_names(out, "chol_rect", B, 5);
  append_vector_names(out, "stz_vec", B, 2);
  append_matrix_names(out, "stz_mat", B, 1, 2);
  out.push_back("anchor");
  return out;
}

inline std::vector<std::string> constrained_names(int B) {
  check_batch(B);
  std::vector<std::string> out;
  append_vector_names(out, "simp", B, 3);
  append_matrix_names(out, "corr", B, 2, 2);
  append_matrix_names(out, "cov", B, 2, 2);
  append_matrix_names(out, "chol_corr", B, 2, 2);
  append_matrix_names(out, "chol_sq", B, 2, 2);
  append_matrix_names(out, "chol_rect", B, 3, 2);
  append_vector_names(out, "stz_vec", B, 3);
  append_matrix_names(out, "stz_mat", B, 2, 3);
  append_matrix_names(out, "stz_row_zero", B, 1, 3);
  append_matrix_names(out, "stz_col_zero", B, 3, 1);
  out.push_back("anchor");
  return out;
}

inline std::vector<std::string> write_names(int B) {
  std::vector<std::string> out = constrained_names(B);
  out.push_back("proof");
  return out;
}

inline std::vector<double> write_values(int B) {
  std::vector<double> out = constrained(B);
  out.push_back(0.8125);
  return out;
}

}  // namespace structured_array_oracle

#endif
