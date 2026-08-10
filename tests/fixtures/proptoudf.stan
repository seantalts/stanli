// propto through a user-defined density. Stan's rule is CmdStan's template
// parameter: a body written with `_lupdf` drops the normalizing constant
// only when the CALLER asked for the unnormalized form, and stanc3 says so
// in the MIR -- the call site carries (FnLpdf false) while the body's own
// call carries (FnLpdf true). Reading the body's flag alone makes every
// user density unnormalized, which is invisible in the gradient and wrong
// in lp__, in a transformed parameter, and anywhere the constant does not
// cancel (log_mix over two different densities, say).
//
// Four terms, four rules, and the whole lp is checkable in closed form:
//   normalized, normalized through a second UDF, unnormalized, and `~`
//   (which is unnormalized by definition).
functions {
  real f_lpdf(real x, real mu) {
    return normal_lupdf(x | mu, 1);
  }
  real g_lpdf(real x) {
    return f_lupdf(x | 0.5);
  }
}
parameters {
  real mu;
}
model {
  target += f_lpdf(mu | 1.0);
  target += g_lpdf(mu | );
  target += f_lupdf(mu | 2.0);
  mu ~ f(3.0);
}
