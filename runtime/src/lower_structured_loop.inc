// Included inside Lowering: isolated structured candidates reuse expression
// lowering, typed views and the ordinary kernel vocabulary.
using RegionNode = StructuredLoop::Node;
RegionNode* region_current = nullptr;
size_t region_emitted = 0;
std::map<std::string, Val> region_cells;
std::map<std::string, IntRange> region_analysis_ints;
int region_control_depth = 0;
bool region_control_active = false;
unsigned structured_outer_depth = 0;

void region_flush() {
  while (region_emitted < g.ops.size()) {
    RegionNode n;
    n.kind = RegionNode::KernelCall;
    n.op = static_cast<int>(region_emitted++);
    region_current->children.push_back(std::move(n));
  }
}
void region_append(RegionNode node) {
  region_flush();
  region_current->children.push_back(std::move(node));
}

// A small data-only UDF can determine a shape exactly even though the
// retained numerical region cannot fold the surrounding control. Probe it
// under a hard statement budget so shape discovery cannot turn a compact
// loop into an unbounded preparation-time replay.
std::optional<DataMap::Entry> region_bounded_data_value(const mir::Expr& e) {
  if (!e.data_only || expr_effectful(e)) return {};
  std::function<bool(const mir::Expr&)> calls_user =
      [&](const mir::Expr& value) {
        if (value.kind == mir::Expr::FunApp &&
            value.fn_lib == mir::Expr::Lib::UserDefined)
          return true;
        for (const auto& arg : value.args)
          if (calls_user(arg)) return true;
        return false;
      };
  if (!calls_user(e)) return {};
  int64_t steps = 0;
  MirHooks hooks;
  hooks.data = [this](const std::string& name) -> const DataMap::Entry* {
    return data.has(name) ? &data.at(name) : nullptr;
  };
  hooks.int_var = [this](const std::string& name, long* value) {
    const auto found = int_env.find(name);
    if (found == int_env.end()) return false;
    *value = found->second;
    return true;
  };
  hooks.stmt = [&](const mir::Stmt&) {
    if (++steps > 4096)
      throw CompileError(
          "stanli compile: structured data probe budget exceeded");
    return false;
  };
  try {
    MirInterp<double> probe(fun_defs, "structured data probe", hooks);
    probe.env() = td.env();
    return probe.eval(e);
  } catch (const std::exception&) {
    return {};
  }
}

std::optional<int64_t> region_bounded_data_integer(const mir::Expr& e) {
  auto value = region_bounded_data_value(e);
  if (!value || !value->is_int || value->i.size() != 1 || value->r.size() != 1)
    return {};
  return value->i[0];
}

void region_alias(int dst, int src) {
  if (dst == src) return;
  RegionNode n;
  n.kind = RegionNode::Alias;
  n.dst = dst;
  n.src = src;
  region_append(std::move(n));
}

// Evaluate a data-only scalar over the finite integer ranges currently in
// scope.  This keeps reusable loop analysis compact: it visits the data
// points, but never emits or unrolls the numerical loop body.
std::optional<RealRange> region_enumerated_data_range(const mir::Expr& e) {
  if (!e.data_only || expr_effectful(e)) return {};
  std::function<bool(const mir::Expr&)> calls_user =
      [&](const mir::Expr& value) {
        if (value.kind == mir::Expr::FunApp &&
            value.fn_lib == mir::Expr::Lib::UserDefined)
          return true;
        for (const auto& arg : value.args)
          if (calls_user(arg)) return true;
        return false;
      };
  if (calls_user(e)) return {};
  std::map<std::string, IntRange> variables;
  std::function<bool(const mir::Expr&)> collect = [&](const mir::Expr& value) {
    if (value.kind == mir::Expr::Var) {
      const auto found = scope.find(value.name);
      if (found != scope.end() && !observation(found->second) &&
          !td.find(value.name)) {
        if (value.unsized.leaf != mir::UnsizedLeaf::Int ||
            value.unsized.depth != 0)
          return false;
        const auto range = int_ranges.find(found->second.slot);
        if (range != int_ranges.end())
          variables[value.name] = range->second;
        else if (auto inferred = region_analysis_ints.find(value.name);
                 inferred != region_analysis_ints.end())
          variables[value.name] = inferred->second;
        else
          return false;
      } else if (found == scope.end()) {
        if (auto inferred = region_analysis_ints.find(value.name);
            inferred != region_analysis_ints.end())
          variables[value.name] = inferred->second;
      }
    }
    for (const auto& arg : value.args)
      if (!collect(arg)) return false;
    return true;
  };
  if (!collect(e)) return {};
  uint64_t combinations = 1;
  for (const auto& variable : variables) {
    const uint64_t width =
        uint64_t(int64_t(variable.second.hi) - variable.second.lo + 1);
    if (width == 0 || combinations > 1000000 / width) return {};
    combinations *= width;
  }
  struct Saved {
    std::string name;
    std::optional<DataMap::Entry> value;
  };
  std::vector<Saved> saved;
  for (const auto& variable : variables) {
    const auto found = td.env().find(variable.first);
    saved.push_back(
        {variable.first, found == td.env().end()
                             ? std::optional<DataMap::Entry>{}
                             : std::optional<DataMap::Entry>{found->second}});
  }
  bool any = false;
  RealRange result;
  std::vector<std::pair<std::string, IntRange>> ordered(variables.begin(),
                                                        variables.end());
  std::function<void(size_t)> enumerate = [&](size_t at) {
    if (at != ordered.size()) {
      for (int64_t value = ordered[at].second.lo;
           value <= ordered[at].second.hi; ++value) {
        DataMap::Entry entry;
        entry.is_int = true;
        entry.i = {static_cast<int>(value)};
        entry.r = {static_cast<double>(value)};
        td.env()[ordered[at].first] = std::move(entry);
        enumerate(at + 1);
      }
      return;
    }
    try {
      DataMap::Entry value = td.eval(e);
      if (value.r.size() != 1 || !std::isfinite(value.r[0])) return;
      if (!any)
        result = {value.r[0], value.r[0]};
      else {
        result.lo = std::min(result.lo, value.r[0]);
        result.hi = std::max(result.hi, value.r[0]);
      }
      if (value.r[0] > 0)
        result.positive_lo = std::min(result.positive_lo, value.r[0]);
      any = true;
    } catch (const std::exception&) {
      // A path-invalid selector may be excluded by an enclosing branch.
      // Other reachable failures remain runtime failures.
    }
  };
  enumerate(0);
  for (auto& item : saved) {
    if (item.value)
      td.env()[item.name] = std::move(*item.value);
    else
      td.env().erase(item.name);
  }
  if (!any) return {};
  return result;
}

std::optional<RealRange> region_real_range(const mir::Expr& e) {
  if (auto enumerated = region_enumerated_data_range(e)) return enumerated;
  if (e.kind == mir::Expr::LitInt)
    return RealRange{double(e.lit_i), double(e.lit_i)};
  if (e.kind == mir::Expr::LitReal) return RealRange{e.lit, e.lit};
  if (e.kind == mir::Expr::Promotion && e.args.size() == 1)
    return region_real_range(e.args[0]);
  if (e.kind == mir::Expr::Var) {
    const auto value = scope.find(e.name);
    if (value != scope.end()) {
      if (const auto found = real_ranges.find(value->second.slot);
          found != real_ranges.end())
        return found->second;
      if (const auto* observed = observation(value->second)) {
        if (!observed->r.empty()) {
          const auto mm =
              std::minmax_element(observed->r.begin(), observed->r.end());
          if (std::isfinite(*mm.first) && std::isfinite(*mm.second)) {
            RealRange result{*mm.first, *mm.second};
            for (double x : observed->r)
              if (x > 0) result.positive_lo = std::min(result.positive_lo, x);
            return result;
          }
        }
      }
    }
    if (const auto* observed = td.find(e.name)) {
      if (!observed->r.empty()) {
        const auto mm =
            std::minmax_element(observed->r.begin(), observed->r.end());
        if (std::isfinite(*mm.first) && std::isfinite(*mm.second)) {
          RealRange result{*mm.first, *mm.second};
          for (double x : observed->r)
            if (x > 0) result.positive_lo = std::min(result.positive_lo, x);
          return result;
        }
      }
    }
    return {};
  }
  if (e.kind == mir::Expr::TernaryIf && e.args.size() == 3) {
    auto a = region_real_range(e.args[1]);
    auto b = region_real_range(e.args[2]);
    if (a && b) {
      RealRange result{std::min(a->lo, b->lo), std::max(a->hi, b->hi)};
      result.positive_lo = std::min(a->positive_lo, b->positive_lo);
      return result;
    }
    return {};
  }
  if (e.kind != mir::Expr::FunApp || e.args.empty()) return {};
  auto a = region_real_range(e.args[0]);
  if (!a) return {};
  if (e.args.size() == 1) {
    if (e.name == "PMinus__") return RealRange{-a->hi, -a->lo};
    if (e.name == "ceil") {
      RealRange result{std::ceil(a->lo), std::ceil(a->hi)};
      if (std::isfinite(a->positive_lo))
        result.positive_lo = std::ceil(a->positive_lo);
      return result;
    }
    if (e.name == "floor")
      return RealRange{std::floor(a->lo), std::floor(a->hi)};
    if (e.name == "fabs" || e.name == "abs") {
      const double hi = std::max(std::abs(a->lo), std::abs(a->hi));
      return RealRange{a->lo <= 0 && a->hi >= 0
                           ? 0.0
                           : std::min(std::abs(a->lo), std::abs(a->hi)),
                       hi};
    }
    return {};
  }
  if (e.args.size() != 2) return {};
  auto b = region_real_range(e.args[1]);
  if (!b) return {};
  RealRange result;
  if (e.name == "Plus__")
    result = {a->lo + b->lo, a->hi + b->hi};
  else if (e.name == "Minus__")
    result = {a->lo - b->hi, a->hi - b->lo};
  else if (e.name == "Times__") {
    double v[] = {a->lo * b->lo, a->lo * b->hi, a->hi * b->lo, a->hi * b->hi};
    result = {*std::min_element(v, v + 4), *std::max_element(v, v + 4)};
  } else if (e.name == "Divide__" && (b->lo > 0 || b->hi < 0)) {
    double v[] = {a->lo / b->lo, a->lo / b->hi, a->hi / b->lo, a->hi / b->hi};
    result = {*std::min_element(v, v + 4), *std::max_element(v, v + 4)};
  } else
    return {};
  if (!std::isfinite(result.lo) || !std::isfinite(result.hi)) return {};
  if (e.name == "Times__" && std::isfinite(a->positive_lo) &&
      std::isfinite(b->positive_lo))
    result.positive_lo = a->positive_lo * b->positive_lo;
  if (e.name == "Divide__" && b->lo > 0 && std::isfinite(a->positive_lo))
    result.positive_lo = a->positive_lo / b->hi;
  return result;
}

