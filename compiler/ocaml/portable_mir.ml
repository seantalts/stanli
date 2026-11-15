open Std
open Middle

type unsized_view = {depth: int; leaf: string}

type wire_expr =
  { e_kind: string
  ; e_name: string
  ; e_fn_lib: string
  ; e_fn_propto: bool
  ; e_lit_i: string
  ; e_lit: float
  ; e_lit_s: string
  ; e_args: wire_expr list
  ; e_type: string
  ; e_unsized: unsized_view
  ; e_data_only: bool
  ; e_promoted: bool
  ; e_raw: string }

type wire_transform = {t_kind: string; t_args: wire_expr list; t_raw: string}

type wire_sized =
  {s_base: string; s_dims: wire_expr list; s_elem_base: string; s_raw: string}

type wire_stmt =
  { st_kind: string
  ; st_decl_id: string
  ; st_decl_type: wire_sized
  ; st_decl_data_only: bool
  ; st_has_init: bool
  ; st_init: wire_expr
  ; st_read_transform: wire_transform option
  ; st_read_dims: wire_expr list
  ; st_lhs: string
  ; st_lhs_idx: wire_expr list
  ; st_rhs: wire_expr
  ; st_target: wire_expr
  ; st_fn_name: string
  ; st_fn_args: wire_expr list
  ; st_check_transform: wire_transform option
  ; st_check_var_name: string
  ; st_loopvar: string
  ; st_lower: wire_expr
  ; st_upper: wire_expr
  ; st_cond: wire_expr
  ; st_body: wire_stmt list
  ; st_raw: string }

type wire_fun =
  { f_name: string
  ; f_arg_names: string list
  ; f_arg_types: string list
  ; f_arg_views: unsized_view list
  ; f_arg_data_only: bool list
  ; f_body: wire_stmt list }

let unsupported_view = {depth= 0; leaf= "Unknown"}

let default_expr () =
  { e_kind= "Unsupported"
  ; e_name= ""
  ; e_fn_lib= "StanLib"
  ; e_fn_propto= false
  ; e_lit_i= "0"
  ; e_lit= 0.
  ; e_lit_s= ""
  ; e_args= []
  ; e_type= ""
  ; e_unsized= unsupported_view
  ; e_data_only= false
  ; e_promoted= false
  ; e_raw= "" }

let default_sized () = {s_base= ""; s_dims= []; s_elem_base= ""; s_raw= ""}

let default_stmt () =
  { st_kind= "Unsupported"
  ; st_decl_id= ""
  ; st_decl_type= default_sized ()
  ; st_decl_data_only= false
  ; st_has_init= false
  ; st_init= default_expr ()
  ; st_read_transform= None
  ; st_read_dims= []
  ; st_lhs= ""
  ; st_lhs_idx= []
  ; st_rhs= default_expr ()
  ; st_target= default_expr ()
  ; st_fn_name= ""
  ; st_fn_args= []
  ; st_check_transform= None
  ; st_check_var_name= ""
  ; st_loopvar= ""
  ; st_lower= default_expr ()
  ; st_upper= default_expr ()
  ; st_cond= default_expr ()
  ; st_body= []
  ; st_raw= "" }

let sexp_string sexp =
  let buffer = Buffer.create 128 in
  let rec write = function
    | Sexplib0.Sexp.Atom atom ->
        Buffer.add_string buffer
          (Sexplib0.Sexp.to_string_mach (Sexplib0.Sexp.Atom atom))
    | List children ->
        Buffer.add_char buffer '(';
        List.iteri children ~f:(fun index child ->
            if index > 0 then Buffer.add_char buffer ' ';
            write child);
        Buffer.add_char buffer ')' in
  write sexp;
  Buffer.contents buffer

let raw_unsized t = sexp_string (UnsizedType.sexp_of_t t)

let raw_expr_pattern pattern =
  sexp_string (Expr.Pattern.sexp_of_t Expr.Typed.sexp_of_t pattern)

let raw_internal internal =
  sexp_string (Internal_fun.sexp_of_t Expr.Typed.sexp_of_t internal)

let raw_fun_kind kind =
  sexp_string (Fun_kind.sexp_of_t Expr.Typed.sexp_of_t kind)

let raw_transform transform =
  sexp_string (Transformation.sexp_of_t Expr.Typed.sexp_of_t transform)

