open Std
open Middle

type unsized_view = {depth: int; leaf: string}

type function_type =
  {ft_args: unsized_view list; ft_suffix: int; ft_propto: bool}

type wire_expr =
  { e_kind: string
  ; e_name: string
  ; e_fn_type: function_type option
  ; e_fn_lib: string
  ; e_fn_suffix: int
  ; e_fn_propto: bool
  ; e_lit_i: string
  ; e_lit: float
  ; e_lit_s: string
  ; e_args: wire_expr list
  ; e_unsized: unsized_view
  ; e_data_only: bool
  ; e_promoted: bool
  ; e_raw: string }

type wire_transform = {t_kind: string; t_args: wire_expr list; t_raw: string}

type wire_sized =
  { s_base: string
  ; s_dims: wire_expr list
  ; s_elem_base: string
  ; s_raw: string
  ; s_components: wire_sized list }

(* How print, reject and fatal_error print their arguments: one argument as
   itself, or an array of tuples of the given depth whose leaves are the
   following arguments. *)
type print_item = Print_arg | Print_tuple of int * print_item list

(* The payload of the statement internals that carry one: FnCheck's
   constraint, checked variable name and value, FnWriteParam's optional
   transform to invert and the value it writes, and the print layout of a
   statement printing an array of tuples. *)
type wire_payload =
  | No_payload
  | Check_payload of wire_transform * string * wire_expr
  | Write_payload of wire_transform option * wire_expr
  | Print_payload of print_item list

and wire_stmt =
  { st_kind: string
  ; st_decl_id: string
  ; st_decl_type: wire_sized
  ; st_decl_unsized: (unsized_view * string) option
  ; st_decl_data_only: bool
  ; st_has_init: bool
  ; st_init: wire_expr
  ; st_read_transform: wire_transform option
  ; st_read_dims: wire_expr list
  ; st_lhs: string
  ; st_lhs_idx: wire_expr list
  ; st_rhs: wire_expr
  ; st_values: wire_expr list
  ; st_targets: string list
  ; st_target: wire_expr
  ; st_fn_lib: string
  ; st_fn_name: string
  ; st_fn_suffix: int
  ; st_fn_args: wire_expr list
  ; st_payload: wire_payload
  ; st_loopvar: string
  ; st_lower: wire_expr
  ; st_upper: wire_expr
  ; st_cond: wire_expr
  ; st_body: wire_stmt list
  ; st_raw: string }

type wire_fun =
  { f_name: string
  ; f_arg_names: string list
  ; f_arg_views: unsized_view list
  ; f_arg_data_only: bool list
  ; f_results: unsized_view list
  ; f_body: wire_stmt list }

type tuple_leaf =
  {tl_variable: string; tl_base: string; tl_path: (int * int) list}

let unsupported_view = {depth= 0; leaf= "Unknown"}

let default_expr () =
  { e_kind= "Unsupported"
  ; e_name= ""
  ; e_fn_type= None
  ; e_fn_lib= "StanLib"
  ; e_fn_suffix= 0
  ; e_fn_propto= false
  ; e_lit_i= "0"
  ; e_lit= 0.
  ; e_lit_s= ""
  ; e_args= []
  ; e_unsized= unsupported_view
  ; e_data_only= false
  ; e_promoted= false
  ; e_raw= "" }

let default_sized () =
  {s_base= ""; s_dims= []; s_elem_base= ""; s_raw= ""; s_components= []}