std::optional<IntRange> region_range(const mir::Expr& e) {
  const auto bounded = [](int64_t a, int64_t b) -> std::optional<IntRange> {
    if (a < INT32_MIN || b > INT32_MAX || a > b) return {};
    return IntRange{static_cast<int32_t>(a), static_cast<int32_t>(b)};
  };
  if (e.kind == mir::Expr::LitInt) return bounded(e.lit_i, e.lit_i);
  if (auto observed = region_bounded_data_integer(e))
    return bounded(*observed, *observed);
  if (e.kind == mir::Expr::Promotion && e.args.size() == 1)
    return region_range(e.args[0]);
  if (e.kind == mir::Expr::Var) {
    auto v = scope.find(e.name);
    if (v != scope.end()) {
      auto r = int_ranges.find(v->second.slot);
      if (r != int_ranges.end()) return r->second;
      if (const auto* en = observation(v->second))
        if (en->is_int && !en->i.empty()) {
          auto mm = std::minmax_element(en->i.begin(), en->i.end());
          return bounded(*mm.first, *mm.second);
        }
      auto inferred = region_analysis_ints.find(e.name);
      if (inferred != region_analysis_ints.end()) return inferred->second;
      return {};
    }
    auto i = int_env.find(e.name);
    if (i != int_env.end()) return bounded(i->second, i->second);
    if (const auto* en = td.find(e.name))
      if (en->is_int && !en->i.empty()) {
        auto mm = std::minmax_element(en->i.begin(), en->i.end());
        return bounded(*mm.first, *mm.second);
      }
    if (auto inferred = region_analysis_ints.find(e.name);
        inferred != region_analysis_ints.end())
      return inferred->second;
    return {};
  }
  if (e.kind == mir::Expr::Indexed && !e.args.empty())
    return region_range(e.args[0]);
  if (e.kind == mir::Expr::EAnd || e.kind == mir::Expr::EOr)
    return IntRange{0, 1};
  if (e.kind == mir::Expr::TernaryIf && e.args.size() == 3) {
    auto a = region_range(e.args[1]), b = region_range(e.args[2]);
    if (a && b) return IntRange{std::min(a->lo, b->lo), std::max(a->hi, b->hi)};
    return {};
  }
  if (e.kind != mir::Expr::FunApp) return {};
  if (is_shape_query(e)) {
    auto shape = try_static_shape_query(e);
    if (shape.state == StaticProbeState::Known)
      return bounded(shape.value, shape.value);
    return {};
  }
  const std::string& f = e.name;
  if (f == "Less__" || f == "Leq__" || f == "Greater__" || f == "Geq__" ||
      f == "Equals__" || f == "NEquals__" || f == "PNot__")
    return IntRange{0, 1};
  if (f == "sum" && e.args.size() == 1 &&
      e.args[0].unsized.leaf == mir::UnsizedLeaf::Int) {
    if (e.args[0].kind == mir::Expr::Var) {
      const auto value = scope.find(e.args[0].name);
      if (value != scope.end()) {
        if (const auto* observed = observation(value->second)) {
          if (observed->is_int && !observed->i.empty()) {
            int64_t total = 0;
            for (int element : observed->i) total += element;
            return bounded(total, total);
          }
        }
        const auto element = int_ranges.find(value->second.slot);
        if (element != int_ranges.end()) {
          const int64_t count = g.slots[value->second.slot].len;
          const int64_t lo =
              element->second.lo < 0 ? count * int64_t(element->second.lo) : 0;
          const int64_t hi =
              element->second.hi > 0 ? count * int64_t(element->second.hi) : 0;
          return bounded(lo, hi);
        }
      }
    }
    return {};
  }
  if (e.args.empty()) return {};
  auto a = region_range(e.args[0]);
  if (!a) return {};
  if (e.args.size() == 1 && f == "PMinus__")
    return bounded(-int64_t(a->hi), -int64_t(a->lo));
  if (e.args.size() != 2) return {};
  auto b = region_range(e.args[1]);
  if (!b) return {};
  if (f == "Plus__")
    return bounded(int64_t(a->lo) + b->lo, int64_t(a->hi) + b->hi);
  if (f == "Minus__")
    return bounded(int64_t(a->lo) - b->hi, int64_t(a->hi) - b->lo);
  if (f == "Times__") {
    int64_t v[] = {int64_t(a->lo) * b->lo, int64_t(a->lo) * b->hi,
                   int64_t(a->hi) * b->lo, int64_t(a->hi) * b->hi};
    return bounded(*std::min_element(v, v + 4), *std::max_element(v, v + 4));
  }
  if ((f == "Divide__" || f == "IntDivide__") && (b->lo > 0 || b->hi < 0)) {
    int64_t v[] = {int64_t(a->lo) / b->lo, int64_t(a->lo) / b->hi,
                   int64_t(a->hi) / b->lo, int64_t(a->hi) / b->hi};
    return bounded(*std::min_element(v, v + 4), *std::max_element(v, v + 4));
  }
  return {};
}
void region_forget(const std::string& name) {
  int_env.erase(name);
  int_locals.erase(name);
  td.env().erase(name);
}
Val region_cell(const std::string& name) {
  auto cell = region_cells.find(name);
  if (cell != region_cells.end()) return cell->second;
  Val source;
  auto value = scope.find(name);
  if (value != scope.end())
    source = value->second;
  else if (auto i = int_env.find(name); i != int_env.end())
    source = constant(static_cast<double>(i->second));
  else {
    int s = env_slot(name);
    if (s < 0) s = uninitialized_decl_slot(name);
    if (s < 0) fail("structured assignment has no binding: " + name);
    source = scope.at(name);
  }
  Val binding{add_slot(g.slots[source.slot].len, false), source.autodiff,
              source.si, source.runtime_dims};
  auto range = int_ranges.find(source.slot);
  if (range != int_ranges.end())
    int_ranges[binding.slot] = range->second;
  else if (const auto* en = observation(source)) {
    if (en->is_int && !en->i.empty()) {
      const auto mm = std::minmax_element(en->i.begin(), en->i.end());
      set_int_range(binding, *mm.first, *mm.second);
    }
  }
  if (auto range = real_ranges.find(source.slot); range != real_ranges.end())
    real_ranges[binding.slot] = range->second;
  else if (const auto* observed = observation(source)) {
    if (observed->r.size() == 1 && std::isfinite(observed->r[0]))
      real_ranges[binding.slot] = {observed->r[0], observed->r[0]};
  }
  region_alias(binding.slot, source.slot);
  region_cells[name] = binding;
  scope[name] = binding;
  region_forget(name);
  return binding;
}
void region_prepare_cells(const mir::Stmt& s) {
  std::vector<std::string> assigned;
  assigned_names(s, &assigned);
  for (const auto& name : assigned)
    if (scope.count(name) || int_env.count(name) || decls.count(name))
      (void)region_cell(name);
}
// A reusable body must account for every iteration, not just the incoming
// values. Compute a conservative fixed point over source dependencies before
// selecting kernels, and discard first-iteration integer observations.
bool region_depends(const mir::Expr& e, const std::set<std::string>& active) {
  if (e.kind == mir::Expr::Var) return active.count(e.name) != 0;
  if (is_shape_query(e) &&
      try_static_shape_query(e).state == StaticProbeState::Known)
    return false;
  for (const auto& a : e.args)
    if (region_depends(a, active)) return true;
  return false;
}
std::set<std::string> region_dependencies(const mir::Stmt& s) {
  std::set<std::string> active;
  for (const auto& item : scope)
    if (!item.second.si.param_free) active.insert(item.first);
  std::function<bool(const mir::Expr&)> depends = [&](const mir::Expr& e) {
    return region_depends(e, active);
  };
  std::function<void(const mir::Stmt&, bool)> visit = [&](const mir::Stmt& st,
                                                          bool control) {
    control |= depends(st.cond);
    if (st.kind == mir::Stmt::For || st.kind == mir::Stmt::While) {
      // An early exit may make even an unconditional data assignment's
      // final value depend on a parameter-sensitive predicate.
      std::function<bool(const mir::Stmt&, bool)> exit_depends =
          [&](const mir::Stmt& child, bool path) {
            path |= depends(child.cond);
            if ((child.kind == mir::Stmt::Break ||
                 child.kind == mir::Stmt::Continue) &&
                path)
              return true;
            if (child.kind == mir::Stmt::For || child.kind == mir::Stmt::While)
              return false;
            for (const auto& c : child.body)
              if (exit_depends(c, path)) return true;
            return false;
          };
      for (const auto& child : st.body) control |= exit_depends(child, control);
    }
    if (st.kind == mir::Stmt::For) {
      control |= depends(st.lower) || depends(st.upper);
      if (control) active.insert(st.loopvar);
    }
    if (st.kind == mir::Stmt::Assignment) {
      bool value = control || depends(st.rhs);
      for (const auto& ix : st.lhs_idx) value |= depends(ix);
      if (value) active.insert(st.lhs);
    }
    if (st.kind == mir::Stmt::Decl &&
        (control || (st.has_init && depends(st.init))))
      active.insert(st.decl_id);
    for (const auto& child : st.body) visit(child, control);
  };
  size_t previous;
  do {
    previous = active.size();
    visit(s, false);
  } while (active.size() != previous);
  return active;
}
void region_widen_carried(const mir::Stmt& s,
                          const std::set<std::string>& active) {
  const auto saved_analysis = region_analysis_ints;
  region_analysis_ints.clear();
  std::set<std::string> carried_names;
  for (const auto& item : scope) {
    carried_names.insert(item.first);
    if (auto range = int_ranges.find(item.second.slot);
        range != int_ranges.end())
      region_analysis_ints[item.first] = range->second;
  }
  for (const auto& item : int_env)
    region_analysis_ints[item.first] = {static_cast<int32_t>(item.second),
                                        static_cast<int32_t>(item.second)};
  // Establish bounded integer locals before asking for carried real ranges.
  // A few fixed-point passes cover the ordinary chains emitted by stanc
  // without executing the loop or duplicating its graph body.
  for (int pass = 0; pass < 4; ++pass) {
    bool changed = false;
    std::function<void(const mir::Stmt&)> infer = [&](const mir::Stmt& st) {
      if (st.kind == mir::Stmt::For) {
        auto lo = region_range(st.lower), hi = region_range(st.upper);
        if (lo && hi) {
          IntRange next{lo->lo, hi->hi};
          auto old = region_analysis_ints.find(st.loopvar);
          if (old == region_analysis_ints.end() || old->second.lo != next.lo ||
              old->second.hi != next.hi) {
            region_analysis_ints[st.loopvar] = next;
            changed = true;
          }
        }
      }
      if (st.kind == mir::Stmt::Assignment && st.lhs_idx.empty() &&
          st.rhs.unsized.leaf == mir::UnsizedLeaf::Int &&
          st.rhs.unsized.depth == 0) {
        if (auto range = region_range(st.rhs)) {
          IntRange next = *range;
          auto old = region_analysis_ints.find(st.lhs);
          if (old != region_analysis_ints.end() &&
              carried_names.count(st.lhs)) {
            next.lo = std::min(next.lo, old->second.lo);
            next.hi = std::max(next.hi, old->second.hi);
          }
          if (old == region_analysis_ints.end() || old->second.lo != next.lo ||
              old->second.hi != next.hi) {
            region_analysis_ints[st.lhs] = next;
            changed = true;
          }
        }
      }
      for (const auto& child : st.body) infer(child);
    };
    infer(s);
    if (!changed) break;
  }
  std::vector<std::string> writes;
  assigned_names(s, &writes);
  for (const auto& name : writes) {
    auto v = scope.find(name);
    if (v == scope.end()) continue;
    if (active.count(name)) v->second.si.param_free = false;
    const bool scalar =
        g.slots[v->second.slot].len == 1 && v->second.si.kind == ViewKind::Flat;
    std::optional<IntRange> integer;
    if (auto found = int_ranges.find(v->second.slot); found != int_ranges.end())
      integer = found->second;
    std::optional<RealRange> real;
    if (auto found = real_ranges.find(v->second.slot);
        found != real_ranges.end())
      real = found->second;
    bool integer_unknown = false, real_unknown = false;
    std::function<void(const mir::Stmt&)> widen = [&](const mir::Stmt& st) {
      if (st.kind == mir::Stmt::Assignment && st.lhs == name &&
          st.lhs_idx.empty()) {
        if (auto r = region_range(st.rhs)) {
          if (integer)
            integer = IntRange{std::min(integer->lo, r->lo),
                               std::max(integer->hi, r->hi)};
          else
            integer = *r;
        } else
          integer_unknown = true;
        if (scalar) {
          auto inferred_real = region_real_range(st.rhs);
          if (auto r = inferred_real) {
            if (real) {
              RealRange merged{std::min(real->lo, r->lo),
                               std::max(real->hi, r->hi)};
              merged.positive_lo = std::min(real->positive_lo, r->positive_lo);
              real = merged;
            } else
              real = *r;
          } else
            real_unknown = true;
        }
      }
      for (const auto& child : st.body) widen(child);
    };
    widen(s);
    region_cells[name] = v->second;
    if (integer && !integer_unknown)
      set_int_range(v->second, integer->lo, integer->hi);
    else
      int_ranges.erase(v->second.slot);
    if (scalar && real && !real_unknown)
      real_ranges[v->second.slot] = *real;
    else
      real_ranges.erase(v->second.slot);
    region_forget(name);
  }
  region_analysis_ints = saved_analysis;
}
void region_assign(const std::string& name, Val value,
                   std::optional<IntRange> range = {},
                   std::optional<RealRange> real_range = {}) {
  Val binding = region_cell(name);
  const DataMap::Entry* observed = observation(value);
  if (!same_view(binding.si, g.slots[binding.slot].len, value.si,
                 g.slots[value.slot].len))
    fail("structured assignment changes logical shape: " + name);
  if (binding.runtime_dims.size() != value.runtime_dims.size())
    fail("structured assignment changes runtime shape rank: " + name);
  for (size_t axis = 0; axis < binding.runtime_dims.size(); ++axis)
    if ((binding.runtime_dims[axis] >= 0) != (value.runtime_dims[axis] >= 0))
      fail("structured assignment changes runtime shape: " + name);
  region_alias(binding.slot, value.slot);
  // One runtime cell can name different SSA values across iterations and
  // control arms.  Its formal AD type must cover every value that may be
  // installed in it; the dynamic handle records which particular version
  // was active on this execution.
  binding.autodiff = binding.autodiff || value.autodiff;
  binding.si.param_free = value.si.param_free && !region_control_active;
  region_cells[name] = binding;
  scope[name] = binding;
  if (observed)
    observe(binding, *observed);
  else
    forget_observation(binding);
  region_forget(name);
  if (range)
    set_int_range(binding, range->lo, range->hi);
  else
    int_ranges.erase(binding.slot);
  if (real_range)
    real_ranges[binding.slot] = *real_range;
  else
    real_ranges.erase(binding.slot);
}

