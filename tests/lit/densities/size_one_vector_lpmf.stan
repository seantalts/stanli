// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: EVAL_FAIL poisson_lpmf: Random variable has size = 3, but Rate parameter has size 1; and they must be the same size
// STANLI-LIT-DATA: {"y": [1, 2, 3]}
// The same size rule against an integer outcome array.
data { array[3] int y; }
parameters { vector<lower=0>[1] lambda; }
model { y ~ poisson(lambda); }
