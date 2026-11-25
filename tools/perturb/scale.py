#!/usr/bin/env python3
"""Scale the data of a Stan model along one size variable.

The model source stays the same; only the data JSON changes. Rows and
columns are tiled cyclically or truncated. Groups are scaled by replicating
every group together with the rows that point into it. Anything this cannot
keep consistent is refused with a reason.

  scale.py MODEL.stan DATA.json --list
  scale.py MODEL.stan DATA.json --set N=1000 [--set J=64] --out OUT.json
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import math
import re
import sys

IDENT = r"[A-Za-z_]\w*"
VECTOR_TYPES = {"vector", "row_vector", "simplex", "unit_vector",
                "sum_to_zero_vector", "ordered", "positive_ordered",
                "complex_vector", "complex_row_vector"}
MATRIX_TYPES = {"matrix", "complex_matrix", "sum_to_zero_matrix",
                "column_stochastic_matrix", "row_stochastic_matrix"}
SQUARE_TYPES = {"cov_matrix", "corr_matrix", "cholesky_factor_corr"}
BASETYPES = ({"int", "real", "complex", "cholesky_factor_cov"}
             | VECTOR_TYPES | MATRIX_TYPES | SQUARE_TYPES)
# Tiling these along their own dimensions breaks the constraint.
CONSTRAINED = {"simplex", "unit_vector", "sum_to_zero_vector", "ordered",
               "positive_ordered", "sum_to_zero_matrix",
               "column_stochastic_matrix", "row_stochastic_matrix",
               "cholesky_factor_cov"} | SQUARE_TYPES
KIND_ORDER = {"groups": 0, "rows": 1, "columns": 2}
BLOCK_RE = re.compile(r"\b(functions|transformed\s+data|data|transformed\s+"
                      r"parameters|parameters|model|generated\s+quantities)"
                      r"\s*\{")
OPEN, CLOSE = "([{", ")]}"


class Refused(Exception):
    @property
    def reason(self):
        return self.args[0]


@dataclass
class Decl:
    name: str
    basetype: str
    array_dims: list = field(default_factory=list)
    type_dims: list = field(default_factory=list)
    lower: str | None = None
    upper: str | None = None
    offset: str | None = None
    multiplier: str | None = None
    axes: list = field(default_factory=list)
    unsupported: str | None = None
    text: str = ""

    def bounds(self):
        return [b for b in (self.lower, self.upper, self.offset,
                            self.multiplier) if b is not None]


@dataclass
class SizeVar:
    name: str
    kind: str | None
    value: int | None
    lower: int | None
    upper: int | None
    inferred: bool = False
    refused: str | None = None


def strip_comments(text):
    out, i = [], 0
    while i < len(text):
        if text.startswith("//", i):
            i = text.find("\n", i)
            i = len(text) if i < 0 else i
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = len(text) if end < 0 else end + 2
            out.append(" ")
        elif text[i] == '"':
            end = text.find('"', i + 1)
            end = len(text) - 1 if end < 0 else end
            out.append(text[i:end + 1])
            i = end + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _close(text, i):
    """Index just past the bracket that closes the one at text[i]."""
    depth = 0
    for j in range(i, len(text)):
        if text[j] in OPEN:
            depth += 1
        elif text[j] in CLOSE:
            depth -= 1
            if depth == 0:
                return j + 1
    raise ValueError(f"unbalanced bracket in {text[i:i + 40]!r}")


def _split(text, sep=","):
    parts, depth, start = [], 0, 0
    for j, c in enumerate(text):
        if c in OPEN:
            depth += 1
        elif c in CLOSE:
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(text[start:j])
            start = j + 1
    parts.append(text[start:])
    return [" ".join(p.split()) for p in parts if p.strip()]


def blocks(stan_text):
    text, out, pos = strip_comments(stan_text), {}, 0
    while m := BLOCK_RE.search(text, pos):
        end = _close(text, m.end() - 1)
        out[" ".join(m.group(1).split())] = text[m.end():end - 1]
        pos = end
    return out


def _angle(s):
    """Split '<...>rest' at the '>' that closes the constraint list."""
    depth = 0
    for j in range(1, len(s)):
        if s[j] in OPEN:
            depth += 1
        elif s[j] in CLOSE:
            depth -= 1
        elif s[j] == ">" and depth == 0:
            return s[1:j], s[j + 1:].lstrip()
    raise ValueError(f"unclosed constraint in {s!r}")


def _brackets(s):
    end = _close(s, 0)
    return _split(s[1:end - 1]), s[end:].lstrip()


def _type_axes(basetype, dims):
    if basetype in SQUARE_TYPES or (basetype == "cholesky_factor_cov"
                                    and len(dims) == 1):
        return [dims[0], dims[0]]
    return list(dims) + (["2"] if basetype.startswith("complex") else [])


def parse_decl(stmt):
    s = " ".join(stmt.split())
    if not s:
        return None
    array_dims = []
    if re.match(r"array\s*\[", s):
        array_dims, s = _brackets(s[s.index("["):])
    m = re.match(IDENT, s)
    if not m:
        raise ValueError(f"cannot parse declaration {stmt.strip()!r}")
    basetype, s = m.group(), s[m.end():].lstrip()
    if basetype not in BASETYPES:
        name = re.findall(IDENT, s.split("=")[0]) or [basetype]
        return Decl(name[-1], basetype, array_dims, unsupported=(
            f"{basetype} types are not supported"), text=stmt)
    d = Decl("", basetype, array_dims, text=stmt)
    if s.startswith("<"):
        inner, s = _angle(s)
        for part in _split(inner):
            key, _, value = part.partition("=")
            setattr(d, key.strip(), " ".join(value.split()))
    if s.startswith("["):
        d.type_dims, s = _brackets(s)
    m = re.match(IDENT, s)
    if not m:
        raise ValueError(f"cannot parse declaration {stmt.strip()!r}")
    d.name, s = m.group(), s[m.end():].lstrip()
    if s.startswith("["):
        d.array_dims, s = _brackets(s)
    d.axes = d.array_dims + _type_axes(basetype, d.type_dims)
    return d


def parse_data_block(stan_text):
    body = blocks(stan_text).get("data", "")
    return [d for d in map(parse_decl, _split(body, ";")) if d]


def parse_dims_elsewhere(stan_text):
    """Dimension expressions declared outside the data block, best effort."""
    dims = []
    for name, body in blocks(stan_text).items():
        if name in ("data", "functions"):
            continue
        for m in re.finditer(r"\barray\s*(?=\[)", body):
            dims += _brackets(body[m.end():])[0]
        types = "|".join(sorted(BASETYPES - {"int", "real", "complex"}))
        for m in re.finditer(rf"\b(?:{types}|int|real|complex)\b", body):
            s = body[m.end():].lstrip()
            if s.startswith("<"):
                s = _angle(s)[1]
            if s.startswith("["):
                found, s = _brackets(s)
                dims += found
            m2 = re.match(rf"({IDENT})\s*(?=\[)", s)
            if m2 and m2.group(1) not in BASETYPES:
                dims += _brackets(s[m2.end():])[0]
    return dims


def _idents(expr):
    return set(re.findall(IDENT, expr or ""))


def _flat(value):
    if isinstance(value, list):
        for x in value:
            yield from _flat(x)
    else:
        yield value


def _lengths(value, depth):
    if depth == 0:
        return [len(value)] if isinstance(value, list) else [None]
    if not isinstance(value, list):
        return [None]
    return [n for x in value for n in _lengths(x, depth - 1)]


def _at_depth(value, depth, fn):
    if depth == 0:
        return fn(value)
    return [_at_depth(x, depth - 1, fn) for x in value]


def _shift(value, by):
    if isinstance(value, list):
        return [_shift(x, by) for x in value]
    return value + by


def _int_bound(expr):
    if expr is None:
        return None
    if re.fullmatch(r"-?\d+", expr):
        return int(expr)
    raise Refused(f"non-numeric bound {expr}")


class Analysis:
    """Everything about the model and its original data that scaling needs."""

    def __init__(self, stan_text, data):
        self.decls = {d.name: d for d in parse_data_block(stan_text)}
        self.data = data
        elsewhere = set().union(*map(_idents, parse_dims_elsewhere(stan_text)))
        self.scalars = [n for n, d in self.decls.items()
                        if d.basetype == "int" and not d.axes
                        and not d.unsupported]
        self.index = {}
        self.inferred = set()
        for d in self.decls.values():
            if (d.basetype == "int" and d.array_dims and d.upper in
                    self.scalars):
                self.index[d.name] = d.upper
        used = set()
        for d in self.decls.values():
            for a in d.axes:
                used |= _idents(a)
        self.candidates = [n for n in self.scalars
                           if n in used or n in elsewhere
                           or n in self.index.values()]
        self._infer()
        self.vars = {n: self._size_var(n) for n in self.candidates}

    def value(self, name):
        v = self.data.get(name)
        return v if isinstance(v, int) and not isinstance(v, bool) else None

    def _infer(self):
        for d in self.decls.values():
            if (d.basetype != "int" or not d.array_dims or d.unsupported
                    or d.upper is not None or d.lower != "1"
                    or d.name not in self.data):
                continue
            values = list(_flat(self.data[d.name]))
            if not values:
                continue
            own = set().union(*map(_idents, d.axes))
            hits = [c for c in self.candidates if c not in own
                    and self.value(c) == max(values)]
            if len(hits) == 1:
                self.index[d.name] = hits[0]
                self.inferred.add(d.name)

    def containers(self, var):
        """(decl, axis position) for every bare use of var as an axis."""
        return [(d, i) for d in self.decls.values()
                for i, a in enumerate(d.axes) if a == var]

    def _size_var(self, name):
        d = self.decls[name]
        into = [a for a, up in self.index.items() if up == name]
        rows = any(i == 0 for _, i in self.containers(name))
        kind = "groups" if into else "rows" if rows else "columns"
        sv = SizeVar(name, kind, self.value(name), None, None,
                     inferred=bool(into) and all(a in self.inferred
                                                 for a in into))
        try:
            sv.lower, sv.upper = _int_bound(d.lower), _int_bound(d.upper)
            self._check_basic(name)
            if kind == "groups":
                self.closure(name)
            else:
                self._check_axes(name, {name})
        except Refused as e:
            sv.refused = f"{name}: {e.reason}" if e.reason.startswith(
                "non-numeric") else e.reason
        return sv

    def _check_basic(self, var):
        value = self.value(var)
        if value is None:
            raise Refused(f"data has no integer value for {var}")
        for d in self.decls.values():
            if d.unsupported:
                if var in _idents(d.text):
                    raise Refused(f"unsupported declaration {d.name} uses "
                                  f"{var} ({d.unsupported})")
                continue
            for a in d.axes:
                if a != var and var in _idents(a):
                    raise Refused(f"compound dimension {a} of {d.name} "
                                  f"uses {var}")
            for b in d.bounds():
                if var in _idents(b) and not (
                        d.name in self.index and b == d.upper == var):
                    raise Refused(f"bound of {d.name} uses {var}")
        for d, i in self.containers(var):
            if d.name not in self.data:
                raise Refused(f"data has no value for {d.name}, which "
                              f"{var} sizes")
            bad = [n for n in _lengths(self.data[d.name], i) if n != value]
            if bad:
                raise Refused(f"shape mismatch: {d.name} has length {bad[0]} "
                              f"along {var}, which is {value}")
        if value == 0 and self.containers(var):
            raise Refused(f"{var} is 0, so its containers are empty and "
                          f"cannot be tiled")
        for d in self.decls.values():
            if (d.basetype == "int" and d.array_dims and not d.unsupported
                    and d.name not in self.index and d.name in self.data
                    and sum(_flat(self.data[d.name])) == value):
                raise Refused(f"ragged group sizes {d.name} sum to {var}")
        for other in self.scalars:
            if other not in self.candidates and self.value(other) == value:
                raise Refused(f"undeclared relation: {other} has the same "
                              f"value as {var} ({value})")

    def _check_axes(self, var, members):
        for d in self.decls.values():
            touched = [i for i, a in enumerate(d.axes) if a in members]
            if len(touched) > 1:
                raise Refused(f"{d.name} has more than one axis that "
                              f"scaling {var} would change")
            if (touched and d.basetype in CONSTRAINED
                    and touched[0] >= len(d.array_dims)):
                raise Refused(f"{d.basetype} {d.name} would not stay valid "
                              f"if tiled along {d.axes[touched[0]]}")

    def closure(self, var):
        members, todo = [var], [var]
        while todo:
            u = todo.pop()
            for arr, up in self.index.items():
                if up != u:
                    continue
                lead = self.decls[arr].axes[0]
                if lead not in self.scalars:
                    raise Refused(f"index array {arr} into {u} has leading "
                                  f"dimension {lead}, not a data variable")
                if lead not in members:
                    members.append(lead)
                    todo.append(lead)
        for m in members[1:]:
            try:
                self._check_basic(m)
            except Refused as e:
                raise Refused(f"replicating {var} changes {m}: "
                              f"{e.reason}") from None
        self._check_axes(var, set(members))
        return members


def size_variables(stan_text, data):
    return Analysis(stan_text, data).vars


def _resize(xs, n, name):
    if n and not xs:
        raise Refused(f"cannot tile empty {name}")
    return [xs[i % len(xs)] for i in range(n)]


def _check_target(sv, target):
    if sv.lower is not None and target < sv.lower:
        raise Refused(f"{sv.name}={target} is below its lower bound "
                      f"{sv.lower}")
    if sv.upper is not None and target > sv.upper:
        raise Refused(f"{sv.name}={target} is above its upper bound "
                      f"{sv.upper}")


def _scale_axis(an, data, var, target):
    for d, i in an.containers(var):
        data[d.name] = _at_depth(data[d.name], i,
                                 lambda xs, n=d.name: _resize(xs, target, n))
    data[var] = target


def _scale_groups(an, data, var, target):
    members = an.closure(var)
    old = {m: data[m] for m in members}
    k = max(1, math.ceil(target / max(old[var], 1)))
    for d in an.decls.values():
        touched = [i for i, a in enumerate(d.axes) if a in members]
        if not touched or d.name not in data:
            continue
        shift = old[an.index[d.name]] if an.index.get(d.name) in old else 0
        data[d.name] = _at_depth(
            data[d.name], touched[0],
            lambda xs, s=shift: [_shift(x, r * s) for r in range(k)
                                 for x in xs])
    for m in members:
        data[m] = old[m] * k
    if target < data[var]:
        _shrink(an, data, var, members, target)


def _shrink(an, data, var, members, target):
    for m in members[1:]:
        into = [a for a, up in an.index.items() if up == m]
        if into:
            raise Refused(f"cannot shrink {var}: {into[0]} indexes into "
                          f"{m}, which replication also scales")
    for m in members[1:]:
        arrays = [a for a, up in an.index.items()
                  if up == var and an.decls[a].axes[0] == m]
        keep = [all(x <= target for a in arrays for x in _flat(data[a][j]))
                for j in range(data[m])]
        for d, i in an.containers(m):
            data[d.name] = _at_depth(
                data[d.name], i, lambda xs: [x for x, k in zip(xs, keep) if k])
        data[m] = sum(keep)
        if m in an.vars:
            _check_target(an.vars[m], data[m])
    for d, i in an.containers(var):
        data[d.name] = _at_depth(data[d.name], i, lambda xs: xs[:target])
    data[var] = target


def _problems(an, data):
    found = set()
    for d in an.decls.values():
        if d.unsupported or d.name not in data:
            continue
        for i, a in enumerate(d.axes):
            if a in an.scalars and isinstance(data.get(a), int):
                if any(n != data[a] for n in _lengths(data[d.name], i)):
                    found.add(f"{d.name} does not have length {a} along "
                              f"axis {i}")
        up = an.index.get(d.name)
        if up and isinstance(data.get(up), int):
            if any(x > data[up] for x in _flat(data[d.name])):
                found.add(f"{d.name} has an index above {up}")
    return found


def scale_many(stan_text, data, targets):
    an = Analysis(stan_text, data)
    order = {n: i for i, n in enumerate(an.decls)}
    for var in targets:
        sv = an.vars.get(var)
        if sv is None:
            raise Refused(f"{var} is not a size variable")
        if sv.refused:
            raise Refused(sv.refused)
        _check_target(sv, targets[var])
    groups = [v for v in targets if an.vars[v].kind == "groups"]
    for i, a in enumerate(groups):
        for b in groups[i + 1:]:
            shared = set(an.closure(a)) & set(an.closure(b))
            if shared:
                raise Refused(f"{a} and {b} conflict: replicating either "
                              f"changes {', '.join(sorted(shared))}")
    before = _problems(an, data)
    new = dict(data)
    for var in sorted(targets, key=lambda v: (KIND_ORDER[an.vars[v].kind],
                                              order[v])):
        if an.vars[var].kind == "groups":
            _scale_groups(an, new, var, targets[var])
        else:
            _scale_axis(an, new, var, targets[var])
    after = _problems(an, new) - before
    if after:
        raise Refused(f"scaled data is inconsistent: {sorted(after)[0]}")
    info = {n: new[n] for n in an.vars if new[n] != data[n]}
    return new, info


def scale(stan_text, data, var, target):
    return scale_many(stan_text, data, {var: target})


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("model")
    p.add_argument("data")
    p.add_argument("--list", action="store_true")
    p.add_argument("--set", action="append", default=[], metavar="VAR=N")
    p.add_argument("--out")
    args = p.parse_args(argv)
    with open(args.model) as f:
        text = f.read()
    with open(args.data) as f:
        data = json.load(f)
    if args.list:
        for sv in size_variables(text, data).values():
            if sv.refused:
                print(f"{sv.name}\t{sv.kind}\trefused: {sv.refused}")
            else:
                print(f"{sv.name}\t{sv.kind}\tvalue={sv.value}\t"
                      f"lower={sv.lower}"
                      + ("\tinferred" if sv.inferred else ""))
        return 0
    if not args.set or not args.out:
        p.error("give --list, or --set VAR=N with --out")
    targets = {}
    for item in args.set:
        name, _, value = item.partition("=")
        targets[name] = int(float(value))
    try:
        new, info = scale_many(text, data, targets)
    except Refused as e:
        print(f"refused: {e.reason}", file=sys.stderr)
        return 1
    with open(args.out, "w") as f:
        json.dump(new, f)
    print(" ".join(f"{n}={v}" for n, v in info.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
