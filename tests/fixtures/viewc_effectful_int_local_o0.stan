// An integer local takes its value from a call that prints. In unoptimized
// MIR the call stays in the local's initializer.
functions {
  array[] int noisy(data array[] int x) {
    print("int effect");
    return x;
  }
}
data { array[1] int x_i; }
parameters { real q; }
model {
  int k = noisy(x_i)[1];
  target += bernoulli_lpmf(k | inv_logit(q));
}
