// Promoted from a COMPILE_FAIL gap: mixed range/gather selections over
// arrays now resolve through the shared index geometry. The reads
// self-check so a wrong cell reports as EVAL_FAIL.
parameters {
  array[2, 3] real a;
}
model {
  array[2, 2] real picked = a[1 : 2, {1, 3}];
  if (abs(picked[1, 2] - a[1, 3]) > 0 || abs(picked[2, 1] - a[2, 1]) > 0)
    reject("mixed selection picked the wrong cells");
  target += picked[1, 1];
}

// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