let default_stmt () =
  { st_kind= "Unsupported"
  ; st_decl_id= ""
  ; st_decl_type= default_sized ()
  ; st_decl_unsized= None
  ; st_decl_data_only= false
  ; st_has_init= false
  ; st_init= default_expr ()
  ; st_read_transform= None
  ; st_read_dims= []
  ; st_lhs= ""
  ; st_lhs_idx= []
  ; st_rhs= default_expr ()
  ; st_values= []
  ; st_targets= []
  ; st_target= default_expr ()
  ; st_fn_lib= "StanLib"
  ; st_fn_name= ""
  ; st_fn_suffix= 0
  ; st_fn_args= []
  ; st_payload= No_payload
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

let raw_sized sized =
  sexp_string (SizedType.sexp_of_t Expr.Typed.sexp_of_t sized)

let raw_stmt stmt = sexp_string (Stmt.Located.sexp_of_t stmt)

let raw_transform transform =
  sexp_string (Transformation.sexp_of_t Expr.Typed.sexp_of_t transform)

let is_data_only = function UnsizedType.DataOnly -> true | _ -> false

let has_tuple = UnsizedType.contains_tuple

(* A tuple leaf's variable: the tuple variable's name followed by each
   component index from the outermost tuple level, as CmdStan names it:
   tuple_leaf_name "x" [2; 1] is "x.2.1". *)
let tuple_leaf_name base components =
  String.concat ~sep:""
    (base :: List.map components ~f:(fun c -> "." ^ Int.to_string c))

(* A tuple type is written as Unknown: after the compiler splits tuples, a
   procedure holds none, and the expression's raw spells the stanc3 type. *)
let unsized_view typ =
  let rec unwind depth = function
    | UnsizedType.UArray inner ->
        if depth = 255 then
          invalid_arg "portable MIR: unsized array nesting exceeds 255";
        unwind (depth + 1) inner
    | leaf -> (depth, leaf) in
  let depth, leaf_type = unwind 0 typ in
  let leaf =
    match leaf_type with
    | UnsizedType.UInt -> "Int"
    | UReal -> "Real"
    | UComplex -> "Complex"
    | UVector -> "Vector"
    | URowVector -> "RowVector"
    | UMatrix -> "Matrix"
    | UTuple _ | UComplexVector | UComplexRowVector | UComplexMatrix | UFun _
     |UMathLibraryFunction ->
        "Unknown"
    | UArray _ -> assert false in
  {depth; leaf}

let unsized_view_and_raw typ =
  let view = unsized_view typ in
  if String.equal view.leaf "Unknown" then (view, raw_unsized typ)
  else (view, "")

(* The views of a function's results: one per leaf of a tuple, depth first,
   or the one view of any other type. *)
let rec result_views = function
  | UnsizedType.UTuple types -> List.concat_map types ~f:result_views
  | typ -> [unsized_view typ]

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

let suffix_tag : Fun_kind.suffix -> int = function
  | Fun_kind.FnPlain -> 0
  | FnRng -> 1
  | FnDist (Density, _) -> 2
  | FnDist (Mass, _) -> 3
  | FnTarget -> 4
  | FnJacobian -> 5

let function_type_of = function
  | UnsizedType.UFun (args, _, suffix, _) ->
      Some
        { ft_args= List.map args ~f:(fun (_, typ) -> unsized_view typ)
        ; ft_suffix= suffix_tag suffix
        ; ft_propto= propto suffix }
  | UInt | UReal | UComplex | UVector | URowVector | UMatrix | UComplexVector
   |UComplexRowVector | UComplexMatrix | UArray _ | UTuple _
   |UMathLibraryFunction ->
      None

(* The leaf variables of each tuple temporary declared so far in scope: a call
   returning a tuple that the compiler did not inline is assigned to such a
   temporary and read only through its components. *)
type env = string list String.Map.t

let with_meta (expr : Expr.Typed.t) base =
  let view, type_raw = unsized_view_and_raw expr.meta.type_ in
  { base with
    e_unsized= view
  ; e_data_only= is_data_only expr.meta.adlevel
  ; e_raw= (if String.is_empty type_raw then base.e_raw else type_raw) }

let unsupported_expr (expr : Expr.Typed.t) =
  { (default_expr ()) with
    e_unsized= unsized_view expr.meta.type_
  ; e_data_only= is_data_only expr.meta.adlevel
  ; e_raw= raw_expr_pattern expr.pattern }

let rec expr_of_t (env : env) (expr : Expr.Typed.t) =
  let open Expr.Pattern in
  match expr.pattern with
  | TupleProjection ({pattern= Var name; _}, component) -> (
      match String.Map.find_opt name env with
      | Some leaves when component >= 1 && component <= List.length leaves ->
          with_meta expr
            { (default_expr ()) with
              e_kind= "Var"
            ; e_name= List.nth leaves (component - 1) }
      | _ -> unsupported_expr expr)
  | TupleProjection _ -> unsupported_expr expr
  | _ when has_tuple expr.meta.type_ -> unsupported_expr expr
  | Var name ->
      (* A density passed as `foo_lupdf` names the definition foo_lpdf and
         asks for its unnormalized form, as stanc3's C++ generation reads
         it. *)
      let fn_type = function_type_of expr.meta.type_ in
      let unnormalized =
        Option.value_map fn_type ~default:false ~f:(fun t -> t.ft_propto) in
      with_meta expr
        { (default_expr ()) with
          e_kind= "Var"
        ; e_name= (if unnormalized then Utils.normalized_name name else name)
        ; e_fn_type= fn_type }
  | Lit (Int, spelling) ->
      let canonical = canonical_int32 spelling in
      with_meta expr
        { (default_expr ()) with
          e_kind= "LitInt"
        ; e_lit_i= canonical
        ; e_lit= Int32.to_float (Int32.of_string canonical) }
  | Lit (Real, spelling) ->
      with_meta expr
        { (default_expr ()) with
          e_kind= "LitReal"
        ; e_lit= float_of_literal spelling }
  | Lit ((Imaginary | Str), spelling) ->
      with_meta expr {(default_expr ()) with e_kind= "LitStr"; e_lit_s= spelling}
  | FunApp (kind, args) -> with_meta expr (fun_app_of_t env kind args)
  | Promotion (inner, _, _) ->
      with_meta expr {(expr_of_t env inner) with e_promoted= true}
  | TernaryIf (cond, if_true, if_false) ->
      with_meta expr
        { (default_expr ()) with
          e_kind= "TernaryIf"
        ; e_args= List.map ~f:(expr_of_t env) [cond; if_true; if_false] }
  | EOr (lhs, rhs) ->
      with_meta expr
        { (default_expr ()) with
          e_kind= "EOr"
        ; e_args= List.map ~f:(expr_of_t env) [lhs; rhs] }
  | EAnd (lhs, rhs) ->
      with_meta expr
        { (default_expr ()) with
          e_kind= "EAnd"
        ; e_args= List.map ~f:(expr_of_t env) [lhs; rhs] }
  | Indexed (base, indices) ->
      with_meta expr
        { (default_expr ()) with
          e_kind= "Indexed"
        ; e_args= expr_of_t env base :: List.map ~f:(index_of_t env) indices }

and fun_app_of_t env kind args =
  let args = List.map ~f:(expr_of_t env) args in
  match kind with
  | Fun_kind.StanLib (name, suffix) ->
      { (default_expr ()) with
        e_kind= "FunApp"
      ; e_name= name
      ; e_fn_lib= "StanLib"
      ; e_fn_suffix= suffix_tag suffix
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
      ; e_fn_suffix= suffix_tag suffix
      ; e_fn_propto= propto suffix
      ; e_args= args }

and index_of_t env = function
  | Index.All -> {(default_expr ()) with e_kind= "IndexAll"}
  | Single expr ->
      { (default_expr ()) with
        e_kind= "IndexSingle"
      ; e_args= [expr_of_t env expr] }
  | Between (lower, upper) ->
      { (default_expr ()) with
        e_kind= "IndexBetween"
      ; e_args= List.map ~f:(expr_of_t env) [lower; upper] }
  | MultiIndex expr ->
      { (default_expr ()) with
        e_kind= "IndexMulti"
      ; e_args= [expr_of_t env expr] }
  | Upfrom expr ->
      { (default_expr ()) with
        e_kind= "IndexUpfrom"
      ; e_args= [expr_of_t env expr] }

(* A call returning several values, the right side of a MultiAssign. It keeps
   its tuple type's Unknown view rather than becoming unsupported. *)
let multi_call_of_t env (expr : Expr.Typed.t) =
  match expr.pattern with
  | FunApp (((StanLib _ | UserDefined _) as kind), args)
    when has_tuple expr.meta.type_ ->
      Some (with_meta expr (fun_app_of_t env kind args))
  | _ -> None

let rec sized_of_t env sized =
  let expr_of_t = expr_of_t env in
  match sized with
  | SizedType.STuple components ->
      { (default_sized ()) with
        s_base= "STuple"
      ; s_components= List.map ~f:(sized_of_t env) components }
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
      let inner = sized_of_t env inner in
      { (default_sized ()) with
        s_base= "SArray"
      ; s_dims= expr_of_t dim :: inner.s_dims
      ; s_elem_base=
          (if String.equal inner.s_base "SArray" then inner.s_elem_base
           else inner.s_base)
      ; s_components= inner.s_components }
  | (SComplexVector _ | SComplexRowVector _ | SComplexMatrix _) as unsupported
    ->
      let sexp = raw_sized unsupported in
      let base =
        match unsupported with
        | SComplexVector _ -> "SComplexVector"
        | SComplexRowVector _ -> "SComplexRowVector"
        | SComplexMatrix _ -> "SComplexMatrix"
        | _ -> assert false in
      {(default_sized ()) with s_base= base; s_raw= sexp}

let transform_of_t env transform =
  let expr_of_t = expr_of_t env in
  let make kind args = {t_kind= kind; t_args= args; t_raw= ""} in
  let supported_atom kind = {(make kind []) with t_raw= kind} in
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
  | Lower arg -> make "Lower" [expr_of_t arg]
  | Upper arg -> make "Upper" [expr_of_t arg]
  | LowerUpper (lower, upper) ->
      make "LowerUpper" (List.map ~f:expr_of_t [lower; upper])
  | Offset arg -> make "Offset" [expr_of_t arg]
  | Multiplier arg -> make "Multiplier" [expr_of_t arg]
  | OffsetMultiplier (offset, multiplier) ->
      make "OffsetMultiplier" (List.map ~f:expr_of_t [offset; multiplier])
  | StochasticColumn -> supported_atom "StochasticColumn"
  | StochasticRow -> supported_atom "StochasticRow"
  | TupleTransformation _ ->
      {(make "Unsupported" []) with t_raw= raw_transform transform}

(* stanc3 declares optimizer temporaries, such as an inlined function's result,
   without sizes; the first whole assignment gives them a shape. *)
let decl_unsized_of_type = function
  | Type.Sized _ -> None
  | Unsized unsized ->
      let view, raw = unsized_view_and_raw unsized in
      Some (view, raw)

let read_param_fields env (expr : Expr.Typed.t) =
  match expr.pattern with
  | Expr.Pattern.FunApp
      (Fun_kind.CompilerInternal (Internal_fun.FnReadParam fields), _) ->
      ( Some (transform_of_t env fields.constrain)
      , List.map ~f:(expr_of_t env) fields.dims )
  | _ -> (None, [])

let unsupported_stmt stmt = {(default_stmt ()) with st_raw= raw_stmt stmt}

(* The component types of a tuple whose components hold no tuple, the only
   tuple a temporary for a call's results may have. *)
let flat_tuple_components = function
  | Type.Sized (SizedType.STuple components)
    when not (List.exists components ~f:SizedType.contains_tuple) ->
      Some (List.map components ~f:(fun c -> Type.Sized c))
  | Unsized (UnsizedType.UTuple components)
    when not (List.exists components ~f:has_tuple) ->
      Some (List.map components ~f:(fun c -> Type.Unsized c))
  | _ -> None

(* The leaves of a returned tuple value written with tuple syntax, depth first;
   None when some leaf is still a tuple. *)
let rec return_leaves (expr : Expr.Typed.t) =
  match (expr.pattern, expr.meta.type_) with
  | FunApp (CompilerInternal FnMakeTuple, components), UTuple _ ->
      Option.map ~f:List.concat
        (Option.all (List.map components ~f:return_leaves))
  | _ when has_tuple expr.meta.type_ -> None
  | _ -> Some [expr]

let string_literal text =
  { (Expr.Helpers.str text) with
    meta= {Expr.Typed.Meta.empty with type_= UReal; adlevel= DataOnly} }

let rec strip_arrays depth = function
  | UnsizedType.UArray inner -> strip_arrays (depth + 1) inner
  | typ -> (depth, typ)

(* The arguments and layout items for one printed value. A tuple outside any
   array is spelled with string literals, as stan_print writes it; an array of
   tuples becomes one tuple item over its leaves. A tuple of another shape is
   unsupported. *)
let rec print_value env (expr : Expr.Typed.t) =
  match (expr.pattern, strip_arrays 0 expr.meta.type_) with
  | _ when not (has_tuple expr.meta.type_) -> ([expr_of_t env expr], [Print_arg])
  | FunApp (CompilerInternal FnMakeTuple, components), (0, UTuple types)
    when List.length components = List.length types ->
      let literal text =
        ([expr_of_t env (string_literal text)], [Print_arg]) in
      let parts =
        List.concat
          (List.mapi components ~f:(fun i component ->
               (if i = 0 then [] else [literal ","])
               @ [print_value env component])) in
      let parts = (literal "(" :: parts) @ [literal ")"] in
      (List.concat_map parts ~f:fst, List.concat_map parts ~f:snd)
  | _ -> (
    match tuple_layout env expr.meta.type_ expr with
    | Some (args, item) -> (args, [item])
    | None -> ([unsupported_expr expr], [Print_arg]))

(* The layout of an array of tuples of type [typ] whose leaves [expr] lists
   with tuple syntax, nested tuples nesting. *)
and tuple_layout env typ (expr : Expr.Typed.t) =
  match (strip_arrays 0 typ, expr.pattern) with
  | (depth, UTuple types), FunApp (CompilerInternal FnMakeTuple, components)
    when List.length components = List.length types ->
      let parts =
        List.map2 types components ~f:(fun component_type component ->
            if has_tuple component_type then
              tuple_layout env component_type component
            else Some ([expr_of_t env component], Print_arg)) in
      Option.map (Option.all parts) ~f:(fun parts ->
          ( List.concat_map parts ~f:fst
          , Print_tuple (depth, List.map parts ~f:snd) ))
  | _ -> None

let is_tuple_item = function
  | Print_arg -> false
  | Print_tuple _ -> true

(* Fresh names for the leaves of tuple temporaries, chosen against every
   identifier of the program so far. *)
type names = {mutable taken: String.Set.t}

let fresh_name names base component =
  let first = tuple_leaf_name base [component] in
  let rec pick n =
    let candidate =
      if n = 0 then first else first ^ "_" ^ Int.to_string n in
    if String.Set.mem candidate names.taken then pick (n + 1)
    else (
      names.taken <- String.Set.add candidate names.taken;
      candidate) in
  pick 0

let rec stmts_of_t names env stmts =
  let env, rev =
    List.fold_left stmts ~init:(env, []) ~f:(fun (env, acc) stmt ->
        let env, wire = stmt_of_t names env stmt in
        (env, List.rev_append wire acc)) in
  (env, List.rev rev)

and scoped names env stmts = snd (stmts_of_t names env stmts)

(* A body that takes one statement: a declaration of a tuple temporary becomes
   several, kept together in an SList. *)
and single names env stmt =
  match snd (stmt_of_t names env stmt) with
  | [one] -> one
  | many -> {(default_stmt ()) with st_kind= "SList"; st_body= many}

and stmt_of_t names (env : env) (stmt : Stmt.Located.t) : env * wire_stmt list
    =
  let open Stmt.Pattern in
  let expr_of_t = expr_of_t env in
  let one wire = (env, [wire]) in
  match stmt.pattern with
  | Decl {decl_adtype; decl_id; decl_type; initialize; _}
    when has_tuple (Type.to_unsized decl_type) -> (
      let env = String.Map.remove decl_id env in
      let call =
        match initialize with
        | Assign expr -> Option.map (multi_call_of_t env expr) ~f:Option.some
        | Uninit | Default -> Some None in
      match (flat_tuple_components decl_type, call) with
      | Some components, Some call ->
          let levels =
            match decl_adtype with
            | UnsizedType.TupleAD levels
              when List.length levels = List.length components ->
                levels
            | level -> List.map components ~f:(fun _ -> level) in
          let leaves =
            List.mapi components ~f:(fun i _ -> fresh_name names decl_id (i + 1))
          in
          let decls =
            List.map3 leaves components levels ~f:(fun leaf component level ->
                { (default_stmt ()) with
                  st_kind= "Decl"
                ; st_decl_id= leaf
                ; st_decl_type=
                    (match component with
                    | Type.Sized sized -> sized_of_t env sized
                    | Unsized _ -> default_sized ())
                ; st_decl_unsized= decl_unsized_of_type component
                ; st_decl_data_only= is_data_only level }) in
          let assign =
            Option.value_map call ~default:[] ~f:(fun call ->
                [ { (default_stmt ()) with
                    st_kind= "MultiAssign"
                  ; st_targets= leaves
                  ; st_rhs= call } ]) in
          (String.Map.add ~key:decl_id ~data:leaves env, decls @ assign)
      | _ -> (env, [unsupported_stmt stmt]))
  | Decl {decl_adtype; decl_id; decl_type; initialize; _} ->
      let initialized, init =
        match initialize with
        | Assign expr -> (true, expr_of_t expr)
        | Uninit | Default -> (false, default_expr ()) in
      let read_transform, read_dims =
        match initialize with
        | Assign expr -> read_param_fields env expr
        | Uninit | Default -> (None, []) in
      ( String.Map.remove decl_id env
      , [ { (default_stmt ()) with
            st_kind= "Decl"
          ; st_decl_id= decl_id
          ; st_decl_type=
              (match decl_type with
              | Type.Sized sized -> sized_of_t env sized
              | Unsized _ -> default_sized ())
          ; st_decl_unsized= decl_unsized_of_type decl_type
          ; st_decl_data_only= is_data_only decl_adtype
          ; st_has_init= initialized
          ; st_init= init
          ; st_read_transform= read_transform
          ; st_read_dims= read_dims } ] )
  | Assignment ((LVariable lhs, []), _, rhs) when String.Map.mem lhs env -> (
    match multi_call_of_t env rhs with
    | Some call ->
        one
          { (default_stmt ()) with
            st_kind= "MultiAssign"
          ; st_targets= String.Map.find lhs env
          ; st_rhs= call }
    | None -> one (unsupported_stmt stmt))
  | Assignment ((LVariable _, _), lhs_type, rhs)
    when has_tuple lhs_type || has_tuple rhs.meta.type_ ->
      one (unsupported_stmt stmt)
  | Assignment ((LVariable lhs, indices), _, rhs) ->
      one
        { (default_stmt ()) with
          st_kind= "Assignment"
        ; st_lhs= lhs
        ; st_lhs_idx= List.map ~f:(index_of_t env) indices
        ; st_rhs= expr_of_t rhs }
  | Assignment ((LTupleProjection _, _), _, _) -> one (unsupported_stmt stmt)
  | TargetPE expr ->
      one {(default_stmt ()) with st_kind= "TargetPE"; st_target= expr_of_t expr}
  | Block body ->
      one
        { (default_stmt ()) with
          st_kind= "Block"
        ; st_body= scoped names env body }
  | SList body ->
      (* An SList is not a scope: its declarations stay visible after it. *)
      let env, body = stmts_of_t names env body in
      (env, [{(default_stmt ()) with st_kind= "SList"; st_body= body}])
  | For {loopvar; lower; upper; body} ->
      one
        { (default_stmt ()) with
          st_kind= "For"
        ; st_loopvar= loopvar
        ; st_lower= expr_of_t lower
        ; st_upper= expr_of_t upper
        ; st_body= [single names (String.Map.remove loopvar env) body] }
  | IfElse (cond, if_true, if_false) ->
      one
        { (default_stmt ()) with
          st_kind= "IfElse"
        ; st_cond= expr_of_t cond
        ; st_body=
            single names env if_true
            :: Option.value_map if_false ~default:[] ~f:(fun s ->
                   [single names env s]) }
  | While (cond, body) ->
      one
        { (default_stmt ()) with
          st_kind= "While"
        ; st_cond= expr_of_t cond
        ; st_body= [single names env body] }
  | Return None -> one {(default_stmt ()) with st_kind= "Return"}
  | Return (Some expr) -> (
    match return_leaves expr with
    | Some values ->
        one
          { (default_stmt ()) with
            st_kind= "Return"
          ; st_values= List.map ~f:expr_of_t values }
    | None -> one (unsupported_stmt stmt))
  | Break -> one {(default_stmt ()) with st_kind= "Break"}
  | Continue -> one {(default_stmt ()) with st_kind= "Continue"}
  | Skip -> one {(default_stmt ()) with st_kind= "Skip"}
  | NRFunApp (kind, args) -> one (nr_fun_app_of_t env kind args)
  | Profile (_, body) ->
      (* The profile name is only for stanc's own timing output, which stanli
         does not produce; unwrap to the plain block it wraps. *)
      one
        { (default_stmt ()) with
          st_kind= "Block"
        ; st_body= scoped names env body }
  | JacobianPE expr ->
      one
        {(default_stmt ()) with st_kind= "JacobianPE"; st_target= expr_of_t expr}

and nr_fun_app_of_t env kind args =
  let ordinary_args () = List.map ~f:(expr_of_t env) args in
  match kind with
  | Fun_kind.CompilerInternal ((FnPrint | FnReject | FnFatalError) as internal)
    ->
      let parts = List.map args ~f:(print_value env) in
      let layout = List.concat_map parts ~f:snd in
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_lib= "Internal"
      ; st_fn_name= internal_name internal
      ; st_fn_args= List.concat_map parts ~f:fst
      ; st_payload=
          (if List.exists layout ~f:is_tuple_item then Print_payload layout
           else No_payload) }
  | Fun_kind.CompilerInternal internal ->
      let name = internal_name internal in
      (* transform_inits' FnWriteParam names the transform to INVERT, turning an
         already-constrained value into a free one; write_array's own carries
         none, its value being constrained already. *)
      let payload =
        match internal with
        | Internal_fun.FnCheck {trans; var_name; var} ->
            Check_payload (transform_of_t env trans, var_name, expr_of_t env var)
        | FnWriteParam {var; unconstrain_opt} ->
            Write_payload
              ( Option.map ~f:(transform_of_t env) unconstrain_opt
              , expr_of_t env var )
        | FnLength | FnMakeArray | FnMakeTuple | FnMakeRowVec | FnNegInf
         |FnReadData | FnReadDeserializer | FnReadParam _ | FnValidateSize
         |FnValidateSizePositive | FnValidateSizeUnitVector | FnPrint
         |FnReject | FnFatalError | FnResizeToMatch | FnNaN | FnDeepCopy
         |FnReadWriteEventsOpenCL _ | FnToRef | FnReturnSoA _ ->
            No_payload in
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_lib= "Internal"
      ; st_fn_name= name
      ; st_fn_args= ordinary_args ()
      ; st_payload= payload }
  | StanLib (name, suffix) ->
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= name
      ; st_fn_suffix= suffix_tag suffix
      ; st_fn_args= ordinary_args () }
  | Operator op ->
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_name= operator_name op
      ; st_fn_args= ordinary_args () }
  | UserDefined (name, suffix) ->
      { (default_stmt ()) with
        st_kind= "NRFunApp"
      ; st_fn_lib= "UserDefined"
      ; st_fn_name= name
      ; st_fn_suffix= suffix_tag suffix
      ; st_fn_args= ordinary_args () }

