// ProgramCompiler: register views and indexing.
#include <stanli/mir_prog.hpp>

namespace stanli {

bool ProgramCompiler::static_view(const mir::Expr& e, Range* out) {
  if (e.data_only && e.unsized.depth != 0 &&
      e.unsized.leaf == mir::UnsizedLeaf::Int && extern_ints) {
    std::vector<long> values;
    std::vector<int64_t> dims;
    if (external_int_array(e, &values, &dims)) {
      Range r{0, (int)values.size()};
      r.kind = ViewKind::Array;
      r.dims = std::move(dims);
      *out = std::move(r);
      return true;
    }
  }
  // A literal array, which stanc's inliner substitutes for an argument:
  // `size(when)` inside an inlined function arrives as `size({0})`.
  if (e.kind == mir::Expr::FunApp &&
      (e.name == "FnMakeArray" || e.name == "FnMakeRowVec")) {
    int64_t len = 0;
    for (const auto& a : e.args) {
      Range part;
      if (!static_view(a, &part)) return false;
      len += part.len;
    }
    Range r{0, (int)len};
    if (e.name == "FnMakeRowVec") {
      r.kind = ViewKind::RowVector;
    } else {
      r.kind = ViewKind::Array;
      r.dims = {(int64_t)e.args.size()};
    }
    *out = r;
    return true;
  }
  if (e.kind == mir::Expr::LitInt || e.kind == mir::Expr::LitReal) {
    *out = Range{0, 1};
    return true;
  }
  // A registered shape-policy call: the shared resolvers report result
  // geometry as a pure function of operand shapes, so the extent of, say,
  // `cols(to_matrix(v, 6, 1))` -- the form stanc's inliner leaves behind
  // for a matrix argument -- is known without building the value. A
  // resolver rejection is not swallowed semantically: the value
  // expression raises it wherever it is really evaluated, while a
  // shape-only use keeps the caller's ordinary diagnostics.
  if (e.kind == mir::Expr::FunApp) {
    if (const BuiltinSpec* slice = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::SliceView)) {
      Range a;
      if (!static_view(e.args[0], &a)) return false;
      const bool append = builtin_slice_is_append(slice->slice);
      Range b;
      if (append && !static_view(e.args[1], &b)) return false;
      std::vector<int64_t> indexes;
      indexes.reserve(e.args.size() - 1);
      for (size_t k = 1; !append && k < e.args.size(); ++k) {
        long index = 0;
        if (!try_cint(e.args[k], &index)) return false;
        indexes.push_back(index);
      }
      try {
        const BuiltinSliceMap map =
            append ? builtin_append_map(*slice,
                                        builtin_argument_shape(e.args[0], a),
                                        builtin_argument_shape(e.args[1], b),
                                        SliceStorageOrder::OuterMajor)
                   : builtin_slice_map(*slice,
                                       builtin_argument_shape(e.args[0], a),
                                       indexes, SliceStorageOrder::OuterMajor);
        *out = shaped(Range{0, (int)map.count}, map.result);
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    if (const BuiltinSpec* grouped = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::GroupedReduction)) {
      Range a;
      if (!static_view(e.args[0], &a)) return false;
      Range b = a;
      if (grouped->arity == 2 && !static_view(e.args[1], &b)) return false;
      const mir::Expr& rhs = e.args[grouped->arity == 2 ? 1 : 0];
      try {
        const BuiltinGroupedDotMap map = builtin_grouped_dot_map(
            *grouped, builtin_argument_shape(e.args[0], a),
            builtin_argument_shape(rhs, b));
        *out = shaped(Range{0, (int)map.groups}, map.result);
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    if (const BuiltinSpec* matrix = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::MatrixOp)) {
      std::vector<BuiltinArgumentShape> shapes;
      shapes.reserve(matrix->arity);
      for (size_t k = 0; k < e.args.size(); ++k) {
        Range operand;
        if (!static_view(e.args[k], &operand)) return false;
        shapes.push_back(builtin_argument_shape(e.args[k], operand));
      }
      try {
        const BuiltinMatrixMap map = builtin_matrix_map(*matrix, shapes);
        *out = shaped(Range{0, (int)map.result.storage_size}, map.result);
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    if (const BuiltinSpec* ctor = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::Constructor)) {
      std::vector<double> arguments;
      arguments.reserve(e.args.size());
      for (size_t k = 0; k < e.args.size(); ++k) {
        double value = 0.0;
        if (ctor->arguments[k] == BuiltinArgumentKind::Integer) {
          long argument = 0;
          if (!try_cint(e.args[k], &argument)) return false;
          value = (double)argument;
        } else if (!try_creal(e.args[k], &value)) {
          return false;
        }
        arguments.push_back(value);
      }
      try {
        const ConstructorValue built =
            evaluate_constructor_builtin(*ctor, arguments);
        Range r{0, (int)built.values.size()};
        r.kind = function_view_kind(ctor->constructor_container);
        if (r.kind == ViewKind::Matrix) {
          r.rows = built.dimensions[0];
          r.cols = built.dimensions[1];
        } else if (r.kind == ViewKind::Array) {
          r.dims = built.dimensions;
        }
        *out = r;
        return true;
      } catch (const std::exception&) {
        return false;
      }
    }
    return false;
  }
  if (e.kind == mir::Expr::Indexed) return static_indexed_view(e, out);
  if (e.kind != mir::Expr::Var) return false;
  if (deferred_shapes.count(e.name)) return false;
  auto rt = reals.find(e.name);
  if (rt != reals.end()) {
    require_assigned(e.name);
    *out = rt->second;
    return true;
  }
  auto known = known_int_arrays.find(e.name);
  if (known != known_int_arrays.end()) {
    Range r{0, (int)known->second.size()};
    r.kind = ViewKind::Array;
    auto dims = known_int_array_dims.find(e.name);
    r.dims = dims == known_int_array_dims.end() ? std::vector<int64_t>{r.len}
                                                : dims->second;
    *out = r;
    return true;
  }
  auto ii = ints.find(e.name);
  if (ii != ints.end()) {
    Range r{0, (int)ii->second.size()};
    if (e.unsized.depth != 0) {
      r.kind = ViewKind::Array;
      r.dims = {(int64_t)ii->second.size()};
    }
    *out = r;
    return true;
  }
  // An outside name: bind it the way expr() would. The binding is one
  // live-in whether it is the shape that is wanted or the value.
  Range ext;
  if (bind_outer(e.name, &ext)) {
    reals[e.name] = ext;
    extern_bound.insert(e.name);
    require_assigned(e.name);
    *out = ext;
    return true;
  }
  return false;
}

bool ProgramCompiler::static_indexed_view(const mir::Expr& e, Range* out) {
  if (e.args.size() == 1 && e.args[0].kind == mir::Expr::Indexed) {
    mir::Expr composed = e.args[0];
    composed.unsized = e.unsized;
    composed.data_only = e.data_only;
    composed.promoted = e.promoted;
    composed.raw = e.raw;
    return static_view(composed, out);
  }
  if (e.args.empty()) return false;
  Range b;
  if (!static_view(e.args[0], &b)) return false;
  if (e.args.size() == 2 && e.args[1].kind == mir::Expr::IndexAll) {
    *out = b;
    return true;
  }
  try {
    if (b.kind == ViewKind::Array) {
      const auto dims = b.dims.empty() ? std::vector<int64_t>{b.len} : b.dims;
      const size_t leaf_axes = leaf_rank(b.leaf);
      const size_t selected = e.args.size() - 1;
      if (dims.size() < leaf_axes || selected > dims.size() - leaf_axes)
        return false;
      // A checked, constant prefix leaves the suffix geometry unchanged.
      // Runtime or invalid selectors must still be evaluated normally:
      // knowing the suffix shape is not permission to erase their effects
      // or bounds errors (including x[1] on an empty outer array).
      for (size_t d = 0; d < selected; ++d) {
        const auto& index = e.args[d + 1];
        long value;
        if (index.kind != mir::Expr::IndexSingle || index.args.size() != 1 ||
            !try_cint(index.args[0], &value) || value < 1 || value > dims[d])
          return false;
      }
      const std::vector<int64_t> suffix(dims.begin() + selected, dims.end());
      Range r{0, (int)checked_shape_product(suffix, "array prefix")};
      if (suffix.size() > leaf_axes) {
        r.kind = ViewKind::Array;
        r.leaf = b.leaf;
        r.dims = suffix;
      } else {
        r.kind = b.leaf;
        if (b.leaf == ViewKind::Matrix) {
          r.rows = suffix[0];
          r.cols = suffix[1];
        }
      }
      *out = r;
      return true;
    }
    if (b.kind == ViewKind::Matrix && e.args.size() == 3) {
      const int64_t nr = (int64_t)matrix_positions(e.args[1], b.rows, "row",
                                                   SelectorSite::MatrixRow)
                             .size();
      const int64_t nc = (int64_t)matrix_positions(e.args[2], b.cols, "column",
                                                   SelectorSite::MatrixColumn)
                             .size();
      const int64_t width = nr * nc;
      Range r{0, (int)width};
      if (width == 1 && !mir::is_unsized(e, mir::UnsizedLeaf::Matrix) &&
          !mir::is_unsized(e, mir::UnsizedLeaf::Vector) &&
          !mir::is_unsized(e, mir::UnsizedLeaf::RowVector)) {
        *out = r;
        return true;
      }
      if (mir::is_unsized(e, mir::UnsizedLeaf::Vector)) {
        r.kind = ViewKind::Vector;
      } else if (mir::is_unsized(e, mir::UnsizedLeaf::RowVector)) {
        r.kind = ViewKind::RowVector;
      } else {
        r.kind = ViewKind::Matrix;
        r.rows = nr;
        r.cols = nc;
      }
      *out = r;
      return true;
    }
    if ((b.kind == ViewKind::Vector || b.kind == ViewKind::RowVector ||
         b.kind == ViewKind::Flat) &&
        e.args.size() == 2) {
      const int64_t n = (int64_t)matrix_positions(e.args[1], b.len, "index",
                                                  b.kind == ViewKind::Flat
                                                      ? SelectorSite::Array
                                                      : SelectorSite::Vector)
                            .size();
      *out = typed(Range{0, (int)n}, e.unsized);
      return true;
    }
  } catch (Bail&) {
    return false;
  }
  return false;
}

std::vector<int64_t> ProgramCompiler::matrix_positions(const mir::Expr& index,
                                                       int64_t extent,
                                                       const std::string& axis,
                                                       SelectorSite site,
                                                       bool emit, bool assign) {
  if (extent < 0 || extent > kMaxRegs)
    bail("matrix " + axis + " has an invalid extent");
  std::vector<long> values;
  if (index.kind == mir::Expr::IndexAll) {
    values.reserve((size_t)extent);
    for (int64_t i = 1; i <= extent; ++i) values.push_back((long)i);
  } else if (index.kind == mir::Expr::IndexSingle && index.args.size() == 1) {
    values.push_back(cint(index.args[0]));
  } else if ((index.kind == mir::Expr::IndexBetween &&
              index.args.size() == 2) ||
             (index.kind == mir::Expr::IndexUpfrom && index.args.size() == 1)) {
    const bool upfrom = index.kind == mir::Expr::IndexUpfrom;
    const RangeSelection range = resolve_range_selector(
        site, extent, cint(index.args[0]),
        upfrom ? std::nullopt : std::optional<int64_t>(cint(index.args[1])));
    if (!range.ok()) {
      if (!emit) bail("matrix " + axis + " range is outside its extent");
      emit_range_reject(range, site, upfrom, assign, extent);
      return {};
    }
    if (range.count > kMaxRegs) bail("matrix " + axis + " range is too large");
    values.reserve((size_t)range.count);
    for (int64_t k = 0; k < range.count; ++k)
      values.push_back((long)(range.first + k));
  } else if (index.kind == mir::Expr::IndexMulti && index.args.size() == 1) {
    values = cints(index.args[0]);
  } else {
    bail("matrix " + axis + " index form");
  }
  std::vector<int64_t> positions;
  positions.reserve(values.size());
  for (long value : values) {
    if (value < 1 || value > extent) {
      std::string detail = "matrix " + axis + " gather index " +
                           std::to_string(value) +
                           " is outside 1:" + std::to_string(extent);
      for (const auto& [name, held] : ints)
        if (held.size() == 1)
          detail += " " + name + "=" + std::to_string(held[0]);
      bail(detail);
    }
    positions.push_back((int64_t)value - 1);
  }
  return positions;
}

std::vector<int64_t> ProgramCompiler::graph_array_offsets(
    const std::vector<int64_t>& dims, ViewKind leaf,
    const std::vector<std::vector<int64_t>>& positions) {
  const size_t leaf_dims =
      leaf == ViewKind::Matrix
          ? 2
          : (leaf == ViewKind::Vector || leaf == ViewKind::RowVector ? 1 : 0);
  if (dims.size() < leaf_dims || positions.size() != dims.size())
    bail("array selection has inconsistent logical extents");
  checked_shape_product(dims, "array selection");
  const size_t n_array = dims.size() - leaf_dims;
  int64_t leaf_width = 1;
  for (size_t d = n_array; d < dims.size(); ++d) leaf_width *= dims[d];
  std::vector<int64_t> outer_stride(n_array, leaf_width);
  for (size_t d = n_array; d-- > 1;)
    outer_stride[d - 1] = outer_stride[d] * dims[d];

  std::vector<int64_t> offsets;
  std::function<void(size_t, int64_t)> arrays = [&](size_t d, int64_t off) {
    if (d != n_array) {
      for (int64_t position : positions[d])
        arrays(d + 1, off + position * outer_stride[d]);
      return;
    }
    if (leaf == ViewKind::Matrix) {
      const int64_t rows = dims[n_array];
      for (int64_t col : positions[n_array + 1])
        for (int64_t row : positions[n_array])
          offsets.push_back(off + col * rows + row);
    } else if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) {
      for (int64_t cell : positions[n_array]) offsets.push_back(off + cell);
    } else {
      offsets.push_back(off);
    }
  };
  arrays(0, 0);
  return offsets;
}

std::vector<int64_t> ProgramCompiler::first_fast_array_offsets(
    const std::vector<int64_t>& dims,
    const std::vector<std::vector<int64_t>>& positions) {
  if (positions.size() != dims.size())
    bail("integer array selection has inconsistent logical extents");
  checked_shape_product(dims, "integer array selection");
  std::vector<int64_t> stride(dims.size(), 1);
  for (size_t d = 1; d < dims.size(); ++d)
    stride[d] = stride[d - 1] * dims[d - 1];
  std::vector<int64_t> offsets;
  std::function<void(int, int64_t)> gather = [&](int d, int64_t off) {
    if (d < 0) {
      offsets.push_back(off);
      return;
    }
    for (int64_t position : positions[(size_t)d])
      gather(d - 1, off + position * stride[(size_t)d]);
  };
  gather((int)dims.size() - 1, 0);
  return offsets;
}

bool ProgramCompiler::same_view(const Range& a, const Range& b) {
  if (a.kind != b.kind) return false;
  // A zero-width flat range is no value, as a void return gives.
  if (a.kind == ViewKind::Flat) return a.len == b.len && a.len <= 1;
  if (a.kind == ViewKind::Vector || a.kind == ViewKind::RowVector)
    return a.len == b.len;
  if (a.kind == ViewKind::Array)
    return a.len == b.len && a.dims == b.dims && a.leaf == b.leaf;
  return a.rows == b.rows && a.cols == b.cols;
}

Range ProgramCompiler::typed(Range r, const mir::UnsizedView& type) {
  if (mir::is_unsized(type, mir::UnsizedLeaf::Vector)) {
    r.kind = ViewKind::Vector;
    r.rows = r.cols = 0;
  } else if (mir::is_unsized(type, mir::UnsizedLeaf::RowVector)) {
    r.kind = ViewKind::RowVector;
    r.rows = r.cols = 0;
  } else if (mir::is_unsized(type, mir::UnsizedLeaf::Matrix) &&
             r.kind != ViewKind::Matrix) {
    bail("matrix expression has unknown logical extents");
  } else if (mir::is_unsized_array(type) && r.kind != ViewKind::Array) {
    bail("array expression has an unknown logical view");
  }
  return r;
}

ViewKind ProgramCompiler::leaf_of(const std::string& base) {
  if (base == "SVector") return ViewKind::Vector;
  if (base == "SRowVector") return ViewKind::RowVector;
  if (base == "SMatrix") return ViewKind::Matrix;
  return ViewKind::Flat;
}

size_t ProgramCompiler::leaf_rank(ViewKind leaf) {
  if (leaf == ViewKind::Matrix) return 2;
  if (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) return 1;
  return 0;
}

Range ProgramCompiler::declared(Range r, const mir::SizedType& type) {
  if (type.base == "SArray") {
    if (type.elem_base != "SReal" && type.elem_base != "SInt" &&
        leaf_of(type.elem_base) == ViewKind::Flat)
      bail(
          "only scalar-array and container-leaf declarations are supported "
          "by the register program");
    r.kind = ViewKind::Array;
    r.leaf = leaf_of(type.elem_base);
    r.dims.clear();
    for (const auto& d : type.dims) r.dims.push_back(cint(d));
    if (r.dims.size() <= leaf_rank(r.leaf))
      bail("array declaration lacks its leaf extents");
    return r;
  }
  if (type.base == "SVector")
    r.kind = ViewKind::Vector;
  else if (type.base == "SRowVector")
    r.kind = ViewKind::RowVector;
  else if (type.base == "SMatrix") {
    r.kind = ViewKind::Matrix;
    r.rows = cint(type.dims[0]);
    r.cols = cint(type.dims[1]);
  }
  return r;
}

bool ProgramCompiler::unsized_accepts(const mir::UnsizedView& type,
                                      const Range& r) {
  if (type.leaf == mir::UnsizedLeaf::Unknown ||
      type.leaf == mir::UnsizedLeaf::Complex)
    return false;
  if (type.depth != 0) {
    if (r.kind != ViewKind::Array) return false;
    const size_t leaf_rank = type.leaf == mir::UnsizedLeaf::Matrix ? 2
                             : type.leaf == mir::UnsizedLeaf::Vector ||
                                     type.leaf == mir::UnsizedLeaf::RowVector
                                 ? 1
                                 : 0;
    // Old register ranges did not stamp the sole scalar-array extent.
    // Retain that compatibility while requiring complete geometry for
    // every structural container newly represented here.
    if (r.dims.empty()) return type.depth == 1 && leaf_rank == 0;
    return r.dims.size() == (size_t)type.depth + leaf_rank;
  }
  if (type.leaf == mir::UnsizedLeaf::Vector) return r.kind == ViewKind::Vector;
  if (type.leaf == mir::UnsizedLeaf::RowVector)
    return r.kind == ViewKind::RowVector;
  if (type.leaf == mir::UnsizedLeaf::Matrix) return r.kind == ViewKind::Matrix;
  return is_scalar(r);
}

Range ProgramCompiler::matrix_row(const Range& m, long i) {
  if (i < 1 || i > m.rows) bail("matrix index out of the declared range");
  const int r = alloc((int)m.cols);
  for (int64_t j = 0; j < m.cols; ++j)
    emit(Program::MOV, r + (int)j, m.reg + (int)(j * m.rows + (i - 1)));
  Range out{r, (int)m.cols};
  out.kind = ViewKind::RowVector;
  return out;
}

bool ProgramCompiler::has_runtime_selector(
    const std::vector<mir::Expr>& selectors) {
  for (const auto& index : selectors) {
    if (index.kind == mir::Expr::IndexAll) continue;
    if (index.kind == mir::Expr::IndexMulti) {
      std::vector<long> values;
      if (index.args.size() != 1 || !try_cints(index.args[0], &values))
        return true;
    } else {
      for (const auto& arg : index.args) {
        long value;
        if (!try_cint(arg, &value)) return true;
      }
    }
  }
  return false;
}

Range ProgramCompiler::dynamic_index(const Range& base,
                                     const std::vector<mir::Expr>& selectors,
                                     const Range* rhs) {
  std::vector<int64_t> dims;
  ViewKind leaf = base.kind;
  if (base.kind == ViewKind::Array) {
    dims = base.dims.empty() ? std::vector<int64_t>{base.len} : base.dims;
    leaf = base.leaf;
  } else if (base.kind == ViewKind::Matrix) {
    dims = {base.rows, base.cols};
  } else if (base.kind == ViewKind::Vector ||
             base.kind == ViewKind::RowVector) {
    dims = {base.len};
  } else {
    bail("runtime indexed access needs a container view");
  }
  if (selectors.size() > dims.size()) bail("too many runtime indices");
  const size_t leaf_rank =
      leaf == ViewKind::Matrix                                    ? 2
      : (leaf == ViewKind::Vector || leaf == ViewKind::RowVector) ? 1
                                                                  : 0;
  if (dims.size() < leaf_rank) bail("incomplete runtime index shape");
  const size_t outer = dims.size() - leaf_rank;
  std::vector<int64_t> strides(dims.size(), 1);
  int64_t stride = 1;
  if (leaf == ViewKind::Matrix) {
    strides[outer] = 1;
    strides[outer + 1] = dims[outer];
    stride = dims[outer] * dims[outer + 1];
  } else if (leaf_rank) {
    stride = dims[outer];
  }
  for (size_t d = outer; d-- > 0;) {
    strides[d] = stride;
    stride *= dims[d];
  }
  auto spec = std::make_shared<DynamicIndexSpec>();
  spec->matrix_leaf = leaf == ViewKind::Matrix;
  std::vector<Range> parts;
  std::vector<int64_t> output_dims;
  std::vector<bool> keep;
  int packed_size = 0, selected_size = 1;
  size_t remaining_outer = 0;
  for (size_t d = 0; d < dims.size(); ++d) {
    DynamicIndexSpec::Axis axis;
    axis.extent = dims[d];
    axis.stride = strides[d];
    axis.input_offset = packed_size;
    const auto* index = d < selectors.size() ? &selectors[d] : nullptr;
    bool drop = false;
    Range values;
    if (!index || index->kind == mir::Expr::IndexAll) {
      axis.kind = DynamicIndexSpec::Axis::All;
      axis.count = dims[d];
    } else if (index->kind == mir::Expr::IndexSingle &&
               index->args.size() == 1) {
      axis.kind = DynamicIndexSpec::Axis::Single;
      axis.count = 1;
      values = expr(index->args[0]);
      if (!is_scalar(values)) bail("runtime index must be scalar");
      drop = true;
    } else if (index->kind == mir::Expr::IndexMulti &&
               index->args.size() == 1) {
      axis.kind = DynamicIndexSpec::Axis::Multi;
      values = expr(index->args[0]);
      if (values.kind != ViewKind::Array ||
          (!values.dims.empty() && values.dims.size() != 1))
        bail("runtime gather indices must be a one-dimensional array");
      axis.count = values.len;
    } else {
      // A range may coexist with runtime selectors on other axes, but its
      // length must remain static. Variable-length ranges still refuse.
      const auto positions =
          matrix_positions(*index, dims[d], "runtime selection",
                           selector_site(d, outer, leaf == ViewKind::Matrix),
                           true, rhs != nullptr);
      axis.kind = DynamicIndexSpec::Axis::Range;
      axis.count = positions.size();
      values =
          Range{konst(positions.empty() ? 1.0 : positions.front() + 1.0), 1};
    }
    if (axis.kind != DynamicIndexSpec::Axis::All) {
      if (values.len > kMaxRegs - packed_size)
        bail("runtime selectors are too large");
      parts.push_back(values);
      packed_size += values.len;
    }
    if (axis.count < 0 || axis.count > kMaxRegs ||
        (axis.count && selected_size > kMaxRegs / axis.count))
      bail("runtime selection is too large");
    selected_size *= static_cast<int>(axis.count);
    keep.push_back(!drop);
    if (!drop) {
      output_dims.push_back(axis.count);
      if (d < outer) ++remaining_outer;
    }
    spec->axes.push_back(axis);
  }
  spec->selected_size = selected_size;
  Range selected{0, selected_size};
  ViewKind result_leaf = leaf;
  if (leaf == ViewKind::Matrix)
    result_leaf =
        keep[outer] ? (keep[outer + 1] ? ViewKind::Matrix : ViewKind::Vector)
                    : (keep[outer + 1] ? ViewKind::RowVector : ViewKind::Flat);
  else if (leaf_rank && !keep[outer])
    result_leaf = ViewKind::Flat;
  if (remaining_outer) {
    selected.kind = ViewKind::Array;
    selected.leaf = result_leaf;
    selected.dims = output_dims;
  } else {
    selected.kind = result_leaf;
    if (result_leaf == ViewKind::Matrix) {
      selected.rows = output_dims.at(0);
      selected.cols = output_dims.at(1);
    }
  }
  if (rhs && !same_view(selected, *rhs))
    bail("runtime indexed assignment logical view mismatch");
  Range packed{packed_size ? alloc(packed_size) : konst(0.0),
               packed_size ? packed_size : 1};
  int at = 0;
  for (const Range& part : parts)
    for (int k = 0; k < part.len; ++k)
      emit(Program::MOV, packed.reg + at++, part.reg + k);
  if (rhs)
    return kernel_call(OP_SET_INDEX_DYNAMIC, {base, packed, *rhs}, base, 0, 0x5,
                       {}, spec, "runtime indexed assignment");
  return kernel_call(OP_INDEX_DYNAMIC, {base, packed}, selected, 0, 0x1, {},
                     spec, "runtime indexed read");
}

void ProgramCompiler::emit_range_reject(const RangeSelection& range,
                                        SelectorSite site, bool upfrom,
                                        bool assign, int64_t extent) {
  Program::Message m;
  if (assign && !range.out_of_range) {
    m.spec.chunks = {"array range assignment selects " +
                     std::to_string(range.count) + " elements"};
    m.spec.error = MessageError::InvalidArgument;
  } else {
    const RangeSelectorError error =
        range_selector_error(range, site, upfrom, assign, extent);
    m.spec.chunks = {error.message};
    m.spec.error =
        error.out_of_range ? MessageError::OutOfRange : MessageError::Domain;
  }
  p.messages.push_back(std::move(m));
  emit(Program::REJECT, 0, (int)p.messages.size() - 1);
}

Range ProgramCompiler::one_dimensional_range(const Range& b,
                                             const mir::Expr& e) {
  const mir::Expr& index = e.args[1];
  const bool upfrom = index.kind == mir::Expr::IndexUpfrom;
  const SelectorSite site =
      b.kind == ViewKind::Flat ? SelectorSite::Array : SelectorSite::Vector;
  const RangeSelection range = resolve_range_selector(
      site, b.len, cint(index.args[0]),
      upfrom ? std::nullopt : std::optional<int64_t>(cint(index.args[1])));
  if (!range.ok()) {
    emit_range_reject(range, site, upfrom, false, b.len);
    return typed(Range{b.reg, 0}, e.unsized);
  }
  if (range.count == 0) return typed(Range{b.reg, 0}, e.unsized);
  return typed(Range{b.reg + (int)range.first - 1, (int)range.count},
               e.unsized);
}

}  // namespace stanli
