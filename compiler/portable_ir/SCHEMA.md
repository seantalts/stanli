# Stanli portable MIR v4

This document defines the stanli-owned wire representation of the MIR slice
consumed by the C++ runtime. It is not a serialization of every stanc3 MIR
field and it is not an API commitment from stanc3.

The canonical producer in `compiler/ocaml/portable_mir.ml` consumes stanc3's
`Middle.Program.Typed.t` after the Stan Math backend transform and stanli's
selected optimization policy. The decoder constructs `stanli::mir::Program`
directly, validates it, then runs the same overload and binding finalizer used
by the legacy S-expression reader.

Version 4 is a canonical ASCII envelope around a compact, fixed-layout binary
payload. The envelope crosses native callbacks, subprocess output, V8,
js_of_ocaml, Web Workers, and existing C-string APIs without transcoding or
embedded-NUL problems. The payload avoids a JSON DOM, repeated field names,
inactive fields, and decimal parsing.

Portable MIR v1 was a pre-release JSON format, v2 (`STANLI2:`) the layout
before every stanc3 fact the runtime reads became a typed field, and v3
(`STANLI3:`) the layout before tuples. v4 adds the tuple fields below; a
program without a tuple encodes the same payload bytes as v3. None of the
older versions is accepted by this runtime; a `STANLI<n>:` document of another version is
rejected with a request to recompile the model. The legacy stanc3
S-expression reader remains a separate input path.

## Envelope and format detection

A canonical document is:

```text
STANLI4:<base64 payload>
```

`STANLI4:` is the eight-byte ASCII version marker. The remainder is standard
RFC 4648 base64 using `A-Z`, `a-z`, `0-9`, `+`, and `/`, with required `=`
padding. There is no whitespace, BOM, or final newline.

The multi-format decoder dispatches as follows:

1. An exact `STANLI4:` prefix selects this decoder.
2. Any other `STANLI` prefix is a document of another version and is
   rejected.
3. Otherwise, after the four ASCII whitespace bytes are skipped, `(` selects
   the legacy S-expression reader.
4. Every other input is rejected.

Once the v4 prefix is seen, malformed base64 or a malformed payload is a hard
v4 error. The decoder never retries another format.

Canonical base64 has exactly one spelling. Padding may occur only in the final
quartet, and unused tail bits must be zero. A decoder rejects non-canonical
tails even when a permissive base64 implementation could recover bytes.

## Scalar primitives

All multibyte values are little-endian.

| Name | Encoding |
| --- | --- |
| `u8` | one unsigned byte |
| `bool` | `u8`, exactly `0` or `1` |
| `u32` | four-byte unsigned integer |
| `i32` | four-byte two's-complement Stan integer |
| `f64` | the exact eight-byte IEEE-754 binary64 payload |
| `string` | `u32` byte count followed by that many UTF-8 bytes |
| `list<T>` | `u32` item count followed by each `T` in order |
| `optional<T>` | `bool`; when true, one `T` follows |

Every `f64` bit pattern is representable, including negative zero, infinities,
and every NaN sign, signaling/quiet bit, and payload. Neither encoder nor
decoder converts through decimal text. A target without IEC 60559 binary64
`double` support cannot build the decoder.

Strings must be well-formed UTF-8 scalar sequences. Names and opaque payloads
are byte-preserving after validation: no Unicode normalization, case folding,
qualification, or newline conversion is performed. Embedded NUL is legal in
the binary payload even though the canonical outer envelope itself contains
only printable ASCII.

## Tag tables

Tags are protocol numbers, not serialized C++ enum ordinals. The decoder maps
each number explicitly so reordering an implementation enum does not change
the wire contract.

### `UnsizedView`

```text
u8 depth
u8 leaf
list<UnsizedView> components   (Tuple only)
```

