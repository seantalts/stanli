#include "lower_internal.hpp"

namespace stanli {
namespace lower_detail {

// Static C++ scalar type without lowering/evaluating the expression. This
// is intentionally small: ordinary ops propagate Val::autodiff from their
// lowered operands; only a data-condition ternary needs the unchosen arm.
bool Lowering::expression_autodiff(const mir::Expr& e) const {
  if (e.unsized.leaf == mir::UnsizedLeaf::Int || e.data_only) return false;
  if (e.promoted) return scalar_autodiff();
  if (e.kind == mir::Expr::Var) {
    const auto formal = udf_formal_autodiff.find(e.name);
    if (formal != udf_formal_autodiff.end()) return formal->second;
    const auto value = scope.find(e.name);
    return value != scope.end() ? value->second.autodiff : scalar_autodiff();
  }
  if (e.kind == mir::Expr::TernaryIf && e.args.size() == 3)
    return expression_autodiff(e.args[1]) || expression_autodiff(e.args[2]);
  bool autodiff = false;
  for (const mir::Expr& arg : e.args)
    autodiff = autodiff || expression_autodiff(arg);
  return autodiff;
}
// Every index the lowering sees is a bind-time constant, so what CmdStan
// bounds-checks at runtime is checked here instead.
std::vector<int64_t> Lowering::index_positions(const mir::Expr& ix,
                                               int64_t extent, const char* what,
                                               const std::string& raw) {
  std::vector<int64_t> out;
  if (ix.name == "IndexAll") {
    for (int64_t i = 0; i < extent; ++i) out.push_back(i);
    return out;
  }
  if (ix.name == "IndexSingle") {
    const int64_t i = eval_int(ix.args[0]);
    check_index(i, extent, what, raw);
    return {i - 1};
  }
  if (is_range(ix)) {
    const StaticRange range = *static_range(ix, extent);
    check_range(range.lo, range.hi, extent, what, raw);
    for (int64_t i = range.lo; i <= range.hi; ++i) out.push_back(i - 1);
    return out;
  }
  if (ix.name == "IndexMulti") {
    DataMap::Entry iv = eval_pure(ix.args[0], "an index list");
    if (!iv.is_int) fail(std::string(what) + " needs int data", raw);
    for (int i : iv.i) {
      check_index(i, extent, what, raw);
      out.push_back(i - 1);
    }
    return out;
  }
  if (ix.name == "IndexUpfrom") {
    const int64_t lo = eval_int(ix.args[0]);
    check_range(lo, extent, extent, what, raw);
    for (int64_t i = lo; i <= extent; ++i) out.push_back(i - 1);
    return out;
  }
  fail(std::string("unsupported ") + what + " " + ix.name, raw);
}

// Target models build int arrays in ascending contiguous writes.  Track the
// initialized prefix in O(1) per immutable slot: overwrites inside it are
// safe, an adjacent write extends it, and any gap/stride fails closed.  The
// interval hull may retain overwritten values, conservatively widening the
// later overflow proof.
void Lowering::propagate_int_update(const Val& out_v, const Val& base,
                                    const Val& rhs, int64_t start,
                                    int64_t stride) {
  // A write of an observed value into an observed base stays observed:
  // splice the element into a copy of the base's entry. Graph slots store
  // arrays outer-major while DataMap entries keep first-index-fast order,
  // so splicing graph offsets is only sound where the two flat orders
  // coincide: scalars, vectors, matrices (column-major on both sides), and
  // one-dimensional flat arrays. Deeper and Eigen-leaf arrays drop the
  // observation instead of recording cells under the wrong order; their
  // reads then evaluate against the graph value itself.
  const bool splice_orders_agree =
      !is_array(base.si) || (array_shape(base.si).dims.size() == 1 &&
                             array_shape(base.si).leaf == ViewKind::Flat);
  if (const DataMap::Entry* be =
          splice_orders_agree ? observation(base) : nullptr) {
    const DataMap::Entry* re = observation(rhs);
    const int64_t rl = g.slots[rhs.slot].len;
    if ((rl == 0 || re) && g.slots[out_v.slot].len == g.slots[base.slot].len) {
      DataMap::Entry en = *be;
      bool ok = true;
      for (int64_t k = 0; k < rl; ++k) {
        const int64_t at = start + k * stride;
        if (at < 0 || at >= (int64_t)en.r.size()) {
          ok = false;
          break;
        }
        const double v = k < (int64_t)re->r.size()
                             ? re->r[(size_t)k]
                             : static_cast<double>(re->i.at((size_t)k));
        en.r[(size_t)at] = v;
        if (!en.i.empty()) en.i[(size_t)at] = (int)v;
      }
      if (ok) observe(out_v, std::move(en));
    }
  }
  const auto base_prefix = int_initialized_prefix.find(base.slot);
  const auto rhs_prefix = int_initialized_prefix.find(rhs.slot);
  const int64_t rhs_len = g.slots[rhs.slot].len;
  if (rhs_len == 0 && base_prefix != int_initialized_prefix.end() &&
      g.slots[out_v.slot].len == g.slots[base.slot].len) {
    int_initialized_prefix[out_v.slot] = base_prefix->second;
    const auto base_range = int_ranges.find(base.slot);
    if (base_range == int_ranges.end())
      int_ranges.erase(out_v.slot);
    else
      int_ranges[out_v.slot] = base_range->second;
    return;
  }
  if (base_prefix == int_initialized_prefix.end() ||
      rhs_prefix == int_initialized_prefix.end() ||
      rhs_prefix->second != rhs_len || stride != 1 || start < 0 ||
      start > base_prefix->second || rhs_len < 0 ||
      start > g.slots[out_v.slot].len - rhs_len ||
      g.slots[out_v.slot].len != g.slots[base.slot].len) {
    int_ranges.erase(out_v.slot);
    int_initialized_prefix.erase(out_v.slot);
    return;
  }
  int_initialized_prefix[out_v.slot] =
      std::max(base_prefix->second, start + rhs_len);

  const auto rhs_range = int_ranges.find(rhs.slot);
  if (rhs_range == int_ranges.end()) {
    int_ranges.erase(out_v.slot);
    return;
  }
  IntRange range = rhs_range->second;
  if (base_prefix->second > 0) {
    const auto base_range = int_ranges.find(base.slot);
    if (base_range == int_ranges.end()) {
      int_ranges.erase(out_v.slot);
      return;
    }
    range.lo = std::min(range.lo, base_range->second.lo);
    range.hi = std::max(range.hi, base_range->second.hi);
  }
  int_ranges[out_v.slot] = range;
}
bool Lowering::scalar_shape_query(const mir::Expr& e) {
  if (e.name == "FnLength") return true;
  const BuiltinSpec* query =
      shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery);
  return query != nullptr && query->shape_query != BuiltinShapeQueryKind::Dims;
}

long Lowering::answer_shape_query(const mir::Expr& e, const SlotInfo& si,
                                  int64_t len) {
  const BuiltinSpec* query =
      shaped_builtin_spec(e.name == "FnLength" ? "size" : e.name, 1,
                          BuiltinShapePolicy::ShapeQuery);
  try {
    return (long)builtin_shape_query(
               *query, view_argument_shape(si, len, BuiltinArgumentKind::Real))
        .front();
  } catch (const std::invalid_argument& error) {
    fail(e.name + ": " + error.what(), e.raw);
  }
}

long Lowering::eval_int(const mir::Expr& e) {
  if (expr_effectful(e))
    fail("effectful expression cannot be used as a compile-time integer",
         e.raw);
  switch (e.kind) {
    case mir::Expr::LitInt:
      return e.lit_i;
    case mir::Expr::Var: {
      auto it = int_env.find(e.name);
      if (it != int_env.end()) return it->second;
      DataMap::Entry* en = td.find(e.name);
      if (en && en->is_int && en->i.size() == 1) return en->i[0];
      // A structured integer may live in a graph slot while its interval
      // proof has collapsed to one value (for example d = rows(mat)).  That
      // value is safe for fixed storage geometry even though it is computed
      // again when the retained body executes.
      if (region_current) {
        const auto value = scope.find(e.name);
        if (value != scope.end()) {
          const auto range = int_ranges.find(value->second.slot);
          if (range != int_ranges.end() && range->second.lo == range->second.hi)
            return range->second.lo;
        }
      }
      fail("size expression needs unknown int " + e.name);
    }
    case mir::Expr::Indexed: {
      // O1 can leave an empty Indexed wrapper around a fully composed
      // integer access, just as it does for real-valued expressions.
      if (e.args.size() == 1) return eval_int(e.args[0]);
      DataMap::Entry* en =
          e.args[0].kind == mir::Expr::Var ? td.find(e.args[0].name) : nullptr;
      if (en && en->is_int && e.args.size() == 2 &&
          e.args[1].name == "IndexSingle") {
        const long index = eval_int(e.args[1].args[0]);
        if (index < 1 || (size_t)index > en->i.size())
          fail("integer index " + std::to_string(index) +
                   " out of bounds for size " + std::to_string(en->i.size()),
               e.raw);
        return en->i[(size_t)index - 1];
      }
      if (en && en->is_int && e.args.size() == 3 &&
          e.args[1].name == "IndexSingle" && e.args[2].name == "IndexSingle" &&
          en->dims.size() == 2) {
        const long row = eval_int(e.args[1].args[0]);
        const long col = eval_int(e.args[2].args[0]);
        if (row < 1 || row > en->dims[0] || col < 1 || col > en->dims[1])
          fail("integer matrix index out of bounds", e.raw);
        return en->i[(size_t)((col - 1) * en->dims[0] + row - 1)];
      }
      // dims(x)[k] and friends: evaluate the base as a compile-time
      // sequence, then index it.
      {
        std::vector<int> vals = const_ints(e.args[0]);
        if (e.args.size() == 2 && e.args[1].name == "IndexSingle") {
          const long ix = eval_int(e.args[1].args[0]);
          if (ix >= 1 && (size_t)ix <= vals.size()) return vals[ix - 1];
        }
      }
      fail("unsupported int index expression", e.raw);
    }
    case mir::Expr::TernaryIf: {
      if (e.args.size() != 3)
        fail("malformed conditional size expression", e.raw);
      const bool condition = eval_int(e.args[0]) != 0;
      return eval_int(e.args[condition ? 1 : 2]);
    }
    case mir::Expr::EOr: {
      if (e.args.size() != 2) fail("malformed logical size expression", e.raw);
      return eval_int(e.args[0]) != 0 || eval_int(e.args[1]) != 0;
    }
    case mir::Expr::EAnd: {
      if (e.args.size() != 2) fail("malformed logical size expression", e.raw);
      return eval_int(e.args[0]) != 0 && eval_int(e.args[1]) != 0;
    }
    case mir::Expr::Promotion:
      fail("malformed promoted size expression", e.raw);
      return eval_int(e.args[0]);
    case mir::Expr::FunApp:
      if (const FunctionSpec* function = function_spec(e);
          function != nullptr && function->builtin() != nullptr &&
          function->builtin()->shape == BuiltinShapePolicy::Elementwise &&
          function->result() == FunctionArgumentKind::Integer) {
        const BuiltinSpec& spec = *function->builtin();
        if (spec.arity == 1)
          return evaluate_integer_unary_builtin(
              spec, static_cast<int>(eval_int(e.args[0])));
        if (spec.arity == 2)
          return evaluate_integer_binary_builtin(
              spec, static_cast<int>(eval_int(e.args[0])),
              static_cast<int>(eval_int(e.args[1])));
        fail(e.name + ": unsupported integer builtin arity", e.raw);
      }
      if (e.name == "sum" && e.args.size() == 1) {
        long acc = 0;
        for (int v : const_ints(e.args[0])) acc += v;
        return acc;
      }
      if (const BuiltinSpec* pred =
              e.args.size() == 2 ? shaped_builtin_spec(
                                       e.name, 2, BuiltinShapePolicy::Predicate)
                                 : nullptr) {
        const auto scalar = [&](const mir::Expr& arg) -> double {
          if (arg.type_ == "UInt") return (double)eval_int(arg);
          if (auto evaluated = try_eval_pure(arg)) {
            if (evaluated->r.size() == 1) return evaluated->r[0];
          }
          if (arg.kind == mir::Expr::Var) {
            const auto it = scope.find(arg.name);
            if (it != scope.end())
              if (const DataMap::Entry* en = observation(it->second))
                if (en->r.size() == 1) return en->r[0];
          }
          fail("comparison operand is not known data", arg.raw);
        };
        const double lhs = scalar(e.args[0]), rhs = scalar(e.args[1]);
        return evaluate_predicate_builtin(*pred, lhs, rhs);
      }
      // Shape queries on slot-bound values (e.g. rows(v) on an inlined
      // UDF's vector argument) answer from binding-owned metadata before
      // the interpreter, which cannot recover vector orientation.
      if (scalar_shape_query(e) && e.args.size() == 1 &&
          e.args[0].kind == mir::Expr::Var) {
        auto sit = scope.find(e.args[0].name);
        if (sit != scope.end())
          return answer_shape_query(e, sit->second.si,
                                    g.slots[sit->second.slot].len);
        auto dl = decls.find(e.args[0].name);
        if (dl != decls.end())
          return answer_shape_query(e, dl->second.si, dl->second.len);
        // A name td knows but neither scope nor decls does: the scalar
        // `int` input. bind_data fills both tables from a declared shape
        // and a scalar int has none, so it falls past both -- the one
        // case, not the none this used to claim. The data_only branch
        // below does not catch it either, because a shape query in a
        // real-valued context is not data_only: `real p = size(n)` in
        // transformed parameters is AutoDiffable, so it reached the
        // failure instead and cost the census stanc3's
        // function-signatures/math/matrix/size.stan.
        //
        // Asking the interpreter is what the previous copy here should
        // have done all along. It answers rows/cols off the MIR type, so
        // the rank-1 orientation bug that copy carried cannot come back
        // through this route.
        if (td.find(e.args[0].name)) {
          try {
            return td.as_int(e);
          } catch (const CompileError&) {
          }
        }
      }
      // Shape query on a COMPUTED value: --O1 inlining substitutes call
      // arguments into the callee's size expressions, so `rows(beta)`
      // arrives as `rows(segment(beta, pos[i], m[i]))`. Lower the
      // argument and answer from its slot metadata; any op this emits
      // is one the body was about to emit anyway.
      if (scalar_shape_query(e) && e.args.size() == 1 &&
          e.args[0].kind != mir::Expr::Var) {
        CallArguments actuals(*this, e);
        const Val v = actuals.at(0).value();
        return answer_shape_query(e, v.si, g.slots[v.slot].len);
      }
      // Anything else data-only the td interpreter can evaluate (sum of an
      // int array in a size expression, etc.).
      if (e.data_only) {
        try {
          return td.as_int(e);
        } catch (const CompileError&) {
        }
      }
      fail("unsupported int size function " + e.name, e.raw);
    default:
      fail("unsupported size expression", e.raw);
  }
}
int64_t Lowering::sized_len(const mir::SizedType& t, int64_t* rows,
                            int64_t* cols) {
  if (t.base == "SInt" || t.base == "SReal") return 1;
  if (t.base == "SVector" || t.base == "SRowVector") {
    const int64_t n = eval_int(t.dims[0]);
    if (n < 0) fail("negative vector extent", t.raw);
    return n;
  }
  if (t.base == "SMatrix") {
    const int64_t r = eval_int(t.dims[0]), c = eval_int(t.dims[1]);
    if (r < 0 || c < 0) fail("negative matrix extent", t.raw);
    if (rows) *rows = r;
    if (cols) *cols = c;
    return checked_product({r, c}, "matrix shape");
  }
  if (t.base == "SArray") {
    return checked_product(sized_dims(t), "array shape");
  }
  fail("unsupported sized type " + t.base, t.raw);
}
SlotInfo Lowering::view_of(const mir::SizedType& t, bool param_free) {
  SlotInfo si;
  si.param_free = param_free;
  if (t.base == "SVector")
    si.kind = ViewKind::Vector;
  else if (t.base == "SRowVector")
    si.kind = ViewKind::RowVector;
  else if (t.base == "SMatrix") {
    si.kind = ViewKind::Matrix;
    si.rows = eval_int(t.dims[0]);
    si.cols = eval_int(t.dims[1]);
  } else if (t.base == "SArray") {
    const std::vector<int64_t> dims = sized_dims(t);
    si.kind = ViewKind::Array;
    si.shape = shape_pool->intern(dims, leaf_kind(t.elem_base));
  }
  return si;
}
SlotInfo Lowering::indexed_view(const SlotInfo& base, size_t n_single,
                                int64_t out_len, const std::string& out_type) {
  SlotInfo si = view_of(out_type);
  si.param_free = base.param_free;
  if (!is_array(base)) return si;
  const ArrayShape& a = array_shape(base);
  const size_t outer = a.dims.size() - (size_t)leaf_rank(a.leaf);
  if (n_single < outer) {
    std::vector<int64_t> suffix(a.dims.begin() + n_single, a.dims.end());
    return array_view(std::move(suffix), a.leaf, base.param_free);
  }
  if (n_single == outer) {
    if (a.leaf == ViewKind::Matrix) {
      const size_t n = a.dims.size();
      return matrix_view(a.dims[n - 2], a.dims[n - 1], base.param_free);
    }
    si.kind = a.leaf;
    si.shape = 0;
    return si;
  }
  (void)out_len;
  return si;
}
bool Lowering::same_view(const SlotInfo& a, int64_t alen, const SlotInfo& b,
                         int64_t blen) const {
  if (a.kind != b.kind) return false;
  switch (a.kind) {
    case ViewKind::Flat:
      return a.shape == 0 && b.shape == 0 && alen == 1 && blen == 1;
    case ViewKind::Vector:
    case ViewKind::RowVector:
      return alen == blen;
    case ViewKind::Matrix:
      return a.rows == b.rows && a.cols == b.cols && a.rows * a.cols == alen &&
             b.rows * b.cols == blen;
    case ViewKind::Array:
      return a.shape != 0 && a.shape == b.shape && alen == blen;
  }
  return false;
}
ExpressionLayout Lowering::elementwise_layout(std::initializer_list<Val> inputs,
                                              bool packet_supported) const {
  if (inputs.size() == 0) return ExpressionLayout::unknown();
  bool all_scalar = true;
  bool all_known = true;
  bool all_packet_access = true;
  for (const Val& input : inputs) all_scalar = all_scalar && is_scalar(input);
  for (const Val& input : inputs) {
    if (is_scalar(input)) continue;
    all_known = all_known && input.layout.known();
    all_packet_access = all_packet_access && input.layout.packet_access();
  }
  return expression_layout::elementwise(all_scalar, packet_supported, all_known,
                                        all_packet_access);
}
// The active scalar type is an independent reason for scalar traversal:
// Matrix<var> has no packet reducer even when its source layout is direct.
// Otherwise the source layout describes the Eigen evaluator that Stan Math
// reduced before graph materialization.
Lowering::ReductionGrouping Lowering::reduction_grouping(const Val& value,
                                                         bool active) const {
  if (active) return ReductionGrouping::Scalar;
  switch (value.layout.kind) {
    case ExpressionLayout::Kind::Unknown:
      return ReductionGrouping::Unknown;
    case ExpressionLayout::Kind::Scalar:
      return ReductionGrouping::Scalar;
    case ExpressionLayout::Kind::Packet:
      return ReductionGrouping::Packet;
    case ExpressionLayout::Kind::Direct:
      return value.layout.element_offset == 0 ? ReductionGrouping::Packet
                                              : ReductionGrouping::Phased;
  }
  return ReductionGrouping::Unknown;
}
// Reducing a slot is valid only when its logical view spans exactly the
// container the MIR overload named. The layout controls grouping; these
// checks only prevent a partial or padded slot from being mistaken for a
// complete vector, matrix, or one-dimensional array.
bool Lowering::extrema_storage(mir::ExtremaSurface surface, const Val& value) {
  const int64_t len = g.slots[value.slot].len;
  switch (surface) {
    case mir::ExtremaSurface::RealVector:
      return is_vector(value.si) || is_row_vector(value.si);
    case mir::ExtremaSurface::RealMatrix:
      return is_matrix(value.si) &&
             checked_product({value.si.rows, value.si.cols},
                             "min/max matrix shape") == len;
    case mir::ExtremaSurface::RealArray:
    case mir::ExtremaSurface::IntArray: {
      if (!is_array(value.si)) return false;
      const ArrayShape& shape = array_shape(value.si);
      return shape.leaf == ViewKind::Flat && shape.dims.size() == 1 &&
             shape.dims[0] == len;
    }
    default:
      return false;
  }
}
IntRange Lowering::prove_runtime_int_extrema(const mir::Expr& e,
                                             const Val& value, int64_t len) {
  if (!in_write_array)
    fail("runtime integer min/max is supported only in generated quantities",
         e.raw);
  // Stan Math raises for an empty integer container; the forward graph
  // kernel cannot reproduce that exception at execution time.
  if (len == 0)
    fail("min/max over an empty int array stays on WaInterp", e.raw);
  if (value.si.param_free)
    fail("min/max needs a runtime-produced int array", e.raw);
  const auto initialized = int_initialized_prefix.find(value.slot);
  if (initialized == int_initialized_prefix.end() || initialized->second != len)
    fail("min/max int array is not definitely initialized", e.raw);
  const auto known = int_ranges.find(value.slot);
  if (known == int_ranges.end())
    fail("min/max int array has unproved integral slot values", e.raw);
  return known->second;
}
Lowering::Val Lowering::lower_extrema_reduction(const mir::Expr& e,
                                                CallArguments& actuals,
                                                const mir::ExtremaCall& call) {
  actuals.require_arity(1);
  Val value = actuals.at(0).value();
  const int64_t len = g.slots[value.slot].len;
  if (!extrema_storage(call.surface, value) || len < 0)
    fail("min/max argument is not the whole declared container", e.raw);

  const bool int_array = call.surface == mir::ExtremaSurface::IntArray;
  const bool active = value.autodiff && !in_write_array;
  const ReductionGrouping grouping =
      int_array ? ReductionGrouping::Scalar : reduction_grouping(value, active);
  if (grouping == ReductionGrouping::Unknown)
    fail("min/max expression grouping is not native", e.raw);
  const bool scalar = grouping == ReductionGrouping::Scalar;
  const bool phased = grouping == ReductionGrouping::Phased;
  const IntRange range =
      int_array ? prove_runtime_int_extrema(e, value, len) : IntRange{};
  Val result =
      with_layout(emit_value(OP_EXTREMA_VEC, {value}, 1, view_of(e.type_),
                             reduction_phase_idata(value, grouping, "min/max")),
                  ExpressionLayout::scalar());
  if (in_write_array || int_array) result.autodiff = false;
  // Bit 0 selects max. Bits 1 and 2 are an exclusive grouping selector:
  // scalar coefficient order and phased packet order respectively.
  g.ops.back().variant =
      static_cast<uint8_t>((call.kind == mir::ExtremaKind::Max ? 1u : 0u) |
                           (scalar ? 2u : 0u) | (phased ? 4u : 0u));
  if (int_array) {
    result.si.param_free = false;
    set_int_range(result, range.lo, range.hi);
  }
  return result;
}
Lowering::Val Lowering::lower_extrema_pair(const mir::Expr& e,
                                           CallArguments& actuals,
                                           mir::ExtremaKind kind) {
  actuals.require_arity(2);
  Val x = actuals.at(0).value();
  Val y = actuals.at(1).value();
  if (!is_scalar(x) || !is_scalar(y))
    fail("min/max scalar overload needs two scalar int arguments", e.raw);
  const bool maximum = kind == mir::ExtremaKind::Max;
  Val result = with_layout(
      emit_value(maximum ? OP_FMAX : OP_FMIN, {x, y}, 1, view_of("UInt")),
      ExpressionLayout::scalar());
  result.autodiff = false;
  result.si.param_free = false;
  const std::optional<IntRange> a = int_operand_range(e.args[0], x);
  const std::optional<IntRange> b = int_operand_range(e.args[1], y);
  if (a && b) {
    set_int_range(result,
                  maximum ? std::max(a->lo, b->lo) : std::min(a->lo, b->lo),
                  maximum ? std::max(a->hi, b->hi) : std::min(a->hi, b->hi));
  } else {
    set_int_initialized(result);
  }
  return result;
}
Lowering::LogicalDims Lowering::logical_dims(const SlotInfo& si, int64_t len,
                                             const std::string& what) {
  if (si.kind == ViewKind::Flat) {
    if (si.shape != 0 || len != 1) fail(what + ": malformed scalar view");
    return {1, 1};
  }
  if (is_vector(si)) return {len, 1};
  if (is_row_vector(si)) return {1, len};
  if (is_matrix(si)) {
    if (checked_product({si.rows, si.cols}, what) != len)
      fail(what + ": malformed matrix view");
    return {si.rows, si.cols};
  }
  fail(what + ": array values do not have one rows/cols view");
}
Lowering::Val Lowering::lower_dims(const mir::Expr& e, CallArguments& actuals) {
  const BuiltinSpec* query =
      shaped_builtin_spec("dims", 1, BuiltinShapePolicy::ShapeQuery);
  if (e.args.size() != 1 || query == nullptr) fail("dims arity", e.raw);
  const Val a = actuals.at(0).value();
  const std::vector<int64_t> dims =
      builtin_shape_query(*query, builtin_argument_shape(e.args[0], a));
  std::vector<double> vals(dims.begin(), dims.end());
  const int slot = add_slot((int64_t)vals.size(), false);
  out.fills.emplace_back(slot, vals);
  Val v{slot, false, array_view({(int64_t)dims.size()}, ViewKind::Flat, true)};
  DataMap::Entry en;
  en.is_int = true;
  en.r = std::move(vals);
  en.i.assign(dims.begin(), dims.end());
  observe(v, std::move(en));
  return v;
}
void Lowering::validate_view(const SlotInfo& si, int64_t len,
                             const std::string& what) {
  if (is_array(si) != (si.shape != 0))
    fail(what + ": array kind and shape id disagree");
  if (si.kind == ViewKind::Flat) {
    if (len != 1) fail(what + ": flat logical value is not a scalar");
    return;
  }
  if (is_matrix(si)) {
    if (checked_product({si.rows, si.cols}, what) != len)
      fail(what + ": matrix extents do not match storage length");
    return;
  }
  if (is_array(si)) {
    if (checked_product(array_shape(si).dims, what) != len)
      fail(what + ": array extents do not match storage length");
    return;
  }
}
// Lazily materialize an env value as a data slot when log_prob uses it.
int Lowering::env_slot(const std::string& name) {
  DataMap::Entry* en = td.find(name);
  // Empty entries are real: `array[0] real x_r` is how ODE models spell
  // "no data for the system", and it still has to become a (zero-length)
  // slot when passed around.
  if (!en) return -1;
  auto dl = decls.find(name);
  if (en->r.empty() && dl == decls.end()) return -1;
  SlotInfo si;
  si.param_free = true;
  if (dl != decls.end()) si = dl->second.si;
  si.param_free = true;
  const bool nested_matrix =
      is_array(si) && array_shape(si).leaf == ViewKind::Matrix;
  // DataMap is first-index-fast. Graph arrays are outer-major, with only
  // an innermost matrix kept column-major, so normalize at materialization.
  std::vector<double> vals = graph_order(*en, is_matrix(si), nested_matrix);
  validate_view(si, (int64_t)vals.size(), "data value " + name);
  const int s = add_slot((int64_t)vals.size(), false);
  out.fills.emplace_back(s, vals);
  Val v{s, false, si, owning_layout(si)};
  scope[name] = v;
  observe(v, *en);
  return s;
}
// Materialize a declared local that has not received its first value yet.
// Stan initializes real locals and containers to NaN (and integer arrays
// to INT_MIN).  Both ordinary expression lowering and a runtime region's
// live-in binder must see that same value: a name can be read inside a
// parameter-dependent branch without being assigned by the branch, so it
// will not appear in the region's live-out/assignment scan.
int Lowering::uninitialized_decl_slot(const std::string& name) {
  auto dl = decls.find(name);
  if (dl == decls.end()) return -1;
  if (dl->second.deferred_shape)
    fail("unsized local read before its first assignment: " + name);
  SlotInfo si = dl->second.si;
  si.param_free = true;
  Val value{add_slot(dl->second.len, false), dl->second.autodiff, si,
            owning_layout(si), dl->second.runtime_dims};
  const double initial =
      dl->second.int_array
          ? static_cast<double>(std::numeric_limits<int>::min())
          : std::numeric_limits<double>::quiet_NaN();
  out.fills.emplace_back(value.slot,
                         std::vector<double>(dl->second.len, initial));
  if (dl->second.int_array) set_uninitialized_int_array(value);
  observe_fill(value, dl->second.int_array, initial, dl->second.len);
  scope[name] = value;
  return value.slot;
}
// ---- expressions ----------------------------------------------------------
Lowering::Val Lowering::lower_expr(const mir::Expr& e) {
  std::optional<Val> structured;
  if (region_current)
    structured = region_expr(e);
  else if (runtime_int_expression(e))
    structured = lower_runtime_scalar(e);
  Val value = structured ? *structured : lower_expr_impl(e);
  if (e.promoted) {
    value.autodiff = expression_autodiff(e);
  } else if (e.kind == mir::Expr::Var) {
    const auto formal = udf_formal_autodiff.find(e.name);
    if (formal != udf_formal_autodiff.end()) value.autodiff = formal->second;
  } else if (e.kind == mir::Expr::TernaryIf && e.args.size() == 3) {
    // A known data condition chooses one implementation value, but C++ has
    // already promoted the expression. MIR records that promoted type, so
    // no arm may be evaluated merely to rediscover it.
    value.autodiff = expression_autodiff(e);
  }
  return value;
}
Lowering::Val Lowering::lower_expr_impl(const mir::Expr& e) {
  switch (e.kind) {
    case mir::Expr::Var: {
      auto it = scope.find(e.name);
      if (it == scope.end()) {
        auto ii = int_env.find(e.name);
        if (ii != int_env.end())
          return constant(static_cast<double>(ii->second));
        const int s = env_slot(e.name);
        if (s >= 0) return scope.at(e.name);
        // A declared local read before its first write: Materialize
        // the same uninitialized container the indexed-assignment path would.
        if (uninitialized_decl_slot(e.name) >= 0) return scope.at(e.name);
        fail("unknown variable " + e.name);
      }
      return it->second;
    }
    case mir::Expr::Indexed: {
      // O1 index composition can leave an empty outer Indexed node around
      // an already-indexed value. The outer node owns the final result
      // type: for M[idx, idx] passed to a UDF that reads x[i, j], the inner
      // single/single access still says UMatrix and this wrapper says UReal.
      // Collapse the wrapper and lower the composed access with that final
      // type instead of rejecting the stale intermediate matrix type.
      if (e.args.size() == 1 && e.args[0].kind == mir::Expr::Indexed) {
        mir::Expr composed = e.args[0];
        composed.type_ = e.type_;
        composed.unsized = e.unsized;
        composed.data_only = e.data_only;
        composed.promoted = e.promoted;
        composed.raw = e.raw;
        return lower_expr(composed);
      }
      // All-Single indices with compile-time values -> element read.
      Val base = lower_expr(e.args[0]);
      // O1 drops a full-span read's All indices, so `m[:, :]` arrives as an
      // Indexed node with none left.
      if (e.args.size() == 1) return base;
      if (e.args.size() == 2 && e.args[1].name == "IndexAll") return base;
      if (std::any_of(
              e.args.begin() + 1, e.args.end(),
              [&](const mir::Expr& ix) { return runtime_selector(ix); }))
        return region_index(base, {e.args.begin() + 1, e.args.end()}, e.type_,
                            e.unsized);
      if (in_write_array && e.args.size() == 2 &&
          e.args[1].name == "IndexSingle" &&
          runtime_int_value(e.args[1].args[0])) {
        const Val index = lower_expr(e.args[1].args[0]);
        if (!is_scalar(index)) fail("runtime index is not scalar", e.raw);
        int64_t count = 0, width = 0;
        if (is_array(base.si)) {
          const ArrayShape& shape = array_shape(base.si);
          const size_t outer =
              shape.dims.size() - (size_t)leaf_rank(shape.leaf);
          if (outer != 1 || shape.dims.empty() ||
              shape.leaf == ViewKind::Matrix)
            fail("runtime index needs one outer array dimension", e.raw);
          count = shape.dims.front();
          width = count == 0 ? 0 : g.slots[base.slot].len / count;
        } else if (is_vector(base.si) || is_row_vector(base.si)) {
          count = g.slots[base.slot].len;
          width = 1;
        } else {
          fail("runtime index needs a vector or flat outer array", e.raw);
        }
        if (count <= 0 || width <= 0 || g.slots[base.slot].len != count * width)
          fail("runtime index has an invalid base shape", e.raw);
        SlotInfo si = indexed_view(base.si, 1, width, e.type_);
        Val value =
            emit_value(OP_DYNAMIC_SLICE, {base, index}, width, si,
                       {checked_immediate(count, "runtime index extent")});
        value.si.param_free = false;
        value.layout = owning_layout(value.si);
        return value;
      }
      bool all_single = true;
      for (size_t k = 1; k < e.args.size(); ++k)
        if (e.args[k].name != "IndexSingle") all_single = false;
      const std::vector<int64_t>* bdims = nullptr;
      if (is_array(base.si)) bdims = &array_shape(base.si).dims;
      const size_t n_idx = e.args.size() - 1;
      // One index on a matrix selects rows. A range or gather is not a
      // contiguous slice in column-major storage, so spell its gather.
      if (e.args.size() == 2 && is_matrix(base.si) &&
          (is_range(e.args[1]) || e.args[1].name == "IndexMulti")) {
        std::vector<int> rows;
        if (auto range = static_range(e.args[1], base.si.rows)) {
          check_range(range->lo, range->hi, base.si.rows, "matrix row range",
                      e.raw);
          for (int64_t i = range->lo; i <= range->hi; ++i)
            rows.push_back((int)i - 1);
        } else {
          DataMap::Entry iv = eval_pure(e.args[1].args[0], "a gather index");
          if (!iv.is_int) fail("matrix row gather needs int data", e.raw);
          for (int i : iv.i) {
            if (i < 1 || i > base.si.rows)
              fail("matrix row gather out of bounds", e.raw);
            rows.push_back(i - 1);
          }
        }
        std::vector<int> gather;
        gather.reserve(rows.size() * (size_t)base.si.cols);
        for (int64_t j = 0; j < base.si.cols; ++j)
          for (int i : rows) gather.push_back((int)(j * base.si.rows) + i);
        SlotInfo si =
            matrix_view((int64_t)rows.size(), base.si.cols, base.si.param_free);
        return with_layout(
            emit_value(OP_GATHER, {base}, (int64_t)rows.size() * base.si.cols,
                       si, gather),
            ExpressionLayout::scalar());
      }
      // A range over the outermost array dimension is contiguous because
      // graph storage keeps each whole outer element together. Preserve
      // the complete suffix shape even when its storage width is zero.
      if (e.args.size() == 2 && is_array(base.si) && is_range(e.args[1])) {
        const ArrayShape& sh = array_shape(base.si);
        const StaticRange range = *static_range(e.args[1], sh.dims.front());
        const int64_t lo = range.lo;
        const int64_t hi = range.hi;
        // hi < lo is an empty slice whatever the endpoints (CmdStan's
        // rvalue checks bounds only when a range is nonempty), and the
        // bounds are data, so rejecting it would make compilation
        // data-dependent.
        if (hi >= lo && (lo < 1 || hi > sh.dims.front()))
          fail("array outer range out of bounds", e.raw);
        std::vector<int64_t> out_dims = sh.dims;
        out_dims[0] = hi >= lo ? hi - lo + 1 : 0;
        const std::vector<int64_t> suffix(sh.dims.begin() + 1, sh.dims.end());
        const int64_t width = checked_product(suffix, "array element");
        const int64_t len = checked_product(out_dims, "array range");
        const int64_t offset = hi >= lo ? (lo - 1) * width : 0;
        SlotInfo si = array_view(std::move(out_dims), sh.leaf);
        return with_layout(
            emit_value(OP_SLICE, {base}, len, si,
                       {checked_immediate(offset, "array range offset")}),
            owning_layout(si));
      }
      // Between subrange read on a 1-D value: v[a:b] is contiguous.
      // hi < lo is empty, not negative-length.
      if (e.args.size() == 2 && is_range(e.args[1])) {
        const StaticRange range =
            *static_range(e.args[1], g.slots[base.slot].len);
        const int64_t lo = range.lo;
        const int64_t hi = range.hi;
        check_range(lo, hi, g.slots[base.slot].len, "range", e.raw);
        const int64_t len = hi >= lo ? hi - lo + 1 : 0;
        const int64_t offset = len ? lo - 1 : 0;
        return with_layout(
            emit_value(OP_SLICE, {base}, len, view_of(e.type_),
                       {checked_immediate(offset, "range offset")}),
            contiguous_layout(base, offset, "range"));
      }
      // A data gather on a one-dimensional scalar array keeps an exact
      // one-dimensional Array view. More structural forms need strides
      // and therefore stay fail-loud rather than masquerading as vectors.
      if (e.args.size() == 2 && is_array(base.si) &&
          e.args[1].name == "IndexMulti") {
        const ArrayShape& sh = array_shape(base.si);
        if (sh.leaf != ViewKind::Flat || sh.dims.size() != 1)
          fail("unsupported index expression", e.raw);
        DataMap::Entry iv = eval_pure(e.args[1].args[0], "a gather index");
        if (!iv.is_int || iv.i.size() != iv.r.size())
          fail("gather index must be int data", e.raw);
        std::vector<int> idata;
        idata.reserve(iv.i.size());
        for (int x : iv.i) {
          if (x < 1 || x > sh.dims[0])
            fail("array gather out of bounds", e.raw);
          idata.push_back(x - 1);
        }
        SlotInfo si = array_view({(int64_t)idata.size()}, ViewKind::Flat);
        return with_layout(
            emit_value(OP_GATHER, {base}, (int64_t)idata.size(), si, idata),
            owning_layout(si));
      }
      // Gather by a data int array: v[idx].
      if (e.args.size() == 2 && e.args[1].name == "IndexMulti") {
        // An empty index is a legitimate data-dependent gather (a slice
        // whose computed length is zero); an int-flagged entry whose int
        // mirror disagrees with its values is not.
        DataMap::Entry iv = eval_pure(e.args[1].args[0], "a gather index");
        if (!iv.is_int || iv.i.size() != iv.r.size())
          fail("gather index must be int data", e.raw);
        std::vector<int> idata;
        idata.reserve(iv.i.size());
        for (int x : iv.i) {
          check_index(x, g.slots[base.slot].len, "gather index", e.raw);
          idata.push_back(x - 1);
        }
        return with_layout(emit_value(OP_GATHER, {base}, (int64_t)idata.size(),
                                      view_of(e.type_), idata),
                           ExpressionLayout::scalar());
      }
      // Matrix row/column slices use the explicit logical view; physical
      // storage remains column-major even when either extent is zero.
      if (e.args.size() == 3 && is_matrix(base.si) &&
          e.args[1].name == "IndexSingle" && e.args[2].name == "IndexAll") {
        const int64_t i = eval_int(e.args[1].args[0]);
        check_index(i, base.si.rows, "matrix row", e.raw);
        return with_layout(
            emit_value(OP_SLICE_STRIDED, {base}, base.si.cols, view_of(e.type_),
                       {checked_immediate(i - 1, "matrix row offset"),
                        checked_immediate(base.si.rows, "matrix row stride")}),
            ExpressionLayout::scalar());
      }
      if (e.args.size() == 3 && is_matrix(base.si) &&
          e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle") {
        const int64_t j = eval_int(e.args[2].args[0]);
        check_index(j, base.si.cols, "matrix column", e.raw);
        const int64_t offset = (j - 1) * base.si.rows;
        return with_layout(
            emit_value(OP_SLICE, {base}, base.si.rows, view_of(e.type_),
                       {checked_immediate(offset, "matrix column offset")}),
            contiguous_layout(base, offset, "matrix column"));
      }
      // Column of a canonical graph-order 2-D array (array[N, S] real):
      // each outer element is contiguous, so successive rows sit S apart.
      if (e.args.size() == 3 && is_array(base.si) && bdims &&
          array_shape(base.si).leaf == ViewKind::Flat && bdims->size() == 2 &&
          e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle") {
        const int64_t k = eval_int(e.args[2].args[0]) - 1;
        const int64_t N = (*bdims)[0], S = (*bdims)[1];
        if (k < 0 || k >= S) fail("array column out of bounds", e.raw);
        return with_layout(
            emit_value(OP_SLICE_STRIDED, {base}, N,
                       array_view({N}, ViewKind::Flat),
                       {checked_immediate(k, "array column offset"),
                        checked_immediate(S, "array column stride")}),
            ExpressionLayout::scalar());
      }
      // Row range of the same layout: A[i, lo:hi] is contiguous.
      if (e.args.size() == 3 && is_array(base.si) && bdims &&
          (array_shape(base.si).leaf == ViewKind::Flat ||
           array_shape(base.si).leaf == ViewKind::Vector ||
           array_shape(base.si).leaf == ViewKind::RowVector) &&
          bdims->size() == 2 && e.args[1].name == "IndexSingle" &&
          is_range(e.args[2])) {
        const int64_t i = eval_int(e.args[1].args[0]);
        const int64_t S = (*bdims)[1];
        const StaticRange range = *static_range(e.args[2], S);
        const int64_t lo = range.lo;
        const int64_t hi = range.hi;
        check_index(i, (*bdims)[0], "array index", e.raw);
        check_range(lo, hi, S, "array range", e.raw);
        const int64_t len = hi >= lo ? hi - lo + 1 : 0;
        SlotInfo si = array_shape(base.si).leaf == ViewKind::Flat
                          ? array_view({len}, ViewKind::Flat)
                          : view_of(e.type_);
        const int64_t offset = len ? (i - 1) * S + lo - 1 : 0;
        const ExpressionLayout layout =
            array_shape(base.si).leaf == ViewKind::Flat
                ? owning_layout(si)
                : ExpressionLayout::direct(len ? lo - 1 : 0);
        return with_layout(
            emit_value(OP_SLICE, {base}, len, si,
                       {checked_immediate(offset, "array row range offset")}),
            layout);
      }
      // A whole vector leaf selected from array[N] vector[S]. The explicit
      // trailing All survives O1 for this spelling and addresses the same
      // contiguous outer-element block as the range directly above.
      if (e.args.size() == 3 && is_array(base.si) && bdims &&
          (array_shape(base.si).leaf == ViewKind::Vector ||
           array_shape(base.si).leaf == ViewKind::RowVector) &&
          bdims->size() == 2 && e.args[1].name == "IndexSingle" &&
          e.args[2].name == "IndexAll") {
        const int64_t i = eval_int(e.args[1].args[0]);
        const int64_t count = (*bdims)[0], width = (*bdims)[1];
        check_index(i, count, "array index", e.raw);
        const int64_t offset = (i - 1) * width;
        SlotInfo si = view_of(e.type_);
        return with_layout(
            emit_value(OP_SLICE, {base}, width, si,
                       {checked_immediate(offset, "array vector offset")}),
            owning_layout(si));
      }
      // Row-range column read M[a:b, j] (contiguous within the column).
      if (e.args.size() == 3 && is_matrix(base.si) && is_range(e.args[1]) &&
          e.args[2].name == "IndexSingle") {
        const StaticRange range = *static_range(e.args[1], base.si.rows);
        const int64_t lo = range.lo;
        const int64_t hi = range.hi;
        const int64_t j = eval_int(e.args[2].args[0]);
        check_index(j, base.si.cols, "matrix column", e.raw);
        check_range(lo, hi, base.si.rows, "matrix row range", e.raw);
        const int64_t len = hi >= lo ? hi - lo + 1 : 0;
        const int64_t offset = len ? (j - 1) * base.si.rows + lo - 1 : 0;
        return with_layout(
            emit_value(OP_SLICE, {base}, len, view_of(e.type_),
                       {checked_immediate(offset, "matrix row range offset")}),
            contiguous_layout(base, offset, "matrix row range"));
      }
      // Any two-axis matrix selection the slices above leave is the
      // Cartesian selection M[rows, cols], not a pairwise zip. Preserve
      // index-array order and duplicates; column-major output means
      // selected columns are outer and selected rows are inner in the flat
      // gather list.
      const auto is_matrix_selector = [](const mir::Expr& index) {
        return index.name == "IndexAll" || index.name == "IndexSingle" ||
               is_range(index) || index.name == "IndexMulti";
      };
      if (e.args.size() == 3 && is_matrix(base.si) &&
          is_matrix_selector(e.args[1]) && is_matrix_selector(e.args[2]) &&
          (e.args[1].name != "IndexSingle" ||
           e.args[2].name != "IndexSingle")) {
        const std::vector<int64_t> rows = index_positions(
            e.args[1], base.si.rows, "matrix row gather", e.raw);
        const std::vector<int64_t> cols = index_positions(
            e.args[2], base.si.cols, "matrix column gather", e.raw);
        std::vector<int> gather;
        gather.reserve(rows.size() * cols.size());
        for (int64_t j : cols)
          for (int64_t i : rows)
            gather.push_back(checked_immediate(j * base.si.rows + i,
                                               "matrix gather offset"));
        SlotInfo si = view_of(e.type_);
        si.param_free = base.si.param_free;
        if (e.type_ == "UMatrix")
          si = matrix_view((int64_t)rows.size(), (int64_t)cols.size(),
                           base.si.param_free);
        return with_layout(
            emit_value(OP_GATHER, {base},
                       (int64_t)rows.size() * (int64_t)cols.size(), si, gather),
            ExpressionLayout::scalar());
      }
      // Params/locals with recorded dims, laid out by flat_addr above.
      // Matrix views are col-major and never take this array-major path.
      if (all_single && bdims && n_idx <= bdims->size() &&
          !is_matrix(base.si)) {
        const auto& D = *bdims;
        const bool mat = array_shape(base.si).leaf == ViewKind::Matrix;
        std::vector<int64_t> ix;
        for (size_t d = 0; d < n_idx; ++d) {
          const int64_t one = eval_int(e.args[1 + d].args[0]);
          check_index(one, D[d], "array index", e.raw);
          ix.push_back(one - 1);
        }
        const Addr a = flat_addr(D, mat, ix);
        if (a.stride != 1)
          return with_layout(
              emit_value(OP_SLICE_STRIDED, {base}, a.len,
                         indexed_view(base.si, n_idx, a.len, e.type_),
                         {checked_immediate(a.off, "indexed offset"),
                          checked_immediate(a.stride, "indexed stride")}),
              ExpressionLayout::scalar());
        if (a.len == 1)
          return with_layout(
              emit_value(OP_INDEX, {base}, 1,
                         indexed_view(base.si, n_idx, 1, e.type_),
                         {checked_immediate(a.off, "indexed offset")}),
              ExpressionLayout::scalar());
        // One whole matrix out of the array keeps its shape, so a later
        // index on it can take the column-major paths above.
        SlotInfo si = indexed_view(base.si, n_idx, a.len, e.type_);
        return with_layout(
            emit_value(OP_SLICE, {base}, a.len, si,
                       {checked_immediate(a.off, "indexed offset")}),
            owning_layout(si));
      }
      // A full array-index prefix pins one vector/row_vector leaf element;
      // exactly one trailing range/all index then reads inside that leaf.
      // The prefix is not all-single-index in stanc's own sense (the trailing
      // index is a range), so this falls outside the block above even
      // though every array position is fixed. Graph storage keeps the
      // pinned leaf contiguous, so this is one contiguous read from its
      // start once flat_addr locates it.
      if (bdims && (array_shape(base.si).leaf == ViewKind::Vector ||
                    array_shape(base.si).leaf == ViewKind::RowVector)) {
        const size_t n_arr = bdims->size() - 1;
        const mir::Expr& last = e.args.back();
        bool prefix_single = e.args.size() == n_arr + 2 &&
                             (is_range(last) || last.name == "IndexAll");
        for (size_t d = 0; prefix_single && d < n_arr; ++d)
          if (e.args[1 + d].name != "IndexSingle") prefix_single = false;
        if (prefix_single) {
          std::vector<int64_t> ix;
          ix.reserve(n_arr);
          for (size_t d = 0; d < n_arr; ++d) {
            const int64_t one = eval_int(e.args[1 + d].args[0]);
            check_index(one, (*bdims)[d], "array index", e.raw);
            ix.push_back(one - 1);
          }
          const Addr a = flat_addr(*bdims, false, ix);
          int64_t lo = 1, hi = a.len;
          const bool ranged = is_range(last);
          if (ranged) {
            const StaticRange range = *static_range(last, a.len);
            lo = range.lo;
            hi = range.hi;
            check_range(lo, hi, a.len, "array leaf range", e.raw);
          }
          const int64_t len = hi >= lo ? hi - lo + 1 : 0;
          SlotInfo si = view_of(e.type_);
          const int64_t offset = len ? a.off + lo - 1 : a.off;
          const ExpressionLayout layout =
              ranged ? ExpressionLayout::direct(len ? lo - 1 : 0)
                     : owning_layout(si);
          return with_layout(
              emit_value(
                  OP_SLICE, {base}, len, si,
                  {checked_immediate(offset, "array vector slice offset")}),
              layout);
        }
      }
      // A single outer-array range kept in full, with fixed row/column
      // indices into every element's matrix: array[N] matrix[R, C][:, i,
      // j]. Graph storage keeps each matrix contiguous and array-major, so
      // this is a strided read of one scalar out of every element.
      if (e.args.size() == 4 && is_array(base.si) && bdims &&
          bdims->size() == 3 && array_shape(base.si).leaf == ViewKind::Matrix &&
          e.args[1].name == "IndexAll" && e.args[2].name == "IndexSingle" &&
          e.args[3].name == "IndexSingle") {
        const int64_t N = (*bdims)[0], R = (*bdims)[1], C = (*bdims)[2];
        const int64_t ri = eval_int(e.args[2].args[0]);
        const int64_t cj = eval_int(e.args[3].args[0]);
        check_index(ri, R, "matrix row", e.raw);
        check_index(cj, C, "matrix column", e.raw);
        const int64_t off = (cj - 1) * R + (ri - 1);
        SlotInfo si = array_view({N}, ViewKind::Flat, base.si.param_free);
        return with_layout(
            emit_value(OP_SLICE_STRIDED, {base}, N, si,
                       {checked_immediate(off, "matrix array cell offset"),
                        checked_immediate(R * C, "matrix array cell stride")}),
            owning_layout(si));
      }
      // Row of a column-major data matrix / 2-D array: strided slice.
      if (all_single && e.args.size() == 2 && is_matrix(base.si) &&
          e.type_ != "UReal" && e.type_ != "UInt") {
        const int64_t t = eval_int(e.args[1].args[0]);
        check_index(t, base.si.rows, "matrix row", e.raw);
        return with_layout(
            emit_value(OP_SLICE_STRIDED, {base}, base.si.cols, view_of(e.type_),
                       {checked_immediate(t - 1, "matrix row offset"),
                        checked_immediate(base.si.rows, "matrix row stride")}),
            ExpressionLayout::scalar());
      }
      // Data-only slicing with no native path (e.g. one matrix out of a
      // data array of matrices) evaluates at compile time.
      const bool base_seen = e.args[0].kind != mir::Expr::Var ||
                             td.find(e.args[0].name) != nullptr;
      if (base_seen) {
        if (auto v = fold_const(e)) return *v;
      }
      int64_t flat = 0;
      if (all_single && e.args.size() == 2 &&
          (e.type_ == "UReal" || e.type_ == "UInt")) {
        const int64_t one = eval_int(e.args[1].args[0]);
        check_index(one, g.slots[base.slot].len, "element", e.raw);
        flat = one - 1;
      } else if (all_single && e.args.size() == 3 && is_matrix(base.si) &&
                 (e.type_ == "UReal" || e.type_ == "UInt")) {
        const int64_t ri = eval_int(e.args[1].args[0]);
        const int64_t cj = eval_int(e.args[2].args[0]);
        check_index(ri, base.si.rows, "matrix row", e.raw);
        check_index(cj, base.si.cols, "matrix column", e.raw);
        flat = (cj - 1) * base.si.rows + (ri - 1);
      } else {
        // Any remaining static selection resolves through the shared
        // index geometry over graph (outer-major) storage. This closes
        // what used to be the unsupported-index gap: mixed selections
        // over deep arrays, matrix-leaf arrays, and upfrom ranges. The
        // shape-specialized patterns above stay as fast paths carrying
        // their aliasing provenance; this generic path always owns its
        // result.
        const BuiltinArgumentShape shape =
            builtin_argument_shape(e.args[0], base);
        const size_t indexes = e.args.size() - 1;
        if (indexes > shape.dimensions.size())
          fail("unsupported index expression: too many indexes for " + e.type_,
               e.raw);
        std::vector<std::vector<int64_t>> selected;
        std::vector<bool> drops;
        selected.reserve(indexes);
        drops.reserve(indexes);
        for (size_t k = 0; k < indexes; ++k) {
          selected.push_back(index_positions(e.args[1 + k], shape.dimensions[k],
                                             "index", e.raw));
          drops.push_back(e.args[1 + k].name == "IndexSingle");
        }
        BuiltinIndexMap map;
        try {
          map = builtin_index_map(shape, selected, drops,
                                  SliceStorageOrder::OuterMajor);
        } catch (const std::invalid_argument& error) {
          fail(std::string("unsupported index expression: ") + error.what(),
               e.raw);
        }
        SlotInfo si;
        if (e.type_ == "UReal" || e.type_ == "UInt") {
          si = view_of(e.type_);
        } else if (e.type_ == "UVector" || e.type_ == "URowVector") {
          si = view_of(e.type_);
          si.param_free = base.si.param_free;
        } else if (e.type_ == "UMatrix") {
          if (map.dimensions.size() != 2)
            fail("unsupported index expression: matrix shape", e.raw);
          si = matrix_view(map.dimensions[0], map.dimensions[1],
                           base.si.param_free);
        } else {
          const ViewKind leaf =
              e.unsized.leaf == mir::UnsizedLeaf::Matrix   ? ViewKind::Matrix
              : e.unsized.leaf == mir::UnsizedLeaf::Vector ? ViewKind::Vector
              : e.unsized.leaf == mir::UnsizedLeaf::RowVector
                  ? ViewKind::RowVector
                  : ViewKind::Flat;
          si = array_view(map.dimensions, leaf, base.si.param_free);
        }
        switch (map.kind) {
          case BuiltinSliceMap::Kind::Contiguous:
            if (map.count == 1 && (e.type_ == "UReal" || e.type_ == "UInt"))
              return with_layout(
                  emit_value(OP_INDEX, {base}, 1, si,
                             {checked_immediate(map.offset, "index offset")}),
                  ExpressionLayout::scalar());
            return with_layout(
                emit_value(OP_SLICE, {base}, map.count, si,
                           {checked_immediate(map.offset, "index offset")}),
                owning_layout(si));
          case BuiltinSliceMap::Kind::Strided:
            return with_layout(
                emit_value(OP_SLICE_STRIDED, {base}, map.count, si,
                           {checked_immediate(map.offset, "index offset"),
                            checked_immediate(map.stride, "index stride")}),
                ExpressionLayout::scalar());
          default: {
            std::vector<int> gather;
            gather.reserve(map.gather.size());
            for (const int64_t cell : map.gather)
              gather.push_back(checked_immediate(cell, "index gather"));
            return with_layout(
                emit_value(OP_GATHER, {base}, map.count, si, gather),
                ExpressionLayout::scalar());
          }
        }
      }
      return with_layout(emit_value(OP_INDEX, {base}, 1, view_of(e.type_),
                                    {checked_immediate(flat, "index offset")}),
                         ExpressionLayout::scalar());
    }
    case mir::Expr::LitInt: {
      Val v = constant(static_cast<double>(e.lit_i));
      set_int_range(v, e.lit_i, e.lit_i);
      return v;
    }
    case mir::Expr::LitReal:
      return constant(e.lit);
    case mir::Expr::FunApp:
      return lower_funapp(e);
    case mir::Expr::TernaryIf: {
      if (expr_effectful(e.args[0]))
        fail("effectful expression cannot be a compile-time condition", e.raw);
      // Shape specialization and ordinary data evaluation can decide a
      // condition even when the complete expression's MIR adlevel is not
      // DataOnly (for example `rows(x) == 0 || theta > 0`).  Only the
      // genuinely unresolved case needs runtime control.
      if (auto condition = try_eval_pure(e.args[0]))
        return lower_expr(e.args[condition->r.at(0) != 0.0 ? 1 : 2]);
      return lower_runtime_ternary(e);
    }
    case mir::Expr::EOr:
    case mir::Expr::EAnd: {
      if (auto v = fold_const(e)) return *v;
      if (runtime_only(e)) return lower_runtime_ternary(e);
      fail("boolean operator on parameters unsupported", e.raw);
    }
    default: {
      if (auto v = fold_const(e)) return *v;
      fail("unsupported expression", e.raw.empty() ? e.name : e.raw);
    }
  }
}
// Low-level emission for dynamic slot lists and graph scaffolding whose
// output dependency is explicit at the call site.
Lowering::Val Lowering::emit_raw(uint16_t opcode, std::vector<int> ins,
                                 int64_t out_len, SlotInfo out_si,
                                 std::vector<int> idata, int out2,
                                 bool autodiff) {
  check_fixed_input_count(ins.size(), opcode);
  Op op;
  op.opcode = opcode;
  op.out2 = out2;
  op.n_in = 0;
  for (int s : ins) op.in[op.n_in++] = s;
  return finish_emit(op, out_len, out_si, std::move(idata), autodiff);
}
// The expression seam: a pure result is parameter-free exactly when all of
// its inputs are. initializer_list avoids a temporary input-list allocation
// and makes forgetting dependency propagation impossible.
Lowering::Val Lowering::emit_value(uint16_t opcode,
                                   std::initializer_list<Val> ins,
                                   int64_t out_len, SlotInfo out_si,
                                   std::vector<int> idata, int out2) {
  check_fixed_input_count(ins.size(), opcode);
  Op op;
  op.opcode = opcode;
  op.out2 = out2;
  op.n_in = 0;
  out_si.param_free = true;
  bool autodiff = false;
  for (const Val& in : ins) {
    op.in[op.n_in++] = in.slot;
    out_si.param_free = out_si.param_free && in.si.param_free;
    autodiff = autodiff || in.autodiff;
  }
  return finish_emit(op, out_len, out_si, std::move(idata), autodiff);
}
// Ask only the MIR interpreter.  Static-shape specialization below uses
// this for selector values and for path-sensitive short-circuit decisions;
// keeping it separate from try_eval_pure prevents recursive specialization.
std::optional<DataMap::Entry> Lowering::try_eval_interpreter(
    const mir::Expr& e) {
  if (expr_effectful(e)) return std::nullopt;
  if (region_current) {
    // A pure user function can still contain a huge loop. Do not execute
    // it as a speculative control/shape probe inside a retained body.
    std::function<bool(const mir::Expr&)> calls_user = [&](const mir::Expr& x) {
      if (x.kind == mir::Expr::FunApp &&
          x.fn_lib == mir::Expr::Lib::UserDefined)
        return true;
      for (const auto& arg : x.args)
        if (calls_user(arg)) return true;
      return false;
    };
    if (calls_user(e)) return std::nullopt;
  }
  try {
    return td.eval(e);
  } catch (const CompileError&) {
    return std::nullopt;
  } catch (const std::domain_error&) {
    return std::nullopt;
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  }
}
Lowering::StaticProbe<Lowering::StaticSelector> Lowering::try_static_selector(
    const mir::Expr& index, int64_t extent) {
  if (index.name == "IndexAll")
    return {StaticProbeState::Known, {extent, false}, {}};
  if (index.name == "IndexSingle" && index.args.size() == 1) {
    const auto at = try_static_int(index.args[0]);
    if (at.state == StaticProbeState::Invalid) return {at.state, {}, at.error};
    // A single index drops its dimension whichever element it selects.
    if (at.state != StaticProbeState::Known)
      return {StaticProbeState::Known, {1, true}, {}};
    if (at.value < 1 || at.value > extent)
      return {
          StaticProbeState::Invalid, {}, "static matrix index out of bounds"};
    return {StaticProbeState::Known, {1, true}, {}};
  }
  if (index.name == "IndexBetween" && index.args.size() == 2) {
    const auto lo = try_static_int(index.args[0]);
    if (lo.state != StaticProbeState::Known) return {lo.state, {}, lo.error};
    const auto hi = try_static_int(index.args[1]);
    if (hi.state != StaticProbeState::Known) return {hi.state, {}, hi.error};
    // Stan's range indexing treats hi < lo as empty and performs no bounds
    // check on either endpoint (the same rule check_range implements).
    if (hi.value < lo.value) return {StaticProbeState::Known, {0, false}, {}};
    if (lo.value < 1 || hi.value > extent)
      return {
          StaticProbeState::Invalid, {}, "static matrix range out of bounds"};
    return {StaticProbeState::Known, {hi.value - lo.value + 1, false}, {}};
  }
  if (index.name == "IndexUpfrom" && index.args.size() == 1) {
    const auto lo = try_static_int(index.args[0]);
    if (lo.state != StaticProbeState::Known) return {lo.state, {}, lo.error};
    if (extent < lo.value) return {StaticProbeState::Known, {0, false}, {}};
    if (lo.value < 1)
      return {
          StaticProbeState::Invalid, {}, "static matrix range out of bounds"};
    return {StaticProbeState::Known, {extent - lo.value + 1, false}, {}};
  }
  if (index.name == "IndexMulti" && index.args.size() == 1) {
    auto evaluated = try_eval_interpreter(index.args[0]);
    if (!evaluated) return {};
    if (!evaluated->is_int || evaluated->i.size() != evaluated->r.size())
      return {StaticProbeState::Invalid,
              {},
              "static matrix gather index is not integer data"};
    for (int at : evaluated->i)
      if (at < 1 || at > extent)
        return {StaticProbeState::Invalid,
                {},
                "static matrix gather index out of bounds"};
    return {StaticProbeState::Known,
            {static_cast<int64_t>(evaluated->i.size()), false},
            {}};
  }
  return {};
}
// The shape of whichever operand carries one, like the binaries.
Lowering::StaticProbe<Lowering::StaticView> Lowering::try_static_broadcast_view(
    const mir::Expr& e) {
  for (const mir::Expr& arg : e.args) {
    if (arg.type_ != e.type_) continue;
    const auto view = try_static_view(arg);
    if (view.state == StaticProbeState::Known) return view;
  }
  return {};
}
// Logical geometry only: this probe must never materialize a data value or
// emit a graph op.  Everything it does not recognize declines to the
// existing runtime-control path.
Lowering::StaticProbe<Lowering::StaticView> Lowering::try_static_view(
    const mir::Expr& e) {
  if (e.kind == mir::Expr::Var) {
    auto value = scope.find(e.name);
    if (value != scope.end())
      return {StaticProbeState::Known,
              {g.slots[value->second.slot].len, value->second.si},
              {}};
    auto declaration = decls.find(e.name);
    if (declaration != decls.end())
      return {StaticProbeState::Known,
              {declaration->second.len, declaration->second.si},
              {}};
    return {};
  }
  if (e.kind == mir::Expr::Promotion && e.args.size() == 1)
    return try_static_view(e.args[0]);
  if (e.kind == mir::Expr::FunApp) {
    if (e.name == "transpose" && e.args.size() == 1) {
      auto base = try_static_view(e.args[0]);
      if (base.state != StaticProbeState::Known) return base;
      std::swap(base.value.si.rows, base.value.si.cols);
      if (is_vector(base.value.si))
        base.value.si.kind = ViewKind::RowVector;
      else if (is_row_vector(base.value.si))
        base.value.si.kind = ViewKind::Vector;
      return base;
    }
    // Elementwise and WholeValue calls preserve their argument's geometry,
    // so a unary one answers with its argument's view and a binary one with
    // the broadcast of its arguments' views.
    const BuiltinSpec* elementwise = shaped_builtin_spec(
        e.name, e.args.size(), BuiltinShapePolicy::Elementwise);
    if (elementwise == nullptr && e.args.size() == 1)
      elementwise =
          shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::WholeValue);
    if (elementwise != nullptr && e.args.size() == 1)
      return try_static_view(e.args[0]);
    const bool scalar_factor =
        e.args.size() == 2 &&
        (is_scalar_type(e.args[0].type_) || is_scalar_type(e.args[1].type_));
    if (elementwise != nullptr || (e.name == "fma" && e.args.size() == 3) ||
        ((e.name == "Times__" || e.name == "multiply") && scalar_factor))
      return try_static_broadcast_view(e);
  }
  if (e.kind != mir::Expr::Indexed || e.args.size() < 2 || e.args.size() > 3)
    return {};
  const auto base = try_static_view(e.args[0]);
  if (base.state != StaticProbeState::Known)
    return {base.state, {}, base.error};
  if (!is_matrix(base.value.si)) return {};

