// STANLI-LIT: PASS
// STANLI-LIT-EXPECT: COMPILE_FAIL to_int: x is nan, but must be in the interval [-2147483648, 2147483647]
transformed data { real x = not_a_number(); int k = to_int(x); }
parameters { real q; } model { q ~ std_normal(); }
