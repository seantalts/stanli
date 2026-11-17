(* Split tuples into one variable per leaf before the dataflow passes run, so
   copy propagation and loop vectorization see ordinary variables.

   A tuple variable becomes one variable per leaf, depth first, and an array
   of tuples becomes one array per leaf: `array[N] tuple(real, vector[2]) p`
   is `p.1`, an array[N] real, and `p.2`, an array[N] vector[2], so
   `p[i].2[j]` is `p.2[i, j]`. A dot cannot occur in a Stan identifier, so
   leaf names cannot collide with the program's own names, and temporaries
   are named `tuple.tmpN`, which no leaf name can spell. Nothing reads
   structure back out of these names.

   What remains tuple-typed afterwards:
   - a call returning a tuple that was not inlined is bound once to a
     temporary of the flat leaf tuple type and read only through `tmp.k`;
   - a function returning a tuple returns the flat leaf tuple;
   - an array of tuples printed by print, reject or fatal_error is passed as
     `FnMakeTuple` of its leaf arrays, typed as the original array of tuples;
   - input_vars and output_vars, which keep their declared types;
   - any statement the pass cannot rewrite, left as it was. *)

open Middle

type expr = Expr.Typed.t
type stmt = Stmt.Located.t

exception Unsupported of string

let unsupported what = raise (Unsupported what)

type 'a tree = Leaf of 'a | Node of 'a tree list

let rec leaves = function
  | Leaf x -> [x]
  | Node parts -> List.concat_map leaves parts

let rec map_tree f = function
  | Leaf x -> Leaf (f x)
  | Node parts -> Node (List.map (map_tree f) parts)

let rec zip_tree a b =
  match (a, b) with
  | Leaf x, Leaf y -> Leaf (x, y)
  | Node xs, Node ys when List.length xs = List.length ys ->
      Node (List.map2 zip_tree xs ys)
  | _ -> unsupported "tuple values of different shapes"

(* Replace the leaves of a tree, in order, from a list. *)
let refill tree values =
  let rest = ref values in
  let tree =
    map_tree
      (fun _ ->
        match !rest with
        | x :: tail ->
            rest := tail;
            x
        | [] -> unsupported "tuple values of different shapes")
      tree in
  tree

(* ---- types ------------------------------------------------------------- *)

let is_tuple t = UnsizedType.contains_tuple t
let is_tuple_expr (e : expr) = is_tuple e.meta.type_

let rec wrap_array depth t =
  if depth = 0 then t else UnsizedType.UArray (wrap_array (depth - 1) t)

let rec type_tree ?(depth = 0) (t : UnsizedType.t) =
  if not (is_tuple t) then Leaf (wrap_array depth t)
  else
    match t with
    | UArray inner -> type_tree ~depth:(depth + 1) inner
    | UTuple parts -> Node (List.map (type_tree ~depth) parts)
    | _ -> unsupported "tuple type"

let rec scalar_ad = function
  | UnsizedType.TupleAD levels ->
      if List.exists (fun l -> scalar_ad l = UnsizedType.AutoDiffable) levels
      then UnsizedType.AutoDiffable
      else DataOnly
  | level -> level

let rec ad_tree (ad : UnsizedType.autodifftype) (t : UnsizedType.t) =
  if not (is_tuple t) then Leaf (scalar_ad ad)
  else
    match (t, ad) with
    | UArray inner, _ -> ad_tree ad inner
    | UTuple parts, TupleAD levels when List.length levels = List.length parts
      ->
        Node (List.map2 ad_tree levels parts)
    | UTuple parts, _ -> Node (List.map (ad_tree ad) parts)
    | _ -> unsupported "tuple type"

(* The leaf paths, as component numbers from 1. *)
let rec path_tree ?(path = []) (t : UnsizedType.t) =
  if not (is_tuple t) then Leaf (List.rev path)
  else
    match t with
    | UArray inner -> path_tree ~path inner
    | UTuple parts ->
        Node (List.mapi (fun k part -> path_tree ~path:((k + 1) :: path) part) parts)
    | _ -> unsupported "tuple type"

let suffix path = String.concat "" (List.map (fun k -> "." ^ string_of_int k) path)
let name_tree base t = map_tree (fun path -> base ^ suffix path) (path_tree t)
let flat_type t = UnsizedType.UTuple (leaves (type_tree t))
let flat_ad ad t = UnsizedType.TupleAD (leaves (ad_tree ad t))

let rec flat_fun_type (t : UnsizedType.t) =
  match t with
  | UFun (args, rt, suffix, mem) ->
      let args =
        List.concat_map
          (fun (ad, t) ->
            if is_tuple t then
              List.combine (leaves (ad_tree ad t)) (leaves (type_tree t))
            else [(ad, flat_fun_type t)])
          args in
      let rt =
        match rt with
        | UnsizedType.ReturnType t when is_tuple t ->
            UnsizedType.ReturnType (flat_type t)
        | rt -> rt in
      UnsizedType.UFun (args, rt, suffix, mem)
  | UArray inner -> UArray (flat_fun_type inner)
  | t -> t

