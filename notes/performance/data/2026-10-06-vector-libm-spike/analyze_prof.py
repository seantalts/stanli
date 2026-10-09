#!/usr/bin/env python3
"""THROWAWAY (spike/vector-libm). Classify sprof samples of bench_grad runs.

analyze_prof.py CORPUS_DIR BENCH MEASURE_MS OUT.json
"""
import collections
import json
import pathlib
import re
import struct
import subprocess
import sys

corpus = pathlib.Path(sys.argv[1])
bench = str(pathlib.Path(sys.argv[2]).resolve())
measure_ns = int(float(sys.argv[3]) * 1e6)
out_path = sys.argv[4]

WRAP = ("log1p_exp|log1m_exp|inv_logit|log_inv_logit|log1m_inv_logit|log_sum_exp|"
        "log_diff_exp|log1p|log1m|expm1|lgamma|digamma|trigamma|lbeta|lchoose|"
        "binomial_coefficient_log|multiply_log|xlogy|xlog1py|lmultiply|lmgamma|"
        "Phi|Phi_approx|inv_Phi|erf|erfc|tanh|atanh|log_softmax|softmax|logit|"
        "inv_cloglog|log_mix|pow|exp|log|sqrt|cbrt|exp2|log2|log10|sin|cos|"
        "tan|sinh|cosh|asinh|acosh|atan|asin|acos|gamma_p|gamma_q|inc_beta|"
        "log_modified_bessel_first_kind|owens_t|tgamma|fma|hypot|square|inv|"
        "inv_sqrt|inv_square|falling_factorial|log_falling_factorial|"
        "rising_factorial|log_rising_factorial|std_normal_log_qf|grad_reg_inc_gamma|"
        "grad_reg_lower_inc_gamma|grad_reg_inc_beta|inc_beta_dda|inc_beta_ddb|"
        "inc_beta_ddz|log_inv_logit_diff|beta|grad_2F1|hypergeometric_2F1|"
        "hypergeometric_pFq|hypergeometric_3F2|F32|grad_F32|log1m_inv_logit")
# sqrt/square/inv/fma are not transcendental; they are matched only so the
# classifier can skip past them to an enclosing transcendental wrapper.
NOT_TRANSC = {"square", "inv", "fma", "sqrt", "inv_sqrt", "inv_square"}
RE_WRAP = re.compile(r"\bstan::math::(?:internal::)?(%s)(?:_fun|_vari)?\b(?!::)" % WRAP)
RE_WRAP_FUN = re.compile(r"\bstan::math::(%s)_fun::fun\b" % WRAP)
RE_BOOST = re.compile(r"\bboost::math::(?:detail::|tools::|lanczos::|policies::)*([A-Za-z0-9_]+)")
RE_EIGEN = re.compile(r"\bEigen::internal::(pexp\w*|plog\w*|generic_\w*(?:log|exp|pow|tanh|erf|sin|cos|atan|expm1|log1p)\w*|p(?:sin|cos|tanh|pow|erf|atan|expm1|log1p|sincos)\w*|generic_pow\w*|pfrexp\w*|pldexp\w*|scalar_(?:exp|log|log1p|expm1|pow|tanh|erf|sin|cos|lgamma|digamma|logistic)\w*_op)")


def norm_libm(name):
    n = name.lstrip("_")
    n = re.sub(r"^(ieee754|kernel|GI|new)_", "", n)
    n = n.lstrip("_")
    n = re.sub(r"^(ieee754|kernel)_", "", n)
    n = re.sub(r"_(fma4|fma|avx2|avx|sse2|sse4_1|ifunc|finite|r|compat|upd|neg|pos)$", "", n)
    n = re.sub(r"_(fma4|fma|avx2|avx|sse2|sse4_1|finite|r)$", "", n)
    n = re.sub(r"(f32x|f64|f32|f64x)$", "", n)
    if n in ("exp1", "exp_1"):
        n = "exp"
    return n or "plt_or_unknown"


