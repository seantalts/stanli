// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
transformed data { matrix[2,3] m = [[1,2,3],[4,5,6]]; }
parameters { real q; } model { q ~ std_normal(); }
generated quantities { array[2,3] real a = to_array_2d(m); real b = a[2,1]; }