let wrap_sized dims st =
  List.fold_right (fun dim inner -> SizedType.SArray (inner, dim)) dims st

let rec sized_tree ?(dims = []) (st : expr SizedType.t) =
  if not (SizedType.contains_tuple st) then Leaf (wrap_sized dims st)
  else
    match st with
    | SArray (inner, dim) -> sized_tree ~dims:(dims @ [dim]) inner
    | STuple parts -> Node (List.map (sized_tree ~dims) parts)
    | _ -> unsupported "tuple type"

(* ---- expressions ------------------------------------------------------- *)

let mk pattern type_ adlevel loc : expr =
  {pattern; meta= Expr.Typed.Meta.create ~type_ ~adlevel ~loc ()}

let variable name type_ adlevel loc = mk (Var name) type_ adlevel loc
let one = Expr.Helpers.int 1
let zero = Expr.Helpers.int 0

let rec exists_expr p (e : expr) =
  p e || Expr.Pattern.fold (fun found e -> found || exists_expr p e) false e.pattern

let effectful =
  exists_expr (fun e ->
      match e.pattern with
      | FunApp (UserDefined _, _)
       |FunApp (StanLib (_, (FnRng | FnTarget | FnJacobian)), _) ->
          true
      | FunApp (CompilerInternal internal, _) ->
          Internal_fun.can_side_effect internal
      | _ -> false)

let mentions names =
  exists_expr (fun e ->
      match e.pattern with Var name -> List.mem name names | _ -> false)

let all_single = List.for_all (function Index.Single _ -> true | _ -> false)

(* `leaf[indices]`, joining a single-indexed leaf's index list, since
   `a[i][j]` is `a[i, j]` then. *)
let index_leaf (leaf : expr) indices type_ adlevel =
  if List.is_empty indices then {leaf with meta= {leaf.meta with type_; adlevel}}
  else
    let pattern =
      match leaf.pattern with
      | Indexed (base, first) when all_single first ->
          Expr.Pattern.Indexed (base, first @ indices)
      | _ -> Indexed (leaf, indices) in
    mk pattern type_ adlevel leaf.meta.loc

(* Statements an expression needs first, in order. *)
type pre = stmt list ref

let emit (pre : pre) stmt = pre := stmt :: !pre
let stmt loc pattern : stmt = {pattern; meta= loc}

let counter = ref 0

let fresh () =
  incr counter;
  "tuple.tmp" ^ string_of_int !counter

let temporaries = Hashtbl.create 16

let declare pre loc name adlevel type_ =
  Hashtbl.replace temporaries name ();
  emit pre
    (stmt loc
       (Decl
          { decl_adtype= adlevel
          ; decl_id= name
          ; decl_type= Unsized type_
          ; initialize= Uninit
          ; decl_annotations= [] }))

let assign loc name indices type_ rhs : stmt =
  stmt loc (Assignment ((LVariable name, indices), type_, rhs))

(* Evaluate a non-tuple expression once, into a temporary. *)
let copy pre (e : expr) =
  let name = fresh () in
  declare pre e.meta.loc name e.meta.adlevel e.meta.type_;
  emit pre (assign e.meta.loc name [] e.meta.type_ e);
  variable name e.meta.type_ e.meta.adlevel e.meta.loc

let hoist pre (e : expr) =
  match e.pattern with Var _ | Lit _ -> e | _ -> copy pre e

(* ---- scopes ------------------------------------------------------------ *)

(* A name bound to its leaf variables, or to None when it is not a tuple (or
   a tuple left unsplit) in this scope. *)
let scopes : (string, expr tree option) Hashtbl.t list ref = ref []

let lookup name =
  let rec go = function
    | [] -> None
    | scope :: rest -> (
        match Hashtbl.find_opt scope name with
        | Some binding -> binding
        | None -> go rest) in
  go !scopes

let bind name binding =
  match !scopes with
  | scope :: _ -> Hashtbl.replace scope name binding
  | [] -> ()

let scoped f =
  scopes := Hashtbl.create 8 :: !scopes;
  Fun.protect ~finally:(fun () -> scopes := List.tl !scopes) f

(* Bind a tuple name to its leaves; returns (name, type, adlevel) per leaf. *)
let bind_tuple name (t : UnsizedType.t) ad loc =
  let names = name_tree name t in
  let types = type_tree t in
  let ads = ad_tree ad t in
  let triples =
    map_tree
      (fun ((n, ty), a) -> (n, ty, a))
      (zip_tree (zip_tree names types) ads) in
  bind name (Some (map_tree (fun (n, ty, a) -> variable n ty a loc) triples));
  leaves triples

(* ---- expression rewriting --------------------------------------------- *)

let higher_order args =
  List.exists
    (fun (a : expr) ->
      match a.meta.type_ with UFun _ -> true | _ -> false)
    args

let make_tuple (parts : expr list) type_ adlevel loc =
  mk (FunApp (CompilerInternal FnMakeTuple, parts)) type_ adlevel loc

