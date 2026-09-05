// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"context_seed": 0.0}
// Shape queries over registered builtin calls inside a runtime-control
// region: the region compiler answers rows/cols/size/num_elements from the
// shared resolvers' result geometry without building the value. stanc's
// inliner produces exactly these forms when a container argument is a
// function call, e.g. `cols(a)` becoming `cols(to_matrix(v, 6, 1))`.
functions {
  real sq_probe(real seed) {
    real total = 0;
    vector[6] v = [seed, 1 + seed, 2, 3 - seed, 4, 5 + seed]';
    matrix[rows(to_matrix(v, 6, 1)), cols(to_matrix(v, 6, 1))] m
        = to_matrix(v, 6, 1);
    for (j in 1 : cols(to_matrix(v, 2, 3))) total += j * 0.5;
    for (i in 1 : rows(append_row(v, v))) total += i * 0.25;
    total += num_elements(rep_matrix(seed, 3, 4)) * 0.125;
    total += size(rep_array(seed, 5));
    total += rows(ones_vector(4)) * 2;
    total += cols(columns_dot_self(to_matrix(v, 2, 3))) * 3;
    total += rows(rows_dot_product(m, m)) * 5;
    total += cols(head(v, 3)') * 7;
    total += cols(crossprod(to_matrix(v, 2, 3))) * 11;
    total += rows(cholesky_decompose(add_diag(crossprod(to_matrix(v, 3, 2)),
                                              rep_vector(9.0, 2)))) * 13;
    total += num_elements(quad_form(diag_matrix(head(v, 2)),
                                    to_matrix(v, 2, 3))) * 0.5;
    total += sum(m) * 0.0625;
    return total;
  }
}
data {
  real context_seed;
}
transformed data {
  real transformed_data_result = sq_probe(context_seed);
}
parameters {
  real probe;
}
model {
  probe ~ std_normal();
  target += sq_probe(probe);
  if (probe > -1e100)
    target += sq_probe(probe);
}
generated quantities {
  real generated_quantities_result = sq_probe(probe);
}