let fun_of_t names (definition : Stmt.Located.t Program.fun_def) =
  { f_name= definition.fdname
  ; f_arg_names= List.map definition.fdargs ~f:(fun (_, name, _) -> name)
  ; f_arg_views= List.map definition.fdargs ~f:(fun (_, _, typ) -> unsized_view typ)
  ; f_arg_data_only=
      List.map definition.fdargs ~f:(fun (adlevel, _, _) -> is_data_only adlevel)
  ; f_results=
      (match definition.fdrt with
      | Void -> []
      | ReturnType typ -> result_views typ)
  ; f_body=
      Option.value_map definition.fdbody ~default:[] ~f:(fun s ->
          [single names String.Map.empty s]) }

(* Once the compiler passes a tuple argument as one argument per leaf, two
   definitions stanc3 told apart, f(tuple(real, real)) and f(real, real), can
   take the same arguments. Distinct stanc3 signatures otherwise stay
   distinct, so equal argument types among definitions with bodies can only
   come from that split. *)
let check_split_overloads (functions : Stmt.Located.t Program.fun_def list) =
  let defined =
    List.filter_map functions ~f:(fun (fn : Stmt.Located.t Program.fun_def) ->
        Option.map fn.fdbody ~f:(fun _ ->
            (fn.fdname, List.map fn.fdargs ~f:(fun (_, _, typ) -> typ)))) in
  let rec scan = function
    | [] -> ()
    | (name, types) :: rest ->
        if
          List.exists rest ~f:(fun (other, other_types) ->
              String.equal name other
              && List.equal ~eq:UnsizedType.equal types other_types)
        then
          failwith
            ("user function " ^ name
           ^ " has overloads that are the same once their tuple arguments \
              are split into components; rename one of them")
        else scan rest in
  scan defined

