# Stanli portable MIR v2

This document defines the stanli-owned wire representation of the MIR slice
consumed by the C++ runtime. It is not a serialization of every stanc3 MIR
field and it is not an API commitment from stanc3.

The canonical producer in `compiler/ocaml/portable_mir.ml` consumes stanc3's
`Middle.Program.Typed.t` after the Stan Math backend transform and stanli's
selected optimization policy. The decoder constructs `stanli::mir::Program`
directly, validates it, then runs the same overload and binding finalizer used
by the legacy S-expression reader.

Version 2 is a canonical ASCII envelope around a compact, fixed-layout binary
payload. The envelope crosses native callbacks, subprocess output, V8,
js_of_ocaml, Web Workers, and existing C-string APIs without transcoding or
embedded-NUL problems. The payload avoids a JSON DOM, repeated field names,
inactive fields, and decimal parsing.

Portable MIR v1 was a pre-release JSON format. It is not accepted by this
runtime and has no compatibility role. The legacy stanc3 S-expression reader
remains a separate transitional input path.

## Envelope and format detection

A canonical document is:

```text
STANLI2:<base64 payload>
```

`STANLI2:` is the eight-byte ASCII version marker. The remainder is standard
RFC 4648 base64 using `A-Z`, `a-z`, `0-9`, `+`, and `/`, with required `=`
padding. There is no whitespace, BOM, or final newline.

The multi-format decoder dispatches as follows:

1. An exact `STANLI2:` prefix selects this decoder.
2. Otherwise, after the four ASCII whitespace bytes are skipped, `(` selects
   the legacy S-expression reader.
3. Every other input is rejected.

Once the v2 prefix is seen, malformed base64 or a malformed payload is a hard
v2 error. The decoder never retries another format. `STANLI3:` and the former
JSON envelope are unknown formats rather than near matches.

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

`depth` is the array nesting depth. Unknown leaf tags are errors.

### Function library

| Tag | Meaning |
| ---: | --- |
| 0 | `StanLib` |
| 1 | `Internal` |
| 2 | `UserDefined` |

### Expression kind

Every expression begins with a kind tag and its active fields, then ends with
the common metadata suffix shown below.

| Tag | Kind | Active fields after tag |
| ---: | --- | --- |
| 0 | `Var` | `string name` |
| 1 | `LitInt` | `i32 value` |
| 2 | `LitReal` | `f64 value` |
| 3 | `LitStr` | `string value` |
| 4 | `FunApp` | `u8 library`, `string name`, `bool propto`, `list<Expr> args` |
| 5 | `Promotion` | `list<Expr> args` |
| 6 | `Indexed` | `list<Expr> args` |
| 7 | `TernaryIf` | `list<Expr> args` |
| 8 | `EOr` | `list<Expr> args` |
| 9 | `EAnd` | `list<Expr> args` |
| 10 | `Unsupported` | none |

The common suffix is:

```text
string type
UnsizedView unsized
bool data_only
bool promoted
string raw
```

For `LitInt`, the C++ compatibility field `Expr.lit` is reconstructed as the
exact `double` conversion of the decoded `i32`; it is not redundantly encoded.
The producer currently flattens stanc3 `Promotion` nodes into the promoted
inner expression and sets `promoted`, matching the legacy structural mapping.
Tag 5 remains defined for direct runtime inputs and validation coverage.

Synthetic index descriptors are ordinary `FunApp` values inside an `Indexed`
argument list. Their canonical names and arities are validated after decode:

| Name | Argument count |
| --- | ---: |
| `IndexAll` | 0 |
| `IndexSingle` | 1 |
| `IndexUpfrom` | 1 |
| `IndexBetween` | 2 |
| `IndexMulti` | 1 |

They use library tag 0 and default function metadata.

### Transform kind

A transform is:

