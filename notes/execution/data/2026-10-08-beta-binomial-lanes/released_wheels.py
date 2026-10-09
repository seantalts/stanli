import os, sys, json, stanli
print("stanli", getattr(stanli, "__version__", "?"))
for m in ("v1", "v3", "v5"):
    model = stanli.Model(stan_file=f"{m}.stan", data="data200.json")
    lp, g = model.log_prob_grad([0.0, 0.0, 0.0, 0.0])
    print(m, repr(float(lp)), [float(x) for x in g])
