# Stanli portable MIR v1

This document defines the stanli-owned, versioned wire representation of the
MIR slice consumed by the C++ runtime.  Version 1 is a deterministic UTF-8 JSON
re-tagging of the fields in `stanli::mir`; it is not a serialization of every
stanc3 MIR field and it is not an API commitment from stanc3.

The first producer is expected to consume stanc3's `Program.Typed.t`.  Its
output must have the same meaning and field values as the structural portion of
`runtime/src/mir_reader.cpp`, before that reader resolves overloaded user
functions and validates bindings. The portable and legacy readers route
through that shared C++ finalizer exactly once after structural decoding;
portable v1 additionally enables its redundant variable-metadata checks.

Version 1 deliberately does not normalize expressions, reconstruct source
syntax, sort declarations, or replace syntactic conventions with higher-level
ones.  In particular, the exact names and expression shapes used by stanc3 are
part of the payload.

## Envelope and format detection

The document is one JSON object:

```json
{
  "stanli_ir": 1,
  "program": { "...": "Program" }
}
```

Both members are required.  `stanli_ir` is simultaneously the format marker
and the version number.  It must be the JSON integer `1`, not the string `"1"`
or the JSON number `1.0`.  No other top-level members are permitted in v1.

A transport that accepts both this format and legacy MIR S-expressions uses
the first non-whitespace byte for dispatch:

- `{` selects portable JSON;
- `(` selects the legacy S-expression reader; and
- every other byte is an error.

Only the four RFC 8259 whitespace bytes, space (`0x20`), tab (`0x09`), LF
(`0x0a`), and CR (`0x0d`), may be skipped for this test.  A UTF-8 BOM is not
accepted.  Once `{` selects this reader, a malformed document, a missing
`stanli_ir`, or an unsupported version is a hard portable-format error.  The
implementation must not retry the legacy reader.

Canonical output begins immediately with
`{"stanli_ir":1,"program":` and contains no leading or trailing whitespace.

## General decoding rules

The type descriptions below use formatted JSON for readability.  JSON object
member order is not semantically significant to a decoder, but canonical
encoders emit members in the order shown.

Unless a type says otherwise:

- every listed member is required, including fields inactive for the selected
  kind;
- `null` is forbidden;
- strings must decode to valid Unicode scalar values and be represented as
  UTF-8;
- arrays preserve their source order and may be empty;
- duplicate object member names are errors;
- unknown object members are errors;
- unknown enum strings are errors; and
- a missing field is never filled with an inferred default.

The two C++ `std::optional<Transform>` fields are represented by required JSON
members whose values are either a `Transform` object or `null`.  These are the
only nullable values in v1.

All names are case-sensitive and byte-preserving after JSON string decoding.
The encoder and decoder must not qualify, demangle, case-fold, Unicode-normalize,
or otherwise rewrite them.

## Scalar encodings

### C++ `long`

`Expr.lit_i` is a JSON string in this grammar:

```text
digit         = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"
nonzero-digit = "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"
int32         = "0" | ["-"] nonzero-digit *digit
```

A leading `+`, a leading zero, and `-0` are forbidden.  The mathematical value
must be in `[-2147483648, 2147483647]`.  Using a string avoids loss through a
JavaScript JSON number and the common 32-bit/64-bit difference in C++ `long`.
The range is the portable Stan integer range and is representable by `long` on
all target platforms.

### C++ `double`

`Expr.lit` is a JSON string containing the exact IEEE-754 binary64 payload:

```text
lower-hex-digit = digit | "a" | "b" | "c" | "d" | "e" | "f"
f64-bits       = "f64:" 16lower-hex-digit
```

The digits encode the unsigned 64-bit bit pattern most-significant nibble
first.  For example:

```text
0.0   -> "f64:0000000000000000"
-0.0  -> "f64:8000000000000000"
1.0   -> "f64:3ff0000000000000"
+Inf  -> "f64:7ff0000000000000"
-Inf  -> "f64:fff0000000000000"
```

