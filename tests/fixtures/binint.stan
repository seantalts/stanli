// Two-argument scalar math with one int argument: the Bessel family,
// binary_log_loss, lmgamma, the two factorials, and ldexp. Stan vectorizes
// all of them over every container shape, and which position holds the int
// is per-function, so both orders appear here.
//
// The case that is not a plain flat pairing is a matrix against an int
// array: a matrix is stored column-major and an array's trailing extents
// are row-major, while stan-math pairs n[i][j] with m(i, j). The exponents
// below are deliberately distinct so a swapped pairing is a factor of two,
// not a rounding difference.
//
// The transformed data block is the half with no assignment width check to
// catch a wrong answer -- the MIR interpreter evaluates it, and a missing
// case there is silently wrong rather than an error.
data {
  int k;
  array[3] int counts;
  // Data, not a transformed-data literal: a rank-three array declared in
  // transformed data does not reach its slot in the row-major order the
  // rest of the runtime addresses arrays by, which is a layout bug of its
  // own and would put this fixture's answer on top of it.
  array[2, 2, 2] int expo3;
  // Same shape as `expo` below but read rather than built, because the
  // interpreter reaches a literal and a data variable by different paths
  // and only one of them can be checked by `expo`.
  array[2, 2] int nn;
}
transformed data {
  array[2, 2] int expo = {{0, 3}, {1, 2}};
  array[3] real td = ldexp({1.5, 2.5, 3.5}, counts);
  real td2 = lmgamma(2, 1.75);
  // The layout case again, on the interpreter's own storage.
  real td3 = sum(ldexp([[1.0, 2.0], [3.0, 4.0]], expo));
  // And once more against an int array that came from the data reader
  // rather than from a literal. Different exponents from td3, so the two
  // cannot be confused for each other.
  real td4 = sum(ldexp([[1.0, 2.0], [3.0, 4.0]], nn));
}
parameters {
  vector[3] a;
  real b;
}
model {
  // Comfortably positive, for the functions that need it.
  vector[3] p = exp(a) + 1;
  // int scalar against a container: the shape every signature of the
  // first six takes.
  target += sum(bessel_first_kind(k, a));
  target += sum(modified_bessel_first_kind(k, a));
  target += sum(bessel_second_kind(k, p));
  target += sum(modified_bessel_second_kind(k, p));
  target += sum(lmgamma(k, p));
  target += binary_log_loss(1, inv_logit(b));
  // Container against an int scalar, and against an int container.
  target += sum(falling_factorial(p, k));
  target += sum(rising_factorial(p, counts));
  target += sum(ldexp(a, counts));
  // Matrix against an int array: the layout case.
  matrix[2, 2] m = [[a[1], a[2]], [a[3], b]];
  target += sum(ldexp(m, expo));
  // An ARRAY of matrices against a deeper int array: the extents the
  // kernel needs are the leaf's, the last two of the logical shape, not
  // the array's outer one. Nothing else reaches that branch -- the
  // conformance sweep builds every matrix with one extent held at 1,
  // which makes the correction the identity.
  array[2] matrix[2, 2] am = ldexp({m, m + 1}, expo3);
  target += sum(am[1]) + sum(am[2]);
  // A real scalar against an int array widens to an array.
  array[3] real sa = ldexp(b, counts);
  target += sum(sa);
  target += td[1] + td[2] + td[3] + td2 + td3 + td4;
}