void region_refine_condition(const mir::Expr& e, bool truth) {
  if (truth && e.kind == mir::Expr::EAnd) {
    for (const auto& arg : e.args) region_refine_condition(arg, true);
    return;
  }
  if (e.kind != mir::Expr::FunApp || e.args.size() != 2) return;
  const mir::Expr* variable = nullptr;
  const mir::Expr* constant_expr = nullptr;
  bool reversed = false;
  if (e.args[0].kind == mir::Expr::Var) {
    variable = &e.args[0];
    constant_expr = &e.args[1];
  } else if (e.args[1].kind == mir::Expr::Var) {
    variable = &e.args[1];
    constant_expr = &e.args[0];
    reversed = true;
  }
  if (!variable) return;
  const auto constant = region_range(*constant_expr);
  const auto value = scope.find(variable->name);
  if (!constant || constant->lo != constant->hi || value == scope.end()) return;
  const auto old = int_ranges.find(value->second.slot);
  if (old == int_ranges.end()) return;
  int64_t lo = old->second.lo, hi = old->second.hi;
  const int64_t c = constant->lo;
  std::string op = e.name;
  if (reversed) {
    if (op == "Less__")
      op = "Greater__";
    else if (op == "Leq__")
      op = "Geq__";
    else if (op == "Greater__")
      op = "Less__";
    else if (op == "Geq__")
      op = "Leq__";
  }
  if (!truth) {
    if (op == "Less__")
      op = "Geq__";
    else if (op == "Leq__")
      op = "Greater__";
    else if (op == "Greater__")
      op = "Leq__";
    else if (op == "Geq__")
      op = "Less__";
    else if (op == "Equals__")
      return;
    else if (op == "NEquals__")
      op = "Equals__";
  }
  if (op == "Less__")
    hi = std::min(hi, c - 1);
  else if (op == "Leq__")
    hi = std::min(hi, c);
  else if (op == "Greater__")
    lo = std::max(lo, c + 1);
  else if (op == "Geq__")
    lo = std::max(lo, c);
  else if (op == "Equals__")
    lo = hi = c;
  if (lo <= hi) set_int_range(value->second, lo, hi);
}

// Compile a fixed-shape indexed access; values of selectors stay runtime.
Val region_index(const Val& base, const std::vector<mir::Expr>& selectors,
                 const std::string& type, mir::UnsizedView result_view,
                 const Val* rhs = nullptr) {
  auto spec = std::make_shared<DynamicIndexSpec>();
  std::vector<int64_t> dims;
  ViewKind leaf = base.si.kind;
  if (is_array(base.si)) {
    dims = array_shape(base.si).dims;
    leaf = array_shape(base.si).leaf;
  } else if (is_matrix(base.si))
    dims = {base.si.rows, base.si.cols};
  else if (is_vector(base.si) || is_row_vector(base.si))
    dims = {g.slots[base.slot].len};
  else
    fail("structured index needs a container");
  const bool mat = leaf == ViewKind::Matrix;
  spec->matrix_leaf = mat;
  if (selectors.size() > dims.size()) fail("too many structured indices");
  const size_t outer = dims.size() - (mat ? 2 : 0);
  std::vector<int64_t> strides(dims.size(), 1);
  int64_t stride = 1;
  if (mat) {
    strides[outer] = 1;
    strides[outer + 1] = dims[outer];
    stride = checked_product({dims[outer], dims[outer + 1]}, "matrix");
  }
  for (size_t d = outer; d-- > 0;) {
    strides[d] = stride;
    stride = checked_product({stride, dims[d]}, "index strides");
  }
  std::vector<Val> indices;
  std::vector<int64_t> index_offsets;
  std::vector<int64_t> output_dims;
  std::vector<int> output_runtime_dims;
  int64_t index_width = 0, selected = 1;
  for (size_t d = 0; d < dims.size(); ++d) {
    DynamicIndexSpec::Axis axis;
    axis.extent = dims[d];
    axis.stride = strides[d];
    axis.input_offset = index_width;
    int output_runtime_dim = -1;
    const mir::Expr* ix = d < selectors.size() ? &selectors[d] : nullptr;
    if (!ix || ix->name == "IndexAll") {
      axis.kind = DynamicIndexSpec::Axis::All;
      axis.count = dims[d];
    } else if (ix->name == "IndexSingle" && ix->args.size() == 1) {
      axis.kind = DynamicIndexSpec::Axis::Single;
      axis.count = 1;
      Val v = lower_expr(ix->args[0]);
      if (g.slots[v.slot].len != 1) fail("non-scalar structured index");
      index_offsets.push_back(index_width);
      indices.push_back(v);
      ++index_width;
    } else if (ix->name == "IndexMulti" && ix->args.size() == 1) {
      axis.kind = DynamicIndexSpec::Axis::Multi;
      Val v = lower_expr(ix->args[0]);
      axis.count = g.slots[v.slot].len;
      index_offsets.push_back(index_width);
      indices.push_back(v);
      index_width += axis.count;
      int runtime_count = -1;
      if (has_runtime_shape(v)) {
        runtime_count = one_runtime_extent(v, "multi-index");
        axis.count_input_offset = index_width;
        Val count{runtime_count, false, view_of("UInt")};
        count.si.param_free = true;
        index_offsets.push_back(index_width);
        indices.push_back(count);
        ++index_width;
      }
      output_runtime_dim = runtime_count;
    } else if (ix->name == "IndexBetween" && ix->args.size() == 2) {
      const auto lo = region_range(ix->args[0]), hi = region_range(ix->args[1]);
      if (!lo || !hi || lo->lo != lo->hi)
        fail("structured slice needs a fixed lower bound and finite capacity");
      axis.kind = DynamicIndexSpec::Axis::Range;
      const bool runtime_upper = hi->lo != hi->hi;
      axis.count = std::max<int64_t>(
          0, (runtime_upper ? dims[d] : int64_t(hi->hi)) - lo->lo + 1);
      Val lower = lower_expr(ix->args[0]);
      index_offsets.push_back(index_width);
      indices.push_back(lower);
      ++index_width;
      int runtime_count = -1;
      if (runtime_upper) {
        if (lo->lo != 1 || hi->lo < 0)
          fail("structured varying slice currently needs 1:n bounds");
        Val upper = lower_expr(ix->args[1]);
        axis.count_input_offset = index_width;
        index_offsets.push_back(index_width);
        indices.push_back(upper);
        ++index_width;
        runtime_count = upper.slot;
      }
      output_runtime_dim = runtime_count;
    } else
      fail("unsupported structured index selector");
    const int base_runtime_dim =
        d < base.runtime_dims.size() ? base.runtime_dims[d] : -1;
    if (base_runtime_dim >= 0) {
      axis.extent_input_offset = index_width;
      Val extent{base_runtime_dim, false, view_of("UInt")};
      extent.si.param_free = true;
      index_offsets.push_back(index_width);
      indices.push_back(extent);
      ++index_width;
      if (axis.kind == DynamicIndexSpec::Axis::All) {
        axis.count_input_offset = axis.extent_input_offset;
        output_runtime_dim = base_runtime_dim;
      }
    }
    selected = checked_product({selected, axis.count}, "structured selection");
    if (axis.kind != DynamicIndexSpec::Axis::Single) {
      output_dims.push_back(axis.count);
      output_runtime_dims.push_back(output_runtime_dim);
    }
    spec->axes.push_back(axis);
  }
  spec->selected_size = selected;
  SlotInfo si = view_of(type);
  if (type == "UMatrix") {
    if (output_dims.size() < 2) fail("structured matrix selection shape");
    si = matrix_view(output_dims[output_dims.size() - 2], output_dims.back());
  } else if (type.find("UArray") != std::string::npos) {
    si = array_view(output_dims, leaf_kind(result_view.leaf));
  }
  Val result;
  if (rhs && g.slots[rhs->slot].len != selected)
    fail("structured indexed assignment width mismatch");
  // Direct fixed-capacity data operands avoid recording CONCAT2
  // intermediates. Updates keep the RHS last, leaving room for at most four
  // selector/count/extent operands in the six-input kernel ABI.
  const size_t max_direct_selectors = rhs ? 4 : 5;
  const bool direct_inputs =
      indices.size() >= 2 && indices.size() <= max_direct_selectors &&
      std::all_of(indices.begin(), indices.end(),
                  [&](const Val& input) {
        return g.slots[input.slot].len > 0 && !input.autodiff &&
               input.si.param_free;
      }) &&
      std::getenv("STANLI_NO_STRUCTURED_DIRECT_INDEX_INPUTS") == nullptr;
  if (direct_inputs) {
    const auto remap = [&](int64_t* offset, int* input) {
      for (size_t i = 0; i < indices.size(); ++i) {
        const int64_t begin = index_offsets[i];
        const int64_t len = g.slots[indices[i].slot].len;
        if (*offset >= begin && *offset < begin + len) {
          *offset -= begin;
          *input = static_cast<int>(i + 1);
          return;
        }
      }
      fail("structured direct index operand has no source");
    };
    for (auto& axis : spec->axes) {
      if (axis.kind != DynamicIndexSpec::Axis::All)
        remap(&axis.input_offset, &axis.selector_input);
      if (axis.count_input_offset >= 0)
        remap(&axis.count_input_offset, &axis.count_input);
      if (axis.extent_input_offset >= 0)
        remap(&axis.extent_input_offset, &axis.extent_input);
    }
    spec->input_count = static_cast<int>(indices.size() + 1 + (rhs ? 1 : 0));
    std::vector<int> inputs;
    inputs.reserve(static_cast<size_t>(spec->input_count));
    inputs.push_back(base.slot);
    SlotInfo output_info = rhs ? base.si : si;
    output_info.param_free = base.si.param_free;
    bool autodiff = base.autodiff;
    for (const Val& input : indices) {
      inputs.push_back(input.slot);
      output_info.param_free = output_info.param_free && input.si.param_free;
      autodiff = autodiff || input.autodiff;
    }
    if (rhs) {
      inputs.push_back(rhs->slot);
      spec->rhs_input = static_cast<int>(inputs.size() - 1);
      output_info.param_free = output_info.param_free && rhs->si.param_free;
      autodiff = autodiff || rhs->autodiff;
    }
    result = emit_raw(
        rhs ? OP_SET_INDEX_DYNAMIC : OP_INDEX_DYNAMIC, std::move(inputs),
        rhs ? g.slots[base.slot].len : selected, output_info, {}, -1, autodiff);
    if (rhs) result.runtime_dims = base.runtime_dims;
  } else {
    Val packed = indices.empty() ? constant(0) : indices[0];
    for (size_t i = 1; i < indices.size(); ++i)
      packed = emit_value(
          OP_CONCAT2, {packed, indices[i]},
          g.slots[packed.slot].len + g.slots[indices[i].slot].len);
    if (rhs) {
      result = emit_value(OP_SET_INDEX_DYNAMIC, {base, packed, *rhs},
                          g.slots[base.slot].len, base.si);
      // An element update changes values, not the declaration-time logical
      // extents of the containing array.
      result.runtime_dims = base.runtime_dims;
    } else {
      result = emit_value(OP_INDEX_DYNAMIC, {base, packed}, selected, si);
    }
  }
  if (!rhs &&
      std::any_of(output_runtime_dims.begin(), output_runtime_dims.end(),
                  [](int slot) { return slot >= 0; }))
    result.runtime_dims = std::move(output_runtime_dims);
  g.ops.back().udata = spec.get();
  g.udata_pool.push_back(spec);
  return result;
}
static bool region_nested_return(const mir::Stmt& s, bool control = false) {
  if (s.kind == mir::Stmt::Return) return control;
  control |= s.kind == mir::Stmt::IfElse || s.kind == mir::Stmt::For ||
             s.kind == mir::Stmt::While;
  for (const auto& c : s.body)
    if (region_nested_return(c, control)) return true;
  return false;
}
// Resolve a pure boolean from any observed data subexpression.  The MIR
// interpreter can only evaluate the complete expression, so an unknown
// loop counter in `counter > 0 && disabled == 0` otherwise hides the
// constant-false RHS and makes us compile an unreachable arm.
std::optional<bool> region_static_truth(const mir::Expr& e) {
  if (expr_effectful(e)) return {};
  if (auto value = try_eval_pure(e)) {
    if (value->r.size() == 1) return value->r[0] != 0.0;
  }
  if ((e.kind != mir::Expr::EAnd && e.kind != mir::Expr::EOr) ||
      e.args.size() != 2)
    return {};
  const auto lhs = region_static_truth(e.args[0]);
  const auto rhs = region_static_truth(e.args[1]);
  if (e.kind == mir::Expr::EAnd) {
    if ((lhs && !*lhs) || (rhs && !*rhs)) return false;
    if (lhs && rhs) return *lhs && *rhs;
  } else {
    if ((lhs && *lhs) || (rhs && *rhs)) return true;
    if (lhs && rhs) return *lhs || *rhs;
  }
  return {};
}

