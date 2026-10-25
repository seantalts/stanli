transformed data {
  array[6] real dr = {2.5, -1.0, 2.5, 0.0, 7.0, -1.0};
  array[6] int di = {3, -1, 3, 0, 7, -1};
  vector[6] dv = to_vector(dr);
  row_vector[6] drv = dv';
  vector[6] w = [1, 2, 3, 4, 5, 6]';
  array[6] real wr = to_array_1d(w);
  array[6] real data_real_asc = sort_asc(dr);
  array[6] real data_real_desc = sort_desc(dr);
  array[6] int data_int_asc = sort_asc(di);
  array[6] int data_int_desc = sort_desc(di);
  vector[6] data_vec_asc = sort_asc(dv);
  vector[6] data_vec_desc = sort_desc(dv);
  row_vector[6] data_rv_asc = sort_asc(drv);
  row_vector[6] data_rv_desc = sort_desc(drv);
  array[6] int data_idx_asc = sort_indices_asc(dr);
  array[6] int data_idx_desc = sort_indices_desc(dr);
  array[6] int data_iidx_asc = sort_indices_asc(di);
  array[6] int data_iidx_desc = sort_indices_desc(di);
  array[6] int data_vidx_asc = sort_indices_asc(dv);
  array[6] int data_vidx_desc = sort_indices_desc(dv);
  array[6] int data_ridx_asc = sort_indices_asc(drv);
  array[6] int data_ridx_desc = sort_indices_desc(drv);
  array[6] int data_rank_real;
  array[6] int data_rank_int;
  array[6] int data_rank_vec;
  array[6] int data_rank_rv;
  for (k in 1:6) {
    data_rank_real[k] = rank(dr, k);
    data_rank_int[k] = rank(di, k);
    data_rank_vec[k] = rank(dv, k);
    data_rank_rv[k] = rank(drv, k);
  }
  real data_score = dot_product(data_real_asc, wr) + dot_product(data_vec_desc, w)
                    + data_rv_asc * w + sum(data_int_desc) + sum(data_idx_asc)
                    + sum(data_ridx_desc) + sum(data_rank_real) + sum(data_rank_int);
}
parameters {
  vector[6] p;
  real q;
}
transformed parameters {
  array[6] real pr = to_array_1d(p);
  row_vector[6] prv = p';
  array[6] real par_real_asc = sort_asc(pr);
  array[6] real par_real_desc = sort_desc(pr);
  vector[6] par_vec_asc = sort_asc(p);
  vector[6] par_vec_desc = sort_desc(p);
  row_vector[6] par_rv_asc = sort_asc(prv);
  row_vector[6] par_rv_desc = sort_desc(prv);
  vector[4] par_tied = sort_asc([p[1], p[2], p[1], p[3]]');
}
model {
  array[6] int par_idx_asc = sort_indices_asc(pr);
  array[6] int par_idx_desc = sort_indices_desc(pr);
  array[6] int par_vidx_asc = sort_indices_asc(p);
  array[6] int par_vidx_desc = sort_indices_desc(p);
  array[6] int par_ridx_asc = sort_indices_asc(prv);
  array[6] int par_ridx_desc = sort_indices_desc(prv);
  array[6] int par_rank_real;
  array[6] int par_rank_vec;
  array[6] int par_rank_rv;
  for (k in 1:6) {
    par_rank_real[k] = rank(pr, k);
    par_rank_vec[k] = rank(p, k);
    par_rank_rv[k] = rank(prv, k);
  }
  target += -0.5 * dot_self(p) - 0.5 * square(q);
  target += dot_product(par_real_asc, wr) + dot_product(par_vec_desc, w);
  target += par_rv_asc * w + par_rv_desc * (7 - w);
  target += dot_product(par_tied, [1, 2, 3, 4]');
  target += dot_product(w[par_vidx_desc], p) + q * sum(par_idx_asc);
  target += q * (par_rank_vec[2] + par_rank_real[3] + par_rank_rv[4] + par_ridx_asc[1]
                 + par_idx_desc[2] + par_vidx_asc[3] + par_ridx_desc[4] + data_score);
}
generated quantities {
  array[6] real gq_real_asc = sort_asc(pr);
  array[6] real gq_real_desc = sort_desc(pr);
  vector[6] gq_vec_asc = sort_asc(p);
  vector[6] gq_vec_desc = sort_desc(p);
  row_vector[6] gq_rv_asc = sort_asc(prv);
  row_vector[6] gq_rv_desc = sort_desc(prv);
  array[6] int gq_idx_asc = sort_indices_asc(pr);
  array[6] int gq_idx_desc = sort_indices_desc(pr);
  array[6] int gq_vidx_asc = sort_indices_asc(p);
  array[6] int gq_ridx_desc = sort_indices_desc(prv);
  array[6] int gq_rank;
  array[6] int gq_data_idx_asc = sort_indices_asc(dr);
  array[6] int gq_data_rank;
  for (k in 1:6) {
    gq_rank[k] = rank(p, k);
    gq_data_rank[k] = rank(dv, k);
  }
  array[6] real gq_data_asc = sort_asc(dr);
  vector[6] gq_data_vec_desc = sort_desc(dv);
  vector[120] data_pack = append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(append_row(to_vector(data_real_asc), to_vector(data_real_desc)), to_vector(data_int_asc)), to_vector(data_int_desc)), to_vector(data_vec_asc)), to_vector(data_vec_desc)), to_vector(data_rv_asc)), to_vector(data_rv_desc)), to_vector(data_idx_asc)), to_vector(data_idx_desc)), to_vector(data_iidx_asc)), to_vector(data_iidx_desc)), to_vector(data_vidx_asc)), to_vector(data_vidx_desc)), to_vector(data_ridx_asc)), to_vector(data_ridx_desc)), to_vector(data_rank_real)), to_vector(data_rank_int)), to_vector(data_rank_vec)), to_vector(data_rank_rv));
}
