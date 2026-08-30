// A fixed scan replaces state after peeling the first iteration.  The
// condition following the loop must read that final carry, not the data
// interpreter's observation of the peeled prefix.
data {
  int<lower=1> N;
}
parameters {
  real theta;
}
model {
  real state = 0;
  for (i in 1 : N) {
    state += 1;
    target += theta;
  }
  if (state > 5)
    target += theta;
  else
    target += 2 * theta;
}