std::optional<Val> region_expr(const mir::Expr& e) {
  if (e.kind == mir::Expr::Indexed) {
    if (e.args.size() == 1) return lower_expr(e.args[0]);
    Val base = lower_expr(e.args[0]);
    return region_index(base, {e.args.begin() + 1, e.args.end()}, e.type_,
                        e.unsized);
  }
  if (e.kind == mir::Expr::EAnd || e.kind == mir::Expr::EOr) {
    mir::Expr zero;
    zero.kind = mir::Expr::LitInt;
    zero.lit_i = 0;
    zero.type_ = "UInt";
    zero.unsized = {0, mir::UnsizedLeaf::Int};
    zero.data_only = true;
    mir::Expr one = zero;
    one.lit_i = 1;
    mir::Expr truth;
    truth.kind = mir::Expr::FunApp;
    truth.name = "NEquals__";
    truth.fn_lib = mir::Expr::Lib::StanLib;
    truth.args = {e.args[1], zero};
    truth.type_ = "UInt";
    truth.unsized = zero.unsized;
    mir::Expr choice = e;
    choice.kind = mir::Expr::TernaryIf;
    choice.args = e.kind == mir::Expr::EAnd
                      ? std::vector<mir::Expr>{e.args[0], truth, zero}
                      : std::vector<mir::Expr>{e.args[0], one, truth};
    return region_expr(choice);
  }
  if (e.kind == mir::Expr::TernaryIf) {
    if (auto c = region_static_truth(e.args[0]))
      return lower_expr(e.args[*c ? 1 : 2]);
    Val condition = lower_expr(e.args[0]);
    region_flush();
    RegionNode branch;
    branch.kind = RegionNode::If;
    branch.condition = condition.slot;
    branch.children.resize(2);
    RegionNode* parent = region_current;
    region_current = &branch.children[0];
    Val a = lower_expr(e.args[1]);
    Val result{add_slot(g.slots[a.slot].len, false), expression_autodiff(e),
               a.si};
    region_alias(result.slot, a.slot);
    region_flush();
    region_current = &branch.children[1];
    Val b = lower_expr(e.args[2]);
    if (!same_view(a.si, g.slots[a.slot].len, b.si, g.slots[b.slot].len))
      fail("structured ternary changes shape");
    region_alias(result.slot, b.slot);
    region_flush();
    region_current = parent;
    region_append(std::move(branch));
    result.si.param_free =
        condition.si.param_free && a.si.param_free && b.si.param_free;
    return result;
  }
  if (e.kind != mir::Expr::FunApp) return {};
  if (e.name == "target" || e.name.find("_jacobian") != std::string::npos)
    fail(
        "structured target reads and Jacobian calls need a separate "
        "contract");
  if (e.fn_lib == mir::Expr::Lib::UserDefined) {
    // Inline first.  Structured statement lowering handles terminal
    // two-arm returns directly; genuinely unsupported early-return shapes
    // reject the native candidate and retain the whole-region fallback.
    try {
      return lower_call_udf(e);
    } catch (const CompileError& error) {
      fail("structured UDF " + e.name + ": " + error.what(), e.raw);
    }
  }
  return lower_runtime_scalar(e);
}

static const std::map<std::string, int>& runtime_comparisons() {
  static const std::map<std::string, int> table = {
      {"Less__", 0}, {"Leq__", 1},    {"Greater__", 2},
      {"Geq__", 3},  {"Equals__", 4}, {"NEquals__", 5},
      {"PNot__", 6}, {"is_nan", 7},   {"is_inf", 8}};
  return table;
}
static const std::map<std::string, int>& runtime_int_arithmetic() {
  static const std::map<std::string, int> table = {
      {"Plus__", 0},      {"Minus__", 1},  {"Times__", 2}, {"Divide__", 3},
      {"IntDivide__", 3}, {"Modulo__", 4}, {"PMinus__", 5}};
  return table;
}
static bool runtime_scalar_form(const mir::Expr& e) {
  if (e.kind != mir::Expr::FunApp || e.fn_lib != mir::Expr::Lib::StanLib)
    return false;
  if (runtime_comparisons().count(e.name)) return true;
  return e.unsized.leaf == mir::UnsizedLeaf::Int && e.unsized.depth == 0 &&
         runtime_int_arithmetic().count(e.name);
}

bool runtime_only(const mir::Expr& e) {
  return !in_write_array && needs_runtime_value(e) && !try_eval_pure(e);
}
bool runtime_int_expression(const mir::Expr& e) {
  if (in_write_array || !runtime_scalar_form(e)) return false;
  bool found = false;
  const auto integer_reads = [&](const auto& self, const mir::Expr& x) -> bool {
    if (x.kind == mir::Expr::Var || x.kind == mir::Expr::Indexed) {
      if (!needs_runtime_value(x)) return true;
      found = true;
      return x.type_ == "UInt";
    }
    for (const auto& argument : x.args)
      if (!self(self, argument)) return false;
    return true;
  };
  return integer_reads(integer_reads, e) && found && !try_eval_pure(e);
}
bool runtime_selector(const mir::Expr& index) {
  for (const auto& argument : index.args)
    if (runtime_only(argument)) return true;
  return false;
}
std::optional<long> static_int(const mir::Expr& e) {
  try {
    return eval_int(e);
  } catch (const CompileError&) {
    if (in_write_array || !needs_runtime_value(e)) throw;
    return std::nullopt;
  }
}
void bind_runtime_int(const std::string& name, const mir::Expr& e,
                      const std::string& raw) {
  Val v = lower_expr(e);
  SlotInfo expected;
  expected.param_free = v.si.param_free;
  require_binding(v, 1, expected, name, raw);
  v.autodiff = false;
  v.si = expected;
  if (in_write_array) v.si.param_free = false;
  scope[name] = v;
  decls[name] = DeclView{1, false, expected};
  int_env.erase(name);
  int_locals.erase(name);
  td.env().erase(name);
}