| `leaf` | Meaning |
| ---: | --- |
| 0 | `Unknown` |
| 1 | `Int` |
| 2 | `Real` |
| 3 | `Complex` |
| 4 | `Vector` |
| 5 | `RowVector` |
| 6 | `Matrix` |
| 7 | `Tuple` |

`depth` is the array nesting depth. Unknown leaf tags are errors. A `Tuple`
view lists its component views, so `array[] tuple(real, vector)` is depth 1,
`Tuple`, components `Real` and `Vector`.

### Function library

| Tag | Meaning |
| ---: | --- |
| 0 | `StanLib` |
| 1 | `Internal` |
| 2 | `UserDefined` |

### Function suffix

stanc3's `Fun_kind.suffix`: the kind of function a name denotes. Stan source
spells it with the name's ending, but consumers read this tag, never the
name.

| Tag | Meaning | Source spelling |
| ---: | --- | --- |
| 0 | `FnPlain` | |
| 1 | `FnRng` | `_rng` |
| 2 | `FnLpdf` | `_lpdf`, `_lupdf` |
| 3 | `FnLpmf` | `_lpmf`, `_lupmf` |
| 4 | `FnTarget` | `_lp` |
| 5 | `FnJacobian` | `_jacobian` |

### `FunctionType`

```text
list<UnsizedView> args
u8 suffix
bool propto
```

The type of a variable that names a function, as a higher-order callback
argument does: the views of the function's arguments, its suffix, and
whether the reference asks for the unnormalized density. A callback written
`foo_lupdf`/`foo_lupmf` is encoded with the definition's name,
`foo_lpdf`/`foo_lpmf`, and `propto` set.

### Expression kind

Every expression begins with a kind tag and its active fields, then ends with
the common metadata suffix shown below.

| Tag | Kind | Active fields after tag |
| ---: | --- | --- |
| 0 | `Var` | `string name`, `optional<FunctionType> function_type` |
| 1 | `LitInt` | `i32 value` |
| 2 | `LitReal` | `f64 value` |
| 3 | `LitStr` | `string value` |
| 4 | `FunApp` | `u8 library`, `string name`, `u8 suffix`, `bool propto`, `list<Expr> args` |
| 5 | `Promotion` | `list<Expr> args` |
| 6 | `Indexed` | `list<Expr> args` |
| 7 | `TernaryIf` | `list<Expr> args` |
| 8 | `EOr` | `list<Expr> args` |
| 9 | `EAnd` | `list<Expr> args` |
| 10 | `Unsupported` | none |
| 11 | `IndexAll` | `list<Expr> args` (empty) |
| 12 | `IndexSingle` | `list<Expr> args` (the index) |
| 13 | `IndexUpfrom` | `list<Expr> args` (the lower bound) |
| 14 | `IndexBetween` | `list<Expr> args` (the lower and upper bounds) |
| 15 | `IndexMulti` | `list<Expr> args` (the integer array of indices) |
| 16 | `TupleProjection` | `list<Expr> args` (the tuple), `u32 component` (from 1) |

The common suffix is:

```text
UnsizedView unsized
bool data_only
list<bool> leaf_data_only   (Tuple view only)
bool promoted
string raw
```

stanc3 gives a tuple-typed value one adlevel per component.
`leaf_data_only` lists whether each leaf of the tuple type is data, depth
first; `data_only` is false for a tuple.

For `LitInt`, the C++ compatibility field `Expr.lit` is reconstructed as the
exact `double` conversion of the decoded `i32`; it is not redundantly encoded.
The producer currently flattens stanc3 `Promotion` nodes into the promoted
inner expression and sets `promoted`, matching the legacy structural mapping.
Tag 5 remains defined for direct runtime inputs and validation coverage.

The index kinds (11-15) occur only after the base of an `Indexed` expression
and in an assignment's `lhs_indices`; their arities are validated after
decode, and their metadata suffix carries default values.

### Transform kind

A transform is:

```text
u8 kind
list<Expr> args
string raw
list<Transform> components   (Tuple only)
```

