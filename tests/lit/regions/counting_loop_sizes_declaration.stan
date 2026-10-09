// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"N": 60, "y": [0.0, 1.0, 0.334, -0.052, -0.194, 0.343, 1.169, 0.981, 0.924, 0.055, 0.559, 0.143, -0.024, -0.13, 0.043, 1.184, 1.026, 0.991, 0.981, 0.009, 0.196, 0.703, 0.871, 1.067, 1.108, -0.161, 0.669, 0.775, 0.51, -0.016, 0.458, -0.157, 1.195, 1.085, 0.576, 0.18, 1.154, 0.616, 1.112, 1.057, 0.513, 0.362, 0.658, 0.39, -0.042, 0.188, 1.0, -0.231, -0.226, 0.702, 0.149, 0.555, 0.454, 0.249, 1.296, 0.013, 0.36, 0.024, 0.712, 0.142]}
// A loop that only counts observations into integer locals, long enough
// (32 iterations or more) and with a short-circuit condition, so that it
// was kept as a run-time region. The arrays declared after it are sized by
// the counters, which then had no value at compile time: "size expression
// needs unknown int". brms writes this in its xbeta family. The counts are
// checked against sums taken another way, and a mismatch rejects.
functions {
  real split_lpdf(vector y, vector mu) {
    int N = size(y);
    int N_zer = 0;
    int N_one = 0;
    int N_oth = 0;
    int i_oth = 0;
    for (i in 1:N) {
      N_zer += y[i] <= 0;
      N_one += y[i] >= 1;
      N_oth += (y[i] > 0) && (y[i] < 1);
    }
    array[N_oth] int oth;
    for (i in 1:N) {
      if ((y[i] > 0) && (y[i] < 1)) {
        i_oth += 1;
        oth[i_oth] = i;
      }
    }
    if (N_zer + N_one + N_oth != N) reject("counts do not add up");
    return normal_lpdf(y[oth] | mu[oth], 1) - N_zer - 2 * N_one;
  }
}
data {
  int<lower=32> N;
  vector[N] y;
}
transformed data {
  int inside = 0;
  for (n in 1:N) inside += y[n] > 0 ? (y[n] < 1 ? 1 : 0) : 0;
  if (inside != 36) reject("the data are not the ones this test was written for");
}
parameters {
  real m;
}
model {
  y ~ split(rep_vector(m, N));
}
