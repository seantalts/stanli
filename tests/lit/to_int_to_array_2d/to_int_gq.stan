// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: OK
parameters { real q; } model { q ~ std_normal(); }
generated quantities { array[2,2] int a = to_int({{1.0,2.0},{3.0,4.0}}); int b = a[2,1]; }