def read_maps(path):
    mods = []
    for line in open(path):
        f = line.split()
        if len(f) < 6 or not f[5].startswith("/"):
            continue
        lo, hi = (int(x, 16) for x in f[0].split("-"))
        mods.append((lo, hi, int(f[2], 16), f[5]))
    base = {}
    for lo, hi, off, name in mods:
        if name not in base or lo - off < base[name]:
            base[name] = lo - off
    return mods, base


def locate(pc, mods, base):
    for lo, hi, off, name in mods:
        if lo <= pc < hi:
            return name, pc - base[name]
    return None, pc


samples = {}  # model -> list of (frames [(mod, addr)])
need = collections.defaultdict(set)
for d in sorted(corpus.iterdir()):
    p = d / "sprof.bin"
    if not p.exists() or not (d / "sprof.bin.maps").exists():
        continue
    raw = p.read_bytes()
    t_end, n, depth = struct.unpack_from("<qii", raw, 0)
    mods, base = read_maps(d / "sprof.bin.maps")
    rec = struct.Struct("<qii%dQ" % depth)
    rows = []
    for i in range(n):
        t, k, width, *pcs = rec.unpack_from(raw, 16 + i * rec.size)
        if t < t_end - int(measure_ns * 0.97):
            continue
        fr = []
        for j in range(2, min(k, depth)):
            pc = pcs[j] - (1 if j > 2 else 0)
            m, a = locate(pc, mods, base)
            fr.append((m, a))
            if m:
                need[m].add(a)
        if fr:
            rows.append((fr, width))
    samples[d.name] = rows

sym = {}
for mod, addrs in need.items():
    addrs = sorted(addrs)
    r = subprocess.run(["llvm-symbolizer-18", "--obj=" + mod, "--inlines", "-C",
                        "--output-style=JSON"],
                       input="\n".join(hex(a) for a in addrs) + "\n",
                       capture_output=True, text=True)
    for a, line in zip(addrs, r.stdout.splitlines()):
        try:
            j = json.loads(line)
            sym[(mod, a)] = [s.get("FunctionName", "") for s in j.get("Symbol", [])] or ["??"]
        except json.JSONDecodeError:
            sym[(mod, a)] = ["??"]


def classify_leaf(mod, addr):
    """-> (kind, fn) for the interrupted pc."""
    names = sym.get((mod, addr), ["??"])
    base = pathlib.Path(mod).name if mod else "?"
    if base.startswith("libm.so") or base.startswith("libmvec"):
        return "libm", norm_libm(names[-1])
    if base.startswith("libc.so"):
        return "other", "libc:" + names[-1]
    if base.startswith("sprof") or mod is None:
        return "other", "?"
    wrap_hit = None
    for nm in names:  # innermost first
        m = RE_EIGEN.search(nm)
        if m:
            return "eigen", m.group(1)
        m = RE_BOOST.search(nm)
        if m and "boost::math" in nm:
            return "boost", m.group(1)
        m = RE_WRAP_FUN.search(nm) or RE_WRAP.search(nm)
        if m and m.group(1) not in NOT_TRANSC and wrap_hit is None:
            wrap_hit = m.group(1)
    if wrap_hit:
        return "wrap", wrap_hit
    return "other", names[-1]


def short(fn):
    fn = fn.replace("(anonymous namespace)", "{anon}").replace("operator()", "operator_call")
    out, depth = [], 0
    for ch in fn:
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    fn = "".join(out).split("(")[0].strip()
    fn = fn.split(" ")[-1] if fn else "??"
    return fn[-80:]


