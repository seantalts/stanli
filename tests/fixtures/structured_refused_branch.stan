// A structured loop candidate that lowers a branch inside an inlined
// function and is refused there. Lowering catches the refusal and goes on.
functions {
  array[] real f2(int a3, matrix a4, array[] real a5) {
    if (((sum(a5) + sum(a5)) > 1.00)) {
      return a5;
    }
    return tanh(rep_array(-0.32, 0));
  }
  real f10_lpmf(int y11, array[] real a12, array[] int a13, real a14) {
    if ((a14 >= 0.07)) {
      vector[size(a12)] l15 = rep_vector(0.0, 0);
      vector[size(f2(y11, rep_matrix(1.46, 0, 0), a12))] l16 = l15;
    }
    return poisson_log_lpmf(y11 | tanh(1.49));
  }
}
data {
  int<lower=0> N0;
}
parameters {
  array[N0] real p20;
  real p21;
}
transformed parameters {
  if ((p21 > 0.41)) {
    reject("r25", p21);
  }
  {
    int w27 = 0;
    while (w27 < N0) {
      if ((tanh(p21) > (p21 * 0.79))) {
        real gq28 = p21;
        if ((f10_lpmf(0 | p20, rep_array(0, 0), p21) < gq28)) {
        }
      }
    }
  }
}
