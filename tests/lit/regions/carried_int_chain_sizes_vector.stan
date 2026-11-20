// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// A chain of three carried integers in a counted loop, inside a loop that
// parameter control keeps. a takes the values 1, 1, 1, 2, 3 and sizes a
// vector. A range taken after two steps of the range analysis held a at 1,
// gave the vector a fixed length of 1, and s came out too small. The model
// rejects unless s matches the sum taken by hand.
parameters {
  real<lower=4.5, upper=5.5> theta;
}
model {
  real s = 0;
  for (g in 1:40) {
    int a = 1;
    int b = 1;
    int c = 1;
    for (j in 1:5) {
      if (j < theta) {
        vector[a] v = rep_vector(1.0, a);
        s += sum(v);
      }
      a = b;
      b = c;
      c += 1;
    }
  }
  array[5] int sizes = {1, 1, 1, 2, 3};
  real expected = 0;
  for (j in 1:5) if (j < theta) expected += 40 * sizes[j];
  if (abs(s - expected) > 1e-9) reject("s = ", s, ", expected ", expected);
  target += -s;
}