def caller(fr):
    """First frame above the leaf that is in the main binary: (physical fn, wrapper or None)."""
    for mod, addr in fr[1:]:
        base = pathlib.Path(mod).name if mod else "?"
        if base.startswith(("libm.so", "libc.so", "sprof", "libmvec")) or mod is None:
            continue
        names = sym.get((mod, addr), ["??"])
        w = None
        for nm in names:
            m = RE_WRAP_FUN.search(nm) or RE_WRAP.search(nm)
            if m and m.group(1) not in NOT_TRANSC:
                w = m.group(1)
                break
            m = RE_BOOST.search(nm)
            if m and "boost::math" in nm:
                w = "boost:" + m.group(1)
                break
        return short(names[-1]), w
    return "?", None


RE_SITE = re.compile(r"^(stanli::(?:dens::)?(?:\{anon\}::)?(?!Executor|run_forward|run_adjoint|ExecutorModel)[\w:]+|stan::math::\w+_(?:lpdf|lpmf|lcdf|lccdf|cdf|log|glm_lpmf|glm_lpdf))$")


def site(fr):
    for mod, addr in fr:
        base = pathlib.Path(mod).name if mod else "?"
        if base.startswith(("libm.so", "libc.so", "sprof", "libmvec")) or mod is None:
            continue
        for nm in sym.get((mod, addr), ["??"]):
            sh = short(nm)
            if RE_SITE.match(sh):
                return sh
    return "?"


LGAMMA = {"lgamma", "digamma", "digamma_imp", "lbeta", "binomial_coefficient_log",
          "trigamma", "lgamma_neg", "lgamma_pos", "tgamma", "lmgamma", "gamma",
          "log_falling_factorial", "log_rising_factorial", "lchoose", "beta"}


def group(kind, fn):
    if kind == "libm" and fn.startswith("fma"):
        return "fma"
    if kind == "libm" and (fn.endswith("l") and fn[:-1] in ("exp", "log", "pow", "log1p", "expm1", "lgamma", "tgamma", "erf", "erfc", "sin", "cos", "sqrt", "floor", "ldexp", "frexp", "fabs")
                           or fn.startswith("powl") or fn.endswith("l_helper")):
        return "longdouble"
    if fn in LGAMMA:
        return "gamma"
    if kind == "boost":
        return "boost_special"
    if kind == "libm" and fn in ("plt_or_unknown", "floor", "ceil", "rint", "trunc", "round", "nearbyint", "fabs", "sqrt", "fmod", "copysign", "isnan", "isinf", "finite", "ldexp", "frexp", "scalbn", "fmax", "fmin", "lround", "lrint", "llround", "llrint", "modf", "nextafter", "hypot", "cbrt", "matherr", "errno_location", "feraiseexcept", "fesetenv", "feholdexcept", "feupdateenv", "fegetenv", "fesetround", "fegetround", "math_err", "math_check_uflow", "math_check_oflow", "math_oflow", "math_uflow", "math_divzero", "math_invalid"):
        return "libm_misc"
    return "elem"  # double-precision elementary function (exp/log/log1p/pow/...)


result = {}
for model, rows in samples.items():
    groups = collections.Counter()
    fns = collections.Counter()
    sites = collections.Counter()
    other = collections.Counter()
    widths = collections.Counter()
    for fr, width in rows:
        wc = "w?" if width < 0 else "w0" if width == 0 else "w1" if width == 1 else "w2-3" if width < 4 else "w4-15" if width < 16 else "w16+"
        kind, fn = classify_leaf(*fr[0])
        if kind == "other":
            groups["other"] += 1
            widths["other/" + wc] += 1
            other[short(fn)] += 1
            continue
        g = group(kind, fn)
        groups[g] += 1
        widths[g + "/" + wc] += 1
        name = fn if kind == "libm" else "%s:%s" % (kind, fn)
        fns["%s/%s" % (g, name)] += 1
        sites["%s/%s @ %s" % (g, name, site(fr))] += 1
    result[model] = dict(n=len(rows), groups=dict(groups), fns=dict(fns),
                         sites=dict(sites), widths=dict(widths), other=dict(other.most_common(8)))
json.dump(result, open(out_path, "w"), indent=1)
print(len(result), "models")