(* The leaves of each tuple input and output variable, depth first. Each slot
   records the array dimensions of the tuple variable before it and the
   component it selects. *)
let tuple_leaves_of (name, sized) =
  let rec leaves before path sized =
    let rec unwind depth = function
      | SizedType.SArray (inner, _) -> unwind (depth + 1) inner
      | elem -> (depth, elem) in
    match unwind 0 sized with
    | depth, SizedType.STuple components ->
        let before = before + depth in
        List.concat
          (List.mapi components ~f:(fun i component ->
               leaves before ((before, i + 1) :: path) component))
    | _ -> [List.rev path] in
  if SizedType.contains_tuple sized then
    List.map (leaves 0 [] sized) ~f:(fun path ->
        { tl_variable= tuple_leaf_name name (List.map path ~f:snd)
        ; tl_base= name
        ; tl_path= path })
  else []

(* Every identifier the program names, so a fresh leaf name avoids them. *)
let program_identifiers (program : Program.Typed.t) =
  let rec expr set (e : Expr.Typed.t) =
    let set =
      match e.pattern with Var name -> String.Set.add name set | _ -> set in
    Expr.Pattern.fold expr set e.pattern in
  let rec lvalue set (base, _) =
    match base with
    | Stmt.Pattern.LVariable name -> String.Set.add name set
    | LTupleProjection (inner, _) -> lvalue set inner in
  let rec stmt set (s : Stmt.Located.t) =
    let set =
      match s.pattern with
      | Decl {decl_id; _} -> String.Set.add decl_id set
      | For {loopvar; _} -> String.Set.add loopvar set
      | Assignment (lhs, _, _) -> lvalue set lhs
      | _ -> set in
    Stmt.Pattern.fold expr stmt set s.pattern in
  let stmts set = List.fold_left ~init:set ~f:stmt in
  let set = String.Set.empty in
  let set =
    List.fold_left program.input_vars ~init:set ~f:(fun set (name, _, _) ->
        String.Set.add name set) in
  let set =
    List.fold_left program.output_vars ~init:set ~f:(fun set (name, _, _) ->
        String.Set.add name set) in
  let set =
    List.fold_left program.functions_block ~init:set
      ~f:(fun set (fn : Stmt.Located.t Program.fun_def) ->
        let set = String.Set.add fn.fdname set in
        let set =
          List.fold_left fn.fdargs ~init:set ~f:(fun set (_, name, _) ->
              String.Set.add name set) in
        Option.value_map fn.fdbody ~default:set ~f:(stmt set)) in
  List.fold_left ~init:set ~f:stmts
    [ program.prepare_data; program.log_prob; program.reverse_mode_log_prob
    ; program.generate_quantities; program.transform_inits
    ; program.unconstrain_array ]