let raw_sized sized =
  sexp_string (SizedType.sexp_of_t Expr.Typed.sexp_of_t sized)

let raw_type typ = sexp_string (Type.sexp_of_t Expr.Typed.sexp_of_t typ)

let raw_stmt_pattern pattern =
  sexp_string
    (Stmt.Pattern.sexp_of_t Expr.Typed.sexp_of_t Stmt.Located.sexp_of_t pattern)

let is_data_only = function UnsizedType.DataOnly -> true | _ -> false

let unsized_view_and_type typ =
  let rec unwind depth = function
    | UnsizedType.UArray inner ->
        if depth = 255 then
          invalid_arg "portable MIR: unsized array nesting exceeds 255";
        unwind (depth + 1) inner
    | leaf -> (depth, leaf) in
  let depth, leaf_type = unwind 0 typ in
  let leaf, atom =
    match leaf_type with
    | UnsizedType.UInt -> ("Int", "UInt")
    | UReal -> ("Real", "UReal")
    | UComplex -> ("Complex", "UComplex")
    | UVector -> ("Vector", "UVector")
    | URowVector -> ("RowVector", "URowVector")
    | UMatrix -> ("Matrix", "UMatrix")
    | UComplexVector | UComplexRowVector | UComplexMatrix | UTuple _ | UFun _
     |UMathLibraryFunction ->
        ("Unknown", "")
    | UArray _ -> assert false in
  let view = {depth; leaf} in
  if String.equal leaf "Unknown" then (view, "", raw_unsized typ)
  else (view, (if depth = 0 then atom else "UArray"), "")

let canonical_int32 spelling =
  try Int32.of_string spelling |> Int32.to_string
  with _ -> invalid_arg ("portable MIR: invalid int32 literal " ^ spelling)

let float_of_literal spelling =
  try Float.of_string spelling
  with _ -> invalid_arg ("portable MIR: invalid real literal " ^ spelling)

let internal_name : Expr.Typed.t Internal_fun.t -> string = function
  | Internal_fun.FnLength -> "FnLength"
  | FnMakeArray -> "FnMakeArray"
  | FnMakeTuple -> "FnMakeTuple"
  | FnMakeRowVec -> "FnMakeRowVec"
  | FnNegInf -> "FnNegInf"
  | FnReadData -> "FnReadData"
  | FnReadDeserializer -> "FnReadDeserializer"
  | FnReadParam _ -> "FnReadParam"
  | FnWriteParam _ -> "FnWriteParam"
  | FnValidateSize -> "FnValidateSize"
  | FnValidateSizePositive -> "FnValidateSizePositive"
  | FnValidateSizeUnitVector -> "FnValidateSizeUnitVector"
  | FnCheck _ -> "FnCheck"
  | FnPrint -> "FnPrint"
  | FnReject -> "FnReject"
  | FnFatalError -> "FnFatalError"
  | FnResizeToMatch -> "FnResizeToMatch"
  | FnNaN -> "FnNaN"
  | FnDeepCopy -> "FnDeepCopy"
  | FnToRef -> "FnToRef"
  | FnReturnSoA _ -> "FnReturnSoA"
  | FnReadWriteEventsOpenCL _ -> "FnReadWriteEventsOpenCL"

let internal_has_payload : Expr.Typed.t Internal_fun.t -> bool = function
  | Internal_fun.FnReadParam _ | FnWriteParam _ | FnCheck _
   |FnReadWriteEventsOpenCL _ | FnReturnSoA _ ->
      true
  | FnLength | FnMakeArray | FnMakeTuple | FnMakeRowVec | FnNegInf
   |FnReadData | FnReadDeserializer | FnValidateSize | FnValidateSizePositive
   |FnValidateSizeUnitVector | FnPrint | FnReject | FnFatalError
   |FnResizeToMatch | FnNaN | FnDeepCopy | FnToRef ->
      false

let propto = function
  | Fun_kind.FnDist (_, Unnormalized) -> true
  | FnPlain | FnRng | FnDist (_, Normalized) | FnTarget | FnJacobian -> false

(* Operators keep the StanLib spelling they had before stanc3 gave them their
   own function kind, so portable MIR is unchanged. *)
let operator_name op = Sexplib0.Sexp.to_string (Operator.sexp_of_t op) ^ "__"