Every bit pattern is permitted, including infinities and every NaN sign,
quiet/signaling bit, and payload.  A decoder reconstructs the value by a bit
copy, never by locale-sensitive numeric parsing.  A producer obtains the bits
from the binary64 value after the same stanc3 computation that the legacy
reader observes; it must not re-evaluate or decimal-round the expression.

The target must provide an IEC 60559/IEEE-754 binary64 `double`.  A target that
does not must reject the document.

### JSON strings

Strings are values, not identifiers in a separate symbol table.  Embedded NUL
and other control characters are allowed through JSON escapes.  Invalid UTF-8,
unpaired UTF-16 surrogate escapes, or a decoded non-scalar Unicode value is an
error.  No Unicode normalization is performed.

## `UnsizedView`

`UnsizedView` is encoded as:

```json
{
  "depth": 0,
  "leaf": "Unknown"
}
```

Both fields are required.

- `depth` is a JSON integer from 0 through 255, matching the C++ `uint8_t`.
- `leaf` is exactly one of `"Unknown"`, `"Int"`, `"Real"`, `"Complex"`,
  `"Vector"`, `"RowVector"`, or `"Matrix"`.

The view is structural.  Array nesting is carried only by `depth`; the leaf
does not change for an array.

## `Expr`

Every expression contains every C++ `Expr` field:

```json
{
  "kind": "Unsupported",
  "name": "",
  "fn_lib": "StanLib",
  "fn_propto": false,
  "lit_i": "0",
  "lit": "f64:0000000000000000",
  "lit_s": "",
  "args": [],
  "type_": "",
  "unsized": { "depth": 0, "leaf": "Unknown" },
  "data_only": false,
  "promoted": false,
  "raw": ""
}
```

The `kind` value is exactly one of:

```text
Var
LitInt
LitReal
LitStr
FunApp
Promotion
Indexed
TernaryIf
EOr
EAnd
Unsupported
```

The `fn_lib` value is exactly one of `"StanLib"`, `"Internal"`, or
`"UserDefined"`.

Field meanings are direct:

| Field | C++ field and v1 meaning |
| --- | --- |
| `kind` | `Expr::kind`, using the exact enumerator spelling above. |
| `name` | `Expr::name`: a variable name or function/internal operation name. |
| `fn_lib` | `Expr::fn_lib`. It remains present for non-`FunApp` expressions. |
| `fn_propto` | `Expr::fn_propto`, including `_lupdf`/`_lupmf` calls. |
| `lit_i` | `Expr::lit_i`, encoded by the `int32` grammar. |
| `lit` | The exact bits of `Expr::lit`, encoded by `f64-bits`. |
| `lit_s` | `Expr::lit_s`, without reinterpreting the Stan string. |
| `args` | `Expr::args`, in order. |
| `type_` | `Expr::type_`, including exact spellings such as `UInt`, `UReal`, `UVector`, `URowVector`, `UMatrix`, and `UArray`. |
| `unsized` | `Expr::unsized`. |
| `data_only` | `Expr::data_only`. |
| `promoted` | `Expr::promoted`. |
| `raw` | `Expr::raw`, an opaque diagnostic payload copied verbatim. |

For a `LitInt`, `lit_i` contains the integer and `lit` contains the exact
binary64 conversion of that same int32, matching the legacy reader.  For a
`LitReal`, `lit` contains the parsed or folded binary64 value.  Inactive literal
fields have their C++ defaults: `"0"`, positive-zero bits, and `""`.

The current reader represents a stanc3 `Promotion` transparently: it retains
the inner expression's `kind` and fields, applies the outer metadata, and sets
`promoted` to `true`.  A v1 producer must reproduce that representation.  It
must not introduce a `kind: "Promotion"` wrapper for a stanc3 promotion.
`"Promotion"` remains a legal enum value because it exists in the C++ type,
but current input does not generate it.

An `Indexed` expression stores its base in `args[0]`, followed by synthetic
`FunApp` expressions describing each index.  Their `name` values are preserved
exactly:

| Index name | `args` layout |
| --- | --- |
| `IndexAll` | empty |
| `IndexSingle` | one index expression |
| `IndexBetween` | lower, then upper |
| `IndexMulti` | one index expression |
| `IndexUpfrom` | one lower-bound expression |

