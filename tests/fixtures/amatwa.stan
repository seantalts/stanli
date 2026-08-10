// An array of matrices, written out. Two layouts meet here and only one is
// right: the array dimension is outermost and each element is contiguous,
// while WITHIN an element the storage is column-major, the same as a plain
// matrix. Indexing m[k, i, j] as if the whole thing were row-major gives a
// transposed matrix with the correct column names on it, which is what
// write_array did until a CmdStan diff of tests/stanc3/vector-size-stmts
// caught it.
//
// Three sources, because they take three different paths to a column: a
// parameter read out of the unconstrained draw, a generated quantity built
// element by element, and data copied straight through (the interpreter
// hands that one over in ITS layout, and only the repack in env_slot puts
// it right -- which the write_array lowering used to skip entirely).
data {
  array[2] matrix[2, 3] d;
}
parameters {
  array[2] matrix[2, 3] m;
}
model {
  target += sum(m[1]) + sum(m[2]);
}
generated quantities {
  array[2] matrix[2, 3] g;
  array[2] matrix[2, 3] gd = d;
  for (k in 1 : 2) {
    for (i in 1 : 2) {
      for (j in 1 : 3) {
        g[k][i, j] = 100 * k + 10 * i + j;
      }
    }
  }
}