let rec expr_of_t (expr : Expr.Typed.t) =
  let open Expr.Pattern in
  let base =
    match expr.pattern with
    | Var name -> {(default_expr ()) with e_kind= "Var"; e_name= name}
    | Lit (Int, spelling) ->
        let canonical = canonical_int32 spelling in
        { (default_expr ()) with
          e_kind= "LitInt"
        ; e_lit_i= canonical
        ; e_lit= Int32.to_float (Int32.of_string canonical) }
    | Lit (Real, spelling) ->
        { (default_expr ()) with
          e_kind= "LitReal"
        ; e_lit= float_of_literal spelling }
    | Lit ((Imaginary | Str), spelling) ->
        {(default_expr ()) with e_kind= "LitStr"; e_lit_s= spelling}
    | FunApp (kind, args) -> fun_app_of_t kind args
    | Promotion (inner, _, _) -> {(expr_of_t inner) with e_promoted= true}
    | TernaryIf (cond, if_true, if_false) ->
        { (default_expr ()) with
          e_kind= "TernaryIf"
        ; e_args= List.map ~f:expr_of_t [cond; if_true; if_false] }
    | EOr (lhs, rhs) ->
        { (default_expr ()) with
          e_kind= "EOr"
        ; e_args= List.map ~f:expr_of_t [lhs; rhs] }
    | EAnd (lhs, rhs) ->
        { (default_expr ()) with
          e_kind= "EAnd"
        ; e_args= List.map ~f:expr_of_t [lhs; rhs] }
    | Indexed (base, indices) ->
        { (default_expr ()) with
          e_kind= "Indexed"
        ; e_args= expr_of_t base :: List.map ~f:index_of_t indices }
    | TupleProjection _ ->
        { (default_expr ()) with
          e_kind= "Unsupported"
        ; e_raw= raw_expr_pattern expr.pattern } in
  let view, type_, type_raw = unsized_view_and_type expr.meta.type_ in
  { base with
    e_type= type_
  ; e_unsized= view
  ; e_data_only= is_data_only expr.meta.adlevel
  ; e_raw= (if String.is_empty type_raw then base.e_raw else type_raw) }

and fun_app_of_t kind args =
  let args = List.map ~f:expr_of_t args in
  match kind with
  | Fun_kind.StanLib (name, suffix) ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= name
      ; e_fn_lib= "StanLib"
      ; e_fn_propto= propto suffix
      ; e_args= args }
  | Operator op ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= operator_name op
      ; e_fn_lib= "StanLib"
      ; e_args= args }
  | CompilerInternal internal ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= internal_name internal
      ; e_fn_lib= "Internal"
      ; e_args= args
      ; e_raw=
          (if internal_has_payload internal then raw_internal internal else "")
      }
  | UserDefined (name, suffix) ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= name
      ; e_fn_lib= "UserDefined"
      ; e_fn_propto= propto suffix
      ; e_args= args }

and index_of_t = function
  | Index.All -> {(default_expr ()) with e_kind= "FunApp"; e_name= "IndexAll"}
  | Single expr ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= "IndexSingle"
      ; e_args= [expr_of_t expr] }
  | Between (lower, upper) ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= "IndexBetween"
      ; e_args= List.map ~f:expr_of_t [lower; upper] }
  | MultiIndex expr ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= "IndexMulti"
      ; e_args= [expr_of_t expr] }
  | Upfrom expr ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= "IndexUpfrom"
      ; e_args= [expr_of_t expr] }

let rec sized_of_t sized =
  match sized with
  | SizedType.SInt -> {(default_sized ()) with s_base= "SInt"}
  | SReal -> {(default_sized ()) with s_base= "SReal"}
  | SComplex -> {(default_sized ()) with s_base= "SComplex"}
  | SVector (_, dim) ->
      {(default_sized ()) with s_base= "SVector"; s_dims= [expr_of_t dim]}
  | SRowVector (_, dim) ->
      {(default_sized ()) with s_base= "SRowVector"; s_dims= [expr_of_t dim]}
  | SMatrix (_, rows, cols) ->
      { (default_sized ()) with
        s_base= "SMatrix"
      ; s_dims= List.map ~f:expr_of_t [rows; cols] }
  | SArray (inner, dim) ->
      let inner = sized_of_t inner in
      { (default_sized ()) with
        s_base= "SArray"
      ; s_dims= expr_of_t dim :: inner.s_dims
      ; s_elem_base=
          (if String.equal inner.s_base "SArray" then inner.s_elem_base
           else inner.s_base) }
  | (SComplexVector _ | SComplexRowVector _ | SComplexMatrix _ | STuple _) as
    unsupported ->
      let sexp = raw_sized unsupported in
      let base =
        match unsupported with
        | SComplexVector _ -> "SComplexVector"
        | SComplexRowVector _ -> "SComplexRowVector"
        | SComplexMatrix _ -> "SComplexMatrix"
        | STuple _ -> "STuple"
        | _ -> assert false in
      {(default_sized ()) with s_base= base; s_raw= sexp}