| Tag | Kind |
| ---: | --- |
| 0 | `Identity` |
| 1 | `Lower` |
| 2 | `Upper` |
| 3 | `LowerUpper` |
| 4 | `Offset` |
| 5 | `Multiplier` |
| 6 | `OffsetMultiplier` |
| 7 | `Simplex` |
| 8 | `Ordered` |
| 9 | `PositiveOrdered` |
| 10 | `CholeskyCorr` |
| 11 | `UnitVector` |
| 12 | `SumToZero` |
| 13 | `Correlation` |
| 14 | `Covariance` |
| 15 | `CholeskyCov` |
| 16 | `StochasticColumn` |
| 17 | `StochasticRow` |
| 18 | `Unsupported` |
| 19 | `Tuple` |

Argument arities are checked against the selected kind. A `Tuple` transform
has no arguments and one transform per component.

### `SizedType`

```text
string base
list<Expr> dims
string elem_base
string raw
list<SizedType> components   (STuple, or SArray of STuple, only)
```

`base` uses the runtime spellings such as `SInt`, `SReal`, `SVector`,
`SRowVector`, `SMatrix`, `SArray`, and `STuple`. `elem_base` identifies the
innermost element for an array and is empty otherwise. Dimension counts are
validated against the base. An `STuple` has no dimensions; an array of
tuples has `SArray` base, `STuple` element base, its array dimensions, and
the tuple's component types.

### `DeclType`

```text
u8 tag
```

| `tag` | Meaning | Fields after tag |
| ---: | --- | --- |
| 0 | `Sized` | `SizedType type` |
| 1 | `Unsized` | `UnsizedView view`, `string raw` |

stanc3 declares optimizer temporaries, such as an inlined function's result,
unsized; the first whole-variable assignment gives such a container its
shape. `raw` spells an `Unknown` leaf's stanc3 type for diagnostics and is
otherwise empty. The decoder gives an unsized `int` or `real` the sized
`SInt`/`SReal` spelling, since a scalar's width is fixed.

### Statement payload

The two compiler-internal statement functions that carry data beyond their
arguments say so with a tag. Any other statement uses tag 0, and the tag must
match the function.

| Tag | Function | Fields after tag |
| ---: | --- | --- |
| 0 | any other | none |
| 1 | `FnCheck` | `Transform constraint`, `string var_name`, `Expr value` |
| 2 | `FnWriteParam` | `optional<Transform> unconstrain`, `Expr value` |

`FnCheck` verifies `value` against `constraint`, whose bounds are the
statement's arguments. `FnWriteParam` writes `value`; in `transform_inits`
`unconstrain` is the transform to invert, turning an already-constrained
value into a free one, and in `generate_quantities` it is absent because the
value is constrained already. The decoder puts `value` first in the
statement's argument list.

### Statement kind

Each statement starts with one tag and then only fields active for that kind.
There is no inactive-field block.

| Tag | Kind | Active fields after tag |
| ---: | --- | --- |
| 0 | `Decl` | `string id`, `DeclType type`, `bool data_only`, `list<bool> leaf_data_only` (tuple type only), `bool has_init`, optional `Expr init`, `optional<Transform> read_transform`, `list<Expr> read_dims`, `string raw` |
| 1 | `Assignment` | `string lhs`, `list<Expr> lhs_indices`, `Expr rhs`, `string raw` |
| 2 | `TargetPE` | `Expr target`, `string raw` |
| 3 | `Block` | `list<Stmt> body`, `string raw` |
| 4 | `SList` | `list<Stmt> body`, `string raw` |
| 5 | `For` | `string loopvar`, `Expr lower`, `Expr upper`, `list<Stmt> body`, `string raw` |
| 6 | `IfElse` | `Expr condition`, `list<Stmt> body`, `string raw` |
| 7 | `While` | `Expr condition`, `list<Stmt> body`, `string raw` |
| 8 | `NRFunApp` | `u8 library`, `string name`, `u8 suffix`, `list<Expr> args`, `u8 payload` and its fields, `string raw` |
| 9 | `Return` | `bool has_value`, optional `Expr value`, `string raw` |
| 10 | `Break` | none |
| 11 | `Continue` | none |
| 12 | `Skip` | `string raw` |
| 13 | `Unsupported` | `string raw` |
| 14 | `JacobianPE` | `Expr target`, `string raw` |
| 15 | `TupleAssignment` | `string lhs`, `list<Expr> lhs_indices`, `list<TupleStep> steps`, `Expr rhs`, `optional<Transform> read_transform`, `list<Expr> read_dims` |