(* Portable MIR is a canonical ASCII envelope around a small fixed-layout
   binary payload. The envelope keeps the existing native, subprocess, V8,
   worker, and C-string transports byte-safe; the payload keeps decoding
   allocation-light. Multibyte scalars are little-endian, strings and lists
   carry uint32 byte/item counts, and sum types carry one-byte tags followed
   only by fields active for that tag. *)

let add_u8 buffer value =
  if value < 0 || value > 0xff then invalid_arg "portable MIR: invalid u8";
  Buffer.add_char buffer (Stdlib.Char.chr value)

let add_u32 buffer value =
  if value < 0 || Int64.of_int value > 0xffff_ffffL then
    invalid_arg "portable MIR: invalid u32";
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
    if not condition then invalid_arg "portable MIR: invalid UTF-8" in
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
      else invalid_arg "portable MIR: invalid UTF-8" in
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
  | leaf -> invalid_arg ("portable MIR: unknown unsized leaf " ^ leaf)

let add_v2_view buffer {depth; leaf} =
  add_u8 buffer depth;
  add_u8 buffer (v2_leaf_tag leaf)

let add_v2_optional add_value buffer = function
  | None -> add_u8 buffer 0
  | Some value ->
      add_u8 buffer 1;
      add_value buffer value

let add_v2_function_type buffer {ft_args; ft_suffix; ft_propto} =
  add_v2_list add_v2_view buffer ft_args;
  add_u8 buffer ft_suffix;
  add_v2_bool buffer ft_propto