  const auto rows = try_static_selector(e.args[1], base.value.si.rows);
  if (rows.state != StaticProbeState::Known)
    return {rows.state, {}, rows.error};
  StaticProbe<StaticSelector> cols{
      StaticProbeState::Known, {base.value.si.cols, false}, {}};
  if (e.args.size() == 3)
    cols = try_static_selector(e.args[2], base.value.si.cols);
  if (cols.state != StaticProbeState::Known)
    return {cols.state, {}, cols.error};

  const bool rd = rows.value.drops_dimension;
  const bool cd = cols.value.drops_dimension;
  StaticView out;
  out.len = checked_product({rows.value.count, cols.value.count},
                            "static matrix subview");
  out.si.param_free = base.value.si.param_free;
  if (!rd && !cd) {
    if (e.type_ != "UMatrix")
      return {StaticProbeState::Invalid,
              {},
              "static matrix subview has an inconsistent result type"};
    out.si = matrix_view(rows.value.count, cols.value.count,
                         base.value.si.param_free);
  } else if (rd && !cd) {
    if (e.type_ != "URowVector")
      return {StaticProbeState::Invalid,
              {},
              "static matrix row has an inconsistent result type"};
    out.si = view_of("URowVector");
    out.si.param_free = base.value.si.param_free;
  } else if (!rd && cd) {
    if (e.type_ != "UVector")
      return {StaticProbeState::Invalid,
              {},
              "static matrix column has an inconsistent result type"};
    out.si = view_of("UVector");
    out.si.param_free = base.value.si.param_free;
  } else {
    if (e.type_ != "UReal")
      return {StaticProbeState::Invalid,
              {},
              "static matrix element has an inconsistent result type"};
    out.si = view_of("UReal");
    out.si.param_free = base.value.si.param_free;
  }
  return {StaticProbeState::Known, out, {}};
}
Lowering::StaticProbe<int64_t> Lowering::try_static_shape_query(
    const mir::Expr& e) {
  if (!is_shape_query(e)) return {};
  const auto view = try_static_view(e.args[0]);
  if (view.state != StaticProbeState::Known) return {view.state, 0, view.error};
  const StaticView& v = view.value;
  if (is_array(v.si)) {
    const ArrayShape& shape = array_shape(v.si);
    if (e.name == "size" || e.name == "FnLength")
      return {StaticProbeState::Known, shape.dims.front(), {}};
    if (e.name == "num_elements") return {StaticProbeState::Known, v.len, {}};
    return {StaticProbeState::Invalid, 0,
            e.name + " is undefined for an array value"};
  }
  const LogicalDims dims = logical_dims(v.si, v.len, e.name);
  if (e.name == "rows") return {StaticProbeState::Known, dims.rows, {}};
  if (e.name == "cols") return {StaticProbeState::Known, dims.cols, {}};
  return {StaticProbeState::Known, v.len, {}};
}
// Replace only shape queries proven from immutable logical geometry.  The
// walk is lazy across Stan's short-circuit forms: an invalid subview in a
// dead RHS/arm must not become a bind-time error merely because this probe
// visited it.
bool Lowering::specialize_static_shapes(mir::Expr* e) {
  bool changed = false;
  if (e->kind == mir::Expr::EAnd || e->kind == mir::Expr::EOr) {
    if (e->args.size() != 2) return false;
    changed = specialize_static_shapes(&e->args[0]);
    auto lhs = try_eval_interpreter(e->args[0]);
    if (!lhs || lhs->r.size() != 1) return changed;
    const bool value = lhs->r[0] != 0.0;
    const bool reaches_rhs = e->kind == mir::Expr::EAnd ? value : !value;
    if (reaches_rhs) changed |= specialize_static_shapes(&e->args[1]);
    return changed;
  }
  if (e->kind == mir::Expr::TernaryIf) {
    if (e->args.size() != 3) return false;
    changed = specialize_static_shapes(&e->args[0]);
    auto condition = try_eval_interpreter(e->args[0]);
    if (!condition || condition->r.size() != 1) return changed;
    const size_t arm = condition->r[0] != 0.0 ? 1 : 2;
    changed |= specialize_static_shapes(&e->args[arm]);
    return changed;
  }
  if (is_shape_query(*e)) {
    const auto value = try_static_shape_query(*e);
    if (value.state == StaticProbeState::Invalid) fail(value.error, e->raw);
    if (value.state == StaticProbeState::Known) {
      if (value.value < std::numeric_limits<int>::min() ||
          value.value > std::numeric_limits<int>::max())
        fail("static shape query exceeds the Stan integer range", e->raw);
      mir::Expr literal;
      literal.kind = mir::Expr::LitInt;
      literal.lit_i = static_cast<long>(value.value);
      literal.type_ = "UInt";
      literal.unsized = {0, mir::UnsizedLeaf::Int};
      literal.data_only = true;
      literal.raw = e->raw;
      *e = std::move(literal);
      return true;
    }
  }
  for (mir::Expr& arg : e->args) changed |= specialize_static_shapes(&arg);
  return changed;
}
std::optional<Lowering::Val> Lowering::fold_const(const mir::Expr& e) {
  if (!e.data_only || e.fn_propto || expr_effectful(e)) return std::nullopt;
  auto evaluated = try_eval_pure(e);
  if (!evaluated) return std::nullopt;
  DataMap::Entry en = std::move(*evaluated);
  if (en.r.size() == 1 &&
      (e.type_ == "UReal" || e.type_ == "UInt" || e.type_ == "UComplex"))
    return constant(en.r[0]);
  SlotInfo si;
  si.param_free = true;
  if (e.unsized.depth != 0) {
    ViewKind leaf = ViewKind::Flat;
    if (e.unsized.leaf == mir::UnsizedLeaf::Vector)
      leaf = ViewKind::Vector;
    else if (e.unsized.leaf == mir::UnsizedLeaf::RowVector)
      leaf = ViewKind::RowVector;
    else if (e.unsized.leaf == mir::UnsizedLeaf::Matrix)
      leaf = ViewKind::Matrix;
    if (en.dims.empty()) en.dims = {(int64_t)en.r.size()};
    si = array_view(en.dims, leaf, true);
  } else {
    stamp_kind(&si, e.type_);
  }
  if (e.type_ == "UMatrix" && en.dims.size() == 2)
    si = matrix_view(en.dims[0], en.dims[1], true);
  const bool nested_matrix =
      e.unsized.depth != 0 && e.unsized.leaf == mir::UnsizedLeaf::Matrix;
  std::vector<double> vals =
      graph_order(en, e.type_ == "UMatrix", nested_matrix);
  const int s = add_slot((int64_t)vals.size(), false);
  out.fills.emplace_back(s, vals);
  Val v{s, false, si, owning_layout(si)};
  observe(v, std::move(en));
  return v;
}
// Matrix shape of an elementwise result: whichever operand carries one
// (both must agree when both do).
SlotInfo Lowering::shape_of(const Val& a, const Val& b) {
  const bool as = is_scalar(a), bs = is_scalar(b);
  const int64_t la = g.slots[a.slot].len, lb = g.slots[b.slot].len;
  if (!as && !bs && !same_view(a.si, la, b.si, lb))
    fail("elementwise op on different logical views");
  SlotInfo si = as && !bs ? b.si : a.si;
  // A pure op is parameter-free when both inputs are; this lets a
  // transformed data matrix still drive OP_MATVEC.
  si.param_free = a.si.param_free && b.si.param_free;
  return si;
}
// Two-argument scalar math with one int argument
// (STANLI_SCALAR_BINARY_INT_FIRST_LIST and its SECOND twin): elementwise
// with scalar broadcast like the all-real binaries, but shape_of does not
// apply. Those two sides may legitimately carry different views --
// `ldexp(matrix, array[,] int)` is a matrix, `falling_factorial(real,
// array[,] int)` is an array -- so the result takes the real side's view
// when it has one and the int side's when the real side is a scalar,
// which is what the signature list says in every case.
Lowering::Val Lowering::lower_binary_int(const BuiltinSpec& spec,
                                         CallArguments& actuals) {
  actuals.require_arity(2);
  const mir::Expr& e = actuals.call_expr();
  Val a = actuals.at(0).value();
  Val b = actuals.at(1).value();
  const std::vector<Val> values{a, b};
  const BuiltinLayout layout = resolved_builtin_layout(e, spec, values);
  SlotInfo si = values[layout.result_argument].si;
  si.param_free = a.si.param_free && b.si.param_free;
  std::vector<int> idata;
  if (layout.integer_matrix_rows != 0)
    idata = {(int)layout.integer_matrix_rows, (int)layout.integer_matrix_cols};
  return with_layout(
      emit_value(spec.opcode, {a, b}, layout.lanes, si, std::move(idata)),
      elementwise_layout({a, b}));
}
// Value of a data-only expression at compile time. The interpreter
// handles most cases; a UDF-local constant lives only as a slot, so fall
// back to that slot's recorded fill.
std::vector<double> Lowering::const_values(const mir::Expr& e) {
  if (expr_effectful(e))
    fail("effectful expression cannot be demanded at compile time", e.raw);
  if (auto evaluated = try_eval_pure(e)) {
    DataMap::Entry en = std::move(*evaluated);
    return en.r;
  }
  Val v = lower_expr(e);
  if (const DataMap::Entry* en = observation(v)) return en->r;
  // A zero-length slot carries no values by construction (`array[0] real`
  // is how ODE models spell "no data for the system").
  if (g.slots[v.slot].len == 0) return {};
  fail("value must be known at compile time: " +
           (e.kind == mir::Expr::Var ? e.name : ("<" + e.name + ">")),
       e.raw);
}
std::vector<int> Lowering::const_ints(const mir::Expr& e) {
  if (expr_effectful(e))
    fail("effectful expression cannot be demanded as compile-time integers",
         e.raw);
  if (auto evaluated = try_eval_pure(e)) {
    DataMap::Entry en = std::move(*evaluated);
    if (en.is_int) return en.i;
    std::vector<int> out;
    for (double d : en.r) out.push_back((int)d);
    return out;
  }
  std::vector<int> out;
  for (double d : const_values(e)) out.push_back((int)d);
  return out;
}
}  // namespace lower_detail
}  // namespace stanli