let transform_of_t transform =
  let supported_atom kind = {t_kind= kind; t_args= []; t_raw= kind} in
  match transform with
  | Transformation.Identity -> supported_atom "Identity"
  | Simplex -> supported_atom "Simplex"
  | Ordered -> supported_atom "Ordered"
  | PositiveOrdered -> supported_atom "PositiveOrdered"
  | CholeskyCorr -> supported_atom "CholeskyCorr"
  | UnitVector -> supported_atom "UnitVector"
  | SumToZero -> supported_atom "SumToZero"
  | Correlation -> supported_atom "Correlation"
  | Covariance -> supported_atom "Covariance"
  | CholeskyCov -> supported_atom "CholeskyCov"
  | Lower arg -> {t_kind= "Lower"; t_args= [expr_of_t arg]; t_raw= ""}
  | Upper arg -> {t_kind= "Upper"; t_args= [expr_of_t arg]; t_raw= ""}
  | LowerUpper (lower, upper) ->
      { t_kind= "LowerUpper"
      ; t_args= List.map ~f:expr_of_t [lower; upper]
      ; t_raw= "" }
  | Offset arg -> {t_kind= "Offset"; t_args= [expr_of_t arg]; t_raw= ""}
  | Multiplier arg -> {t_kind= "Multiplier"; t_args= [expr_of_t arg]; t_raw= ""}
  | OffsetMultiplier (offset, multiplier) ->
      { t_kind= "OffsetMultiplier"
      ; t_args= List.map ~f:expr_of_t [offset; multiplier]
      ; t_raw= "" }
  | (StochasticRow | StochasticColumn | TupleTransformation _) as unsupported ->
      {t_kind= "Unsupported"; t_args= []; t_raw= raw_transform unsupported}

let decl_sized_of_type = function
  | Type.Sized sized -> sized_of_t sized
  | Unsized unsized as typ ->
      let base =
        match unsized with
        | UnsizedType.UInt -> "SInt"
        | UReal -> "SReal"
        | _ -> "" in
      {(default_sized ()) with s_base= base; s_raw= raw_type typ}

let read_param_fields (expr : Expr.Typed.t) =
  match expr.pattern with
  | Expr.Pattern.FunApp
      (Fun_kind.CompilerInternal (Internal_fun.FnReadParam fields), _) ->
      (Some (transform_of_t fields.constrain), List.map ~f:expr_of_t fields.dims)
  | _ -> (None, [])