let add_v2_meta buffer expr =
  add_v2_view buffer expr.e_unsized;
  add_v2_bool buffer expr.e_data_only;
  add_v2_bool buffer expr.e_promoted;
  add_v2_string buffer expr.e_raw

let v2_lib_tag = function
  | "StanLib" -> 0
  | "Internal" -> 1
  | "UserDefined" -> 2
  | library -> invalid_arg ("portable MIR: unknown function library " ^ library)

let rec add_v2_expr buffer expr =
  (match expr.e_kind with
  | "Var" ->
      add_u8 buffer 0;
      add_v2_string buffer expr.e_name;
      add_v2_optional add_v2_function_type buffer expr.e_fn_type
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
      add_u8 buffer expr.e_fn_suffix;
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
  | "IndexAll" ->
      add_u8 buffer 11;
      add_v2_list add_v2_expr buffer expr.e_args
  | "IndexSingle" ->
      add_u8 buffer 12;
      add_v2_list add_v2_expr buffer expr.e_args
  | "IndexUpfrom" ->
      add_u8 buffer 13;
      add_v2_list add_v2_expr buffer expr.e_args
  | "IndexBetween" ->
      add_u8 buffer 14;
      add_v2_list add_v2_expr buffer expr.e_args
  | "IndexMulti" ->
      add_u8 buffer 15;
      add_v2_list add_v2_expr buffer expr.e_args
  | kind -> invalid_arg ("portable MIR: unknown expression kind " ^ kind));
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
  | "StochasticColumn" -> 16
  | "StochasticRow" -> 17
  | "Unsupported" -> 18
  | kind -> invalid_arg ("portable MIR: unknown transform kind " ^ kind)