// These kernels also serve consumers outside a retained region. Integer
// identity and dependence do not imply that a value is known at prepare.
std::optional<Val> lower_runtime_scalar(const mir::Expr& e) {
  if (e.kind != mir::Expr::FunApp || e.fn_lib != mir::Expr::Lib::StanLib)
    return {};
  const auto& comparisons = runtime_comparisons();
  auto comparison = comparisons.find(e.name);
  if (comparison != comparisons.end()) {
    Val a = lower_expr(e.args.at(0)), v;
    if (e.args.size() == 1)
      v = emit_value(OP_COMPARE, {a}, 1, view_of("UInt"));
    else {
      Val b = lower_expr(e.args.at(1));
      v = emit_value(OP_COMPARE, {a, b}, 1, view_of("UInt"));
    }
    g.ops.back().variant = static_cast<uint8_t>(comparison->second);
    v.autodiff = false;
    set_int_range(v, 0, 1);
    return v;
  }
  if (e.unsized.leaf == mir::UnsizedLeaf::Int && e.unsized.depth == 0) {
    const auto& arithmetic = runtime_int_arithmetic();
    auto operation = arithmetic.find(e.name);
    if (operation != arithmetic.end()) {
      auto range = region_range(e);
      Val a = lower_expr(e.args.at(0)), v;
      if (e.args.size() == 1)
        v = emit_value(OP_INT_ARITH, {a}, 1, view_of("UInt"));
      else {
        Val b = lower_expr(e.args.at(1));
        v = emit_value(OP_INT_ARITH, {a, b}, 1, view_of("UInt"));
      }
      g.ops.back().variant = static_cast<uint8_t>(operation->second);
      v.autodiff = false;
      if (range) set_int_range(v, range->lo, range->hi);
      return v;
    }
  }
  return {};
}