These are representation facts, not invitations to replace the names with a
new JSON index shape.

The portable decoder rejects kind-specific structural mismatches before
shared MIR finalization. `Indexed` always has a base; it may contain no index
descriptors when upstream transformations retain a no-op wrapper. `TernaryIf`
has three arguments; `EOr` and `EAnd` have two; and an explicit `Promotion`
wrapper has one. Each synthetic index descriptor that is present must use one
of the five names and exact argument layouts above. Recognized `type_`
spellings must also agree with `unsized`: a non-array uses its matching leaf
spelling, while any positive array depth uses `"UArray"`.

Recognized functions whose runtime consumers address arguments positionally
must also have a language-valid argument count. In particular,
`wiener_lpdf` accepts either its five-argument or seven-argument language form;
other counts are malformed. Decoding preserves both forms even when a given
execution backend subsequently reports that it implements only one of them.

Likewise, compiler-internal and operator names remain untouched.  Examples
whose syntax is inspected downstream include `FnReadParam`, `FnCheck`,
`FnWriteParam`, `PNot__`, `EOr`, `Transpose__`, `Minus__`, and
`emit_generated_quantities__`.  The list is not exhaustive: every value of
`name` is opaque and exact.

## `SizedType`

Every sized type is encoded as:

```json
{
  "base": "",
  "dims": [],
  "elem_base": "",
  "raw": ""
}
```

All four members are required.

- `base` is `SizedType::base`, preserving tags such as `SInt`, `SReal`,
  `SComplex`, `SVector`, `SRowVector`, `SMatrix`, and `SArray` exactly.
- `dims` is `SizedType::dims`.  For nested `SArray` values it is outer-to-inner,
  followed by the leaf vector or matrix dimensions, exactly as constructed by
  the existing reader.
- `elem_base` is `SizedType::elem_base`.  For `SArray`, it is the innermost
  element base.  It remains the empty string where the C++ reader leaves it
  empty.
- `raw` is the complete opaque diagnostic payload.  It may be nonempty for a
  usable scalar temporary whose stanc3 declaration was unsized; nonempty
  `raw` therefore does not by itself mean that the type is unsupported.

There is no enum restriction on `base`: an unrecognized upstream tag is
preserved there and accompanied by nonempty `raw`, allowing the existing
lowering to issue its unsupported-sized-type diagnostic.

## `Transform`

Every transform is encoded as:

```json
{
  "kind": "Identity",
  "args": [],
  "raw": ""
}
```

All three members are required.  `kind` is exactly one of:

```text
Identity
Lower
Upper
LowerUpper
Offset
Multiplier
OffsetMultiplier
Simplex
Ordered
PositiveOrdered
CholeskyCorr
UnitVector
SumToZero
Correlation
Covariance
CholeskyCov
Unsupported
```

`args` preserves the current argument order.  In current MIR, `Lower`, `Upper`,
`Offset`, and `Multiplier` have one argument; `LowerUpper` and
`OffsetMultiplier` have two; and the remaining supported tags have none.
Malformed arities are not repaired by the wire decoder.

`raw` is copied verbatim and is never interpreted.  The legacy reader records
the original atom in `raw` even for a supported atomic transform, so a producer
that is replacing that reader records the same complete spelling.  It must not
assume that only `Unsupported` transforms carry `raw`.

## `Stmt`

Every statement contains every C++ `Stmt` field:

```json
{
  "kind": "Unsupported",
  "decl_id": "",
  "decl_type": {
    "base": "",
    "dims": [],
    "elem_base": "",
    "raw": ""
  },
  "decl_data_only": false,
  "has_init": false,
  "init": { "...": "Expr" },
  "read_transform": null,
  "read_dims": [],
  "lhs": "",
  "lhs_idx": [],
  "rhs": { "...": "Expr" },
  "target": { "...": "Expr" },
  "fn_name": "",
  "fn_args": [],
  "check_transform": null,
  "check_var_name": "",
  "loopvar": "",
  "lower": { "...": "Expr" },
  "upper": { "...": "Expr" },
  "cond": { "...": "Expr" },
  "body": [],
  "raw": ""
}
```

