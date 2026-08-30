functions {
  array[] vector choose_vectors(real theta, array[] vector a,
                                array[] vector b) {
    if (theta > 0)
      return a;
    else
      return b;
  }
}
parameters {
  real theta;
  array[2] vector[2] a;
  array[2] vector[2] b;
}
model {
  array[2] vector[2] selected = choose_vectors(theta, a, b);
  target += sum(selected[1]);
}