// An integer guard supplies an upper bound only when every non-exiting
// iteration has an unconditional positive update and the endpoint is
// invariant.
std::optional<int64_t> region_while_capacity(const mir::Stmt& s,
                                             std::string* proven_counter) {
  const mir::Expr* guard = nullptr;
  std::function<void(const mir::Expr&)> find = [&](const mir::Expr& e) {
    if (e.kind == mir::Expr::EAnd) {
      for (const auto& a : e.args) find(a);
      return;
    }
    if (!guard && e.kind == mir::Expr::FunApp &&
        (e.name == "Less__" || e.name == "Leq__") && e.args.size() == 2 &&
        e.args[0].kind == mir::Expr::Var)
      guard = &e;
  };
  find(s.cond);
  if (!guard) return {};
  const std::string counter = guard->args[0].name;
  *proven_counter = counter;
  const bool integer_counter =
      guard->args[0].unsized.leaf == mir::UnsizedLeaf::Int;
  auto int_start = region_range(guard->args[0]);
  auto int_end = region_range(guard->args[1]);
  auto real_start = region_real_range(guard->args[0]);
  auto real_end = region_real_range(guard->args[1]);
  if (integer_counter ? (!int_start || !int_end) : (!real_start || !real_end)) {
    if (std::getenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS")) {
      std::cerr << "stanli_structured while capacity ranges counter=" << counter
                << " integer=" << integer_counter << " start="
                << bool(integer_counter ? bool(int_start) : bool(real_start))
                << " end="
                << bool(integer_counter ? bool(int_end) : bool(real_end))
                << " real_slots=" << real_ranges.size() << "\n";
      std::function<void(const mir::Expr&)> show = [&](const mir::Expr& x) {
        if (x.kind == mir::Expr::Var) {
          auto sv = scope.find(x.name);
          std::cerr << "  var " << x.name
                    << " slot=" << (sv == scope.end() ? -1 : sv->second.slot);
          if (sv != scope.end()) {
            auto rr = real_ranges.find(sv->second.slot);
            if (rr != real_ranges.end())
              std::cerr << " range=[" << rr->second.lo << "," << rr->second.hi
                        << "]";
          }
          std::cerr << "\n";
        }
        for (const auto& a : x.args) show(a);
      };
      show(guard->args[1]);
    }
    return {};
  }
  std::vector<std::string> assigned;
  assigned_names(s, &assigned);
  for (const auto& name : assigned)
    if (expr_references(guard->args[1], name)) return {};
  int64_t int_step = 0;
  double real_step_lo = 0, real_step_hi = 0;
  bool valid = true;
  std::function<void(const mir::Stmt&, bool)> advances =
      [&](const mir::Stmt& st, bool unconditional) {
        if (st.kind == mir::Stmt::For || st.kind == mir::Stmt::While) {
          std::vector<std::string> writes;
          assigned_names(st, &writes);
          if (std::find(writes.begin(), writes.end(), counter) !=
              writes.end()) {
            valid = false;
          }
          return;
        }
        if (st.kind == mir::Stmt::Continue) {
          valid = false;
          return;
        }
        if (st.kind == mir::Stmt::Assignment && st.lhs == counter) {
          const auto& e = st.rhs;
          if (!unconditional || !st.lhs_idx.empty() ||
              e.kind != mir::Expr::FunApp || e.name != "Plus__" ||
              e.args.size() != 2 || e.args[0].kind != mir::Expr::Var ||
              e.args[0].name != counter) {
            valid = false;
            return;
          }
          auto int_increment = region_range(e.args[1]);
          auto real_increment = region_real_range(e.args[1]);
          if (integer_counter) {
            if (!int_increment || int_increment->lo <= 0 ||
                int_increment->lo != int_increment->hi) {
              valid = false;
              return;
            }
          } else {
            if (!real_increment || real_increment->lo <= 0 ||
                !std::isfinite(real_increment->hi)) {
              valid = false;
              return;
            }
          }
          for (const auto& name : assigned)
            if (expr_references(e.args[1], name)) {
              valid = false;
              return;
            }
          if (integer_counter)
            int_step += int_increment->lo;
          else {
            real_step_lo += real_increment->lo;
            real_step_hi += real_increment->hi;
          }
        }
        for (const auto& c : st.body)
          advances(c, unconditional && st.kind != mir::Stmt::IfElse);
      };
  for (const auto& st : s.body) advances(st, true);
  if (!valid) {
    if (std::getenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS"))
      std::cerr << "stanli_structured while capacity invalid update counter="
                << counter << "\n";
    return {};
  }
  int64_t count = 0;
  auto v = scope.find(counter);
  if (integer_counter) {
    if (int_step <= 0) return {};
    const int64_t limit =
        int64_t(int_end->hi) + (guard->name == "Leq__" ? 1 : 0);
    if (limit + int_step - 1 > INT32_MAX) return {};
    const int64_t distance = std::max<int64_t>(0, limit - int_start->lo);
    count = (distance + int_step - 1) / int_step;
    if (v != scope.end())
      set_int_range(v->second, int_start->lo,
                    std::max<int64_t>(int_start->hi, limit + int_step - 1));
  } else {
    if (!(real_step_lo > 0)) return {};
    const double distance = std::max(0.0, real_end->hi - real_start->lo);
    const double raw = std::ceil(distance / real_step_lo) +
                       (guard->name == "Leq__" ? 1.0 : 0.0) + 2.0;
    if (!std::isfinite(raw) || raw > double(INT64_MAX)) return {};
    count = static_cast<int64_t>(raw);
    // Require addition to make representable progress at the largest
    // reachable magnitude; otherwise no finite double iteration bound was
    // proven even though the real-number interval is positive.
    const double magnitude =
        std::max(std::abs(real_start->lo), std::abs(real_end->hi));
    if (!(magnitude + real_step_lo > magnitude)) return {};
    if (v != scope.end())
      real_ranges[v->second.slot] = {
          real_start->lo,
          std::max(real_start->hi, real_end->hi + real_step_hi)};
    if (std::getenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS"))
      std::cerr << "stanli_structured real while counter=" << counter
                << " start=[" << real_start->lo << "," << real_start->hi
                << "] end=[" << real_end->lo << "," << real_end->hi
                << "] step=[" << real_step_lo << "," << real_step_hi
                << "] capacity=" << count << "\n";
  }
  *proven_counter = counter;
  return count;
}
void lower_region_stmt(const mir::Stmt& s) {
  switch (s.kind) {
    case mir::Stmt::Block:
    case mir::Stmt::SList:
      for (const auto& c : s.body) lower_stmt(c);
      return;
    case mir::Stmt::For: {
      region_prepare_cells(s);
      auto lo = region_range(s.lower), hi = region_range(s.upper);
      const bool known = lo && hi;
      const int64_t count =
          known ? std::max<int64_t>(0, int64_t(hi->hi) - lo->lo + 1) : -1;
      // Recognize the common Stan construction
      //   for (i in 1:size(a)) a[i] = bounded_integer_expression;
      // so later integer reductions know that every element is initialized.
      // Only unconditional assignments through transparent blocks qualify.
      std::vector<std::pair<std::string, const mir::Expr*>> full_int_writes;
      std::function<void(const mir::Stmt&)> find_full_int_writes =
          [&](const mir::Stmt& body) {
            if (body.kind == mir::Stmt::Block ||
                body.kind == mir::Stmt::SList) {
              for (const auto& child : body.body) find_full_int_writes(child);
              return;
            }
            if (body.kind != mir::Stmt::Assignment || body.lhs_idx.size() != 1)
              return;
            const mir::Expr& index = body.lhs_idx[0];
            if (index.name != "IndexSingle" || index.args.size() != 1 ||
                index.args[0].kind != mir::Expr::Var ||
                index.args[0].name != s.loopvar)
              return;
            if (s.upper.kind != mir::Expr::FunApp || s.upper.name != "size" ||
                s.upper.args.size() != 1 ||
                s.upper.args[0].kind != mir::Expr::Var ||
                s.upper.args[0].name != body.lhs)
              return;
            full_int_writes.emplace_back(body.lhs, &body.rhs);
          };
      if (known && lo->lo == 1 && lo->hi == 1 && hi->lo == hi->hi)
        for (const auto& child : s.body) find_full_int_writes(child);
      if (count == 0) return;
      bool has_loop_control = false;
      std::function<void(const mir::Stmt&)> scan_control =
          [&](const mir::Stmt& body) {
            if (body.kind == mir::Stmt::Break ||
                body.kind == mir::Stmt::Continue)
              has_loop_control = true;
            if (body.kind == mir::Stmt::For || body.kind == mir::Stmt::While)
              return;
            for (const auto& child : body.body) scan_control(child);
          };
      for (const auto& child : s.body) scan_control(child);
      if (known && count == 1 && lo->lo == lo->hi && hi->lo == hi->hi &&
          lo->lo == hi->hi && !has_loop_control) {
        const auto saved_scope = scope.find(s.loopvar);
        const std::optional<Val> saved_iterator = saved_scope == scope.end()
                                                      ? std::optional<Val>{}
                                                      : saved_scope->second;
        const auto saved_int = int_env.find(s.loopvar);
        const std::optional<long> saved_integer =
            saved_int == int_env.end() ? std::optional<long>{}
                                       : std::optional<long>{saved_int->second};
        const auto saved_data = td.env().find(s.loopvar);
        const std::optional<DataMap::Entry> saved_entry =
            saved_data == td.env().end()
                ? std::optional<DataMap::Entry>{}
                : std::optional<DataMap::Entry>{saved_data->second};
        Val iterator = constant(static_cast<double>(lo->lo));
        set_int_range(iterator, lo->lo, lo->lo);
        scope[s.loopvar] = iterator;
        int_env[s.loopvar] = lo->lo;
        DataMap::Entry entry;
        entry.is_int = true;
        entry.i = {lo->lo};
        entry.r = {static_cast<double>(lo->lo)};
        td.env()[s.loopvar] = std::move(entry);
        for (const auto& child : s.body) lower_stmt(child);
        if (saved_iterator)
          scope[s.loopvar] = *saved_iterator;
        else
          scope.erase(s.loopvar);
        if (saved_integer)
          int_env[s.loopvar] = *saved_integer;
        else
          int_env.erase(s.loopvar);
        if (saved_entry)
          td.env()[s.loopvar] = *saved_entry;
        else
          td.env().erase(s.loopvar);
        region_cells.erase(s.loopvar);
        return;
      }
      Val lower = lower_expr(s.lower), upper = lower_expr(s.upper);
      const auto saved_scope = scope.find(s.loopvar);
      const std::optional<Val> saved_iterator = saved_scope == scope.end()
                                                    ? std::optional<Val>{}
                                                    : saved_scope->second;
      const auto dependencies = region_dependencies(s);
      Val iterator{add_slot(1, false), false, view_of("UInt")};
      iterator.si.param_free = lower.si.param_free && upper.si.param_free;
      scope[s.loopvar] = iterator;
      region_forget(s.loopvar);
      if (known) set_int_range(iterator, lo->lo, hi->hi);
      region_widen_carried(s, dependencies);
      // One additional pass resolves loop-carried bounds whose update uses
      // another carried integer (ctsem's time[rowi] - time[prevrow]).
      region_widen_carried(s, dependencies);
      RegionNode loop;
      loop.kind = RegionNode::For;
      loop.lower = lower.slot;
      loop.upper = upper.slot;
      loop.iterator = iterator.slot;
      loop.children.resize(1);
      region_flush();
      RegionNode* parent = region_current;
      region_current = &loop.children[0];
      const bool saved_control = region_control_active;
      region_control_active |= !lower.si.param_free || !upper.si.param_free;
      ++region_control_depth;
      for (const auto& c : s.body) lower_stmt(c);
      --region_control_depth;
      region_flush();
      region_current = parent;
      region_append(std::move(loop));
      region_control_active = saved_control;
      region_widen_carried(s, dependencies);
      for (const auto& write : full_int_writes) {
        const auto value = scope.find(write.first);
        if (value == scope.end() || !is_array(value->second.si) ||
            g.slots[value->second.slot].len != hi->hi)
          continue;
        if (auto range = region_range(*write.second))
          set_int_range(value->second, range->lo, range->hi);
      }
      if (saved_iterator)
        scope[s.loopvar] = *saved_iterator;
      else
        scope.erase(s.loopvar);
      region_cells.erase(s.loopvar);
      return;
    }
    case mir::Stmt::While: {
      region_prepare_cells(s);
      std::string counter;
      const bool proven = region_while_capacity(s, &counter).has_value();
      // The counter proof widened its range; all other carried integers
      // are unknown until a declaration/assignment inside the body bounds
      // them.
      const int counter_slot =
          proven && scope.count(counter) ? scope.at(counter).slot : -1;
      std::optional<IntRange> counter_range;
      std::optional<RealRange> counter_real_range;
      if (counter_slot >= 0) {
        if (auto found = int_ranges.find(counter_slot);
            found != int_ranges.end())
          counter_range = found->second;
        if (auto found = real_ranges.find(counter_slot);
            found != real_ranges.end())
          counter_real_range = found->second;
      }
      const auto dependencies = region_dependencies(s);
      region_widen_carried(s, dependencies);
      if (counter_range) int_ranges[counter_slot] = *counter_range;
      if (counter_real_range) real_ranges[counter_slot] = *counter_real_range;
      RegionNode loop;
      loop.kind = RegionNode::While;
      loop.children.resize(2);
      region_flush();
      RegionNode* parent = region_current;
      region_current = &loop.children[0];
      Val condition = lower_expr(s.cond);
      loop.condition = condition.slot;
      region_flush();
      region_current = &loop.children[1];
      const bool saved_control = region_control_active;
      region_control_active |= !condition.si.param_free;
      ++region_control_depth;
      for (const auto& c : s.body) lower_stmt(c);
      --region_control_depth;
      region_flush();
      region_current = parent;
      region_append(std::move(loop));
      region_control_active = saved_control;
      region_widen_carried(s, dependencies);
      return;
    }
    case mir::Stmt::IfElse: {
      if (udf_depth > 0 && s.body.size() == 2) {
        mir::Stmt effects = s;
        mir::Expr then_value, else_value;
        if (peel_terminal_return(&effects.body[0], &then_value) &&
            peel_terminal_return(&effects.body[1], &else_value)) {
          lower_stmt(effects);
          mir::Expr choice;
          choice.kind = mir::Expr::TernaryIf;
          choice.args = {s.cond, then_value, else_value};
          choice.type_ = then_value.type_;
          choice.unsized = then_value.unsized;
          choice.data_only =
              s.cond.data_only && then_value.data_only && else_value.data_only;
          choice.raw = s.raw;
          throw LpReturn{lower_expr(choice)};
        }
      }
      if (auto c = region_static_truth(s.cond)) {
        const size_t arm = *c ? 0 : 1;
        if (arm < s.body.size()) lower_stmt(s.body[arm]);
        return;
      }
      region_prepare_cells(s);
      Val condition = lower_expr(s.cond);
      region_flush();
      RegionNode branch;
      branch.kind = RegionNode::If;
      branch.condition = condition.slot;
      branch.children.resize(2);
      const auto base_scope = scope;
      const auto base_cells = region_cells;
      const auto base_ints = int_env;
      const auto base_locals = int_locals;
      const auto base_decls = decls;
      const auto base_env = td.env();
      const auto base_ranges = int_ranges;
      const auto base_real_ranges = real_ranges;
      const bool base_control = region_control_active;
      RegionNode* parent = region_current;
      std::map<int, IntRange> arms[2];
      std::map<int, RealRange> real_arms[2];
      std::map<std::string, Val> arm_scopes[2];
      for (size_t arm = 0; arm < 2; ++arm) {
        scope = base_scope;
        region_cells = base_cells;
        int_env = base_ints;
        int_locals = base_locals;
        decls = base_decls;
        td.env() = base_env;
        int_ranges = base_ranges;
        real_ranges = base_real_ranges;
        region_refine_condition(s.cond, arm == 0);
        region_control_active = base_control || !condition.si.param_free;
        region_current = &branch.children[arm];
        ++region_control_depth;
        if (arm < s.body.size()) lower_stmt(s.body[arm]);
        --region_control_depth;
        region_flush();
        arms[arm] = int_ranges;
        real_arms[arm] = real_ranges;
        arm_scopes[arm] = scope;
      }
      scope = base_scope;
      region_cells = base_cells;
      int_env = base_ints;
      int_locals = base_locals;
      decls = base_decls;
      td.env() = base_env;
      int_ranges = base_ranges;
      real_ranges = base_real_ranges;
      region_control_active = base_control;
      std::vector<std::string> writes;
      assigned_names(s, &writes);
      for (const auto& name : writes)
        if (auto v = scope.find(name); v != scope.end()) {
          const auto va = arm_scopes[0].find(name),
                     vb = arm_scopes[1].find(name);
          v->second.si.param_free =
              condition.si.param_free && va != arm_scopes[0].end() &&
              vb != arm_scopes[1].end() && va->second.si.param_free &&
              vb->second.si.param_free;
          v->second.autodiff =
              (va != arm_scopes[0].end() && va->second.autodiff) ||
              (vb != arm_scopes[1].end() && vb->second.autodiff);
          region_cells[name] = v->second;
          if (auto declaration = decls.find(name); declaration != decls.end())
            declaration->second.autodiff = v->second.autodiff;
          region_forget(name);
          auto a = arms[0].find(v->second.slot),
               b = arms[1].find(v->second.slot);
          if (a != arms[0].end() && b != arms[1].end())
            set_int_range(v->second, std::min(a->second.lo, b->second.lo),
                          std::max(a->second.hi, b->second.hi));
          else
            int_ranges.erase(v->second.slot);
          auto ra = real_arms[0].find(v->second.slot),
               rb = real_arms[1].find(v->second.slot);
          if (ra != real_arms[0].end() && rb != real_arms[1].end()) {
            RealRange merged{std::min(ra->second.lo, rb->second.lo),
                             std::max(ra->second.hi, rb->second.hi)};
            merged.positive_lo =
                std::min(ra->second.positive_lo, rb->second.positive_lo);
            real_ranges[v->second.slot] = merged;
          } else
            real_ranges.erase(v->second.slot);
        }
      region_current = parent;
      region_append(std::move(branch));
      return;
    }
    case mir::Stmt::Decl: {
      if (s.read_transform)
        fail("parameter declaration inside structured loop");
      region_cells.erase(s.decl_id);
      // Container sizes are data-only in Stan, but a retained loop can
      // compute them at execution time. Allocate a proved maximum and
      // carry the current logical extent in a scalar graph slot. This is
      // graph metadata only; StructuredLoop still owns one fixed arena.
      const bool one_dimensional_container =
          (s.decl_type.base == "SVector" || s.decl_type.base == "SRowVector" ||
           s.decl_type.base == "SArray") &&
          s.decl_type.dims.size() == 1;
      if (one_dimensional_container &&
          needs_runtime_value(s.decl_type.dims[0])) {
        if (s.has_init)
          fail("runtime-sized structured declaration initializer unsupported");
        const auto range = region_range(s.decl_type.dims[0]);
        if (!range || range->hi < 0)
          fail("runtime-sized structured declaration " + s.decl_id +
               " needs a finite capacity");
        const int64_t capacity = range->hi;
        const bool fixed_extent = range->lo == range->hi;
        Val extent;
        if (!fixed_extent) {
          extent = lower_expr(s.decl_type.dims[0]);
          if (!is_scalar(extent) || extent.autodiff)
            fail(
                "runtime-sized structured declaration needs a data integer "
                "extent");
          // A declaration evaluates its extent once.  Preserve that value
          // even if the source integer is assigned again later in the
          // iteration.
          Val snapshot{add_slot(1, false), false, view_of("UInt")};
          snapshot.si.param_free = extent.si.param_free;
          region_alias(snapshot.slot, extent.slot);
          if (auto r = int_ranges.find(extent.slot); r != int_ranges.end())
            set_int_range(snapshot, r->second.lo, r->second.hi);
          extent = snapshot;
        }
        SlotInfo si;
        if (s.decl_type.base == "SVector")
          si = view_of("UVector");
        else if (s.decl_type.base == "SRowVector")
          si = view_of("URowVector");
        else
          si = array_view({capacity}, leaf_kind(s.decl_type.elem_base));
        si.param_free = true;
        const bool int_array =
            s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt";
        Val value{
            add_slot(capacity, false), false, si,
            fixed_extent ? std::vector<int>{} : std::vector<int>{extent.slot}};
        const double initial =
            int_array ? static_cast<double>(std::numeric_limits<int>::min())
                      : std::numeric_limits<double>::quiet_NaN();
        out.fills.emplace_back(value.slot,
                               std::vector<double>(capacity, initial));
        if (int_array) set_uninitialized_int_array(value);
        scope[s.decl_id] = value;
        decls[s.decl_id] =
            DeclView{capacity, false, si, int_array, false, value.runtime_dims};
        region_forget(s.decl_id);
        (void)region_cell(s.decl_id);
        return;
      }
      if (s.decl_type.base == "SInt") {
        auto range =
            s.has_init ? region_range(s.init) : std::optional<IntRange>{};
        Val value = s.has_init ? lower_expr(s.init) : constant(INT32_MIN);
        const bool parameter_free = value.si.param_free;
        value.autodiff = false;
        value.si = view_of("UInt");
        value.si.param_free = parameter_free;
        scope[s.decl_id] = value;
        decls[s.decl_id] = DeclView{1, false, value.si};
        region_forget(s.decl_id);
        if (range) set_int_range(value, range->lo, range->hi);
        (void)region_cell(s.decl_id);
      } else {
        try {
          lower_stmt_impl(s);
        } catch (const CompileError& e) {
          fail("structured declaration " + s.decl_id + ": " + e.what());
        }
        if (!scope.count(s.decl_id)) {
          const auto declaration = decls.find(s.decl_id);
          if (declaration != decls.end() && declaration->second.deferred_shape)
            return;
          (void)uninitialized_decl_slot(s.decl_id);
        }
        (void)region_cell(s.decl_id);
      }
      return;
    }
    case mir::Stmt::Assignment: {
      if (!scope.count(s.lhs)) {
        const auto declaration = decls.find(s.lhs);
        if (declaration != decls.end() && declaration->second.deferred_shape &&
            s.lhs_idx.empty()) {
          lower_stmt_impl(s);
          (void)region_cell(s.lhs);
          return;
        }
      }
      (void)region_cell(s.lhs);
      auto range = region_range(s.rhs);
      auto real_range = region_real_range(s.rhs);
      Val rhs = lower_expr(s.rhs);
      if (!observation(rhs))
        if (auto observed = region_bounded_data_value(s.rhs))
          observe(rhs, std::move(*observed));
      if (!range) {
        const auto inferred = int_ranges.find(rhs.slot);
        if (inferred != int_ranges.end()) range = inferred->second;
      }
      if (!s.lhs_idx.empty()) {
        Val base = scope.at(s.lhs);
        rhs = region_index(base, s.lhs_idx, s.rhs.type_, s.rhs.unsized, &rhs);
        range = {};
        real_range = {};
      }
      region_assign(s.lhs, rhs, range, real_range);
      return;
    }
    case mir::Stmt::TargetPE: {
      Val t = lower_expr(s.target);
      if (g.slots[t.slot].len != 1) t = emit_value(OP_SUM_VEC, {t}, 1);
      RegionNode n;
      n.kind = RegionNode::Target;
      n.src = t.slot;
      region_append(std::move(n));
      return;
    }
    case mir::Stmt::Break:
    case mir::Stmt::Continue: {
      RegionNode n;
      n.kind =
          s.kind == mir::Stmt::Break ? RegionNode::Break : RegionNode::Continue;
      region_append(std::move(n));
      return;
    }
    case mir::Stmt::Return:
      if (region_control_depth)
        fail("structured early return needs local replay");
      lower_stmt_impl(s);
      return;
    case mir::Stmt::NRFunApp:
      if (s.fn_name == "FnWriteParam")
        fail("structured CSV emission unsupported");
      lower_stmt_impl(s);
      region_flush();
      return;
    case mir::Stmt::Skip:
      return;
    default:
      fail("unsupported structured statement");
  }
}
static int64_t region_setting(const char* name, int64_t fallback) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return fallback;
  char* end = nullptr;
  const long long value = std::strtoll(raw, &end, 10);
  return end && *end == 0 && value >= 0 ? static_cast<int64_t>(value)
                                        : fallback;
}
bool region_runtime_control(const mir::Stmt& root) const {
  std::set<std::string> visited;
  std::function<bool(const mir::Stmt&, unsigned)> statement;
  std::function<bool(const mir::Expr&, unsigned)> expression =
      [&](const mir::Expr& e, unsigned depth) {
        if (depth > 32) return false;
        if (e.kind == mir::Expr::TernaryIf && !e.args.empty() &&
            !e.args[0].data_only)
          return true;
        if ((e.kind == mir::Expr::EAnd || e.kind == mir::Expr::EOr) &&
            !e.data_only)
          return true;
        if (e.kind == mir::Expr::FunApp &&
            e.fn_lib == mir::Expr::Lib::UserDefined) {
          const auto found = fun_defs.find(e.name);
          if (found != fun_defs.end() && visited.insert(e.name).second)
            for (const auto& body : found->second->body)
              if (statement(body, depth + 1)) return true;
        }
        for (const auto& argument : e.args)
          if (expression(argument, depth + 1)) return true;
        return false;
      };
  statement = [&](const mir::Stmt& s, unsigned depth) {
    if (depth > 32) return false;
    if (s.kind == mir::Stmt::While) return true;
    if (s.kind == mir::Stmt::IfElse && !s.cond.data_only) return true;
    for (const mir::Expr* value :
         {&s.init, &s.rhs, &s.target, &s.lower, &s.upper, &s.cond})
      if (expression(*value, depth + 1)) return true;
    for (const auto& value : s.lhs_idx)
      if (expression(value, depth + 1)) return true;
    for (const auto& value : s.fn_args)
      if (expression(value, depth + 1)) return true;
    for (const auto& value : s.decl_type.dims)
      if (expression(value, depth + 1)) return true;
    for (const auto& child : s.body)
      if (statement(child, depth + 1)) return true;
    return false;
  };
  return statement(root, 0);
}

bool region_auto_profitable(
    const mir::Stmt& s,
    std::optional<std::pair<int64_t, int64_t>> exact_bounds) const {
  if (structured_outer_depth != 1) return false;
  if (s.kind == mir::Stmt::For) {
    if (!exact_bounds) return false;
    if (exact_bounds->second - exact_bounds->first + 1 < 32) return false;
  } else if (s.kind != mir::Stmt::While) {
    return false;
  }
  return region_runtime_control(s);
}
// Positive, deliberately narrow profitability evidence. Tiny scalar runs
// avoid region dispatch/version bookkeeping and expose the existing scalar
// fusion path, while duplicating only a bounded amount of compiler work.
// Merely fitting the expansion limit is NOT evidence for unrolling.
static bool region_scalar_expression(const mir::Expr& e) {
  if (e.type_ != "UReal" && e.type_ != "UInt") return false;
  switch (e.kind) {
    case mir::Expr::Var:
    case mir::Expr::LitInt:
    case mir::Expr::LitReal:
      return true;
    case mir::Expr::Promotion:
      return e.args.size() == 1 && region_scalar_expression(e.args[0]);
    case mir::Expr::FunApp: {
      if (e.fn_lib != mir::Expr::Lib::StanLib) return false;
      const std::string& f = e.name;
      if (f != "Plus__" && f != "Minus__" && f != "Times__" &&
          f != "Divide__" && f != "PMinus__" && f != "PPlus__" && f != "fma" &&
          f != "tanh" && f != "exp" && f != "log" && f != "square" &&
          f != "sqrt")
        return false;
      for (const auto& a : e.args)
        if (!region_scalar_expression(a)) return false;
      return true;
    }
    default:
      return false;
  }
}
static bool region_scalar_body(const mir::Stmt& s) {
  switch (s.kind) {
    case mir::Stmt::Block:
    case mir::Stmt::SList:
      for (const auto& c : s.body)
        if (!region_scalar_body(c)) return false;
      return true;
    case mir::Stmt::Decl:
      return !s.read_transform && s.decl_type.dims.empty() &&
             (s.decl_type.base == "SReal" || s.decl_type.base == "SInt") &&
             (!s.has_init || region_scalar_expression(s.init));
    case mir::Stmt::Assignment:
      return s.lhs_idx.empty() && region_scalar_expression(s.rhs);
    case mir::Stmt::TargetPE:
      return region_scalar_expression(s.target);
    case mir::Stmt::Skip:
      return true;
    default:
      return false;
  }
}
bool region_unroll_profitable(const mir::Stmt& s) {
  if (s.kind != mir::Stmt::For) return false;
  const auto lo = region_range(s.lower), hi = region_range(s.upper);
  if (!lo || !hi || lo->lo != lo->hi || hi->lo != hi->hi) return false;
  const int64_t count = std::max<int64_t>(0, int64_t(hi->hi) - lo->lo + 1);
  // Eliminating a zero/one-trip wrapper does not duplicate the body. Any
  // nested loops still make their own independent representation decision.
  if (count <= 1) return true;
  if (count > 4) return false;
  for (const auto& c : s.body)
    if (!region_scalar_body(c)) return false;
  const uint64_t limit = static_cast<uint64_t>(std::min<int64_t>(
      128, region_setting("STANLI_STRUCTURED_UNROLL_LIMIT", 65536)));
  return region_cost(s, limit + 1) <= limit;
}
uint64_t region_cost(const mir::Stmt& s, uint64_t limit, unsigned depth = 0) {
  if (depth > 32) return limit;
  uint64_t body = 1;
  const auto plus = [&](uint64_t a, uint64_t b) {
    return a >= limit - b ? limit : a + b;
  };
  std::function<uint64_t(const mir::Expr&)> expression =
      [&](const mir::Expr& e) {
        uint64_t cost = 1;
        if (e.kind == mir::Expr::Var) {
          auto value = scope.find(e.name);
          if (value != scope.end())
            cost = std::min<uint64_t>(
                limit, std::max<int64_t>(1, g.slots[value->second.slot].len));
        }
        if (e.kind == mir::Expr::FunApp &&
            e.fn_lib == mir::Expr::Lib::UserDefined) {
          auto f = fun_defs.find(e.name);
          if (f == fun_defs.end()) return limit;
          for (const auto& stmt : f->second->body)
            cost = plus(cost, region_cost(stmt, limit, depth + 1));
        }
        for (const auto& a : e.args) cost = plus(cost, expression(a));
        return cost;
      };
  body = plus(body, expression(s.rhs));
  body = plus(body, expression(s.target));
  body = plus(body, expression(s.init));
  body = plus(body, expression(s.cond));
  if (s.kind == mir::Stmt::Decl && !s.decl_type.dims.empty()) {
    uint64_t width = 1;
    for (const auto& dim : s.decl_type.dims) {
      const auto range = region_range(dim);
      if (!range || range->hi < 0) {
        width = limit;
        break;
      }
      const uint64_t extent = static_cast<uint64_t>(range->hi);
      width = extent && width > limit / extent ? limit : width * extent;
    }
    body = plus(body, std::min(width, limit));
  }
  for (const auto& c : s.body)
    body = plus(body, region_cost(c, limit, depth + 1));
  if (s.kind == mir::Stmt::While) return limit;
  if (s.kind == mir::Stmt::For) {
    auto lo = region_range(s.lower), hi = region_range(s.upper);
    if (!lo || !hi) return limit;
    const uint64_t n = static_cast<uint64_t>(
        std::max<int64_t>(0, int64_t(hi->hi) - lo->lo + 1));
    if (n && body > limit / n) return limit;
    body *= n;
  }
  return std::min(body, limit);
}

bool try_lower_region(
    const mir::Stmt& s,
    std::optional<std::pair<int64_t, int64_t>> exact_bounds = std::nullopt) {
  const StructuredMode mode = structured_policy;
  if (in_write_array || mode == StructuredMode::Off) return false;
  // Make the representation decision before constructing a trial graph.
  // In auto mode, incomplete cost information is evidence for nothing.
  if (mode == StructuredMode::Auto && !region_auto_profitable(s, exact_bounds))
    return false;
  const bool force = mode == StructuredMode::Force;
  const bool profitable_unroll = region_unroll_profitable(s);
  if (!force && profitable_unroll) return false;
  const auto refuse = [&](const std::string& reason) {
    if (std::getenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS"))
      std::fprintf(stderr, "stanli_structured refused: %s\n", reason.c_str());
    if (force)
      fail("compact loop lowering unavailable: " + reason +
           "; forced structured lowering cannot use the legacy path");
    return false;
  };
  if (target_scale != 1.0)
    return refuse("enclosing target scaling needs a compatible region");
  std::shared_ptr<StructuredLoop> plan;
  std::vector<int> parent_inputs;
  std::vector<std::string> names;
  std::vector<Val> results;
  std::shared_ptr<ShapeInterner> shapes;
  std::string refusal;
  std::string native_refusal;
  // Candidate construction is isolated and native-only. Unsupported bodies
  // abandon the trial and leave the parent graph unchanged.
  {
    bool candidate_ready = false;
    try {
      Lowering trial(data, prep, dumper, prep_graph,
                     std::make_shared<ShapeInterner>(*shape_pool));
      trial.fun_defs = fun_defs;
      trial.decls = decls;
      trial.td.env() = td.env();
      trial.int_env = int_env;
      trial.int_locals = int_locals;
      trial.propto_ctx = propto_ctx;
      trial.udf_depth = udf_depth;
      trial.udf_autodiff_ctx = udf_autodiff_ctx;
      trial.udf_formal_autodiff = udf_formal_autodiff;
      auto p = std::make_shared<StructuredLoop>();
      trial.region_current = &p->root;
      std::map<int, int> imported;
      std::map<int, bool> import_active, import_data_only;
      const auto import_slot = [&](int parent, bool active, bool data_only) {
        if (parent < 0) return;
        if (!imported.count(parent)) {
          imported[parent] = trial.add_slot(g.slots[parent].len, false);
          import_data_only[parent] = true;
        }
        import_active[parent] = import_active[parent] || active;
        import_data_only[parent] = import_data_only[parent] && data_only;
      };
      // Runtime geometry slots can be anonymous declaration snapshots, so
      // collect them explicitly rather than assuming every imported slot
      // has its own source-level name.
      for (const auto& item : scope) {
        import_slot(item.second.slot, item.second.autodiff,
                    item.second.si.param_free);
        for (int dim : item.second.runtime_dims) import_slot(dim, false, true);
      }
      for (const auto& in : imported) {
        if (auto range = int_ranges.find(in.first); range != int_ranges.end())
          trial.int_ranges[in.second] = range->second;
        if (auto range = real_ranges.find(in.first); range != real_ranges.end())
          trial.real_ranges[in.second] = range->second;
      }
      for (const auto& item : scope) {
        Val value = item.second;
        value.slot = imported.at(item.second.slot);
        for (int& dim : value.runtime_dims)
          if (dim >= 0) dim = imported.at(dim);
        trial.scope[item.first] = value;
        if (const auto* en = observation(item.second))
          trial.observe(value, *en);
        if (auto declaration = trial.decls.find(item.first);
            declaration != trial.decls.end())
          for (int& dim : declaration->second.runtime_dims)
            if (dim >= 0) dim = imported.at(dim);
      }
      std::vector<std::string> candidate_names;
      std::vector<Val> candidate_results;
      std::vector<int> candidate_inputs;
      std::vector<std::string> writes;
      assigned_names(s, &writes);
      std::set<std::string> locals;
      collect_loop_locals(s, &locals);
      for (const auto& name : writes)
        if (!locals.count(name) &&
            (scope.count(name) || int_env.count(name) || decls.count(name)))
          candidate_names.push_back(name);
      trial.region_prepare_cells(s);
      trial.lower_stmt(s);
      trial.region_flush();
      for (const auto& name : candidate_names) {
        auto found = trial.scope.find(name);
        if (found == trial.scope.end())
          trial.fail("missing structured live-out");
        candidate_results.push_back(found->second);
        p->outputs.push_back(found->second.slot);
      }
      // Runtime dimensions are graph values too. Export each distinct one
      // alongside the ordinary live-outs so a nested retained region cannot
      // lose its logical geometry at the OP_LOOP boundary.
      std::map<int, size_t> output_index;
      for (size_t i = 0; i < p->outputs.size(); ++i)
        output_index.emplace(p->outputs[i], i);
      for (const auto& value : candidate_results)
        for (int dim : value.runtime_dims)
          if (dim >= 0 && !output_index.count(dim)) {
            output_index[dim] = p->outputs.size();
            p->outputs.push_back(dim);
          }
      p->body = std::move(trial.g);
      p->fills = std::move(trial.out.fills);
      std::set<int> used;
      for (const auto& op : p->body.ops) {
        used.insert(op.out);
        if (op.out2 >= 0) used.insert(op.out2);
        for (int k = 0; k < op.n_in; ++k) used.insert(op.in[k]);
      }
      std::function<void(const RegionNode&)> visit = [&](const RegionNode& n) {
        for (int id : {n.src, n.dst, n.condition, n.iterator, n.lower, n.upper})
          if (id >= 0) used.insert(id);
        for (const auto& child : n.children) visit(child);
      };
      visit(p->root);
      for (int id : p->outputs) used.insert(id);
      for (const auto& in : imported) {
        if (!used.count(in.second)) {
          p->body.slots[in.second].len = 0;
          continue;
        }
        p->imports.push_back(
            {in.second, static_cast<int>(candidate_inputs.size()), 0,
             import_active[in.first], import_data_only[in.first]});
        candidate_inputs.push_back(in.first);
      }
      if (candidate_inputs.size() > 6) {
        const size_t packed = candidate_inputs.size() - 5;
        int64_t at = 0;
        for (size_t i = 0; i < packed; ++i) {
          p->imports[i].input = 0;
          p->imports[i].offset = at;
          at += g.slots[candidate_inputs[i]].len;
        }
        for (size_t i = packed; i < candidate_inputs.size(); ++i)
          p->imports[i].input = static_cast<int>(i - packed + 1);
      }
      if (std::any_of(p->body.ops.begin(), p->body.ops.end(),
                      [](const Op& op) { return op.opcode == OP_ISLAND; }))
        fail("structured body requires register-program fallback");
      if (mode == StructuredMode::Auto) {
        const auto retains_control = [&](const auto& self,
                                         const RegionNode& node) -> bool {
          if (node.kind == RegionNode::If || node.kind == RegionNode::While)
            return true;
          for (const auto& child : node.children)
            if (self(self, child)) return true;
          return false;
        };
        if (!retains_control(retains_control, p->root))
          fail("automatic candidate has no retained runtime control");
      }
      const auto has_target = [&](const auto& self,
                                  const RegionNode& node) -> bool {
        if (node.kind == RegionNode::Target) return true;
        for (const auto& child : node.children)
          if (self(self, child)) return true;
        return false;
      };
      p->has_target = has_target(has_target, p->root);
      p->prepare();
      candidate_ready = true;
      names = std::move(candidate_names);
      results = std::move(candidate_results);
      parent_inputs = std::move(candidate_inputs);
      shapes = std::move(trial.shape_pool);
      plan = std::move(p);
    } catch (const CompileError& e) {
      refusal = e.what();
    } catch (const std::logic_error& e) {
      refusal = e.what();
    } catch (const LpReturn&) {
      refusal = "unbound function return";
    } catch (const LoopBreak&) {
      refusal = "unbound break";
    } catch (const LoopContinue&) {
      refusal = "unbound continue";
    }
    if (!candidate_ready) native_refusal = refusal;
    if (!candidate_ready && std::getenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS"))
      std::fprintf(stderr, "stanli_structured native candidate (%s): %s\n",
                   s.loopvar.c_str(), refusal.c_str());
  }
  const bool diagnostics = std::getenv("STANLI_STRUCTURED_LOOP_DIAGNOSTICS") ||
                           std::getenv("STANLI_PROFILE_PREP");
  if (!plan) {
    return refuse("native: " + native_refusal);
  }
  // No proof/refusal follows publication. Allocation failure aborts this
  // whole compilation; it never falls through into a partially changed graph.
  *shape_pool = std::move(*shapes);
  std::vector<int> inputs = parent_inputs;
  if (inputs.size() > 6) {
    const size_t count = inputs.size() - 5;
    int packed = inputs[0];
    int64_t len = g.slots[packed].len;
    for (size_t i = 1; i < count; ++i) {
      len += g.slots[inputs[i]].len;
      packed = emit_raw(OP_CONCAT2, {packed, inputs[i]}, len, {}).slot;
    }
    inputs.erase(inputs.begin(),
                 inputs.begin() + static_cast<ptrdiff_t>(count));
    inputs.insert(inputs.begin(), packed);
  }
  int64_t output_size = plan->has_target ? 1 : 0;
  for (int id : plan->outputs) output_size += plan->body.slots[id].len;
  Val result = emit_raw(OP_LOOP, inputs, output_size, {});
  g.ops.back().udata = plan.get();
  g.udata_pool.push_back(plan);
  std::vector<int64_t> output_offsets(plan->outputs.size());
  int64_t total_output_size = 0;
  std::map<int, size_t> output_index;
  for (size_t i = 0; i < plan->outputs.size(); ++i) {
    output_offsets[i] = total_output_size;
    output_index.emplace(plan->outputs[i], i);
    total_output_size += plan->body.slots[plan->outputs[i]].len;
  }
  for (size_t i = 0; i < names.size(); ++i) {
    const int64_t len = plan->body.slots[plan->outputs[i]].len;
    Val value = emit_raw(
        len == 1 ? OP_INDEX : OP_SLICE, {result.slot}, len, results[i].si,
        {checked_immediate(output_offsets[i], "structured output offset")}, -1,
        results[i].autodiff);
    // Dependence belongs to each output, not to the packed OP_LOOP slot.
    // A data-only output is still a runtime value: do not publish a constant
    // observation or retain its incoming compile-time integer binding.
    value.si.param_free = results[i].si.param_free;
    value.runtime_dims = results[i].runtime_dims;
    for (int& dim : value.runtime_dims) {
      if (dim < 0) continue;
      const size_t index = output_index.at(dim);
      dim = emit_raw(OP_INDEX, {result.slot}, 1, view_of("UInt"),
                     {checked_immediate(output_offsets[index],
                                        "structured extent output offset")},
                     -1, false)
                .slot;
    }
    scope[names[i]] = value;
    region_forget(names[i]);
    auto dl = decls.find(names[i]);
    if (dl == decls.end())
      decls[names[i]] = DeclView{len,   value.autodiff, value.si,
                                 false, false,          value.runtime_dims};
    else
      dl->second.runtime_dims = value.runtime_dims;
  }
  if (plan->has_target) {
    bool active = false;
    for (const auto& in : plan->imports) active |= in.active;
    target_terms.push_back(
        emit_raw(OP_INDEX, {result.slot}, 1, {},
                 {checked_immediate(total_output_size, "target output offset")},
                 -1, active)
            .slot);
  }
  if (diagnostics)
    std::fprintf(stderr, "stanli_structured selected nodes=%zu kernels=%zu\n",
                 plan->node_count, plan->body.ops.size());
  return true;
}