let add_v2_transform buffer transform =
  add_u8 buffer (v2_transform_tag transform.t_kind);
  add_v2_list add_v2_expr buffer transform.t_args;
  add_v2_string buffer transform.t_raw

let is_tuple_sized sized =
  String.equal sized.s_base "STuple"
  || String.equal sized.s_base "SArray"
     && String.equal sized.s_elem_base "STuple"

let rec add_v2_sized buffer sized =
  add_v2_string buffer sized.s_base;
  add_v2_list add_v2_expr buffer sized.s_dims;
  add_v2_string buffer sized.s_elem_base;
  add_v2_string buffer sized.s_raw;
  if is_tuple_sized sized then
    add_v2_list add_v2_sized buffer sized.s_components

let rec add_print_item buffer = function
  | Print_arg -> add_u8 buffer 0
  | Print_tuple (depth, components) ->
      add_u8 buffer 1;
      add_u8 buffer depth;
      add_v2_list add_print_item buffer components

let rec add_v2_stmt buffer stmt =
  match stmt.st_kind with
  | "Decl" ->
      add_u8 buffer 0;
      add_v2_string buffer stmt.st_decl_id;
      (match stmt.st_decl_unsized with
      | None ->
          add_u8 buffer 0;
          add_v2_sized buffer stmt.st_decl_type
      | Some (view, raw) ->
          add_u8 buffer 1;
          add_v2_view buffer view;
          add_v2_string buffer raw);
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
      add_u8 buffer (v2_lib_tag stmt.st_fn_lib);
      add_v2_string buffer stmt.st_fn_name;
      add_u8 buffer stmt.st_fn_suffix;
      add_v2_list add_v2_expr buffer stmt.st_fn_args;
      (match stmt.st_payload with
      | No_payload -> add_u8 buffer 0
      | Check_payload (transform, var_name, var) ->
          add_u8 buffer 1;
          add_v2_transform buffer transform;
          add_v2_string buffer var_name;
          add_v2_expr buffer var
      | Write_payload (transform, var) ->
          add_u8 buffer 2;
          add_v2_optional add_v2_transform buffer transform;
          add_v2_expr buffer var
      | Print_payload layout ->
          add_u8 buffer 3;
          add_v2_list add_print_item buffer layout);
      add_v2_string buffer stmt.st_raw
  | "Return" ->
      add_u8 buffer 9;
      add_v2_list add_v2_expr buffer stmt.st_values;
      add_v2_string buffer stmt.st_raw
  | "Break" -> add_u8 buffer 10
  | "Continue" -> add_u8 buffer 11
  | "Skip" ->
      add_u8 buffer 12;
      add_v2_string buffer stmt.st_raw
  | "Unsupported" ->
      add_u8 buffer 13;
      add_v2_string buffer stmt.st_raw
  | "JacobianPE" ->
      add_u8 buffer 14;
      add_v2_expr buffer stmt.st_target;
      add_v2_string buffer stmt.st_raw
  | "MultiAssign" ->
      add_u8 buffer 15;
      add_v2_list add_v2_string buffer stmt.st_targets;
      add_v2_expr buffer stmt.st_rhs;
      add_v2_string buffer stmt.st_raw
  | kind -> invalid_arg ("portable MIR: unknown statement kind " ^ kind)