let rec stmt_of_t (stmt : Stmt.Located.t) =
  let open Stmt.Pattern in
  match stmt.pattern with
  | Decl {decl_adtype; decl_id; decl_type; initialize} ->
      let initialized, init =
        match initialize with
        | Assign expr -> (true, expr_of_t expr)
        | Uninit | Default -> (false, default_expr ()) in
      let read_transform, read_dims =
        match initialize with
        | Assign expr -> read_param_fields expr
        | Uninit | Default -> (None, []) in
      { (default_stmt ()) with
        st_kind= "Decl"
      ; st_decl_id= decl_id
      ; st_decl_type= decl_sized_of_type decl_type
      ; st_decl_data_only= is_data_only decl_adtype
      ; st_has_init= initialized
      ; st_init= init
      ; st_read_transform= read_transform
      ; st_read_dims= read_dims }
  | Assignment ((LVariable lhs, indices), _, rhs) ->
      { (default_stmt ()) with
        st_kind= "Assignment"
      ; st_lhs= lhs
      ; st_lhs_idx= List.map ~f:index_of_t indices
      ; st_rhs= expr_of_t rhs }
  | Assignment ((LTupleProjection _, _), _, _) ->
      { (default_stmt ()) with
        st_kind= "Unsupported"
      ; st_raw= raw_stmt_pattern stmt.pattern }
  | TargetPE expr ->
      {(default_stmt ()) with st_kind= "TargetPE"; st_target= expr_of_t expr}
  | Block body ->
      { (default_stmt ()) with
        st_kind= "Block"
      ; st_body= List.map ~f:stmt_of_t body }
  | SList body ->
      { (default_stmt ()) with
        st_kind= "SList"
      ; st_body= List.map ~f:stmt_of_t body }
  | For {loopvar; lower; upper; body} ->
      { (default_stmt ()) with
        st_kind= "For"
      ; st_loopvar= loopvar
      ; st_lower= expr_of_t lower
      ; st_upper= expr_of_t upper
      ; st_body= [stmt_of_t body] }
  | IfElse (cond, if_true, if_false) ->
      { (default_stmt ()) with
        st_kind= "IfElse"
      ; st_cond= expr_of_t cond
      ; st_body=
          stmt_of_t if_true
          :: Option.value_map if_false ~default:[] ~f:(fun s -> [stmt_of_t s])
      }
  | While (cond, body) ->
      { (default_stmt ()) with
        st_kind= "While"
      ; st_cond= expr_of_t cond
      ; st_body= [stmt_of_t body] }
  | Return expr ->
      { (default_stmt ()) with
        st_kind= "Return"
      ; st_has_init= Option.is_some expr
      ; st_rhs= Option.value_map expr ~default:(default_expr ()) ~f:expr_of_t }
  | Break -> {(default_stmt ()) with st_kind= "Break"}
  | Continue -> {(default_stmt ()) with st_kind= "Continue"}
  | Skip -> {(default_stmt ()) with st_kind= "Skip"}
  | NRFunApp (kind, args) -> nr_fun_app_of_t kind args
  | Profile (_, body) ->
      (* The profile name is only for stanc's own timing output, which
         stanli does not produce; unwrap to the plain block it wraps. *)
      { (default_stmt ()) with
        st_kind= "Block"
      ; st_body= List.map ~f:stmt_of_t body }
  | JacobianPE _ ->
      { (default_stmt ()) with
        st_kind= "Unsupported"
      ; st_raw= raw_stmt_pattern stmt.pattern }

and nr_fun_app_of_t kind args =
  let ordinary_args = List.map ~f:expr_of_t args in
  match kind with
  | Fun_kind.CompilerInternal internal ->
      let name = internal_name internal in
      (* One optional-transform slot serves both internals that carry one,
         disambiguated by the function name. FnCheck names the constraint it
         verifies; transform_inits' FnWriteParam names the transform to
         INVERT, turning an already-constrained value into a free one.
         write_array's own FnWriteParam carries none -- its value is
         constrained already -- so documents produced before transform_inits
         was encoded stay byte-identical. *)
      let payload_args, payload_transform, check_var_name =
        match internal with
        | Internal_fun.FnCheck {trans; var_name; var} ->
            ([expr_of_t var], Some (transform_of_t trans), var_name)
        | FnWriteParam {var; unconstrain_opt} ->
            ([expr_of_t var], Option.map ~f:transform_of_t unconstrain_opt, "")
        | _ -> ([], None, "") in
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= name
      ; st_fn_args= payload_args @ ordinary_args
      ; st_check_transform= payload_transform
      ; st_check_var_name= check_var_name }
  | StanLib (name, _) ->
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= name
      ; st_fn_args= ordinary_args }
  | Operator op ->
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= operator_name op
      ; st_fn_args= ordinary_args }
  | UserDefined _ ->
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= raw_fun_kind kind
      ; st_fn_args= ordinary_args }

let fun_of_t (definition : Stmt.Located.t Program.fun_def) =
  let arg_names, arg_types, arg_views, arg_data_only =
    List.fold_right definition.fdargs ~init:([], [], [], [])
      ~f:(fun (adlevel, name, typ) (names, types, views, levels) ->
        let view, _, _ = unsized_view_and_type typ in
        ( name :: names
        , raw_unsized typ :: types
        , view :: views
        , is_data_only adlevel :: levels )) in
  { f_name= definition.fdname
  ; f_arg_names= arg_names
  ; f_arg_types= arg_types
  ; f_arg_views= arg_views
  ; f_arg_data_only= arg_data_only
  ; f_body=
      Option.value_map definition.fdbody ~default:[] ~f:(fun s -> [stmt_of_t s])
  }

