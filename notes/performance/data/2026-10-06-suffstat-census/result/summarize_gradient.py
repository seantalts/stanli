import json, sys, statistics as st, math
rows=[json.loads(l) for l in open(sys.argv[1])]
ok=[r for r in rows if r["status"]=="ok"]; bad=[r for r in rows if r["status"]!="ok"]
print(len(rows),"models;",len(bad),"failed:",[ (r["model"],r["note"][:50]) for r in bad][:8])
def ratio(r,a,b): return st.median(r["samples"][a])/st.median(r["samples"][b])
sp=sorted(((ratio(r,"off","on"),ratio(r,"on","aa"),r["model"],st.median(r["samples"]["off"]),st.median(r["samples"]["on"])) for r in ok),reverse=True)
gm=lambda xs: math.exp(sum(map(math.log,xs))/len(xs))
print("geomean off/on %.4f ; A/A on/aa geomean %.4f"%(gm([s[0] for s in sp]),gm([s[1] for s in sp])))
aa=sorted(abs(math.log(s[1])) for s in sp); print("A/A |log ratio| p50 %.4f p95 %.4f max %.4f"%(aa[len(aa)//2],aa[int(len(aa)*.95)],aa[-1]))
print(">=1.1x:",sum(s[0]>=1.1 for s in sp),">=2x:",sum(s[0]>=2 for s in sp),">=10x:",sum(s[0]>=10 for s in sp))
print("top:"); [print("  %-40s %8.2fx  (%.0f -> %.0f ns; aa %.3f)"%(s[2],s[0],s[3],s[4],s[1])) for s in sp[:40] if s[0]>=1.05]
print("slower than 2%:"); [print("  %-40s %8.3fx  (%.0f -> %.0f ns; aa %.3f)"%(s[2],s[0],s[3],s[4],s[1])) for s in sp if s[0]<0.98]