A `TupleAssignment` assigns through tuple projections: `x[i].2[j] = rhs` is
`lhs` x, `lhs_indices` [i], and the one step (2, [j]). A `TupleStep` is
`u32 component` (from 1) followed by `list<Expr> indices`. stanc3 reads a
tuple parameter one component at a time, `x.2 = FnReadParam(...)`; such an
assignment carries the read's transform and dimensions as `Decl` does. The
decoder reads it as an `Assignment` with its steps.

`JacobianPE` is `jacobian += target`. The decoder reads it as a `TargetPE`
whose `jacobian` flag is set: stanli always evaluates the density with the
Jacobian, so it adds to the target like any other increment.

The `IfElse` body has one or two statements in then/else order. `For` and
`While` each have exactly one body statement.

### `FunDef`

```text
string name
list<string> arg_names
list<UnsizedView> arg_views
list<bool> arg_data_only
list<bool> leaf_data_only   (once per tuple argument, in order)
list<Stmt> body
```

The three argument lists must have identical lengths. Function names are stanc3's names,
including its `_stan_` prefix for a C++ keyword, which the finalizer removes.
A `propto` flag is true only for an `FnLpdf`/`FnLpmf` call written
`_lupdf`/`_lupmf`.

When a name has several definitions, the shared finalizer gives each its
argument signature as a separate `overload` field and records on every call,
statement call and callback variable the overload its argument views (for a
callback, its `function_type` views) select; names stay unchanged.

### `Program`

The root payload is the following sequence with no wrapper object:

```text
list<InputVar> input_vars
list<Stmt> prepare_data
list<Stmt> log_prob
list<Stmt> generate_quantities
u32 transformed_parameters_begin
u32 generated_quantities_begin
list<FunDef> fun_defs
list<OutputVar> output_vars
list<Stmt> transform_inits
```

stanc3 separates `generate_quantities`, its write_array body, into
parameter, transformed-parameter and generated-quantity sections with
top-level early-return guards on `emit_transformed_parameters__` and
`emit_generated_quantities__`. The runtime always emits every section, so
the producer drops the two guards and records the index of the statement
each later section begins with; a missing section begins where the next
one does. The decoder requires
`transformed_parameters_begin <= generated_quantities_begin <=` the
statement count.

An `InputVar` is `string name` followed by `SizedType type`. An `OutputVar`
is

```text
string name
u8 block
SizedType constrained_type
```

| `block` | Meaning |
| ---: | --- |
| 0 | `Parameters` |
| 1 | `TransformedParameters` |
| 2 | `GeneratedQuantities` |