(* Portable MIR v2 is a canonical ASCII envelope around a small fixed-layout
   binary payload. The envelope keeps the existing native, subprocess, V8,
   worker, and C-string transports byte-safe; the payload keeps decoding
   allocation-light. Multibyte scalars are little-endian, strings and lists
   carry uint32 byte/item counts, and sum types carry one-byte tags followed
   only by fields active for that tag. *)

let add_u8 buffer value =
  if value < 0 || value > 0xff then invalid_arg "portable MIR v2: invalid u8";
  Buffer.add_char buffer (Stdlib.Char.chr value)

let add_u32 buffer value =
  if value < 0 || Int64.of_int value > 0xffff_ffffL then
    invalid_arg "portable MIR v2: invalid u32";
  for shift = 0 to 3 do
    add_u8 buffer ((value lsr (shift * 8)) land 0xff)
  done

let add_i32 buffer value =
  let value = Int32.of_string value in
  for shift = 0 to 3 do
    add_u8 buffer
      Int32.(to_int (logand (shift_right_logical value (shift * 8)) 0xffl))
  done

let add_i64_bits buffer value =
  let value = Int64.bits_of_float value in
  for shift = 0 to 7 do
    add_u8 buffer
      Int64.(to_int (logand (shift_right_logical value (shift * 8)) 0xffL))
  done

let validate_utf8 string =
  let length = String.length string in
  let continuation byte = byte land 0xc0 = 0x80 in
  let require condition =
    if not condition then invalid_arg "portable MIR v2: invalid UTF-8" in
  let rec loop i =
    if i < length then
      let byte = Char.code string.[i] in
      if byte < 0x80 then loop (i + 1)
      else if byte >= 0xc2 && byte <= 0xdf then (
        require (i + 1 < length && continuation (Char.code string.[i + 1]));
        loop (i + 2))
      else if byte >= 0xe0 && byte <= 0xef then (
        require (i + 2 < length);
        let second = Char.code string.[i + 1] in
        let third = Char.code string.[i + 2] in
        require (continuation third);
        require
          (if byte = 0xe0 then second >= 0xa0 && second <= 0xbf
           else if byte = 0xed then second >= 0x80 && second <= 0x9f
           else continuation second);
        loop (i + 3))
      else if byte >= 0xf0 && byte <= 0xf4 then (
        require (i + 3 < length);
        let second = Char.code string.[i + 1] in
        let third = Char.code string.[i + 2] in
        let fourth = Char.code string.[i + 3] in
        require (continuation third && continuation fourth);
        require
          (if byte = 0xf0 then second >= 0x90 && second <= 0xbf
           else if byte = 0xf4 then second >= 0x80 && second <= 0x8f
           else continuation second);
        loop (i + 4))
      else invalid_arg "portable MIR v2: invalid UTF-8" in
  loop 0

let add_v2_string buffer string =
  validate_utf8 string;
  add_u32 buffer (String.length string);
  Buffer.add_string buffer string

let add_v2_bool buffer value = add_u8 buffer (if value then 1 else 0)

let add_v2_list add_item buffer items =
  add_u32 buffer (List.length items);
  List.iter items ~f:(add_item buffer)

let v2_leaf_tag = function
  | "Unknown" -> 0
  | "Int" -> 1
  | "Real" -> 2
  | "Complex" -> 3
  | "Vector" -> 4
  | "RowVector" -> 5
  | "Matrix" -> 6
  | leaf -> invalid_arg ("portable MIR v2: unknown unsized leaf " ^ leaf)

let add_v2_view buffer {depth; leaf} =
  add_u8 buffer depth;
  add_u8 buffer (v2_leaf_tag leaf)

let add_v2_meta buffer expr =
  add_v2_string buffer expr.e_type;
  add_v2_view buffer expr.e_unsized;
  add_v2_bool buffer expr.e_data_only;
  add_v2_bool buffer expr.e_promoted;
  add_v2_string buffer expr.e_raw

let v2_lib_tag = function
  | "StanLib" -> 0
  | "Internal" -> 1
  | "UserDefined" -> 2
  | library -> invalid_arg ("portable MIR v2: unknown function library " ^ library)

