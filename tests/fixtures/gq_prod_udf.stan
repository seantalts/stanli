functions {
  real vector_product(vector x) {
    // Keep this call visible in optimized MIR: a UDF formal can bind a
    // shifted Eigen Block even when the source-level caller passes a vector.
    array[2] matrix[2, 2] uninlined_shape_guard;
    return prod(x);
  }
}
parameters {
  vector[5] x;
  matrix[2, 5] p;
}
model {
  x ~ normal(0, 1);
}
generated quantities {
  real pr = vector_product(x);
  real transposed_surface = prod(1.0 - x');
  real target_surface = prod(rep_vector(1.0, 5) - p[2]');
}
