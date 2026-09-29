parameters { vector[2] shape; }
model { shape ~ std_normal(); }
generated quantities { array[2] real draws = gamma_rng(exp(shape), 1.5); }
