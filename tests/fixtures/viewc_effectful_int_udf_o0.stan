// viewc_effectful_int_udf as unoptimized MIR, where noisy stays a call.
functions {
  array[] int noisy(data array[] int x) {
    print("int effect");
    return x;
  }
}
data { array[1] int x_i; }
parameters { real q; }
model { target += bernoulli_lpmf(noisy(x_i)[1] | inv_logit(q)); }
