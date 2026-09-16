// Full-width outputs, live-in reassignment, and a multi-output near miss.
data { int<lower=0> N; }
parameters {
  real selector;
  vector[N] x;
}
model {
  vector[N] v = 2 * x;
  real a = selector;
  real b = selector + 1;
  real c = selector + 2;
  if (selector > 0) v *= selector;
  else v += rep_vector(selector, N);
  target += sum(v);
  if (selector > 0) a = square(a);
  else a = -a;
  target += a;
  if (selector < 0) {
    b *= 2;
    c *= 3;
  } else {
    b += selector;
    c -= selector;
  }
  target += b + 4 * c;
}
