// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// a is declared with no elements, so the assignment in the branch gives it
// two. The test in the branch has to read size 2. The region compiler used
// to ask the lowering outside the branch, which still saw size 0.
parameters {
  real p;
}
model {
  array[0] vector[0] a;
  real r = 0;
  if (p < 100) {
    a = {[0.28, 0.53]', [0.78, 1.03]'};
    if (size(a) * 0.5 + 0.5 > 1) {
      r = 1;
    } else {
      r = 2;
    }
  }
  if (r != 1) reject("got ", r);
}
