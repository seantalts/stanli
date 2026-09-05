// A GLM with a LANGUAGE-LEVEL SCALAR outcome. Legal Stan -- stan-math
// broadcasts the outcome over X's rows -- but the kernels map one integer
// per row out of idata, so the shape used to read past the end of a
// one-element group and return a silently wrong lp. This one cannot simply
// be replicated either: stan-math's <false> poisson_log_glm subtracts
// lgamma(y+1) once for a scalar and once per row for an array, so the
// gradients would be right and the lp a constant off CmdStan's. The
// the shared GLM payload retains the scalar-vs-array distinction.
data {
  int<lower=1> N;
  int<lower=1> K;
  matrix[N, K] x;
  int<lower=0> y;
}
parameters {
  real alpha;
  vector[K] beta;
}
model {
  target += poisson_log_glm_lpmf(y | x, alpha, beta);
}
