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
  | FnReadWriteEventsOpenCL _ -> "FnReadWriteEventsOpenCL"

let internal_has_payload : Expr.Typed.t Internal_fun.t -> bool = function
  | Internal_fun.FnReadParam _ | FnWriteParam _ | FnCheck _
   |FnReadWriteEventsOpenCL _ ->
      true
  | FnLength | FnMakeArray | FnMakeTuple | FnMakeRowVec | FnNegInf
   |FnReadData | FnReadDeserializer | FnValidateSize | FnValidateSizePositive
   |FnValidateSizeUnitVector | FnPrint | FnReject | FnFatalError
   |FnResizeToMatch | FnNaN | FnDeepCopy ->
      false

let propto = function
  | Fun_kind.FnLpdf true | FnLpmf true -> true
  | FnPlain | FnRng | FnLpdf false | FnLpmf false | FnTarget | FnJacobian ->
      false

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
  | Fun_kind.StanLib (name, suffix, _) ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= name
      ; e_fn_lib= "StanLib"
      ; e_fn_propto= propto suffix
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
  | Skip -> {(default_stmt ()) with st_kind= "Skip"}
  | NRFunApp (kind, args) -> nr_fun_app_of_t kind args
  | JacobianPE _ | Break | Continue | Profile _ ->
      { (default_stmt ()) with
        st_kind= "Unsupported"
      ; st_raw= raw_stmt_pattern stmt.pattern }

and nr_fun_app_of_t kind args =
  let ordinary_args = List.map ~f:expr_of_t args in
  match kind with
  | Fun_kind.CompilerInternal internal ->
      let name = internal_name internal in
      let payload_args, check_transform, check_var_name =
        match internal with
        | Internal_fun.FnCheck {trans; var_name; var} ->
            ([expr_of_t var], Some (transform_of_t trans), var_name)
        | FnWriteParam {var; _} -> ([expr_of_t var], None, "")
        | _ -> ([], None, "") in
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= name
      ; st_fn_args= payload_args @ ordinary_args
      ; st_check_transform= check_transform
      ; st_check_var_name= check_var_name }
  | StanLib (name, _, _) ->
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= name
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

let add_json_string buffer string =
  let length = String.length string in
  let continuation byte = byte land 0xc0 = 0x80 in
  let require condition =
    if not condition then invalid_arg "portable MIR: string is not valid UTF-8"
  in
  Buffer.add_char buffer '"';
  let rec loop i =
    if i < length then
      let byte = Char.code string.[i] in
      if byte < 0x80 then (
        (match byte with
        | 0x08 -> Buffer.add_string buffer "\\b"
        | 0x09 -> Buffer.add_string buffer "\\t"
        | 0x0a -> Buffer.add_string buffer "\\n"
        | 0x0c -> Buffer.add_string buffer "\\f"
        | 0x0d -> Buffer.add_string buffer "\\r"
        | 0x22 -> Buffer.add_string buffer "\\\""
        | 0x5c -> Buffer.add_string buffer "\\\\"
        | control when control < 0x20 ->
            Buffer.add_string buffer (Printf.sprintf "\\u%04x" control)
        | _ -> Buffer.add_char buffer string.[i]);
        loop (i + 1))
      else if byte >= 0xc2 && byte <= 0xdf then (
        require (i + 1 < length && continuation (Char.code string.[i + 1]));
        Buffer.add_substring buffer string i 2;
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
        Buffer.add_substring buffer string i 3;
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
        Buffer.add_substring buffer string i 4;
        loop (i + 4))
      else invalid_arg "portable MIR: string is not valid UTF-8" in
  loop 0;
  Buffer.add_char buffer '"'

let add_bool buffer value = Buffer.add_string buffer (Bool.to_string value)

