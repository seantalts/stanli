// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// Functions that return a container from more than one place. The overloads
// keep stanc from inlining them, so stanli rewrites the returns itself. Each
// call runs with a positive and a negative argument, so both arms run. The
// condition in sized reads data, so its two sizes need no single shape.
functions {
  void expect(real got, real want, int id) {
    if (got != want) reject("case ", id, ": got ", got, ", want ", want);
  }
  vector guard(vector x) {
    if (x[1] > 0) return x;
    return -x;
  }
  real guard(real x) {
    return x;
  }
  matrix keep_or_double(matrix m, real c) {
    matrix[rows(m), cols(m)] doubled = 2 * m;
    int changed = 0;
    if (c > 0) changed = 1;
    if (changed) {
      return doubled;
    } else {
      return m;
    }
  }
  real keep_or_double(real x) {
    return x;
  }
  array[] int signs(real c) {
    if (c < 0) return {-1, 0};
    return {1, 2};
  }
  real signs(vector x) {
    return 0;
  }
  row_vector first_below(row_vector v, real bound) {
    for (i in 1:cols(v)) {
      for (j in 1:2) {
        if (v[i] * j < bound) return v * j;
      }
    }
    return v;
  }
  real first_below(real x) {
    return x;
  }
  vector sized(int n) {
    if (n > 0) return rep_vector(1, n);
    return rep_vector(2, 3);
  }
  real sized(real x) {
    return x;
  }
  void check_values(real p) {
    real q = square(p) + 0.5;
    expect(sum(guard([q, 2]')), q + 2, 1);
    expect(sum(guard([-q, 2]')), q - 2, 2);
    matrix[0, 0] empty;
    expect(num_elements(keep_or_double(empty, q)), 0, 3);
    expect(num_elements(keep_or_double(empty, -q)), 0, 4);
    expect(sum(keep_or_double(rep_matrix(q, 1, 2), q)), 4 * q, 5);
    expect(sum(keep_or_double(rep_matrix(q, 1, 2), -q)), 2 * q, 6);
    expect(sum(signs(q)), 3, 7);
    expect(sum(signs(-q)), -1, 8);
    expect(sum(sized(2)), 2, 15);
    expect(sum(sized(0)), 6, 16);
    for (k in 1:3) {
      expect(sum(first_below([1, -q * k], -1.5 * q * k)), 2 * (1 - q * k),
             8 + k);
      expect(sum(first_below([1, q * k], -1.5 * q * k)), 1 + q * k, 11 + k);
    }
  }
}
parameters {
  real p;
}
model {
  p ~ normal(0, 1);
  check_values(p);
}
generated quantities {
  check_values(p);
}