`output_vars` lists the parameters, transformed parameters and generated
quantities in the order `generate_quantities` writes them, which is CmdStan's
column order. The runtime assigns each executed `FnWriteParam` to the first
variable whose columns (the product of its constrained type's extents) are
not all written; a write that still reads a variable by name selects that
variable, skipping zero-width variables before it. Column names, CSV section
boundaries and the complex-output check all come from the variable so
assigned, so a write the optimizer turned into a literal is named correctly.

`FnReadData` is a statement, `FnReadData(x, context__, "x")`, that reads the
named input whole into the declared `x`. `transform_inits` is stanc3's inverse
parameter-transform section: its `FnReadData` statements name PARAMETERS read
from a caller-supplied init context rather than model data, and each parameter
ends in an `FnWriteParam` carrying the transform to invert.

Every section is required, `transform_inits` included; a model without
parameters writes it as an empty list.

The order of every list is semantic and preserved. Source locations and
stanc3 fields absent from `stanli::mir` are not encoded.

stanc3 reads a tuple input whole, `FnReadData(d, context__, "d")`, while
CmdStan's JSON reader keys each leaf by its slots: `d.1`, `d.2.1`. The
finalizer, below, maps one read to the other.

## Unsupported and opaque payloads

The producer must not silently omit or reinterpret an unsupported construct.
Expressions, statements, and transforms use their `Unsupported` tag; a sized
type retains its exact unrecognized `base`. Each carries a complete
deterministic opaque spelling in `raw`; lowering reports such a node as an
unsupported form.

`raw` is diagnostic text only: every fact the runtime acts on has a typed
field, and no consumer parses `raw`. It is still included in canonical byte
equality so native OCaml, js_of_ocaml, and Windows builds cannot drift
unnoticed.

## Validation and finalization

After structural decoding and before a `Program` is exposed, the runtime:

1. checks expression, index, transform, sized-type, statement, and known
   function arities;
2. checks function argument list lengths;
3. records the overload each user-function call selects and rejects
   colliding overloads;
4. replaces each tuple variable with one variable per leaf, an array of
   tuples becoming a tuple of arrays, inlines each function returning a
   tuple and passes a tuple argument as one argument per leaf, so no engine
   sees a tuple (`runtime/src/mir_tuples.cpp`);
5. validates variable and function bindings; and
6. rejects trailing payload bytes.

Validation runs before downstream code can index positional argument vectors.
Malformed input therefore becomes a compiler error rather than undefined
behavior. `data_only` is producer metadata and is checked only where the
compiler phase makes equality meaningful; generated-quantities reads can
legitimately differ from declaration-level autodiff metadata.

## Resource limits

The decoder enforces these limits before or during allocation:

| Resource | Limit |
| --- | ---: |
| Canonical envelope bytes | 268,435,456 |
| Decoded payload bytes | 268,435,456 |
| One UTF-8 string | 16,777,216 bytes |
| Total decoded string bytes | 268,435,456 bytes |
| One list | 1,000,000 items |
| Aggregate list backing storage | 268,435,456 bytes |
| Scalar/list values | 10,000,000 |
| Active reader scopes | 80,000 |

A list count larger than the remaining payload, or one that would exceed the
aggregate backing-storage budget, is rejected before `reserve()`.
All fixed-width reads and strings check remaining bytes before advancing. The
base64 decoder validates the envelope size before reserving its output.
The nesting counter advances once for a decoded node and once for each decoded
list containing it. A unary chain therefore consumes roughly two reader scopes
per node. Up to 256 nested nodes are read on the calling thread; deeper input
is read on a helper thread with a large stack, up to the 80,000 scope budget,
and anything deeper is rejected as nested too deeply.

## Canonical bytes and conformance

For a fixed stanc3 revision, source, include map, and pass selection, every
producer must emit identical bytes. Canonical encoding requires:

1. the exact `STANLI4:` prefix;
2. the tag numbers and field order in this document;
3. little-endian fixed-width scalars;
4. original list order;
5. exact binary64 payloads;
6. validated, unnormalized UTF-8 strings;
7. standard padded base64 with zero unused tail bits; and
8. no whitespace or final newline.

Conformance gates compare native OCaml and js_of_ocaml results as raw ASCII
bytes, decode both through the C++ reader, exercise lowering and execution,
and cover folded binary64 values, checked int32 overflow, Unicode, includes,
user functions, warnings, and frontend errors. Windows uses the same encoder
and participates in the same byte comparison.

A future incompatible layout gets a new ASCII header and a new decoder. It
must never reuse `STANLI4:` with different tag meanings or field order.