The abbreviated expression objects above stand for complete `Expr` objects;
abbreviation is not permitted on the wire.  `kind` is exactly one of:

```text
Decl
Assignment
TargetPE
Block
SList
For
IfElse
While
NRFunApp
Return
Skip
Unsupported
```

The fields active for current statement kinds are:

| Kind | Active fields |
| --- | --- |
| `Decl` | `decl_id`, `decl_type`, `decl_data_only`, `has_init`, `init`, `read_transform`, `read_dims` |
| `Assignment` | `lhs`, `lhs_idx`, `rhs` |
| `TargetPE` | `target` |
| `Block` | `body` |
| `SList` | `body` |
| `For` | `loopvar`, `lower`, `upper`, `body` (one statement) |
| `IfElse` | `cond`, `body` (then, followed by optional else) |
| `While` | `cond`, `body` (one statement) |
| `NRFunApp` | `fn_name`, `fn_args`, and for `FnCheck`, `check_transform` and `check_var_name` |
| `Return` | `has_init`, and `rhs` when `has_init` is true |
| `Skip` | no kind-specific fields |
| `Unsupported` | `raw` |

`For` and `While` contain exactly one body statement. `IfElse` contains one
or two (then, followed by the optional else). Other non-container statement
kinds have an empty `body`; only `Block` and `SList` may contain an arbitrary
statement list.

Inactive fields are still required and carry the values that the C++ struct
would contain, normally their default-constructed values.  They are not
discarded or repurposed.  In particular, `has_init` is shared by `Decl` and
`Return`, exactly as in the C++ representation.

For `FnReadParam`, `read_transform` and `read_dims` preserve the transform and
dimension payload that the legacy reader extracts from the internal function
node.  For `FnCheck`, the checked value remains first in `fn_args`, followed by
its bound expressions; `check_transform` carries the relation and
`check_var_name` carries the exact payload name.  `FnWriteParam` likewise keeps
the value extracted from its internal payload as the first `fn_args` element.

Statement shape must be retained exactly.  For example, the write-array section
boundaries are currently recovered from `IfElse` conditions containing
`PNot__`, `EOr`, `emit_transformed_parameters__`, and
`emit_generated_quantities__`.  V1 does not replace those idioms with section
markers.

## `FunDef`

Every user-function definition is encoded as:

```json
{
  "name": "",
  "arg_names": [],
  "arg_types": [],
  "arg_views": [],
  "arg_data_only": [],
  "body": []
}
```

All six members are required.

- `name` is the original `fdname` from stanc3.  The producer must not append the
  C++ overload signature.
- `arg_names` preserves argument names in declaration order.
- `arg_types` preserves the complete unsized type spellings used by the current
  representation, such as `UReal` or `(UArray UReal)`.  The string is opaque;
  it is not regenerated from `arg_views`.
- `arg_views` contains one `UnsizedView` per argument.
- `arg_data_only` contains one JSON boolean per argument.
- `body` preserves statement order.

The four argument arrays must have the same length.  This is a structural
constraint, not a license for a decoder to truncate longer arrays.
For each recognized scalar/container/array spelling in `arg_types`, the exact
nested `UArray` spelling must agree with the corresponding `arg_views` leaf
and depth.

stanc3 can emit several definitions with the same `fdname`.  Their exact
original names and exact `UserDefined` call names travel on the wire.  After
decoding, the shared C++ finalizer performs the existing overload selection,
signature suffixing, and call rewriting once.  A document containing names
that were already signature-suffixed is not a conforming v1 producer output.

## `Program`

The `program` member is:

```json
{
  "input_vars": [],
  "prepare_data": [],
  "log_prob": [],
  "generate_quantities": [],
  "fun_defs": [],
  "output_vars": []
}
```

All six members are required and correspond one-for-one to the C++ `Program`
fields in the order shown.

Each `input_vars` element represents the C++ pair as:

```json
{
  "name": "N",
  "type": {
    "base": "SInt",
    "dims": [],
    "elem_base": "",
    "raw": ""
  }
}
```

