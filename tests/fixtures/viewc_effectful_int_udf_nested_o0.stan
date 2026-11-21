// viewc_effectful_int_udf_nested as unoptimized MIR, where pick stays a
// call too.
functions {
  array[] int noisy(data array[] int x) {
    print("int effect");
    return x;
  }
  int pick(data array[] int x) {
    return noisy(x)[1];
  }
}
data {
  array[1] int x_i;
}
parameters {
  real q;
}
model {
  target += bernoulli_lpmf(pick(x_i) | inv_logit(q));
}