let rec add_v2_expr buffer expr =
  (match expr.e_kind with
  | "Var" ->
      add_u8 buffer 0;
      add_v2_string buffer expr.e_name
  | "LitInt" ->
      add_u8 buffer 1;
      add_i32 buffer expr.e_lit_i
  | "LitReal" ->
      add_u8 buffer 2;
      add_i64_bits buffer expr.e_lit
  | "LitStr" ->
      add_u8 buffer 3;
      add_v2_string buffer expr.e_lit_s
  | "FunApp" ->
      add_u8 buffer 4;
      add_u8 buffer (v2_lib_tag expr.e_fn_lib);
      add_v2_string buffer expr.e_name;
      add_v2_bool buffer expr.e_fn_propto;
      add_v2_list add_v2_expr buffer expr.e_args
  | "Promotion" ->
      add_u8 buffer 5;
      add_v2_list add_v2_expr buffer expr.e_args
  | "Indexed" ->
      add_u8 buffer 6;
      add_v2_list add_v2_expr buffer expr.e_args
  | "TernaryIf" ->
      add_u8 buffer 7;
      add_v2_list add_v2_expr buffer expr.e_args
  | "EOr" ->
      add_u8 buffer 8;
      add_v2_list add_v2_expr buffer expr.e_args
  | "EAnd" ->
      add_u8 buffer 9;
      add_v2_list add_v2_expr buffer expr.e_args
  | "Unsupported" -> add_u8 buffer 10
  | kind -> invalid_arg ("portable MIR v2: unknown expression kind " ^ kind));
  add_v2_meta buffer expr

let v2_transform_tag = function
  | "Identity" -> 0
  | "Lower" -> 1
  | "Upper" -> 2
  | "LowerUpper" -> 3
  | "Offset" -> 4
  | "Multiplier" -> 5
  | "OffsetMultiplier" -> 6
  | "Simplex" -> 7
  | "Ordered" -> 8
  | "PositiveOrdered" -> 9
  | "CholeskyCorr" -> 10
  | "UnitVector" -> 11
  | "SumToZero" -> 12
  | "Correlation" -> 13
  | "Covariance" -> 14
  | "CholeskyCov" -> 15
  | "Unsupported" -> 16
  | kind -> invalid_arg ("portable MIR v2: unknown transform kind " ^ kind)

let add_v2_transform buffer transform =
  add_u8 buffer (v2_transform_tag transform.t_kind);
  add_v2_list add_v2_expr buffer transform.t_args;
  add_v2_string buffer transform.t_raw

let add_v2_optional add_value buffer = function
  | None -> add_u8 buffer 0
  | Some value ->
      add_u8 buffer 1;
      add_value buffer value

let add_v2_sized buffer sized =
  add_v2_string buffer sized.s_base;
  add_v2_list add_v2_expr buffer sized.s_dims;
  add_v2_string buffer sized.s_elem_base;
  add_v2_string buffer sized.s_raw

let rec add_v2_stmt buffer stmt =
  match stmt.st_kind with
  | "Decl" ->
      add_u8 buffer 0;
      add_v2_string buffer stmt.st_decl_id;
      add_v2_sized buffer stmt.st_decl_type;
      add_v2_bool buffer stmt.st_decl_data_only;
      add_v2_bool buffer stmt.st_has_init;
      if stmt.st_has_init then add_v2_expr buffer stmt.st_init;
      add_v2_optional add_v2_transform buffer stmt.st_read_transform;
      add_v2_list add_v2_expr buffer stmt.st_read_dims;
      add_v2_string buffer stmt.st_raw
  | "Assignment" ->
      add_u8 buffer 1;
      add_v2_string buffer stmt.st_lhs;
      add_v2_list add_v2_expr buffer stmt.st_lhs_idx;
      add_v2_expr buffer stmt.st_rhs;
      add_v2_string buffer stmt.st_raw
  | "TargetPE" ->
      add_u8 buffer 2;
      add_v2_expr buffer stmt.st_target;
      add_v2_string buffer stmt.st_raw
  | "Block" ->
      add_u8 buffer 3;
      add_v2_list add_v2_stmt buffer stmt.st_body;
      add_v2_string buffer stmt.st_raw
  | "SList" ->
      add_u8 buffer 4;
      add_v2_list add_v2_stmt buffer stmt.st_body;
      add_v2_string buffer stmt.st_raw
  | "For" ->
      add_u8 buffer 5;
      add_v2_string buffer stmt.st_loopvar;
      add_v2_expr buffer stmt.st_lower;
      add_v2_expr buffer stmt.st_upper;
      add_v2_list add_v2_stmt buffer stmt.st_body;
      add_v2_string buffer stmt.st_raw
  | "IfElse" ->
      add_u8 buffer 6;
      add_v2_expr buffer stmt.st_cond;
      add_v2_list add_v2_stmt buffer stmt.st_body;
      add_v2_string buffer stmt.st_raw
  | "While" ->
      add_u8 buffer 7;
      add_v2_expr buffer stmt.st_cond;
      add_v2_list add_v2_stmt buffer stmt.st_body;
      add_v2_string buffer stmt.st_raw
  | "NRFunApp" ->
      add_u8 buffer 8;
      add_v2_string buffer stmt.st_fn_name;
      add_v2_list add_v2_expr buffer stmt.st_fn_args;
      add_v2_optional add_v2_transform buffer stmt.st_check_transform;
      add_v2_string buffer stmt.st_check_var_name;
      add_v2_string buffer stmt.st_raw
  | "Return" ->
      add_u8 buffer 9;
      add_v2_bool buffer stmt.st_has_init;
      if stmt.st_has_init then add_v2_expr buffer stmt.st_rhs;
      add_v2_string buffer stmt.st_raw
  | "Break" -> add_u8 buffer 10
  | "Continue" -> add_u8 buffer 11
  | "Skip" ->
      add_u8 buffer 12;
      add_v2_string buffer stmt.st_raw
  | "Unsupported" ->
      add_u8 buffer 13;
      add_v2_string buffer stmt.st_raw
  | kind -> invalid_arg ("portable MIR v2: unknown statement kind " ^ kind)