Both `name` and `type` are required and no other members are permitted.

`prepare_data`, `log_prob`, and `generate_quantities` are arrays of `Stmt`.
`fun_defs` is an array of `FunDef`.  `output_vars` is an array of strings.
Declaration, definition, statement, dimension, argument, and output-variable
order is significant and must never be sorted or deduplicated.

As in the current C++ representation, `generate_quantities` is the entire
write-array body, including parameter reads, transformed-parameter work,
generated quantities, guards, and writes.  `output_vars` contains exact names
in `FnWriteParam` emission order.  The encoder must not split, prune, or
reassemble this body.

## Defaults and inactive fields

The all-default expression shown in the `Expr` section is the value embedded in
inactive `Stmt` expression fields.  The all-default sized type and empty arrays
and strings likewise match ordinary C++ value initialization.  A conforming
producer emits those defaults when the legacy structural reader would leave a
field untouched.

This redundancy is intentional.  V1 mirrors the C++ data type rather than
creating a second kind-dependent model whose defaults and reconstruction logic
could drift.  A decoder copies every field before running finalization; it does
not erase inactive fields.

## Unsupported and opaque payloads

An upstream expression, statement, sized type, or transform variant that
cannot be represented by a supported v1 kind must use the corresponding
existing unsupported channel:

- `Expr.kind = "Unsupported"` and `Expr.raw`;
- `Stmt.kind = "Unsupported"` and `Stmt.raw`;
- `Transform.kind = "Unsupported"` and `Transform.raw`; or
- the exact unknown `SizedType.base` and `SizedType.raw`.

The `raw` value is the complete, deterministic, opaque spelling of the source
payload.  The producer must not truncate it.  The decoder copies it verbatim,
and semantic consumers must use it only in diagnostics.  The legacy reader's
short diagnostic `dump` budgets do not apply to portable v1; preserving more
diagnostic text does not change program meaning.

An empty `raw` is valid for a default-constructed inactive field.  A producer
using an unsupported channel for an actual upstream node must provide a
nonempty `raw` value so failure is attributable.  `raw` can also be nonempty on
supported values, so consumers must dispatch on the enum/base rather than on
whether `raw` is empty.

If a new upstream construct occurs at a level that has no corresponding C++
unsupported channel, the producer must fail compilation clearly and emit no
portable document.  It must never silently omit the construct, approximate it,
or reuse a superficially similar tag.

An unknown JSON enum spelling is not an implicit `Unsupported`; it is malformed
v1 and must be rejected.  Producers explicitly select the known
`"Unsupported"` spelling and supply `raw`.

## Finalization and semantic invariants

After a complete structural decode, the portable reader first validates the
closed-world expression, statement, transform, sized-type, and redundant type
metadata rules described above. It then runs the same finalization used by the
legacy path, with the portable metadata checks enabled:

1. resolve overloaded user-defined function names and calls;
2. construct function and variable bindings; and
3. run the existing selected binding, type-view, ad-level, UDF-arity, and
   `FnCheck` validation.

This finalization runs exactly once.  It is not part of canonical JSON
generation and must be shared rather than independently reimplemented for the
two wire formats.  Failure rejects the entire document; no partial program is
returned.

Derived classifications such as `ProdGrouping`, `ExtremaKind`, and `EmitGuard`
are not serialized fields.  They are recomputed from the exact decoded
expression and statement shapes by the existing C++ helpers.

## Unknown versions and fields

Version 1 is closed-world:

- a `stanli_ir` value other than integer `1` is rejected before decoding
  `program`;
- a missing required member is rejected;
- an extra member at any object level is rejected;
- a duplicate member is rejected even when the duplicate values agree; and
- an unknown enum string is rejected.

A future producer that needs new fields, tags, or compatibility behavior must
use a new version.  A v1 reader must not guess that a later version is backward
compatible and must not partially decode it.

## Resource limits

A v1 reader checks the input-byte limit before parsing and checks nesting,
value, array, and object limits as the parser constructs the document. String
limits are checked as soon as each JSON token has been decoded; the input-byte
limit bounds the parser's temporary allocation for that token. Each limit is
independent; exceeding any one rejects the whole document.