```text
u8 kind
list<Expr> args
string raw
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
| 16 | `Unsupported` |

Argument arities are checked against the selected kind.

### `SizedType`

```text
string base
list<Expr> dims
string elem_base
string raw
```

`base` uses the runtime spellings such as `SInt`, `SReal`, `SVector`,
`SRowVector`, `SMatrix`, and `SArray`. `elem_base` identifies the innermost
element for an array and is empty otherwise. Dimension counts are validated
against the base.

### Statement kind

Each statement starts with one tag and then only fields active for that kind.
There is no inactive-field block.

| Tag | Kind | Active fields after tag |
| ---: | --- | --- |
| 0 | `Decl` | `string id`, `SizedType type`, `bool data_only`, `bool has_init`, optional `Expr init`, `optional<Transform> read_transform`, `list<Expr> read_dims`, `string raw` |
| 1 | `Assignment` | `string lhs`, `list<Expr> lhs_indices`, `Expr rhs`, `string raw` |
| 2 | `TargetPE` | `Expr target`, `string raw` |
| 3 | `Block` | `list<Stmt> body`, `string raw` |
| 4 | `SList` | `list<Stmt> body`, `string raw` |
| 5 | `For` | `string loopvar`, `Expr lower`, `Expr upper`, `list<Stmt> body`, `string raw` |
| 6 | `IfElse` | `Expr condition`, `list<Stmt> body`, `string raw` |
| 7 | `While` | `Expr condition`, `list<Stmt> body`, `string raw` |
| 8 | `NRFunApp` | `string name`, `list<Expr> args`, `optional<Transform> payload_transform`, `string check_var_name`, `string raw` |
| 9 | `Return` | `bool has_value`, optional `Expr value`, `string raw` |
| 10 | `Break` | none |
| 11 | `Continue` | none |
| 12 | `Skip` | `string raw` |
| 13 | `Unsupported` | `string raw` |

The `IfElse` body has one or two statements in then/else order. `For` and
`While` each have exactly one body statement. `FnCheck` and `FnWriteParam`
payload fields follow the same structural conventions as the legacy mapping.

### `FunDef`

```text
string name
list<string> arg_names
list<string> arg_types
list<UnsizedView> arg_views
list<bool> arg_data_only
list<Stmt> body
```

The four argument lists must have identical lengths. `arg_types` preserves the
complete stanc3 unsized-type spelling; `arg_views` is the runtime's redundant
structural view and must agree with it. Producer-side function names and
user-defined calls are unsuffixed. The shared finalizer appends overload
signatures and rejects collisions exactly once.

### `Program`

The root payload is the following sequence with no wrapper object:

```text
list<InputVar> input_vars
list<Stmt> prepare_data
list<Stmt> log_prob
list<Stmt> generate_quantities
list<FunDef> fun_defs
list<string> output_vars
list<Stmt> transform_inits   (optional; see below)
```

An `InputVar` is `string name` followed by `SizedType type`.

`payload_transform` is the one optional-transform slot an `NRFunApp` carries.
Its meaning follows the internal function name, and the decoder splits the two
apart so no consumer has to ask: on `FnCheck` it is the constraint being
verified; on the `FnWriteParam` of `transform_inits` it is the transform to
INVERT, turning an already-constrained value into a free one. The
`FnWriteParam`s of `generate_quantities` carry none -- their values are
constrained already.

`transform_inits` is stanc3's inverse parameter-transform section: its
`FnReadData` calls name PARAMETERS read from a caller-supplied init context
rather than model data, and each parameter ends in an `FnWriteParam` carrying
the transform to invert.

It is the only optional element of the payload, and it is optional in exactly
one direction. It trails every required section, so a document written by a
producer from before it existed simply ends after `output_vars` and decodes
with an empty `transform_inits` -- the same state as a model whose section
could not be encoded, which callers already have to handle. A decoder must not
treat its absence as truncation, and must still reject bytes beyond it. The
current producer always emits it, empty list included, so the canonical bytes
for a given input remain unique.

The order of every list is semantic and preserved. Source locations and
stanc3 fields absent from `stanli::mir` are not encoded.

## Unsupported and opaque payloads

The producer must not silently omit or reinterpret an unsupported construct.
Expressions, statements, and transforms use their `Unsupported` tag; a sized
type retains its exact unrecognized `base`. Each carries a complete
deterministic opaque spelling in `raw`. Supported nodes may also carry `raw`
when the legacy mapping uses an internal payload. The decoder preserves those
bytes; lowering either consumes the supported payload or reports an
unsupported-form error.

`raw` is diagnostic/compatibility data rather than executable syntax. It is
still included in canonical byte equality so native OCaml, js_of_ocaml, and
Windows builds cannot drift unnoticed.

## Validation and finalization

After structural decoding and before a `Program` is exposed, the runtime:

1. checks expression, index, transform, sized-type, statement, and known
   function arities;
2. checks the redundant type/view metadata used by the lowerer;
3. checks function argument list lengths and type/view agreement;
4. resolves user-defined overload names and rejects final-name collisions;
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

1. the exact `STANLI2:` prefix;
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
must never reuse `STANLI2:` with different tag meanings or field order.
Appending an optional trailing section, as `transform_inits` did, is the one
change that keeps the header: it cannot alter how any earlier byte is read.