let add_f64 buffer value =
  let bits = Int64.bits_of_float value in
  let digits = "0123456789abcdef" in
  Buffer.add_string buffer "\"f64:";
  for shift = 15 downto 0 do
    let nibble =
      Int64.(to_int (logand (shift_right_logical bits (shift * 4)) 0xfL)) in
    Buffer.add_char buffer digits.[nibble]
  done;
  Buffer.add_char buffer '"'

let add_array add_item buffer items =
  Buffer.add_char buffer '[';
  List.iteri items ~f:(fun index item ->
      if index > 0 then Buffer.add_char buffer ',';
      add_item buffer item);
  Buffer.add_char buffer ']'

let add_unsized buffer {depth; leaf} =
  Buffer.add_string buffer "{\"depth\":";
  Buffer.add_string buffer (Int.to_string depth);
  Buffer.add_string buffer ",\"leaf\":";
  add_json_string buffer leaf;
  Buffer.add_char buffer '}'

let rec add_expr buffer expr =
  Buffer.add_string buffer "{\"kind\":";
  add_json_string buffer expr.e_kind;
  Buffer.add_string buffer ",\"name\":";
  add_json_string buffer expr.e_name;
  Buffer.add_string buffer ",\"fn_lib\":";
  add_json_string buffer expr.e_fn_lib;
  Buffer.add_string buffer ",\"fn_propto\":";
  add_bool buffer expr.e_fn_propto;
  Buffer.add_string buffer ",\"lit_i\":";
  add_json_string buffer expr.e_lit_i;
  Buffer.add_string buffer ",\"lit\":";
  add_f64 buffer expr.e_lit;
  Buffer.add_string buffer ",\"lit_s\":";
  add_json_string buffer expr.e_lit_s;
  Buffer.add_string buffer ",\"args\":";
  add_array add_expr buffer expr.e_args;
  Buffer.add_string buffer ",\"type_\":";
  add_json_string buffer expr.e_type;
  Buffer.add_string buffer ",\"unsized\":";
  add_unsized buffer expr.e_unsized;
  Buffer.add_string buffer ",\"data_only\":";
  add_bool buffer expr.e_data_only;
  Buffer.add_string buffer ",\"promoted\":";
  add_bool buffer expr.e_promoted;
  Buffer.add_string buffer ",\"raw\":";
  add_json_string buffer expr.e_raw;
  Buffer.add_char buffer '}'

let add_transform buffer transform =
  Buffer.add_string buffer "{\"kind\":";
  add_json_string buffer transform.t_kind;
  Buffer.add_string buffer ",\"args\":";
  add_array add_expr buffer transform.t_args;
  Buffer.add_string buffer ",\"raw\":";
  add_json_string buffer transform.t_raw;
  Buffer.add_char buffer '}'

let add_optional_transform buffer = function
  | None -> Buffer.add_string buffer "null"
  | Some transform -> add_transform buffer transform

let add_sized buffer sized =
  Buffer.add_string buffer "{\"base\":";
  add_json_string buffer sized.s_base;
  Buffer.add_string buffer ",\"dims\":";
  add_array add_expr buffer sized.s_dims;
  Buffer.add_string buffer ",\"elem_base\":";
  add_json_string buffer sized.s_elem_base;
  Buffer.add_string buffer ",\"raw\":";
  add_json_string buffer sized.s_raw;
  Buffer.add_char buffer '}'