| Resource | Maximum |
| --- | ---: |
| Input bytes, including leading/trailing whitespace | 268,435,456 (256 MiB) |
| JSON nesting depth | 512 |
| Aggregate JSON values | 10,000,000 |
| Elements in any one JSON array | 1,000,000 |
| Members in any one JSON object | 64 |
| Decoded UTF-8 bytes in any one string, including a member name | 16,777,216 (16 MiB) |
| Aggregate decoded UTF-8 bytes in strings, including member names | 268,435,456 (256 MiB) |

An aggregate JSON value count includes each object, array, string value,
number, boolean, and null value once; object member names are not separate
values for that count, although their decoded bytes do count toward both string
limits.  Nesting counts the top-level object as depth one.  Arithmetic used to
count, size, or allocate must be checked for overflow.

The type-level limits also apply: `UnsizedView.depth` is at most 255 and integer
literals are int32.  Resource rejection is a format error, not an opportunity
to return a truncated program or `raw` string.

## Canonical byte representation

All stanli compiler artifacts that emit portable MIR use the same canonical
encoder.  Canonical v1 bytes obey these rules:

1. Encode as UTF-8 with no BOM.
2. Emit no insignificant whitespace and no trailing newline.
3. Emit object members in the order shown in this document.  In particular,
   the envelope order is `stanli_ir`, then `program`; structure fields follow
   their C++ declaration order.
4. Preserve array order exactly.
5. Emit `stanli_ir` and `UnsizedView.depth` as the shortest unsigned decimal
   JSON integers.  No other v1 field uses a JSON number.
6. Emit booleans as `true` or `false` and absent transforms as `null`.
7. Emit `lit_i` and `lit` in their scalar canonical grammars above.
8. In JSON strings, escape quotation mark and reverse solidus as `\"` and
   `\\`.  Use `\b`, `\t`, `\n`, `\f`, and `\r` for those five controls; encode
   any other U+0000 through U+001F value as lowercase `\u00xx`.  Do not escape
   solidus.  Encode every other Unicode scalar value directly as UTF-8.
9. Do not normalize Unicode or alter opaque strings and names before applying
   JSON escaping.

A decoder may accept otherwise valid RFC 8259 whitespace and equivalent JSON
string escapes, subject to the limits and strict member/type rules above.
Re-encoding any accepted document produces the canonical bytes.

Native OCaml, JavaScript, and Windows compiler builds that emit v1 must produce
identical canonical bytes for the same stanc3 MIR and pass selection.  Exact
binary64 bits, complete opaque payloads, fixed field order, and explicit
inactive fields make that comparison independent of locale, platform newline
convention, JavaScript number precision, and JSON object-map iteration order.

## Complete field inventory

This checklist is normative and exists to make omissions visible during
implementation review.

- `UnsizedView`: `depth`, `leaf`.
- `Expr`: `kind`, `name`, `fn_lib`, `fn_propto`, `lit_i`, `lit`, `lit_s`,
  `args`, `type_`, `unsized`, `data_only`, `promoted`, `raw`.
- `Transform`: `kind`, `args`, `raw`.
- `SizedType`: `base`, `dims`, `elem_base`, `raw`.
- `Stmt`: `kind`, `decl_id`, `decl_type`, `decl_data_only`, `has_init`, `init`,
  `read_transform`, `read_dims`, `lhs`, `lhs_idx`, `rhs`, `target`, `fn_name`,
  `fn_args`, `check_transform`, `check_var_name`, `loopvar`, `lower`, `upper`,
  `cond`, `body`, `raw`.
- `FunDef`: `name`, `arg_names`, `arg_types`, `arg_views`, `arg_data_only`,
  `body`.
- `Program`: `input_vars`, `prepare_data`, `log_prob`,
  `generate_quantities`, `fun_defs`, `output_vars`.

Source locations and stanc3 fields absent from these C++ types are not present
on the v1 wire.  This is an encoding of the runtime's consumed MIR slice, not a
general-purpose stanc3 archive.
