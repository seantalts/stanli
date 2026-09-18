// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
// STANLI-LIT-DATA: {"a":[7,-7,7,-7,0],"b":[3,3,-3,-3,2],"expected":[1,-1,1,-1,0]}
functions {
  real remainder_check(real x, int a, int b, int expected) {
    int n = a;
    if (x > 1000) return x;
    if (x <= 0) n = -a;
    return x + square(n % b - (x > 0 ? expected : -expected));
  }
}
data {
  array[5] int a;
  array[5] int b;
  array[5] int expected;
}
parameters { real x; }
model {
  x ~ std_normal();
  for (i in 1:5) {
    if (remainder_check(x, a[i], b[i], expected[i]) != x)
      reject("incorrect integer remainder");
  }
}
