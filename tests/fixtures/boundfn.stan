// Stan's bound transforms, callable as ordinary functions rather than
// written on a declaration. `_constrain` is the declaration transform's
// value with no jacobian, `_jacobian` is the same value and also adds the
// log absolute jacobian determinant to the target, and `_unconstrain` is
// the inverse. All three vectorize over every container shape, with each
// bound either shared by the whole container or one value per element.
//
// The calls sit in a `_jacobian` user function because that is the shape
// the conformance sweep generates, and because --O1 inlines it into
// log_prob and into write_array alike -- and write_array instantiates
// `jacobian__ = false`, so the increments have to disappear there.
//
// Each result is summed into its own target term rather than combined, so
// the reference in tests/test_lower.cpp can reassociate the same way.
functions {
  real bounds_jacobian(vector a, vector lo, vector hi, real s, matrix m,
                       array[] vector na) {
    real c = 0;
    // Lower: a shared bound, then one bound value per element. The
    // per-element form is what the kernels used to read element 0 of.
    c += sum(lower_bound_constrain(a, s));
    c += sum(lower_bound_jacobian(a, lo));
    c += sum(lower_bound_unconstrain(lower_bound_constrain(a, s), s));
    // Upper, on a matrix leaf: the declaration only accepts a 2x2 view, so
    // this is where a result that lost its extents would die.
    matrix[2, 2] uc = upper_bound_constrain(m, s);
    c += sum(uc);
    c += sum(upper_bound_jacobian(a, hi));
    c += sum(upper_bound_unconstrain(upper_bound_constrain(a, s), s));
    // Lower-upper with both bounds per element, and with both shared --
    // the shared pair is the arithmetic the declaration path was
    // calibrated with, so it has to stay bitwise.
    c += sum(lower_upper_bound_constrain(a, lo, hi));
    c += sum(lower_upper_bound_jacobian(a, -3.0, 3.0));
    c += sum(lower_upper_bound_unconstrain(
        lower_upper_bound_constrain(a, lo, hi), lo, hi));
    // offset/multiplier over a nested array leaf, the shape the sweep
    // enumerates up to eight deep.
    array[2] vector[2] oc = offset_multiplier_constrain(na, s, 2.5);
    c += sum(oc[1]);
    c += sum(oc[2]);
    c += sum(offset_multiplier_jacobian(a, lo, hi));
    c += sum(offset_multiplier_unconstrain(a, lo, hi));
    return c;
  }
}
data {
  matrix[2, 2] md;
}
parameters {
  vector[2] a;
  real s;
}
transformed parameters {
  // lo < hi and hi > 0, so every argument stays inside the transform's
  // support and hi also serves as a positive multiplier.
  real total = bounds_jacobian(a, a - 4, exp(a) + 1, s, md, {a, a});
}
model {
  target += total;
}