(* A non-tuple expression with every tuple inside it gone. *)
let rec rewrite (pre : pre) (e : expr) : expr =
  if is_tuple_expr e then unsupported "a tuple value in this position";
  let meta = e.meta in
  match e.pattern with
  | Var _ -> (
      match meta.type_ with
      | UFun _ -> {e with meta= {meta with type_= flat_fun_type meta.type_}}
      | _ -> e)
  | Lit _ -> e
  | TupleProjection (inner, k) -> (
      match project pre inner k with
      | Leaf leaf -> {leaf with meta= {leaf.meta with type_= meta.type_}}
      | Node _ -> unsupported "tuple projection")
  | Indexed (base, indices) -> (
      let projected =
        match base.pattern with TupleProjection _ -> true | _ -> false in
      let base' = rewrite pre base in
      let indices' = rewrite_indices pre indices in
      match base'.pattern with
      | Indexed (root, first) when projected && all_single first ->
          {e with pattern= Indexed (root, first @ indices')}
      | _ -> {e with pattern= Indexed (base', indices')})
  | FunApp (kind, args) -> rewrite_call pre e kind args
  | TernaryIf (cond, a, b) ->
      let cond' = rewrite pre cond in
      let pre_a = ref [] and pre_b = ref [] in
      let a' = rewrite pre_a a in
      let b' = rewrite pre_b b in
      if List.is_empty !pre_a && List.is_empty !pre_b then
        {e with pattern= TernaryIf (cond', a', b')}
      else
        (* Each branch's statements run only when it is taken. *)
        let name = fresh () in
        declare pre meta.loc name meta.adlevel meta.type_;
        let arm p v =
          stmt meta.loc (Block (List.rev (assign meta.loc name [] meta.type_ v :: !p)))
        in
        emit pre (stmt meta.loc (IfElse (cond', arm pre_a a', Some (arm pre_b b'))));
        variable name meta.type_ meta.adlevel meta.loc
  | EAnd (a, b) | EOr (a, b) ->
      let a' = rewrite pre a in
      let pre_b = ref [] in
      let b' = rewrite pre_b b in
      let is_and = match e.pattern with EAnd _ -> true | _ -> false in
      if List.is_empty !pre_b then
        {e with pattern= (if is_and then EAnd (a', b') else EOr (a', b'))}
      else
        (* The right side's statements run only when it is evaluated. *)
        let name = fresh () in
        declare pre meta.loc name DataOnly UInt;
        let set v = assign meta.loc name [] UInt v in
        let right =
          stmt meta.loc
            (Block
               (List.rev !pre_b
               @ [stmt meta.loc (IfElse (b', set one, None))])) in
        emit pre (set zero);
        if is_and then emit pre (stmt meta.loc (IfElse (a', right, None)))
        else emit pre (stmt meta.loc (IfElse (a', set one, Some right)));
        variable name UInt DataOnly meta.loc
  | Promotion (inner, ut, ad) -> {e with pattern= Promotion (rewrite pre inner, ut, ad)}

and rewrite_indices pre indices =
  List.map
    (fun (index : expr Index.t) ->
      match index with
      | All -> Index.All
      | Single i -> Single (rewrite pre i)
      | Upfrom i -> Upfrom (rewrite pre i)
      | Between (l, u) ->
          let l = rewrite pre l in
          let u = rewrite pre u in
          Between (l, u)
      | MultiIndex i -> MultiIndex (rewrite pre i))
    indices

(* Arguments of a call, each tuple argument spread into its leaves. *)
and spread pre args =
  List.concat_map
    (fun (a : expr) -> if is_tuple_expr a then leaves (value_of pre a) else [rewrite pre a])
    args

and rewrite_call pre (e : expr) kind args =
  let has_tuple_arg = List.exists is_tuple_expr args in
  let kind' =
    match kind with
    | Fun_kind.CompilerInternal internal ->
        Fun_kind.CompilerInternal (Internal_fun.map (rewrite pre) internal)
    | kind -> kind in
  match kind with
  | (CompilerInternal FnLength | StanLib ("size", _)) when has_tuple_arg -> (
      (* An array of tuples is as long as each of its leaves. *)
      match args with
      | [a] -> (
          match leaves (value_of pre a) with
          | first :: _ -> {e with pattern= FunApp (kind', [first])}
          | [] -> unsupported "empty tuple")
      | _ -> unsupported "size of a tuple")
  | UserDefined _ -> {e with pattern= FunApp (kind', spread pre args)}
  | StanLib _ when has_tuple_arg && higher_order args ->
      {e with pattern= FunApp (kind', spread pre args)}
  | _ when has_tuple_arg -> unsupported "a library function taking a tuple"
  | _ ->
      let args' = List.map (rewrite pre) args in
      {e with pattern= FunApp (kind', args')}

(* Component k of a tuple expression. *)
and project pre (inner : expr) k : expr tree =
  match inner.pattern with
  | FunApp (CompilerInternal FnMakeTuple, parts)
    when k >= 1 && k <= List.length parts ->
      (* Every component still runs, in order, for its draws and effects. *)
      let any_effect = List.exists effectful parts in
      let values =
        List.map
          (fun part ->
            let v = value_of pre part in
            if any_effect && effectful part then hoist_value pre part v else v)
          parts in
      List.nth values (k - 1)
  | _ -> (
      match value_of pre inner with
      | Node parts when k >= 1 && k <= List.length parts -> List.nth parts (k - 1)
      | _ -> unsupported "tuple projection")

(* The leaves of an expression, which may be of tuple type. *)
and value_of pre (e : expr) : expr tree =
  if not (is_tuple_expr e) then Leaf (rewrite pre e)
  else
    let meta = e.meta in
    match e.pattern with
    | Var name -> (
        match lookup name with
        | Some tree -> tree
        | None -> unsupported ("tuple variable " ^ name))
    | TupleProjection (inner, k) -> project pre inner k
    | Indexed (base, indices) ->
        let tree = value_of pre base in
        let indices = rewrite_indices pre indices in
        let indices =
          if List.length (leaves tree) > 1 then
            List.map (Index.map (fun i -> if effectful i then hoist pre i else i)) indices
          else indices in
        let typed = zip_tree (type_tree meta.type_) (ad_tree meta.adlevel meta.type_) in
        map_tree
          (fun (leaf, (type_, adlevel)) -> index_leaf leaf indices type_ adlevel)
          (zip_tree tree typed)
    | FunApp (CompilerInternal FnMakeTuple, parts) ->
        Node (List.map (value_of pre) parts)
    | FunApp (CompilerInternal FnMakeArray, elements) ->
        let values = List.map (fun el -> leaves (value_of pre el)) elements in
        let types = type_tree meta.type_ in
        let ads = ad_tree meta.adlevel meta.type_ in
        let typed = leaves (zip_tree types ads) in
        let columns =
          List.mapi
            (fun k (type_, adlevel) ->
              let column =
                List.map
                  (fun row ->
                    match List.nth_opt row k with
                    | Some x -> x
                    | None -> unsupported "array of different tuples")
                  values in
              mk (FunApp (CompilerInternal FnMakeArray, column)) type_ adlevel meta.loc)
            typed in
        refill types columns
    | FunApp ((UserDefined _ | StanLib _), _) -> bind_call pre e
    | TernaryIf (cond, a, b) ->
        let cond' = rewrite pre cond in
        let result = temporary_tree pre e in
        let arm v =
          let p = ref [] in
          let value = value_of p v in
          let stores = assign_leaves meta.loc result value in
          stmt meta.loc (Block (List.rev !p @ stores)) in
        let arm_a = arm a in
        let arm_b = arm b in
        emit pre (stmt meta.loc (IfElse (cond', arm_a, Some arm_b)));
        result
    | Promotion (inner, ut, ad) ->
        (* A tuple promotion promotes each leaf to its component's type. *)
        let tree = value_of pre inner in
        let targets = zip_tree (type_tree meta.type_) (ad_tree meta.adlevel meta.type_) in
        let scalars = zip_tree (type_tree ut) (ad_tree ad ut) in
        let typed = zip_tree targets scalars in
        map_tree
          (fun ((leaf : expr), ((type_, adlevel), (scalar, scalar_ad))) ->
            mk (Promotion (leaf, scalar, scalar_ad)) type_ adlevel leaf.meta.loc)
          (zip_tree tree typed)
    | _ -> unsupported "this kind of tuple expression"

(* A call returning a tuple that is not inlined: evaluated once into a
   temporary of the flat leaf tuple, then read through its projections. *)
and bind_call pre (e : expr) =
  let meta = e.meta in
  let call =
    match e.pattern with
    | FunApp (kind, args) -> (
        match kind with
        | UserDefined _ -> Expr.Pattern.FunApp (kind, spread pre args)
        | StanLib _ when higher_order args -> FunApp (kind, spread pre args)
        | _ when List.exists is_tuple_expr args ->
            unsupported "a library function taking a tuple"
        | _ -> FunApp (kind, List.map (rewrite pre) args))
    | _ -> unsupported "tuple call" in
  let ft = flat_type meta.type_ and fad = flat_ad meta.adlevel meta.type_ in
  let name = fresh () in
  declare pre meta.loc name fad ft;
  emit pre (assign meta.loc name [] ft (mk call ft fad meta.loc));
  let tmp = variable name ft fad meta.loc in
  let typed = zip_tree (type_tree meta.type_) (ad_tree meta.adlevel meta.type_) in
  let k = ref 0 in
  map_tree
    (fun (type_, adlevel) ->
      incr k;
      mk (TupleProjection (tmp, !k)) type_ adlevel meta.loc)
    typed

(* Fresh unsized temporaries for a tuple value of e's type. *)
and temporary_tree pre (e : expr) =
  let typed = zip_tree (type_tree e.meta.type_) (ad_tree e.meta.adlevel e.meta.type_) in
  map_tree
    (fun (type_, adlevel) ->
      let name = fresh () in
      declare pre e.meta.loc name adlevel type_;
      variable name type_ adlevel e.meta.loc)
    typed

and hoist_value pre (e : expr) value =
  match value with
  | Leaf leaf -> Leaf (hoist pre leaf)
  | Node _ ->
      let result = temporary_tree pre e in
      List.iter (emit pre) (assign_leaves e.meta.loc result value);
      result

(* `target = value` leaf by leaf, each target a leaf variable, possibly
   indexed. *)
and assign_leaves loc target value =
  List.map
    (fun ((t : expr), v) -> store loc t v)
    (leaves (zip_tree target value))

and store loc (target : expr) value =
  match target.pattern with
  | Var name -> assign loc name [] target.meta.type_ value
  | Indexed ({pattern= Var name; meta}, indices) -> assign loc name indices meta.type_ value
  | _ -> unsupported "assigning through nested indexing of a tuple"

(* ---- statements -------------------------------------------------------- *)

let is_decl (s : stmt) = match s.pattern with Decl _ -> true | _ -> false

let decl_mentions_temporary (s : stmt) =
  match s.pattern with
  | Decl {decl_type= Sized st; _} ->
      SizedType.fold
        (fun found e ->
          found
          || exists_expr
               (fun e ->
                 match e.pattern with
                 | Var name -> Hashtbl.mem temporaries name
                 | _ -> false)
               e)
        false st
  | _ -> false

let reads_param (s : stmt) =
  match s.pattern with
  | Decl {initialize= Assign {pattern= FunApp (CompilerInternal (FnReadParam _), _); _}; _}
    ->
      true
  | _ -> false

(* A statement after the statements its expressions need. When those declare
   temporaries, they and the statement go in a block of their own so a
   temporary never becomes transformed data or outlives its use; a variable
   the statement declares stays outside the block. *)
let finish loc (pre : stmt list) (main : stmt list) =
  if not (List.exists is_decl pre) then pre @ main
  else if not (List.exists is_decl main) then [stmt loc (Block (pre @ main))]
  else if List.exists (fun s -> decl_mentions_temporary s || reads_param s) main then
    pre @ main
  else
    let outside, inside =
      List.fold_left
        (fun (outside, inside) (s : stmt) ->
          match s.pattern with
          | Decl ({decl_id; decl_type; initialize= Assign init; _} as d) ->
              ( {s with pattern= Decl {d with initialize= Uninit}} :: outside
              , assign s.meta decl_id [] (Type.to_unsized decl_type) init :: inside )
          | Decl _ -> (s :: outside, inside)
          | _ -> (outside, s :: inside))
        ([], []) main in
    List.rev outside @ [stmt loc (Block (pre @ List.rev inside))]

let with_pre loc f =
  let pre = ref [] in
  let main = f pre in
  finish loc (List.rev !pre) main

let block_of loc = function
  | [s] -> s
  | stmts -> stmt loc (Block stmts)

let rewrite_sized pre (st : expr SizedType.t) = SizedType.map (rewrite pre) st

let rewrite_type pre = function
  | Type.Sized st -> Type.Sized (rewrite_sized pre st)
  | Unsized t -> Unsized t

(* The tree of lvalue leaves an assignment's left side names. *)
let rec target_of pre (lbase, indices) : expr tree =
  let tree =
    match lbase with
    | Stmt.Pattern.LVariable name -> (
        match lookup name with
        | Some tree -> tree
        | None -> unsupported ("tuple variable " ^ name))
    | LTupleProjection (inner, k) -> (
        match target_of pre inner with
        | Node parts when k >= 1 && k <= List.length parts -> List.nth parts (k - 1)
        | _ -> unsupported "tuple projection") in
  if List.is_empty indices then tree
  else
    let indices = rewrite_indices pre indices in
    let indices =
      if List.length (leaves tree) > 1 then
        let names = List.filter_map (fun (l : expr) ->
          match l.pattern with Var n | Indexed ({pattern= Var n; _}, _) -> Some n | _ -> None)
          (leaves tree) in
        List.map
          (Index.map (fun i ->
               if mentions names i then copy pre i
               else if effectful i then hoist pre i
               else i))
          indices
      else indices in
    map_tree
      (fun (leaf : expr) ->
        (match leaf.pattern with
        | Indexed (_, first) when not (all_single first) ->
            unsupported "indexing after a multiple index of a tuple"
        | _ -> ());
        (* The type is that of the leaf variable, which is what stores need. *)
        index_leaf leaf indices leaf.meta.type_ leaf.meta.adlevel)
      tree

let rec lvalue_root (lbase, _) =
  match lbase with
  | Stmt.Pattern.LVariable name -> name
  | LTupleProjection (inner, _) -> lvalue_root inner

let is_projection_lvalue (lbase, _) =
  match lbase with Stmt.Pattern.LTupleProjection _ -> true | LVariable _ -> false

let leaf_name (leaf : expr) =
  match leaf.pattern with
  | Var n | Indexed ({pattern= Var n; _}, _) -> n
  | _ -> ""

let rewrite_assignment loc pre lvalue ut rhs =
  let root = lvalue_root lvalue in
  match lookup root with
  | None ->
      if is_projection_lvalue lvalue then unsupported ("tuple variable " ^ root);
      let lbase, indices = lvalue in
      let indices = rewrite_indices pre indices in
      let rhs = rewrite pre rhs in
      [stmt loc (Assignment ((lbase, indices), ut, rhs))]
  | Some _ -> (
      let target = target_of pre lvalue in
      let value = value_of pre rhs in
      let targets = leaves target and values = leaves value in
      if List.length targets <> List.length values then
        unsupported "tuple assignment of different shapes";
      (* A leaf read after an earlier leaf was written is copied first. *)
      let written = List.map leaf_name targets in
      let aliased =
        List.exists
          (fun (k, v) -> mentions (List.filteri (fun j _ -> j < k) written) v)
          (List.mapi (fun k v -> (k, v)) values) in
      let value =
        if List.length targets > 1 && aliased then
          map_tree (fun (v : expr) ->
              match v.pattern with Lit _ -> v | _ -> copy pre v) value
        else value in
      match (target, value) with
      | Leaf t, Leaf v -> [store loc t v]
      | _ -> assign_leaves loc target value)

(* print and reject spell a tuple as stan_print does: (a,b). An array of
   tuples is passed whole, as the tuple of its leaf arrays. *)
let rec spell loc (t : UnsizedType.t) adlevel value =
  let text s = mk (Lit (Str, s)) UReal DataOnly loc in
  match (t, value) with
  | _, Leaf v -> [v]
  | UTuple parts, Node values when List.length parts = List.length values ->
      let ads =
        match adlevel with
        | UnsizedType.TupleAD l when List.length l = List.length parts -> l
        | a -> List.map (fun _ -> a) parts in
      let inner =
        List.mapi
          (fun k ((part, ad), v) ->
            (if k > 0 then [text ","] else []) @ spell loc part ad v)
          (List.combine (List.combine parts ads) values) in
      (text "(" :: List.concat inner) @ [text ")"]
  | UArray _, _ -> [make_tuple (leaves value) t adlevel loc]
  | _ -> unsupported "printing a tuple"

let read_constrain_dims constrain st =
  let rec constrain_get_dims (st : expr SizedType.t) =
    match st with
    | SInt | SReal | SComplex | STuple _ -> []
    | SArray (t, dim) -> dim :: constrain_get_dims t
    | SVector (_, d) | SRowVector (_, d) | SComplexVector d | SComplexRowVector d -> [d]
    | SMatrix (_, _, dim2) | SComplexMatrix (_, dim2) -> [dim2] in
  match constrain with
  | Transformation.CholeskyCorr | Correlation | Covariance -> constrain_get_dims st
  | _ -> SizedType.get_dims st

(* stanc3 reads a tuple parameter one component at a time, an array of
   tuples in loops over its elements. The reads of `name` among stmts, as
   (component path, read) pairs, or None if stmts do anything else. *)
let rec param_reads name (s : stmt) =
  match s.pattern with
  | Assignment (lvalue, _, {pattern= FunApp (CompilerInternal (FnReadParam {constrain; _}), []); meta})
    when String.equal (lvalue_root lvalue) name ->
      let rec path (lbase, _) acc =
        match lbase with
        | Stmt.Pattern.LVariable _ -> acc
        | LTupleProjection (inner, k) -> path inner (k :: acc) in
      Some [(path lvalue [], (constrain, meta))]
  | For {body; _} -> param_reads name body
  | Block body | SList body ->
      List.fold_left
        (fun acc s ->
          match (acc, param_reads name s) with
          | Some a, Some b -> Some (a @ b)
          | _ -> None)
        (Some []) body
  | _ -> None

type proc = {tuple_params: string list}

let rec rewrite_stmt proc (s : stmt) : stmt list =
  try rewrite_stmt_exn proc s
  with Unsupported _ | Invalid_argument _ | Not_found | Failure _ ->
    (match s.pattern with
    | Decl {decl_id; _} -> bind decl_id None
    | _ -> ());
    [s]

and rewrite_body proc (s : stmt) : stmt =
  scoped (fun () ->
      match s.pattern with
      | Block body -> {s with pattern= Block (rewrite_block proc body)}
      | _ -> block_of s.meta (rewrite_stmt proc s))

and rewrite_block proc (stmts : stmt list) : stmt list =
  match stmts with
  | [] -> []
  | ({pattern= Decl {decl_id; decl_type= Sized st; decl_adtype; decl_annotations; _}; meta}
     as s)
    :: rest
    when List.mem decl_id proc.tuple_params -> (
      let rec take acc = function
        | s :: rest -> (
            match param_reads decl_id s with
            | Some reads when not (List.is_empty reads) -> take (acc @ reads) rest
            | _ -> (acc, s :: rest))
        | [] -> (acc, []) in
      let reads, rest' = take [] rest in
      match
        try
          Some
            (with_pre meta (fun pre ->
                 let st = rewrite_sized pre st in
                 let t = SizedType.to_unsized st in
                 let named = bind_tuple decl_id t decl_adtype meta in
                 let paths = leaves (path_tree t) in
                 let sized = leaves (sized_tree st) in
                 List.map2
                   (fun (path, (name, _, adlevel)) leaf_st ->
                     let read_constrain, (read_meta : Expr.Typed.Meta.t) =
                       match List.assoc_opt path reads with
                       | Some r -> r
                       | None -> unsupported "a parameter component without a read" in
                     let constrain =
                       Transformation.map (rewrite pre) read_constrain in
                     let init =
                       mk
                         (FunApp
                            ( CompilerInternal
                                (FnReadParam
                                   { constrain
                                   ; dims= read_constrain_dims constrain leaf_st
                                   ; mem_pattern= SizedType.get_mem_pattern leaf_st })
                            , [] ))
                         (SizedType.to_unsized leaf_st) (scalar_ad read_meta.adlevel)
                         read_meta.loc in
                     stmt meta
                       (Decl
                          { decl_adtype= adlevel
                          ; decl_id= name
                          ; decl_type= Sized leaf_st
                          ; initialize= Assign init
                          ; decl_annotations }))
                   (List.combine paths named) sized))
        with Unsupported _ | Invalid_argument _ | Not_found | Failure _ -> None
      with
      | Some stmts -> stmts @ rewrite_block proc rest'
      | None ->
          bind decl_id None;
          s :: rewrite_block proc rest)
  | s :: rest ->
      let first = rewrite_stmt proc s in
      first @ rewrite_block proc rest

and rewrite_stmt_exn proc (s : stmt) : stmt list =
  let loc = s.meta in
  let same pattern = [{s with pattern}] in
  match s.pattern with
  | Decl ({decl_id; decl_type; decl_adtype; initialize; decl_annotations} as d) ->
      let t = Type.to_unsized decl_type in
      if not (is_tuple t) then
        with_pre loc (fun pre ->
            let decl_type = rewrite_type pre decl_type in
            let initialize =
              match initialize with
              | Assign e -> Stmt.Pattern.Assign (rewrite pre e)
              | i -> i in
            bind decl_id None;
            [{s with pattern= Decl {d with decl_type; initialize}}])
      else
        with_pre loc (fun pre ->
            let value =
              match initialize with
              | Assign e -> Some (value_of pre e)
              | _ -> None in
            let types =
              match rewrite_type pre decl_type with
              | Sized st -> map_tree (fun st -> Type.Sized st) (sized_tree st)
              | Unsized t -> map_tree (fun t -> Type.Unsized t) (type_tree t) in
            let named = bind_tuple decl_id t decl_adtype loc in
            let inits =
              match value with
              | Some v -> List.map (fun v -> Stmt.Pattern.Assign v) (leaves v)
              | None -> List.map (fun _ -> initialize) named in
            if List.length inits <> List.length named then
              unsupported "tuple declaration of different shapes";
            List.map2
              (fun ((name, _, adlevel), decl_type) initialize ->
                stmt loc
                  (Decl
                     { decl_adtype= adlevel
                     ; decl_id= name
                     ; decl_type
                     ; initialize
                     ; decl_annotations }))
              (List.combine named (leaves types))
              inits)
  | Assignment (lvalue, ut, rhs) ->
      with_pre loc (fun pre -> rewrite_assignment loc pre lvalue ut rhs)
  | TargetPE e -> with_pre loc (fun pre -> same (TargetPE (rewrite pre e)))
  | JacobianPE e -> with_pre loc (fun pre -> same (JacobianPE (rewrite pre e)))
  | NRFunApp (kind, args) -> with_pre loc (fun pre -> rewrite_nr_call loc pre kind args)
  | Return (Some e) when is_tuple_expr e ->
      with_pre loc (fun pre ->
          let value = value_of pre e in
          let t = e.meta.type_ in
          same
            (Return
               (Some
                  (make_tuple (leaves value) (flat_type t)
                     (flat_ad e.meta.adlevel t) e.meta.loc))))
  | Return (Some e) -> with_pre loc (fun pre -> same (Return (Some (rewrite pre e))))
  | Return None | Break | Continue | Skip -> [s]
  | IfElse (cond, a, b) ->
      with_pre loc (fun pre ->
          let cond = rewrite pre cond in
          let a = rewrite_body proc a in
          let b = Option.map (rewrite_body proc) b in
          same (IfElse (cond, a, b)))
  | While (cond, body) ->
      let pre = ref [] in
      let cond' = rewrite pre cond in
      let body' = rewrite_body proc body in
      if List.is_empty !pre then same (While (cond', body'))
      else
        (* The condition's statements run at the top of each iteration, so
           `continue` reaches them too. *)
        let check = stmt loc (IfElse (cond', stmt loc Skip, Some (stmt loc Break))) in
        same
          (While
             (one, stmt loc (Block (List.rev !pre @ [check; body']))))
  | For {loopvar; lower; upper; body} ->
      with_pre loc (fun pre ->
          let lower = rewrite pre lower in
          let upper = rewrite pre upper in
          let body =
            scoped (fun () ->
                bind loopvar None;
                rewrite_body proc body) in
          same (For {loopvar; lower; upper; body}))
  | Block body -> scoped (fun () -> same (Block (rewrite_block proc body)))
  | Profile (name, body) -> scoped (fun () -> same (Profile (name, rewrite_block proc body)))
  | SList body -> same (SList (rewrite_block proc body))

and rewrite_nr_call loc pre (kind : expr Fun_kind.t) args =
  let call kind args = [stmt loc (NRFunApp (kind, args))] in
  match (kind, args) with
  | CompilerInternal FnReadData, [({pattern= Var name; _} as v); context; {pattern= Lit (Str, key); meta}]
    when is_tuple_expr v -> (
      (* CmdStan's JSON reader names a tuple's leaves by their slots. *)
      match lookup name with
      | None -> unsupported ("tuple variable " ^ name)
      | Some tree ->
          List.map2
            (fun (leaf : expr) path ->
              stmt loc
                (NRFunApp
                   ( kind
                   , [ leaf; context
                     ; { pattern= Lit (Str, key ^ suffix path)
                       ; meta= {meta with type_= leaf.meta.type_; adlevel= DataOnly} } ] )))
            (leaves tree)
            (leaves (path_tree v.meta.type_)))
  | CompilerInternal (FnPrint | FnReject | FnFatalError), _ ->
      let args =
        List.concat_map
          (fun (a : expr) ->
            if is_tuple_expr a then spell a.meta.loc a.meta.type_ a.meta.adlevel (value_of pre a)
            else [rewrite pre a])
          args in
      call kind args
  | CompilerInternal (FnCheck {var; _}), _ when is_tuple_expr var ->
      unsupported "checking a whole tuple"
  | CompilerInternal (FnWriteParam {var; _}), _ when is_tuple_expr var ->
      unsupported "writing a whole tuple"
  | CompilerInternal internal, _ ->
      let internal = Internal_fun.map (rewrite pre) internal in
      if List.exists is_tuple_expr args then
        unsupported "a compiler function taking a tuple";
      call (CompilerInternal internal) (List.map (rewrite pre) args)
  | UserDefined _, _ -> call kind (spread pre args)
  | StanLib _, _ when higher_order args -> call kind (spread pre args)
  | _, _ when List.exists is_tuple_expr args ->
      unsupported "a library function taking a tuple"
  | _, _ -> call kind (List.map (rewrite pre) args)

let rewrite_fundef (fd : stmt Program.fun_def) =
  scoped (fun () ->
      let fdargs =
        List.concat_map
          (fun (ad, name, t) ->
            if is_tuple t then
              List.map (fun (n, ty, a) -> (a, n, ty)) (bind_tuple name t ad fd.fdloc)
            else (
              bind name None;
              [(ad, name, flat_fun_type t)]))
          fd.fdargs in
      let fdrt =
        match fd.fdrt with
        | ReturnType t when is_tuple t -> UnsizedType.ReturnType (flat_type t)
        | rt -> rt in
      let proc = {tuple_params= []} in
      let fdbody = Option.map (fun (body : stmt) -> block_of body.meta (rewrite_stmt proc body)) fd.fdbody in
      {fd with fdargs; fdrt; fdbody})

let rec stmt_has_tuple (s : stmt) =
  (match s.pattern with
  | Decl {decl_type; _} -> is_tuple (Type.to_unsized decl_type)
  | _ -> false)
  || Stmt.Pattern.fold
       (fun found e -> found || exists_expr is_tuple_expr e)
       (fun found s -> found || stmt_has_tuple s)
       false s.pattern

let has_tuples (mir : Program.Typed.t) =
  List.exists
    (fun (fd : stmt Program.fun_def) ->
      List.exists (fun (_, _, t) -> is_tuple t) fd.fdargs
      || (match fd.fdrt with ReturnType t -> is_tuple t | Void -> false)
      || Option.fold ~none:false ~some:stmt_has_tuple fd.fdbody)
    mir.functions_block
  || List.exists (List.exists stmt_has_tuple)
       [ mir.prepare_data; mir.log_prob; mir.reverse_mode_log_prob
       ; mir.generate_quantities; mir.transform_inits; mir.unconstrain_array ]

let scalarize (mir : Program.Typed.t) : Program.Typed.t =
  if not (has_tuples mir) then mir
  else (
    counter := 0;
    Hashtbl.reset temporaries;
    scopes := [];
    let tuple_params =
      List.filter_map
        (fun (name, _, (o : _ Program.outvar)) ->
          match o.out_block with
          | Parameters when SizedType.contains_tuple o.out_constrained_st -> Some name
          | _ -> None)
        mir.output_vars in
    let plain = {tuple_params= []} in
    let reading = {tuple_params} in
    let functions_block = List.map rewrite_fundef mir.functions_block in
    let result =
      scoped (fun () ->
          let prepare_data = rewrite_block plain mir.prepare_data in
          let section proc stmts = scoped (fun () -> rewrite_block proc stmts) in
          let log_prob = section reading mir.log_prob in
          let reverse_mode_log_prob = section reading mir.reverse_mode_log_prob in
          let generate_quantities = section reading mir.generate_quantities in
          let transform_inits = section plain mir.transform_inits in
          let unconstrain_array = section plain mir.unconstrain_array in
          { mir with
            functions_block
          ; prepare_data
          ; log_prob
          ; reverse_mode_log_prob
          ; generate_quantities
          ; transform_inits
          ; unconstrain_array }) in
    result)
