functions {
  array[] real rhs_flat(real t, array[] real y, array[] real theta,
                        array[] real x_r, array[] int x_i) {
    return {theta[1] * y[1] + x_r[1] + x_i[1]};
  }
  array[] real rhs_depth2(real t, array[] real y, array[,] real theta,
                          array[] real x_r, array[] int x_i) {
    return {theta[1, 1] * y[1]};
  }
  array[] real rhs_depth2_int(real t, array[] real y, array[] real theta,
                              array[] real x_r, array[,] int x_i) {
    return {theta[1] * y[1] + x_i[1, 1]};
  }
  array[] real rhs_vectors(real t, array[] real y, array[] vector theta,
                           array[] real x_r, array[] int x_i) {
    return {theta[1][1] * y[1]};
  }
  array[] real rhs_matrices(real t, array[] real y, array[] matrix theta,
                            array[] real x_r, array[] int x_i) {
    return {theta[1][1, 1] * y[1]};
  }
}
parameters {
  real q;
}
model {
  target += q;
}
