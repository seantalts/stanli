data { int divisor; }
parameters { real x; }
model { x ~ std_normal(); }
generated quantities {
  int draw = binomial_rng(9, inv_logit(x));
  int positive = divide(draw, divisor);
  int negative = divide(-draw, divisor);
  int operator_result = draw / divisor;
  int remainder = draw % divisor;
  int negative_remainder = -draw % divisor;
  real promoted = divide(draw + 1, divisor);
  real promoted_operator = (draw + 1) / divisor;
  real promoted_remainder = -draw % divisor;
  int choice = bernoulli_rng(inv_logit(x));
  int direct_sum = sum({choice, choice, 1});
  int direct_min = min({choice, 2});
  int direct_max = max({choice, 2});
  real after = normal_rng(0, 1);
}
