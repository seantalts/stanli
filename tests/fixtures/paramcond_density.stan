// Densities inside parameter-dependent branches, chosen from OUTSIDE the
// handful the register machine used to speak.
//
// The register machine carried its own density list, a subset of the
// runtime's. A density the runtime supports everywhere else -- as a graph
// op, in a vectorized statement, anywhere -- became a hard compile error
// purely for sitting inside an `if` on a parameter, because that region
// has to compile to the register machine or not at all. `student_t_lpdf`
// is the sharpest case: it takes four arguments, and the instruction had
// three operand fields.
//
// So: `chi_square` (two arguments), `gumbel` (three), `student_t` (four),
// and `rayleigh` (two), none of them in the old list, across both arms.
data {
  real<lower=0> y;
}
parameters {
  real<lower=0> nu;
  real mu;
}
model {
  if (mu > 0) {
    target += chi_square_lpdf(y | nu);
    target += student_t_lpdf(y | nu, mu, 1.0);
  } else {
    target += gumbel_lpdf(y | mu, 1.0);
    target += rayleigh_lpdf(y | nu);
  }
}
