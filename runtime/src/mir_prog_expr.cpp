// ProgramCompiler: expressions and conditions.
#include <stanli/mir_prog.hpp>

namespace stanli {

Range ProgramCompiler::expr(const mir::Expr& e) {
  // A complete, concretely evaluable pure UDF is already executable by the
  // surrounding MIR interpreter. Materialize its result once instead of
  // expanding its call tree into a finite register program. The successful
  // callback is the proof of concreteness: stanc can label the recursive
  // remainder AutoDiffable even after all its actuals became data literals.
  if (e.kind == mir::Expr::FunApp && e.fn_lib == mir::Expr::Lib::UserDefined &&
      mir::is_unsized(e, mir::UnsizedLeaf::Real)) {
    double value = 0.0;
    if (external_real(e, &value)) return {konst(value), 1};
  }
  switch (e.kind) {
    case mir::Expr::LitInt:
      return {konst((double)e.lit_i), 1};
    case mir::Expr::LitReal:
      return {konst(e.lit), 1};
    case mir::Expr::Var: {
      if (deferred_shapes.count(e.name))
        bail("unsized local read before its first assignment: " + e.name);
      auto it = reals.find(e.name);
      if (it != reals.end()) {
        require_assigned(e.name);
        return it->second;
      }
      auto known = known_int_arrays.find(e.name);
      if (known != known_int_arrays.end()) {
        auto dims = known_int_array_dims.find(e.name);
        const std::vector<int64_t> logical_dims =
            dims == known_int_array_dims.end()
                ? std::vector<int64_t>{(int64_t)known->second.size()}
                : dims->second;
        const std::vector<double> vals =
            int_array_graph_values(known->second, logical_dims);
        const int r = alloc((int)vals.size());
        emit_const(r, vals.data(), (int)vals.size());
        Range out{r, (int)vals.size()};
        out.kind = ViewKind::Array;
        out.dims = logical_dims;
        return out;
      }
      auto ii = ints.find(e.name);
      if (ii != ints.end()) {
        const std::vector<double> vals(ii->second.begin(), ii->second.end());
        const int r = alloc((int)vals.size());
        emit_const(r, vals.data(), (int)vals.size());
        Range out{r, (int)vals.size()};
        if (e.unsized.depth != 0) {
          out.kind = ViewKind::Array;
          out.dims = {(int64_t)vals.size()};
        }
        return out;
      }
      Range ext;
      if (bind_outer(e.name, &ext)) {
        reals[e.name] = ext;
        extern_bound.insert(e.name);
        require_assigned(e.name);
        return ext;
      }
      bail("unknown variable " + e.name);
    }
    case mir::Expr::Indexed: {
      // O1's index-composition pass may retain a base-only outer Indexed
      // node.  Its metadata is the final type while the inner node still
      // carries the pre-composition matrix type, so collapse it before any
      // logical-view decision.
      if (e.args.size() == 1 && e.args[0].kind == mir::Expr::Indexed) {
        mir::Expr composed = e.args[0];
        composed.unsized = e.unsized;
        composed.data_only = e.data_only;
        composed.promoted = e.promoted;
        composed.raw = e.raw;
        return expr(composed);
      }
      if (e.args.empty()) bail("index form");
      const Range b = expr(e.args[0]);
      // Index composition can erase every selector of a full span. The
      // remaining base-only node is the identity, as in MirInterp.
      if (e.args.size() == 1) return b;
      if (e.args.size() == 2 && e.args[1].kind == mir::Expr::IndexAll) return b;
      const std::vector<mir::Expr> selectors(e.args.begin() + 1, e.args.end());
      if ((b.kind == ViewKind::Matrix || b.kind == ViewKind::Array ||
           (e.args.size() == 2 && e.args[1].kind == mir::Expr::IndexMulti)) &&
          has_runtime_selector(selectors)) {
        // Preserve the existing single-op path for a final scalar array
        // index; other geometries use the shared checked kernel below.
        bool final_scalar =
            b.kind == ViewKind::Array && b.leaf == ViewKind::Flat &&
            selectors.size() == (b.dims.empty() ? 1 : b.dims.size());
        for (size_t k = 0; k < selectors.size() && final_scalar; ++k) {
          long fixed;
          final_scalar = selectors[k].kind == mir::Expr::IndexSingle &&
                         selectors[k].args.size() == 1 &&
                         (k + 1 == selectors.size() ||
                          try_cint(selectors[k].args[0], &fixed));
        }
        if (!final_scalar) return dynamic_index(b, selectors);
      }
      // General compile-time matrix selection. Registers are column-major,
      // so selected columns are outer and rows inner; this covers All,
      // Single, Between, and Multi in any pair while preserving selector
      // order and duplicate gather indices.
      if (b.kind == ViewKind::Matrix && e.args.size() == 3) {
        const std::vector<int64_t> rows =
            matrix_positions(e.args[1], b.rows, "row of " + e.args[0].name,
                             SelectorSite::MatrixRow, true);
        const std::vector<int64_t> cols =
            matrix_positions(e.args[2], b.cols, "column of " + e.args[0].name,
                             SelectorSite::MatrixColumn, true);
        if (!rows.empty() && cols.size() > (size_t)kMaxRegs / rows.size())
          bail("matrix selection needs too many registers");
        const size_t width = rows.size() * cols.size();
        if (width == 1 && !mir::is_unsized(e, mir::UnsizedLeaf::Matrix) &&
            !mir::is_unsized(e, mir::UnsizedLeaf::Vector) &&
            !mir::is_unsized(e, mir::UnsizedLeaf::RowVector))
          return {b.reg + (int)(cols[0] * b.rows + rows[0]), 1};
        const int r = alloc((int)width);
        int at = 0;
        for (int64_t j : cols)
          for (int64_t i : rows)
            emit(Program::MOV, r + at++, b.reg + (int)(j * b.rows + i));
        Range out{r, (int)width};
        if (mir::is_unsized(e, mir::UnsizedLeaf::Vector)) {
          out.kind = ViewKind::Vector;
        } else if (mir::is_unsized(e, mir::UnsizedLeaf::RowVector)) {
          out.kind = ViewKind::RowVector;
        } else {
          out.kind = ViewKind::Matrix;
          out.rows = (int64_t)rows.size();
          out.cols = (int64_t)cols.size();
        }
        return out;
      }
      // A matrix row, `m[i]` or `m[i, ]`. Ahead of the all-single check
      // below, which the explicit `IndexAll` would not pass.
      if (b.kind == ViewKind::Matrix && e.args.size() >= 2 &&
          e.args[1].kind == mir::Expr::IndexSingle &&
          (e.args.size() == 2 ||
           (e.args.size() == 3 && e.args[2].kind == mir::Expr::IndexAll)))
        return matrix_row(b, cint(e.args[1].args[0]));
      if ((b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
           b.kind == ViewKind::Flat) &&
          e.args.size() == 2 && e.args[1].kind == mir::Expr::IndexBetween)
        return one_dimensional_range(b, e);
      if ((b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
           b.kind == ViewKind::Flat) &&
          e.args.size() == 2 && e.args[1].kind == mir::Expr::IndexMulti) {
        const std::vector<long> indices = cints(e.args[1].args[0]);
        const int r = alloc((int)indices.size());
        for (size_t k = 0; k < indices.size(); ++k) {
          if (indices[k] < 1 || indices[k] > b.len)
            bail("gather index out of the declared range");
          emit(Program::MOV, r + (int)k, b.reg + (int)indices[k] - 1);
        }
        return typed(Range{r, (int)indices.size()}, e.unsized);
      }
      if (b.kind == ViewKind::Matrix) {
        if (e.args.size() == 3) {
          const long i = cint(e.args[1].args[0]);
          const long j = cint(e.args[2].args[0]);
          if (i < 1 || i > b.rows || j < 1 || j > b.cols)
            bail("matrix index out of the declared range");
          return {b.reg + (int)((j - 1) * b.rows + i - 1), 1};
        }
        // Any remaining one-index matrix selection (a row range,
        // gather, or upfrom) resolves through the shared index geometry
        // over the column-major registers.
        if (e.args.size() == 2) {
          const std::vector<int64_t> rows =
              matrix_positions(e.args[1], b.rows, "row of " + e.args[0].name,
                               SelectorSite::MatrixRow, true);
          const BuiltinIndexMap map =
              builtin_index_map({b.rows, b.cols}, 2, {rows},
                                {e.args[1].kind == mir::Expr::IndexSingle},
                                SliceStorageOrder::OuterMajor);
          if (map.count > kMaxRegs)
            bail("matrix selection needs too many registers");
          const int r = alloc((int)map.count);
          for (int64_t k = 0; k < map.count; ++k) {
            const int64_t cell = map.kind == BuiltinSliceMap::Kind::Contiguous
                                     ? map.offset + k
                                 : map.kind == BuiltinSliceMap::Kind::Strided
                                     ? map.offset + k * map.stride
                                     : map.gather[(size_t)k];
            emit(Program::MOV, r + (int)k, b.reg + (int)cell);
          }
          Range out{r, (int)map.count};
          if (mir::is_unsized(e, mir::UnsizedLeaf::Matrix) &&
              map.dimensions.size() == 2) {
            out.kind = ViewKind::Matrix;
            out.rows = map.dimensions[0];
            out.cols = map.dimensions[1];
          } else {
            out = typed(out, e.unsized);
          }
          return out;
        }
        bail("matrix index form");
      }

      if (b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
          b.kind == ViewKind::Flat) {
        if (e.args.size() != 2) bail("one-dimensional index form");
        if (e.args[1].kind == mir::Expr::IndexUpfrom)
          return one_dimensional_range(b, e);
        // Only a Single selects one element; any other index kind was
        // silently misread as one before this guard.
        if (e.args[1].kind != mir::Expr::IndexSingle)
          bail("one-dimensional index form " + e.args[1].name);
        long ix;
        if (try_cint(e.args[1].args[0], &ix)) {
          if (ix >= 1 && ix <= b.len) return {b.reg + (int)ix - 1, 1};
          if (b.len < 1) bail("index out of the declared range");
          Program::Message message;
          message.spec.chunks = {index_out_of_range_message(
              b.kind == ViewKind::Flat ? "array[uni, ...] index"
                                       : "vector[uni] indexing",
              b.len, ix)};
          message.spec.error = MessageError::OutOfRange;
          p.messages.push_back(std::move(message));
          emit(Program::REJECT, 0, (int)p.messages.size() - 1);
          return {b.reg, 1};
        }
        const Range iv = expr(e.args[1].args[0]);
        if (!is_scalar(iv)) bail("runtime index is not scalar");
        const int r = alloc(1);
        p.code.push_back(
            Program::Instr{Program::DYN_INDEX, r, b.reg, iv.reg, 0, b.len});
        return {r, 1};
      }

      if (b.kind != ViewKind::Array)
        bail("indexing this logical view is unsupported");
      const std::vector<int64_t> dims =
          b.dims.empty() ? std::vector<int64_t>{b.len} : b.dims;
      const size_t n_idx = e.args.size() - 1;
      if (n_idx > dims.size()) bail("too many array indices");

      // Register-file arrays are outer-major (graph order).  A selection
      // can therefore be strided even when it fixes a leading index, so
      // gather the complete result rather than pretending it is a
      // contiguous suffix.  This also covers array slices such as
      // `whichobs[row, 1:n]` and selections which continue into a
      // vector/matrix leaf.
      std::vector<std::vector<int64_t>> positions;
      std::vector<bool> drops;
      positions.reserve(dims.size());
      drops.reserve(dims.size());
      bool all_static = true;
      size_t runtime_dim = dims.size();
      for (size_t d = 0; d < n_idx; ++d) {
        const mir::Expr& index = e.args[d + 1];
        if (index.kind == mir::Expr::IndexSingle && index.args.size() == 1) {
          long ignored;
          if (!try_cint(index.args[0], &ignored)) {
            all_static = false;
            runtime_dim = d;
            break;
          }
        }
        positions.push_back(
            matrix_positions(index, dims[d], "array",
                             selector_site(d, dims.size() - leaf_rank(b.leaf),
                                           b.leaf == ViewKind::Matrix),
                             true));
        drops.push_back(index.kind == mir::Expr::IndexSingle);
      }
      if (!all_static) {
        if (b.leaf != ViewKind::Flat || n_idx != dims.size() ||
            runtime_dim + 1 != dims.size() ||
            e.args[runtime_dim + 1].kind != mir::Expr::IndexSingle ||
            e.args[runtime_dim + 1].args.size() != 1)
          bail("runtime array index must be the final scalar index");
        positions.push_back({0});
        const std::vector<int64_t> base_offsets =
            graph_array_offsets(dims, b.leaf, positions);
        if (base_offsets.size() != 1)
          bail("runtime array index has an ambiguous base");
        const Range iv = expr(e.args[runtime_dim + 1].args[0]);
        if (!is_scalar(iv)) bail("runtime array index is not scalar");
        const int r = alloc(1);
        p.code.push_back(Program::Instr{Program::DYN_INDEX, r, b.reg, iv.reg,
                                        (int32_t)base_offsets[0],
                                        (int32_t)dims[runtime_dim]});
        return {r, 1};
      }
      const size_t leaf_axes =
          b.leaf == ViewKind::Matrix                                    ? 2
          : b.leaf == ViewKind::Vector || b.leaf == ViewKind::RowVector ? 1
                                                                        : 0;
      BuiltinIndexMap map;
      try {
        // The shared index geometry over outer-major storage; trailing
        // axes keep their full extent inside the resolver.
        map = builtin_index_map(dims, leaf_axes, positions, drops,
                                SliceStorageOrder::OuterMajor);
      } catch (const std::invalid_argument& error) {
        bail("array selection: " + std::string(error.what()));
      }
      const int64_t width = map.count;
      if (width > kMaxRegs) bail("array selection needs too many registers");
      std::vector<int64_t> out_dims = map.dimensions;
      const int r = alloc((int)width);
      for (int64_t k = 0; k < width; ++k) {
        const int64_t cell = map.kind == BuiltinSliceMap::Kind::Contiguous
                                 ? map.offset + k
                             : map.kind == BuiltinSliceMap::Kind::Strided
                                 ? map.offset + k * map.stride
                                 : map.gather[(size_t)k];
        emit(Program::MOV, r + (int)k, b.reg + (int)cell);
      }
      if (width == 1 && (mir::language_scalar(e))) return {r, 1};
      Range out{r, (int)width};
      if (mir::is_unsized(e, mir::UnsizedLeaf::Vector)) {
        out.kind = ViewKind::Vector;
      } else if (mir::is_unsized(e, mir::UnsizedLeaf::RowVector)) {
        out.kind = ViewKind::RowVector;
      } else if (mir::is_unsized(e, mir::UnsizedLeaf::Matrix)) {
        if (out_dims.size() != 2) bail("matrix index form");
        out.kind = ViewKind::Matrix;
        out.rows = out_dims[0];
        out.cols = out_dims[1];
      } else {
        out.kind = ViewKind::Array;
        out.leaf = e.unsized.leaf == mir::UnsizedLeaf::Matrix ? ViewKind::Matrix
                   : e.unsized.leaf == mir::UnsizedLeaf::Vector
                       ? ViewKind::Vector
                   : e.unsized.leaf == mir::UnsizedLeaf::RowVector
                       ? ViewKind::RowVector
                       : ViewKind::Flat;
        out.dims = std::move(out_dims);
      }
      return out;
    }
    case mir::Expr::TernaryIf: {
      long c;
      if (try_cint(e.args[0], &c)) return expr(e.args[c != 0 ? 1 : 2]);
      return branchy_select(e.args[0], e.args[1], e.args[2]);
    }
    case mir::Expr::EOr:
    case mir::Expr::EAnd: {
      const Range a = expr(e.args[0]);
      if (!is_scalar(a)) bail("logical operator on a container");
      const int r = alloc(1);
      logical_value(r, a.reg, e.args[0]);

      int done = -1;
      if (e.kind == mir::Expr::EOr) {
        const int rhs = emit(Program::JZ, 0, r);
        done = emit(Program::JMP, 0);
        p.code[(size_t)rhs].dst = (int)p.code.size();
      } else {
        done = emit(Program::JZ, 0, r);
      }
      const Range b = expr(e.args[1]);
      if (!is_scalar(b)) bail("logical operator on a container");
      logical_value(r, b.reg, e.args[1]);
      p.code[(size_t)done].dst = (int)p.code.size();
      return {r, 1};
    }
    case mir::Expr::FunApp:
      return fun(e);
    default:
      bail("expression");
  }
}

bool ProgramCompiler::is_zero_or_one(const mir::Expr& e) {
  if (e.kind == mir::Expr::EAnd || e.kind == mir::Expr::EOr) return true;
  if (e.kind != mir::Expr::FunApp || e.args.empty() || e.args.size() > 2)
    return false;
  const BuiltinSpec* spec =
      shaped_builtin_spec(e.name, e.args.size(), BuiltinShapePolicy::Predicate);
  return spec != nullptr && spec->predicate != BuiltinPredicate::None;
}

void ProgramCompiler::logical_value(int dst, int src,
                                    const mir::Expr& operand) {
  if (is_zero_or_one(operand))
    emit(Program::MOV, dst, src);
  else
    emit(Program::NE, dst, src, konst(0.0));
}

void ProgramCompiler::cond_jump(const mir::Expr& e, bool jump_when,
                                std::vector<int>* targets) {
  long folded;
  if (try_cint(e, &folded)) {
    if ((folded != 0) == jump_when) targets->push_back(emit(Program::JMP, 0));
    return;
  }
  if ((e.kind == mir::Expr::EAnd || e.kind == mir::Expr::EOr) &&
      e.args.size() == 2) {
    if ((e.kind == mir::Expr::EAnd) != jump_when) {
      cond_jump(e.args[0], jump_when, targets);
      cond_jump(e.args[1], jump_when, targets);
    } else {
      std::vector<int> decided;
      cond_jump(e.args[0], !jump_when, &decided);
      cond_jump(e.args[1], jump_when, targets);
      patch_jumps(decided);
    }
    return;
  }
  if (is_logical_not(e)) {
    cond_jump(e.args[0], !jump_when, targets);
    return;
  }
  const Range v = expr(e);
  if (!is_scalar(v)) bail("branch on a container");
  const int test = emit(Program::JZ, 0, v.reg);
  if (!jump_when) {
    targets->push_back(test);
    return;
  }
  targets->push_back(emit(Program::JMP, 0));
  p.code[(size_t)test].dst = (int)p.code.size();
}

Range ProgramCompiler::branchy_select(const mir::Expr& c, const mir::Expr& a,
                                      const mir::Expr& b) {
  std::vector<int> to_else;
  cond_jump(c, false, &to_else);
  // Compile the arms first to learn the width, then re-emit into place.
  const Range av = expr(a);
  const int dst = alloc(av.len);
  for (int k = 0; k < av.len; ++k) emit(Program::MOV, dst + k, av.reg + k);
  const int jmp = emit(Program::JMP, 0);
  patch_jumps(to_else);
  const Range bv = expr(b);
  if (bv.len != av.len) bail("conditional arms of different widths");
  if (!same_view(av, bv)) bail("conditional arms of different logical views");
  for (int k = 0; k < bv.len; ++k) emit(Program::MOV, dst + k, bv.reg + k);
  p.code[(size_t)jmp].dst = (int)p.code.size();
  Range out = av;
  out.reg = dst;
  return out;
}

}  // namespace stanli