let rec add_stmt buffer stmt =
  Buffer.add_string buffer "{\"kind\":";
  add_json_string buffer stmt.st_kind;
  Buffer.add_string buffer ",\"decl_id\":";
  add_json_string buffer stmt.st_decl_id;
  Buffer.add_string buffer ",\"decl_type\":";
  add_sized buffer stmt.st_decl_type;
  Buffer.add_string buffer ",\"decl_data_only\":";
  add_bool buffer stmt.st_decl_data_only;
  Buffer.add_string buffer ",\"has_init\":";
  add_bool buffer stmt.st_has_init;
  Buffer.add_string buffer ",\"init\":";
  add_expr buffer stmt.st_init;
  Buffer.add_string buffer ",\"read_transform\":";
  add_optional_transform buffer stmt.st_read_transform;
  Buffer.add_string buffer ",\"read_dims\":";
  add_array add_expr buffer stmt.st_read_dims;
  Buffer.add_string buffer ",\"lhs\":";
  add_json_string buffer stmt.st_lhs;
  Buffer.add_string buffer ",\"lhs_idx\":";
  add_array add_expr buffer stmt.st_lhs_idx;
  Buffer.add_string buffer ",\"rhs\":";
  add_expr buffer stmt.st_rhs;
  Buffer.add_string buffer ",\"target\":";
  add_expr buffer stmt.st_target;
  Buffer.add_string buffer ",\"fn_name\":";
  add_json_string buffer stmt.st_fn_name;
  Buffer.add_string buffer ",\"fn_args\":";
  add_array add_expr buffer stmt.st_fn_args;
  Buffer.add_string buffer ",\"check_transform\":";
  add_optional_transform buffer stmt.st_check_transform;
  Buffer.add_string buffer ",\"check_var_name\":";
  add_json_string buffer stmt.st_check_var_name;
  Buffer.add_string buffer ",\"loopvar\":";
  add_json_string buffer stmt.st_loopvar;
  Buffer.add_string buffer ",\"lower\":";
  add_expr buffer stmt.st_lower;
  Buffer.add_string buffer ",\"upper\":";
  add_expr buffer stmt.st_upper;
  Buffer.add_string buffer ",\"cond\":";
  add_expr buffer stmt.st_cond;
  Buffer.add_string buffer ",\"body\":";
  add_array add_stmt buffer stmt.st_body;
  Buffer.add_string buffer ",\"raw\":";
  add_json_string buffer stmt.st_raw;
  Buffer.add_char buffer '}'

let add_fun buffer fn =
  Buffer.add_string buffer "{\"name\":";
  add_json_string buffer fn.f_name;
  Buffer.add_string buffer ",\"arg_names\":";
  add_array add_json_string buffer fn.f_arg_names;
  Buffer.add_string buffer ",\"arg_types\":";
  add_array add_json_string buffer fn.f_arg_types;
  Buffer.add_string buffer ",\"arg_views\":";
  add_array add_unsized buffer fn.f_arg_views;
  Buffer.add_string buffer ",\"arg_data_only\":";
  add_array add_bool buffer fn.f_arg_data_only;
  Buffer.add_string buffer ",\"body\":";
  add_array add_stmt buffer fn.f_body;
  Buffer.add_char buffer '}'

let add_input buffer (name, sized) =
  Buffer.add_string buffer "{\"name\":";
  add_json_string buffer name;
  Buffer.add_string buffer ",\"type\":";
  add_sized buffer sized;
  Buffer.add_char buffer '}'

let encode (program : Program.Typed.t) =
  let inputs =
    List.map program.input_vars ~f:(fun (name, _, sized) ->
        (name, sized_of_t sized)) in
  let buffer = Buffer.create 65536 in
  Buffer.add_string buffer "{\"stanli_ir\":1,\"program\":{\"input_vars\":";
  add_array add_input buffer inputs;
  Buffer.add_string buffer ",\"prepare_data\":";
  add_array add_stmt buffer (List.map ~f:stmt_of_t program.prepare_data);
  Buffer.add_string buffer ",\"log_prob\":";
  add_array add_stmt buffer (List.map ~f:stmt_of_t program.log_prob);
  Buffer.add_string buffer ",\"generate_quantities\":";
  add_array add_stmt buffer (List.map ~f:stmt_of_t program.generate_quantities);
  Buffer.add_string buffer ",\"fun_defs\":";
  add_array add_fun buffer (List.map ~f:fun_of_t program.functions_block);
  Buffer.add_string buffer ",\"output_vars\":";
  add_array add_json_string buffer
    (List.map program.output_vars ~f:(fun (name, _, _) -> name));
  Buffer.add_string buffer "}}";
  Buffer.contents buffer
