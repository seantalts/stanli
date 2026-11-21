// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL lub_constrain: lb is 1, but must be less than 0
// STANLI-LIT-DATA: {"N": 0, "lo": 1, "hi": 0}
// Stan checks scalar bounds even when the container has no elements.
data { int N; real lo; real hi; }
parameters { vector<lower=lo, upper=hi>[N] v; real t; }
model { t ~ normal(0, 1); }