let add_v2_fun buffer fn =
  add_v2_string buffer fn.f_name;
  add_v2_list add_v2_string buffer fn.f_arg_names;
  add_v2_list add_v2_string buffer fn.f_arg_types;
  add_v2_list add_v2_view buffer fn.f_arg_views;
  add_v2_list add_v2_bool buffer fn.f_arg_data_only;
  add_v2_list add_v2_stmt buffer fn.f_body

let add_v2_input buffer (name, sized) =
  add_v2_string buffer name;
  add_v2_sized buffer sized

let base64_alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

let encode_base64 input =
  let length = String.length input in
  if length > 201_326_586 then
    invalid_arg "portable MIR v2: payload exceeds envelope limit";
  let output = Buffer.create (((length + 2) / 3) * 4) in
  let emit value = Buffer.add_char output base64_alphabet.[value] in
  let rec loop offset =
    if offset + 3 <= length then (
      let a = Char.code input.[offset] in
      let b = Char.code input.[offset + 1] in
      let c = Char.code input.[offset + 2] in
      emit (a lsr 2);
      emit (((a land 0x03) lsl 4) lor (b lsr 4));
      emit (((b land 0x0f) lsl 2) lor (c lsr 6));
      emit (c land 0x3f);
      loop (offset + 3))
    else if offset < length then (
      let a = Char.code input.[offset] in
      emit (a lsr 2);
      if offset + 1 < length then (
        let b = Char.code input.[offset + 1] in
        emit (((a land 0x03) lsl 4) lor (b lsr 4));
        emit ((b land 0x0f) lsl 2);
        Buffer.add_char output '=')
      else (
        emit ((a land 0x03) lsl 4);
        Buffer.add_string output "==")) in
  loop 0;
  Buffer.contents output

let encode (program : Program.Typed.t) =
  let inputs =
    List.map program.input_vars ~f:(fun (name, _, sized) ->
        (name, sized_of_t sized)) in
  let buffer = Buffer.create 32768 in
  add_v2_list add_v2_input buffer inputs;
  add_v2_list add_v2_stmt buffer (List.map ~f:stmt_of_t program.prepare_data);
  add_v2_list add_v2_stmt buffer (List.map ~f:stmt_of_t program.log_prob);
  add_v2_list add_v2_stmt buffer
    (List.map ~f:stmt_of_t program.generate_quantities);
  add_v2_list add_v2_fun buffer (List.map ~f:fun_of_t program.functions_block);
  add_v2_list add_v2_string buffer
    (List.map program.output_vars ~f:(fun (name, _, _) -> name));
  (* Trailing, and therefore optional: a document written before this section
     existed ends here and decodes with an empty transform_inits. *)
  add_v2_list add_v2_stmt buffer
    (List.map ~f:stmt_of_t program.transform_inits);
  "STANLI2:" ^ encode_base64 (Buffer.contents buffer)
