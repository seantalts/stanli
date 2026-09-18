// A parameter's size computed by a transformed-data for-loop accumulator
// (`sumnt2 += ...`), not a bare data value. The interpreter that runs
// transformed data used to lose the running total's int-ness on the first
// loop iteration (a whole-variable reassignment came back through real
// arithmetic), so the parameter's declared size looked like an unknown
// runtime value instead of the computed constant it is.
data {
  int<lower=1> nots;
  array[nots] int<lower=1> nts;
}
transformed data {
  int sumnt2 = 0;
  for (i in 1 : nots)
    sumnt2 += nts[i] * nts[i];
  // Keep trailing geometry that cannot be recovered from a flattened empty
  // value. The write-array lowering must receive the declaration facts from
  // the same prepared handoff as sumnt2.
  array[0, 2] matrix[3, 4] empty_tensor =
      rep_array(rep_matrix(0, 3, 4), 0, 2);
  array[1, 2] matrix[3, 4] shaped_tensor =
      rep_array(rep_matrix(0, 3, 4), 1, 2);
  for (j in 1 : 2)
    shaped_tensor[1, j] = rep_matrix(sumnt2 + j, 3, 4);
}
parameters {
  vector[sumnt2] x;
}
model {
  x ~ normal(0, 1);
}
generated quantities {
  int prepared_size = sumnt2;
  array[0, 2] matrix[3, 4] empty_copy = empty_tensor;
  matrix[3, 4] prepared_leaf = shaped_tensor[1, 2];
}