let add_v2_fun buffer fn =
  add_v2_string buffer fn.f_name;
  add_v2_list add_v2_string buffer fn.f_arg_names;
  add_v2_list add_v2_view buffer fn.f_arg_views;
  add_v2_list add_v2_bool buffer fn.f_arg_data_only;
  add_v2_list add_v2_view buffer fn.f_results;
  add_v2_list add_v2_stmt buffer fn.f_body

let add_v2_input buffer (name, sized) =
  add_v2_string buffer name;
  add_v2_sized buffer sized

let block_tag = function
  | Program.Parameters -> 0
  | TransformedParameters -> 1
  | GeneratedQuantities -> 2

let add_v2_output buffer (name, block, sized, annotations) =
  add_v2_string buffer name;
  add_u8 buffer (block_tag block);
  add_v2_sized buffer sized;
  add_v2_list add_v2_string buffer annotations

let add_tuple_leaf buffer {tl_variable; tl_base; tl_path} =
  add_v2_string buffer tl_variable;
  add_v2_string buffer tl_base;
  add_v2_list
    (fun buffer (before, component) ->
      add_u32 buffer before;
      add_u32 buffer component)
    buffer tl_path

let base64_alphabet =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

let encode_base64 input =
  let length = String.length input in
  if length > 201_326_586 then
    invalid_arg "portable MIR: payload exceeds envelope limit";
  let output = Buffer.create ((length + 2) / 3 * 4) in
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

(* stanc3 separates write_array's three sections with top-level early-return
   guards on its emit flags, which it recognizes with these predicates
   (Transform_Mir's tparam_start and gq_start). The runtime always emits every
   section, so the guards are dropped and their positions kept: the statement
   indices where the transformed-parameter and generated-quantity sections
   begin. *)
let rec mentions name (expr : Expr.Typed.t) =
  match expr.pattern with
  | Var var -> String.equal var name
  | pattern ->
      Expr.Pattern.fold (fun found e -> found || mentions name e) false pattern

let write_array_sections stmts =
  let guard (stmt : Stmt.Located.t) =
    match stmt.pattern with
    | IfElse
        ( { pattern=
              FunApp (_, [{pattern= Var "emit_generated_quantities__"; _}])
          ; _ }
        , _
        , _ ) ->
        Some `Generated_quantities
    | IfElse (cond, _, _) when mentions "emit_transformed_parameters__" cond ->
        Some `Transformed_parameters
    | _ -> None in
  let kept, length, tp_begin, gq_begin =
    List.fold_left stmts ~init:([], 0, None, None)
      ~f:(fun (kept, length, tp_begin, gq_begin) stmt ->
        match guard stmt with
        | Some `Transformed_parameters when Option.is_none tp_begin ->
            (kept, length, Some length, gq_begin)
        | Some `Generated_quantities when Option.is_none gq_begin ->
            (kept, length, tp_begin, Some length)
        | _ -> (stmt :: kept, length + 1, tp_begin, gq_begin)) in
  let gq_begin = Option.value gq_begin ~default:length in
  (List.rev kept, Option.value tp_begin ~default:gq_begin, gq_begin)

let encode (program : Program.Typed.t) =
  check_split_overloads program.functions_block;
  let names = {taken= program_identifiers program} in
  let procedure stmts = scoped names String.Map.empty stmts in
  let inputs =
    List.map program.input_vars ~f:(fun (name, _, sized) -> (name, sized)) in
  let outputs =
    List.map program.output_vars
      ~f:(fun (name, _, Program.{out_block; out_constrained_st; out_annotations; _})
         ->
        ( name
        , out_block
        , out_constrained_st
        , List.map out_annotations ~f:(fun (a : Annotation.t) -> a.name) )) in
  let buffer = Buffer.create 32768 in
  add_v2_list add_v2_input buffer
    (List.map inputs ~f:(fun (name, sized) -> (name, sized_of_t String.Map.empty sized)));
  add_v2_list add_v2_stmt buffer (procedure program.prepare_data);
  add_v2_list add_v2_stmt buffer (procedure program.log_prob);
  let write_array, tp_begin, gq_begin =
    write_array_sections program.generate_quantities in
  (* A declaration can become several statements, so each section is encoded
     on its own, the declarations of one staying visible in the next. *)
  let before_tp, rest = List.split_n write_array tp_begin in
  let tp, gq = List.split_n rest (gq_begin - tp_begin) in
  let env, before_tp = stmts_of_t names String.Map.empty before_tp in
  let env, tp = stmts_of_t names env tp in
  let _, gq = stmts_of_t names env gq in
  add_v2_list add_v2_stmt buffer (before_tp @ tp @ gq);
  add_u32 buffer (List.length before_tp);
  add_u32 buffer (List.length before_tp + List.length tp);
  add_v2_list add_v2_fun buffer
    (List.map ~f:(fun_of_t names) program.functions_block);
  add_v2_list add_v2_output buffer
    (List.map outputs ~f:(fun (name, block, sized, annotations) ->
         (name, block, sized_of_t String.Map.empty sized, annotations)));
  add_v2_list add_v2_stmt buffer (procedure program.transform_inits);
  add_v2_list add_tuple_leaf buffer
    (List.concat_map ~f:tuple_leaves_of
       (inputs
       @ List.map outputs ~f:(fun (name, _, sized, _) -> (name, sized))));
  "STANLI5:" ^ encode_base64 (Buffer.contents buffer)
