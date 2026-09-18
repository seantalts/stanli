parameters { vector[3] q; }
model {
  target += normal_id_glm_lpdf(q | [0.1, 0.2, 0.3]', 0.5, [0.7]', 1.2);
  target += normal_id_glm_lpdf(q | [0.1, 0.2], 0.5, [0.7, 0.8]', 1.2);
  target += normal_id_glm_lpdf(0.3 | q, 0.5, [0.7]', 1.2);
  target += normal_id_glm_lpdf([0.1, 0.2]' | q', 0.5, [0.7, 0.8, 0.9]', 1.2);
}
