parameters {
  vector[4] theta;
}
transformed parameters {
  vector[4] assigned = theta;
  vector[2] gathered;
  matrix[2, 2] laid_out = to_matrix(theta, 2, 2);
  {
    array[2] int gather_index = {4, 2};
    gathered = theta[gather_index];
  }
  assigned[2 : 3] = theta[1 : 2];
  laid_out[2, 1] = gathered[1];
}
model {
  target += assigned[1] + 2 * assigned[2] + 3 * assigned[3]
            + 4 * assigned[4];
  target += gathered[1] + 0.5 * gathered[2] + sum(laid_out);
}
