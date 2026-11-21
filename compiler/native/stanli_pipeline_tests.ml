open Middle

let compile ?(passes = Stanli_pipeline.default_pass_selection)
    ?(prune_unused_sections = true) ?(model_only = false) ?(cache_signatures = true) code =
  match
    Stanli_pipeline.compile_mir_with_passes ~passes ~prune_unused_sections ~model_only ~cache_signatures
      ~model_name:"pass_selection_test" code
  with
  | {result= Ok mir; _} -> mir
  | {result= Error _; _} -> failwith "Stan source did not compile"

let passes vectorize_loops =
  {Stanli_pipeline.default_pass_selection with vectorize_loops}

let budget max_statement_depth_cost =
  { Stanli_pipeline.default_pass_selection with
    max_o1_statement_depth_cost= Some max_statement_depth_cost }

let no_budget =
  { Stanli_pipeline.default_pass_selection with
    max_o1_statement_depth_cost= None }

let compile_portable code =
  match
    Stanli_pipeline.compile_portable ~model_name:"pass_selection_test" code
  with
  | {result= Ok encoded; _} -> encoded
  | {result= Error _; _} -> failwith "Stan source did not compile"

let compile_upstream_stable_o1 code =
  let flags =
    { Driver.Flags.default with
      optimization_level= Analysis_and_optimization.Optimize.O0 } in
  match
    Driver.Entry.stan2mir "pass_selection_test" (`Code code) flags (fun _ -> ())
  with
  | Ok mir ->
      let module O = Analysis_and_optimization.Optimize in
      O.optimization_suite
        ~settings:{ (O.level_optimizations O.O1) with
          preserve_stability=true; partial_evaluation=false } mir
  | Error _ -> failwith "Stan source did not compile"

let compile_upstream_o0 code =
  let flags =
    { Driver.Flags.default with
      optimization_level= Analysis_and_optimization.Optimize.O0 } in
  match
    Driver.Entry.stan2mir "pass_selection_test" (`Code code) flags (fun _ -> ())
  with
  | Ok mir -> mir
  | Error _ -> failwith "Stan source did not compile"

let encode mir = Portable_mir.encode mir

let compile_result ~passes code =
  Stanli_pipeline.compile_mir_with_passes ~passes
    ~model_name:"pass_selection_test" code

let rec count_for_stmt (stmt : Stmt.Located.t) =
  let current = match stmt.pattern with Stmt.Pattern.For _ -> 1 | _ -> 0 in
  Stmt.Pattern.fold
    (fun count (_ : Expr.Typed.t) -> count)
    (fun count child -> count + count_for_stmt child)
    current stmt.pattern

let count_log_prob_fors mir =
  List.fold_left
    (fun count stmt -> count + count_for_stmt stmt)
    0 mir.Program.log_prob

let rec has_vector_density_stmt (stmt : Stmt.Located.t) =
  let current =
    match stmt.pattern with
    | Stmt.Pattern.TargetPE
        { Expr.pattern=
            Expr.Pattern.FunApp
              ( Fun_kind.StanLib (_, Fun_kind.FnDist _)
              , {Expr.meta= {Expr.Typed.Meta.type_= UnsizedType.UVector; _}; _}
                :: _ )
        ; _ } ->
        true
    | _ -> false in
  current
  || Stmt.Pattern.fold
       (fun found (_ : Expr.Typed.t) -> found)
       (fun found child -> found || has_vector_density_stmt child)
       false stmt.pattern

let log_prob_has_vector_density mir =
  List.exists has_vector_density_stmt mir.Program.log_prob

let require condition message = if not condition then failwith message

let has_call name mir =
  let rec expr (e : Expr.Typed.t) =
    (match e.pattern with
     | Expr.Pattern.FunApp (Fun_kind.StanLib (called, _), _) ->
         String.equal called name
     | _ -> false)
    || Expr.Pattern.fold (fun found e -> found || expr e) false e.pattern in
  let rec stmt (s : Stmt.Located.t) =
    Stmt.Pattern.fold (fun found e -> found || expr e)
      (fun found s -> found || stmt s) false s.pattern in
  List.exists stmt mir.Program.log_prob

let arithmetic_preservation_test () =
  let implicit = compile {|
    parameters { real x; real y; real z; }
    model { target += x * y + z; }
  |} in
  let explicit = compile {|
    parameters { real x; real y; real z; }
    model { target += fma(x, y, z); }
  |} in
  require (not (has_call "fma" implicit))
    "ordinary multiplication and addition acquired fused rounding";
  require (has_call "fma" explicit)
    "an explicit fused operation lost its source semantics"

let fast_math_test () =
  let code = {|
    parameters { real a; real b; real z; }
    model { target += a * b + z; }
  |} in
  let compile_fast ~fast_math code =
    match Stanli_pipeline.compile_mir ~fast_math ~model_name:"fast_math_test" code with
    | {result= Ok mir; _} -> mir
    | {result= Error _; _} -> failwith "Stan source did not compile" in
  let default = compile_fast ~fast_math:false code in
  let fast = compile_fast ~fast_math:true code in
  require (not (has_call "fma" default))
    "fast_math=false fused a multiply-add";
  require (has_call "fma" fast)
    "fast_math=true did not run partial evaluation";
  require (String.equal (encode default) (encode (compile code)))
    "fast_math=false changed the default output";
  let portable ?fast_math () =
    match
      Stanli_pipeline.compile_portable ?fast_math ~model_name:"fast_math_test" code
    with
    | {result= Ok encoded; _} -> encoded
    | {result= Error _; _} -> failwith "Stan source did not compile" in
  require (String.equal (portable ()) (portable ~fast_math:false ()))
    "an explicit fast_math=false differs from the default";
  require (not (String.equal (portable ()) (portable ~fast_math:true ())))
    "fast_math=true left the portable encoding unchanged"

let repeated_scalar_argument_test () =
  let mir = compile {|
    functions {
      real piecewise(real x, real weight) {
        if (x < 1) return x * (weight - 1);
        return x * weight;
      }
    }
    parameters { real theta; real weight; }
    model { target += piecewise(exp(theta), weight); }
  |} in
  let rec expr n (e : Expr.Typed.t) =
    let n = match e.pattern with
      | FunApp (Fun_kind.StanLib ("exp", _), _) -> n + 1
      | _ -> n in
    Expr.Pattern.fold expr n e.pattern in
  let rec stmt n (s : Stmt.Located.t) =
    Stmt.Pattern.fold expr stmt n s.pattern in
  require (List.fold_left stmt 0 mir.Program.log_prob = 1)
    "inlining duplicated a repeated scalar function argument"

let inlining_effect_order_test () =
  let inline source =
    compile_upstream_o0 source
    |> Analysis_and_optimization.Optimize.function_inlining in
  let rec expr events (e : Expr.Typed.t) =
    let events = match e.pattern with
      | FunApp (Fun_kind.StanLib ("normal_rng", _), _) ->
          events @ ["normal_rng"]
      | _ -> events in
    Expr.Pattern.fold expr events e.pattern in
  let rec stmt events (s : Stmt.Located.t) =
    let events = match s.pattern with
      | NRFunApp (Fun_kind.CompilerInternal FnPrint, _) -> events @ ["print"]
      | _ -> events in
    Stmt.Pattern.fold expr stmt events s.pattern in
  let events mir = List.fold_left stmt [] mir.Program.generate_quantities in
  let ordered = inline {|
    functions {
      real twice(real x, real y) { return x * x + y * y; }
      real announce(real x) { print(x); return x; }
    }
    generated quantities {
      real a = twice(normal_rng(0, 1), announce(2));
      real b = twice(announce(3), normal_rng(4, 1));
    }
  |} in
  require (events ordered = ["print"; "normal_rng"; "normal_rng"; "print"])
    "inlining reordered argument effects";
  let void = inline {|
    functions { void twice(real x) { print(x, x); } }
    generated quantities { twice(normal_rng(0, 1)); }
  |} in
  require (events void = ["normal_rng"; "print"])
    "inlining duplicated a repeated argument of a void function"

let matching_loop =
  {|
    data {
      int<lower=0> N;
      vector[N] y;
    }
    parameters {
      real mu;
      real<lower=0> sigma;
    }
    model {
      for (n in 1:N) {
        y[n] ~ normal(mu, sigma);
      }
    }
  |}

let side_effect_loop =
  {|
    functions {
      real bump_lp(real x) {
        target += x;
        return x;
      }
    }
    data {
      int<lower=0> N;
      vector[N] y;
    }
    parameters {
      real<lower=0> sigma;
    }
    model {
      for (n in 1:N) {
        target += normal_lpdf(y[n] | bump_lp(sigma), sigma);
      }
    }
  |}

let o1_equivalence_models =
  [ ( "ordinary"
    , {|
        data { int<lower=0> N; vector[N] y; }
        parameters { real mu; real<lower=0> sigma; }
        model { y ~ normal(mu, sigma); }
      |} )
  ; ( "nested UDF"
    , {|
        functions {
          real inner(real x) { return square(x); }
          real outer(real x) { return inner(x) + 1; }
        }
        parameters { real x; }
        model { target += outer(x); }
      |} )
  ; ( "folded binary64"
    , {|
        parameters { real x; }
        model { target += x + (0.1 + 0.2); }
      |} )
  ; ( "checked int32 overflow"
    , {|
        transformed data {
          int x = 50000 * 50000;
          int y = (50000 * 50000) / 50000;
        }
      |} )
  ; ( "Unicode"
    , {|
        transformed data { print("pi π, snowman ☃, wave 👋"); }
        parameters { real x; }
        model { x ~ std_normal(); }
      |} )
  ; ( "generated quantities"
    , {|
        parameters { real x; }
        model { x ~ std_normal(); }
        generated quantities {
          real twice_x = 2 * x;
          real y = normal_rng(x, 1);
        }
      |} )
  ; ("matching loop", matching_loop) ]

(* Calls to [name] in an expression or statement, each with whether it sits
   in a loop body or a conditional branch rather than running
   unconditionally. *)
let calls_of name stmts =
  let found = ref [] in
  let rec expr guarded (e : Expr.Typed.t) =
    (match e.pattern with
    | FunApp (Fun_kind.UserDefined (called, _), _) when String.equal called name ->
        found := guarded :: !found
    | _ -> ());
    Expr.Pattern.fold (fun () e -> expr guarded e) () e.pattern in
  let rec stmt guarded (s : Stmt.Located.t) =
    match s.pattern with
    | While (cond, body) -> expr guarded cond; stmt true body
    | IfElse (cond, a, b) -> expr guarded cond; stmt true a; Option.iter (stmt true) b
    | _ -> Stmt.Pattern.fold (fun () e -> expr guarded e) (fun () s -> stmt guarded s) () s.pattern in
  List.iter (stmt false) stmts;
  List.rev !found

(* What may remain of tuples after Stanli_tuples: a flat tuple temporary
   assigned a call and read through projections, a flat tuple return, and an
   array of tuples printed whole. *)
let check_tuple_boundaries (mir : Program.Typed.t) =
  let open UnsizedType in
  let flat = function
    | UTuple parts -> List.for_all (fun t -> not (contains_tuple t)) parts
    | _ -> false in
  let temporaries = Hashtbl.create 8 in
  let fail what = failwith ("tuple left outside a boundary form: " ^ what) in
  let rec expr (e : Expr.Typed.t) =
    match e.pattern with
    | TupleProjection ({pattern= Var tmp; _}, _) when Hashtbl.mem temporaries tmp -> ()
    | _ ->
        if contains_tuple e.meta.type_ then fail (Fmt.str "%a" Expr.Typed.pp e);
        Expr.Pattern.fold (fun () e -> expr e) () e.pattern in
  let printed (e : Expr.Typed.t) =
    match (e.pattern, e.meta.type_) with
    | FunApp (CompilerInternal FnMakeTuple, parts), UArray _ -> List.iter expr parts
    | _ -> expr e in
  let rec stmt (s : Stmt.Located.t) =
    match s.pattern with
    | Decl {decl_id; decl_type; _} when contains_tuple (Type.to_unsized decl_type) ->
        if not (flat (Type.to_unsized decl_type)) then fail decl_id;
        Hashtbl.replace temporaries decl_id ()
    | Assignment ((LVariable tmp, []), _, {pattern= FunApp ((UserDefined _ | StanLib _), args); meta})
      when contains_tuple meta.type_ ->
        if not (Hashtbl.mem temporaries tmp && flat meta.type_) then fail tmp;
        List.iter expr args
    | Return (Some {pattern= FunApp (CompilerInternal FnMakeTuple, parts); meta})
      when flat meta.type_ ->
        List.iter expr parts
    | NRFunApp (CompilerInternal (FnPrint | FnReject | FnFatalError), args) ->
        List.iter printed args
    | Assignment ((LTupleProjection _, _), _, _) -> fail "tuple projection assignment"
    | _ -> Stmt.Pattern.fold (fun () e -> expr e) (fun () s -> stmt s) () s.pattern in
  List.iter
    (fun (fn : Stmt.Located.t Program.fun_def) ->
      List.iter (fun (_, name, t) -> if contains_tuple t then fail name) fn.fdargs;
      (match fn.fdrt with
      | ReturnType t when contains_tuple t && not (flat t) -> fail fn.fdname
      | _ -> ());
      Option.iter stmt fn.fdbody)
    mir.functions_block;
  List.iter (List.iter stmt)
    [ mir.prepare_data; mir.log_prob; mir.reverse_mode_log_prob
    ; mir.generate_quantities; mir.transform_inits; mir.unconstrain_array ]

let tuple_tests () =
  (* Overloading keeps these functions from being inlined, so each call
     returns its tuple through a temporary. *)
  let effect_once = {|
    functions {
      real pair_sum(tuple(real, real) t) { return t.1 + t.2; }
      tuple(real, real) draw_pair_rng(real mu) {
        print("draw_pair_rng ", mu);
        return (normal_rng(mu, 1), 1.0);
      }
      tuple(real, real) draw_pair_rng(vector mu) { return (normal_rng(mu[1], 1), 2.0); }
    }
    transformed data { real td = pair_sum(draw_pair_rng(0.5)); }
    parameters { real mu; }
    model { mu ~ normal(0, 1); }
    generated quantities {
      real y = pair_sum(draw_pair_rng(mu));
      tuple(real, real) both = draw_pair_rng(mu);
      real first = draw_pair_rng(mu).1;
    }
  |} in
  let fast =
    match Stanli_pipeline.compile_mir ~fast_math:true ~model_name:"tuples" effect_once with
    | {result= Ok mir; _} -> mir
    | {result= Error _; _} -> failwith "Stan source did not compile" in
  let fallback =
    match (compile_result ~passes:(budget 0) effect_once).result with
    | Ok mir -> mir
    | Error (Stanli_pipeline.Internal_error m | Unsupported m) ->
        failwith ("budgeted tuple model: " ^ m)
    | Error (Frontend_error e) -> failwith (Fmt.str "budgeted tuple model: %a" (fun ppf e -> Frontend.Errors.pp ppf e) e) in
  List.iter
    (fun (label, mir) ->
      check_tuple_boundaries mir;
      require (List.length (calls_of "draw_pair_rng" mir.Program.prepare_data) = 1)
        (label ^ ": a tuple call in transformed data did not run exactly once");
      require (List.length (calls_of "draw_pair_rng" mir.generate_quantities) = 3)
        (label ^ ": a generated-quantities tuple call was duplicated or lost"))
    [("O1", compile effect_once); ("fast math", fast); ("O0 fallback", fallback)];
  let guarded = {|
    functions {
      tuple(real, int) advance(real x) { return (x - 1, x > 0); }
      tuple(real, int) advance(vector x) { return (x[1] - 1, x[1] > 0); }
    }
    parameters { real mu; }
    model { mu ~ normal(0, 1); }
    generated quantities {
      real x = 3;
      int n = 0;
      while (advance(x).2 && n < 10) {
        n += 1;
        x = advance(x).1;
        if (n == 2) continue;
      }
      int a = (n > 3) && advance(x).2;
      int b = (n > 3) || advance(x).2;
      real c = n > 3 ? advance(x).1 : 0;
    }
  |} in
  List.iter
    (fun (label, mir) ->
      check_tuple_boundaries mir;
      let calls = calls_of "advance" mir.Program.generate_quantities in
      require (List.length calls = 5) (label ^ ": a guarded tuple call was duplicated or lost");
      require (List.for_all Fun.id calls)
        (label ^ ": a tuple call in a while condition, a right operand or a \
                  ternary branch runs unconditionally"))
    [ ("O1", compile guarded)
    ; ( "O0 fallback"
      , match (compile_result ~passes:(budget 0) guarded).result with
        | Ok mir -> mir
        | Error _ -> failwith "budgeted tuple model did not compile" ) ];
  (* Splitting runs before pruning, so dropping an unused function must not
     renumber the temporaries of the code that remains. *)
  let stability = {|
    functions {
      tuple(real, real) pair(real x) { return (x, x + 1); }
      tuple(real, real) pair(vector x) { return (x[1], x[1] + 1); }
      tuple(real, real) unused(real x) { return pair(x); }
    }
    data { tuple(real, array[2] int) d; }
    parameters { real mu; }
    model { mu ~ normal(pair(mu).2 + d.1, 1); }
    generated quantities {
      real y = pair(mu).1;
      tuple(real, real) z = pair(mu);
    }
  |} in
  let pruned = compile ~model_only:true stability in
  let unpruned = compile ~prune_unused_sections:false stability in
  require (not (List.exists (fun fn -> String.equal fn.Program.fdname "unused") pruned.functions_block))
    "the unused tuple function survived pruning";
  let projected =
    {unpruned with functions_block=
       List.filter
         (fun fn -> List.exists (fun kept -> String.equal kept.Program.fdname fn.Program.fdname)
                      pruned.functions_block)
         unpruned.functions_block} in
  require (String.equal (encode pruned) (encode projected))
    "pruning renamed a tuple temporary or changed a split procedure"

(* Partial evaluation fuses a real a * b + c into fma. An integer one stays
   integer arithmetic, because fma returns a real. *)
let fast_math_integer_test () =
  let code = {|
    data { int d; int i; int j; array[10] real x; }
    parameters { real a; }
    model { target += a * x[d * (i - 1) + j]; target += a * a + x[1]; }
  |} in
  let mir =
    match Stanli_pipeline.compile_mir ~fast_math:true ~model_name:"fast_math_int" code with
    | {result= Ok mir; _} -> mir
    | {result= Error _; _} -> failwith "Stan source did not compile" in
  let rec int_fma (e : Expr.Typed.t) =
    (match e.pattern with
     | Expr.Pattern.FunApp (Fun_kind.StanLib ("fma", _), _) ->
         e.meta.type_ = UnsizedType.UInt
     | _ -> false)
    || Expr.Pattern.fold (fun found e -> found || int_fma e) false e.pattern in
  let rec stmt (s : Stmt.Located.t) =
    Stmt.Pattern.fold (fun found e -> found || int_fma e)
      (fun found s -> found || stmt s) false s.pattern in
  require (has_call "fma" mir) "fast_math=true did not fuse a real multiply-add";
  require (not (List.exists stmt mir.Program.log_prob))
    "fast_math=true turned an integer index into a real fma"

let () =
  let builtin_sets = Lazy.force Stan_math_signatures.lazy_signatures_alist in
  let materialized () =
    List.fold_left (fun n (_, entries) -> n + if Lazy.is_val entries then 1 else 0)
      0 builtin_sets in
  require (List.length builtin_sets > 500) "missing built-in signature names";
  require (materialized () = 0) "built-in registry eagerly materialized overloads";
  ignore (compile "parameters { real x; } model { x ~ normal(0, 1); }");
  require (materialized () > 0 && materialized () < List.length builtin_sets / 2)
    "simple compilation forced unrelated built-in overloads";
  arithmetic_preservation_test ();
  fast_math_test ();
  fast_math_integer_test ();
  repeated_scalar_argument_test ();
  inlining_effect_order_test ();
  tuple_tests ();
  List.iter (fun code ->
      match (Stanli_pipeline.compile_mir_with_passes
        ~passes:Stanli_pipeline.default_pass_selection ~model_name:"undefined" code).result with
      | Error (Stanli_pipeline.Frontend_error _) -> ()
      | _ -> failwith "undefined user function escaped the lazy environment scan")
    [ "functions { real f(real x); } model {}"
    ; "functions { real f(real x); real f(real x, real y) { return x+y; } } model {}" ];
  let module S = Frontend.SignatureMismatch in
  let open UnsizedType in
  let signatures =
    [ ("normal_lpdf", [(AutoDiffable, UReal); (DataOnly, UReal); (DataOnly, UReal)])
    ; ("normal_lpdf", [(DataOnly, UReal); (DataOnly, UReal); (DataOnly, UReal)])
    ; ("add", [(AutoDiffable, UInt); (DataOnly, UReal)])
    ; ("add", [(AutoDiffable, UVector); (DataOnly, UVector)])
    ; ("does_not_exist", [(AutoDiffable, UReal)])
    ; ("normal_lpdf", [(AutoDiffable, UMatrix)]) ] in
  let lookup (name, args) =
    match S.matching_stanlib_function name args with
    | S.UniqueMatch (ret, kind, promotions, location) ->
        S.UniqueMatch (ret, kind Fun_kind.FnPlain, promotions, location)
    | S.AmbiguousMatch result -> S.AmbiguousMatch result
    | S.SignatureErrors result -> S.SignatureErrors result in
  let expected = List.map lookup signatures in
  S.with_stanlib_cache (fun () ->
      for _ = 1 to 3 do
        require (List.map lookup signatures = expected)
          "cached signature result/promotion/error changed";
        S.with_stanlib_cache (fun () ->
            require (List.map lookup signatures = expected)
              "nested signature cache changed results")
      done;
      (try S.with_stanlib_cache (fun () -> raise Exit) with Exit -> ());
      require (List.map lookup signatures = expected)
        "failed nested signature lookup changed outer scope");
  require (List.map lookup signatures = expected)
    "signature scope changed subsequent uncached lookup";
  let reachability_model = {|
    functions {
      real never_called(real x) { return exp(x); }
      real helper(real x) { return x * x; }
      vector rhs(real t, vector y) { return helper(t) * y; }
      real overloaded(real x) { return x + 1; }
      real overloaded(real x, real y) { return x + y; }
    }
    transformed data { real a = overloaded(1.0); }
    parameters { real x; }
    model {
      array[1] vector[1] z = ode_rk45(rhs, rep_vector(x, 1), 0.0, {1.0});
      target += overloaded(z[1, 1], a);
    }
    generated quantities { real check = overloaded(x); }
  |} in
  let pruned = compile ~model_only:true reachability_model in
  let retained name =
    List.exists (fun fn -> String.equal fn.Program.fdname name)
      pruned.functions_block in
  require (not (retained "never_called")) "unreachable UDF survived pruning";
  List.iter (fun name -> require (retained name) ("lost reachable UDF " ^ name))
    ["rhs"; "helper"; "overloaded"];
  require (List.length pruned.functions_block = 4)
    "pruning did not retain both overloads and the transitive ODE callback";
  require (pruned.reverse_mode_log_prob = [] && pruned.unconstrain_array = [])
    "unused backend procedures survived pruning";
  let old = compile ~prune_unused_sections:false reachability_model in
  require (List.length (compile reachability_model).functions_block = 5)
    "general compilation lost an exported function";
  let projected_old =
    {old with functions_block=
       List.filter (fun fn -> retained fn.Program.fdname) old.functions_block} in
  require (String.equal (encode pruned) (encode projected_old))
    "pruning changed a consumed procedure or retained function body";

  List.iter
    (fun (name, code) ->
      let pass_off_bytes =
        encode (compile ~passes:(passes false) ~prune_unused_sections:false code) in
      let upstream_o1_bytes = encode (compile_upstream_stable_o1 code) in
      require
        (String.equal pass_off_bytes upstream_o1_bytes)
        ("pass-off output differs from upstream stable O1 for " ^ name);
      let pass_on_bytes =
        encode (compile ~passes:(passes true) code) in
      require
        (String.equal pass_on_bytes
           (encode (compile ~cache_signatures:false ~passes:(passes true) code)))
        ("signature cache changes portable MIR for " ^ name);
      let production_bytes = compile_portable code in
      require
        (String.equal production_bytes pass_on_bytes)
        ("production output differs from O1 plus vectorize_loops for " ^ name))
    o1_equivalence_models;

  (* The structural guard is decided after isolated inlining but before any
     dataflow optimization.  A deliberately tiny budget makes this ordinary
     model exercise the fallback without putting a pathological compiler case
     in the unit suite. *)
  let fallback = compile_result ~passes:(budget 0) matching_loop in
  let fallback_mir =
    match fallback.result with
    | Ok mir -> mir
    | Error _ -> failwith "budgeted fallback model did not compile" in
  require
    (String.equal (encode fallback_mir)
       (encode (compile_upstream_o0 matching_loop)))
    "budgeted fallback did not return untouched transformed O0 MIR";
  require
    (match fallback.diagnostics with
    | [Stanli_pipeline.O1_budget_exceeded {cost; budget= 0; _}] -> cost > 0
    | _ -> false)
    "budgeted fallback did not report its structural cost and budget";

  let guarded_o1 =
    compile_result ~passes:(budget Stdlib.max_int) matching_loop in
  let unguarded_o1 = compile_result ~passes:no_budget matching_loop in
  let require_o1 result label =
    match result.Stanli_pipeline.result with
    | Ok mir ->
        require
          (String.equal (encode mir)
             (encode (compile ~passes:(passes true) matching_loop)))
          (label ^ " changed ordinary O1 output");
        require
          (List.is_empty result.diagnostics)
          (label ^ " emitted a fallback diagnostic")
    | Error _ -> failwith (label ^ " did not compile") in
  require_o1 guarded_o1 "feature-on under-budget path";
  require_o1 unguarded_o1 "feature-off path";

  let malformed =
    compile_result ~passes:(budget 0) "parameters { real x } model { }" in
  require
    (match malformed.result with
    | Error (Stanli_pipeline.Frontend_error _) -> true
    | _ -> false)
    "the O1 fallback accepted malformed Stan source";
  require
    (List.is_empty malformed.diagnostics)
    "malformed Stan source was mislabeled as an O1 fallback";

  let off = compile ~passes:(passes false) matching_loop in
  let on = compile ~passes:(passes true) matching_loop in
  let production = compile matching_loop in
  require (count_log_prob_fors off > 0) "pass-off removed the matching loop";
  require (count_log_prob_fors on = 0) "pass-on did not vectorize the loop";
  require
    (count_log_prob_fors production = 0)
    "production selection did not vectorize the loop";
  require
    (log_prob_has_vector_density on)
    "pass-on did not produce a vector density statement";
  require
    (log_prob_has_vector_density production)
    "production selection did not produce a vector density statement";

  let side_effect_off =
    compile ~passes:(passes false) side_effect_loop in
  let side_effect_on =
    compile ~passes:(passes true) side_effect_loop in
  require
    (count_log_prob_fors side_effect_on > 0)
    "pass-on removed a side-effecting loop";
  require
    (not (log_prob_has_vector_density side_effect_off))
    "pass-off vectorized the density beside a side effect";
  require
    (log_prob_has_vector_density side_effect_on)
    "pass-on left the density beside a side effect in the loop"

(* A reader for the portable MIR payload written from SCHEMA.md, independent
   of the encoder, so the tests check the documented layout. It rejects any
   tag the schema does not list and any trailing byte. *)
module Wire = struct
  type view = int * int

  type expr =
    {kind: int; name: string; args: expr list; view: view; lit_s: string}

  type item = Arg | Tuple of int * item list

  type stmt =
    | Decl of {id: string; view: view option; data_only: bool}
    | Assign of string * expr
    | Multi of string list * expr
    | Return of expr list
    | Call of {name: string; args: expr list; layout: item list option}
    | Body of int * stmt list
    | Unsupported
    | Other of int * expr list

  type fn =
    {fname: string; arg_views: view list; results: view list; body: stmt list}

  type program =
    { inputs: string list
    ; procedures: stmt list list
    ; functions: fn list
    ; exprs: expr list
    ; leaves: (string * string * (int * int) list) list }

  let decode_base64 text =
    let value c =
      match c with
      | 'A' .. 'Z' -> Char.code c - 65
      | 'a' .. 'z' -> Char.code c - 71
      | '0' .. '9' -> Char.code c + 4
      | '+' -> 62
      | '/' -> 63
      | _ -> failwith "bad base64" in
    let out = Buffer.create (String.length text) in
    let n = String.length text in
    let i = ref 0 in
    while !i < n do
      let q k = if text.[!i + k] = '=' then 0 else value text.[!i + k] in
      let bits = (q 0 lsl 18) lor (q 1 lsl 12) lor (q 2 lsl 6) lor q 3 in
      Buffer.add_char out (Char.chr (bits lsr 16));
      if text.[!i + 2] <> '=' then
        Buffer.add_char out (Char.chr ((bits lsr 8) land 0xff));
      if text.[!i + 3] <> '=' then Buffer.add_char out (Char.chr (bits land 0xff));
      i := !i + 4
    done;
    Buffer.contents out

  let read encoded =
    let prefix = "STANLI5:" in
    if not (String.starts_with ~prefix encoded) then failwith "missing STANLI5:";
    let s =
      decode_base64
        (String.sub encoded (String.length prefix)
           (String.length encoded - String.length prefix)) in
    let pos = ref 0 in
    let exprs = ref [] in
    let u8 () =
      let v = Char.code s.[!pos] in
      incr pos; v in
    let u32 () =
      let a = u8 () in let b = u8 () in let c = u8 () in let d = u8 () in
      a lor (b lsl 8) lor (c lsl 16) lor (d lsl 24) in
    let skip n = pos := !pos + n in
    let bool () =
      match u8 () with 0 -> false | 1 -> true | _ -> failwith "bad bool" in
    let string () =
      let n = u32 () in
      let v = String.sub s !pos n in
      skip n; v in
    let list f = List.init (u32 ()) (fun _ -> f ()) in
    let optional f = if bool () then Some (f ()) else None in
    let view () =
      let depth = u8 () in
      let leaf = u8 () in
      if leaf > 6 then failwith ("unknown view leaf " ^ string_of_int leaf);
      (depth, leaf) in
    let function_type () =
      ignore (list view); ignore (u8 ()); ignore (bool ()) in
    let rec expr () =
      let kind = u8 () in
      let name = ref "" and args = ref [] and lit_s = ref "" in
      (match kind with
      | 0 -> name := string (); ignore (optional function_type)
      | 1 -> skip 4
      | 2 -> skip 8
      | 3 -> lit_s := string ()
      | 4 ->
          ignore (u8 ()); name := string (); ignore (u8 ()); ignore (bool ());
          args := list expr
      | 10 -> ()
      | k when k >= 5 && k <= 15 -> args := list expr
      | k -> failwith ("unknown expression kind " ^ string_of_int k));
      let v = view () in
      ignore (bool ()); ignore (bool ()); ignore (string ());
      let e = {kind; name= !name; args= !args; view= v; lit_s= !lit_s} in
      exprs := e :: !exprs;
      e in
    let transform () =
      let kind = u8 () in
      if kind > 18 then failwith ("unknown transform kind " ^ string_of_int kind);
      ignore (list expr); ignore (string ()) in
    let rec sized () =
      let base = string () in
      ignore (list expr);
      let elem = string () in
      ignore (string ());
      if base = "STuple" || (base = "SArray" && elem = "STuple") then
        ignore (list sized) in
    let rec item () =
      match u8 () with
      | 0 -> Arg
      | 1 -> let depth = u8 () in Tuple (depth, list item)
      | k -> failwith ("unknown print item " ^ string_of_int k) in
    let rec stmt () =
      match u8 () with
      | 0 ->
          let id = string () in
          let v =
            match u8 () with
            | 0 -> sized (); None
            | 1 -> let v = view () in ignore (string ()); Some v
            | _ -> failwith "bad decl type" in
          let data_only = bool () in
          if bool () then ignore (expr ());
          ignore (optional transform); ignore (list expr); ignore (string ());
          Decl {id; view= v; data_only}
      | 1 ->
          let lhs = string () in
          ignore (list expr);
          let rhs = expr () in
          ignore (string ()); Assign (lhs, rhs)
      | (2 | 14) as k -> let e = expr () in ignore (string ()); Other (k, [e])
      | (3 | 4) as k -> let b = list stmt in ignore (string ()); Body (k, b)
      | 5 ->
          ignore (string ()); ignore (expr ()); ignore (expr ());
          let b = list stmt in ignore (string ()); Body (5, b)
      | (6 | 7) as k ->
          ignore (expr ()); let b = list stmt in ignore (string ()); Body (k, b)
      | 8 ->
          ignore (u8 ());
          let name = string () in
          ignore (u8 ());
          let args = list expr in
          let layout =
            match u8 () with
            | 0 -> None
            | 1 -> transform (); ignore (string ()); ignore (expr ()); None
            | 2 -> ignore (optional transform); ignore (expr ()); None
            | 3 -> Some (list item)
            | k -> failwith ("unknown payload " ^ string_of_int k) in
          ignore (string ()); Call {name; args; layout}
      | 9 -> let values = list expr in ignore (string ()); Return values
      | (10 | 11) as k -> Other (k, [])
      | 12 -> ignore (string ()); Other (12, [])
      | 13 -> ignore (string ()); Unsupported
      | 15 ->
          let targets = list string in
          let call = expr () in
          ignore (string ()); Multi (targets, call)
      | k -> failwith ("unknown statement kind " ^ string_of_int k) in
    let inputs = list (fun () -> let n = string () in sized (); n) in
    let prepare = list stmt in
    let log_prob = list stmt in
    let gq = list stmt in
    ignore (u32 ()); ignore (u32 ());
    let functions =
      list (fun () ->
          let fname = string () in
          ignore (list string);
          let arg_views = list view in
          ignore (list bool);
          let results = list view in
          {fname; arg_views; results; body= list stmt}) in
    ignore
      (list (fun () ->
           ignore (string ()); ignore (u8 ()); sized (); ignore (list string)));
    let inits = list stmt in
    let leaves =
      list (fun () ->
          let variable = string () in
          let base = string () in
          let path = list (fun () -> let b = u32 () in (b, u32 ())) in
          (variable, base, path)) in
    if !pos <> String.length s then failwith "trailing payload bytes";
    { inputs
    ; procedures= [prepare; log_prob; gq; inits]
    ; functions
    ; exprs= !exprs
    ; leaves }

  let rec stmts_of = function
    | Body (_, body) -> body @ List.concat_map stmts_of body
    | _ -> []

  let all_stmts program =
    let roots =
      List.concat program.procedures
      @ List.concat_map (fun fn -> fn.body) program.functions in
    roots @ List.concat_map stmts_of roots

  let rec exprs_of e = e :: List.concat_map exprs_of e.args

  let stmt_exprs = function
    | Assign (_, e) | Multi (_, e) -> exprs_of e
    | Return es | Call {args= es; _} | Other (_, es) ->
        List.concat_map exprs_of es
    | Decl _ | Body _ | Unsupported -> []

  let has_unsupported program =
    List.mem Unsupported (all_stmts program)
    || List.exists (fun e -> e.kind = 10) program.exprs
end

let find_function (program : Wire.program) name =
  List.find (fun fn -> fn.Wire.fname = name) program.functions

let tuple_wire_tests () =
  require (Portable_mir.tuple_leaf_name "x" [2; 1] = "x.2.1")
    "tuple_leaf_name did not join the components with dots";
  require (Portable_mir.tuple_leaf_name "x" [] = "x")
    "tuple_leaf_name changed a name without components";
  (* Every ordinary model reads back exactly under the documented layout,
     with no tuple tag and an empty leaf list. *)
  List.iter
    (fun (name, code) ->
      let program = Wire.read (compile_portable code) in
      require (program.leaves = []) ("tuple leaves for " ^ name))
    o1_equivalence_models;
  let functions = Wire.read (compile_portable {|
    functions {
      void hello(real x) { print(x); return; }
      real g(real x) { if (x > 0) return x; return -x; }
      vector h(vector v) { return 2 * v; }
    }
    parameters { real x; }
    model { hello(x); target += g(x) + sum(h(rep_vector(x, 2))); }
  |}) in
  let returns fn =
    List.filter_map
      (function Wire.Return values -> Some (List.length values) | _ -> None)
      (Wire.all_stmts {functions with procedures= []; functions= [fn]}) in
  let hello = find_function functions "hello" in
  let g = find_function functions "g" in
  let h = find_function functions "h" in
  require (hello.results = []) "a void function has a result";
  require (returns hello = [0]) "a void return carries a value";
  require (g.results = [(0, 2)]) "a real function's result is not one Real";
  require (returns g = [1; 1]) "a real function's returns are not one value";
  require (h.results = [(0, 4)]) "a vector function's result is not one Vector";
  require (h.arg_views = [(0, 4)]) "a vector argument is not one Vector";
  (* A tuple-returning call the compiler did not inline: its temporary becomes
     one declaration per leaf and one MultiAssign, and each component read
     becomes a read of its leaf. *)
  let calls = Wire.read (encode (compile_upstream_o0 {|
    functions {
      tuple(real, vector) f(real x, vector v) { return (x * 2, v * x); }
    }
    data { matrix[3, 3] A; }
    parameters { real x; vector[2] v; }
    model {
      tuple(matrix[3, 3], matrix[3, 3]) t = qr_thin(A * x);
      tuple(real, vector[2]) u = f(x, v);
      target += sum(t.1) + sum(t.2) + u.1 + sum(u.2);
    }
  |})) in
  require (not (Wire.has_unsupported calls))
    "a split tuple temporary left an unsupported node";
  (* A component read in a size, an initializer, a loop bound. *)
  let sizes = Wire.read (encode (compile_upstream_o0 {|
    functions { tuple(int, real) f(real x) { return (2, x); } }
    parameters { real x; }
    model {
      tuple(int, real) t = f(x);
      vector[t.1] v = rep_vector(t.2, t.1);
      real s = t.2;
      for (i in 1:t.1) target += v[i];
      target += s;
    }
  |})) in
  require (not (Wire.has_unsupported sizes))
    "a component read in a size or initializer is unsupported";
  let f = find_function calls "f" in
  require (f.results = [(0, 2); (0, 4)])
    "a tuple function's results are not its leaves";
  require (returns f = [2]) "a tuple return does not list its leaves";
  let log_prob = List.nth calls.procedures 1 in
  let stmts = log_prob @ List.concat_map Wire.stmts_of log_prob in
  let multi =
    List.filter_map
      (function Wire.Multi (targets, call) -> Some (targets, call) | _ -> None)
      stmts in
  require
    (List.map (fun (targets, call) -> (targets, call.Wire.name, call.view)) multi
     = [ (["t.1"; "t.2"], "qr_thin", (0, 0))
       ; (["u.1"; "u.2"], "f", (0, 0)) ])
    "a tuple-returning call is not a MultiAssign to its leaves";
  let decls =
    List.filter_map
      (function Wire.Decl {id; data_only; _} -> Some (id, data_only) | _ -> None)
      stmts in
  List.iter
    (fun leaf ->
      require (List.mem (leaf, false) decls) ("leaf " ^ leaf ^ " not declared"))
    ["t.1"; "t.2"; "u.1"; "u.2"];
  let reads =
    List.concat_map Wire.stmt_exprs stmts
    |> List.filter_map (fun e -> if e.Wire.kind = 0 then Some e.name else None) in
  List.iter
    (fun leaf -> require (List.mem leaf reads) ("leaf " ^ leaf ^ " not read"))
    ["t.1"; "t.2"; "u.1"; "u.2"];
  (* Tuple inputs and outputs list their leaves with typed paths. A model
     whose tuples the compiler has not split yet still encodes: what cannot
     be written becomes unsupported rather than an error. *)
  let interface = Wire.read (compile_portable {|
    functions {
      tuple(real, vector) f(real x, vector v) { return (x * 2, v * x); }
    }
    data {
      array[2] tuple(real, vector[2]) p;
      tuple(real, array[3] tuple(int, real)) q;
    }
    parameters { tuple(real, real) r; }
    model {
      tuple(real, vector[2]) u = f(r.1, p[1].2);
      target += u.1 + sum(u.2) + q.1;
      print(p, (r.1, 3));
    }
    generated quantities { tuple(int, real) g = (1, r.2); }
  |}) in
  require
    (interface.leaves
     = [ ("p.1", "p", [(1, 1)]); ("p.2", "p", [(1, 2)])
       ; ("q.1", "q", [(0, 1)]); ("q.2.1", "q", [(0, 2); (1, 1)])
       ; ("q.2.2", "q", [(0, 2); (1, 2)])
       ; ("r.1", "r", [(0, 1)]); ("r.2", "r", [(0, 2)])
       ; ("g.1", "g", [(0, 1)]); ("g.2", "g", [(0, 2)]) ])
    "tuple leaves of the inputs and outputs are wrong";
  require (interface.inputs = ["p"; "q"]) "tuple inputs lost"

(* print, reject and fatal_error take an array of tuples as its leaves and a
   layout; a tuple outside an array as string literals around its leaves. *)
let tuple_print_test () =
  let mir = compile_upstream_o0 {|
    transformed data {
      array[2] real a;
      array[2] vector[2] b;
      array[2] int c;
      real s = 1;
      print(s);
    }
  |} in
  let open UnsizedType in
  let expr type_ pattern =
    {Expr.meta= {Expr.Typed.Meta.empty with type_; adlevel= DataOnly}; pattern} in
  let var type_ name = expr type_ (Expr.Pattern.Var name) in
  let tuple type_ args =
    expr type_ (Expr.Pattern.FunApp (Fun_kind.CompilerInternal FnMakeTuple, args)) in
  let nested =
    tuple (UArray (UTuple [UReal; UTuple [UInt; UVector]]))
      [ var (UArray UReal) "a"
      ; tuple (UArray (UTuple [UInt; UVector]))
          [var (UArray UInt) "c"; var (UArray UVector) "b"] ] in
  let plain = tuple (UTuple [UReal; UReal]) [var UReal "s"; var UReal "s"] in
  let rewrite (stmt : Stmt.Located.t) =
    match stmt.pattern with
    | NRFunApp ((CompilerInternal FnPrint as kind), _) ->
        { stmt with
          pattern=
            SList
              [ stmt
              ; {stmt with pattern= NRFunApp (kind, [nested; plain])} ] }
    | _ -> stmt in
  let program =
    Wire.read (encode {mir with prepare_data= List.map rewrite mir.prepare_data}) in
  let calls =
    List.filter_map
      (function
        | Wire.Call {name= "FnPrint"; args; layout} ->
            Some
              ( List.map
                  (fun e -> if e.Wire.kind = 3 then e.lit_s else e.name)
                  args
              , layout )
        | _ -> None)
      (Wire.all_stmts program) in
  require
    (calls
     = [ (["s"], None)
       ; ( ["a"; "c"; "b"; "("; "s"; ","; "s"; ")"]
         , Some
             Wire.
               [ Tuple (1, [Arg; Tuple (0, [Arg; Arg])])
               ; Arg; Arg; Arg; Arg; Arg ] ) ])
    "print of tuples has the wrong arguments or layout"

(* A nested array of tuples printed from source keeps its nesting in the
   layout. *)
let tuple_print_source_test () =
  let program = Wire.read (compile_portable {|
    parameters { real x; }
    model { x ~ normal(0, 1); }
    generated quantities {
      array[2] tuple(int, tuple(real, vector[2])) g;
      for (i in 1:2) g[i] = (i, (2 * i * x, [i * x, 2 * i * x]'));
      print("g=", g);
    }
  |}) in
  require (not (Wire.has_unsupported program))
    "printing a nested array of tuples left an unsupported node";
  let layouts =
    List.filter_map
      (function
        | Wire.Call {name= "FnPrint"; layout; _} -> Some layout | _ -> None)
      (Wire.all_stmts program) in
  require
    (layouts = [Some Wire.[Arg; Tuple (1, [Arg; Tuple (0, [Arg; Arg])])]])
    "a nested array of tuples printed from source has the wrong layout"

(* Splitting tuple arguments can make two overloads take the same arguments;
   a forward declaration and its definition are not overloads. *)
let split_overload_test () =
  ignore (compile_portable {|
    functions { real f(real a); real f(real a) { return a; } }
    parameters { real x; }
    model { target += f(x); }
  |});
  let mir = compile_upstream_o0 {|
    functions {
      real f(real a, real b) { return a + b; }
      real f(real a, int b) { return a * b; }
    }
    parameters { real x; }
    model { target += f(x, x) + f(x, 1); }
  |} in
  let same =
    match mir.functions_block with
    | first :: second :: rest ->
        first :: {second with fdargs= first.fdargs} :: rest
    | _ -> failwith "expected two overloads" in
  match encode {mir with functions_block= same} with
  | _ -> failwith "colliding overloads were encoded"
  | exception Portable_mir.Not_supported message ->
      require
        (String.starts_with ~prefix:"user function f has overloads" message)
        ("unexpected overload message: " ^ message)

(* Phase A's own failures: a promotion around a call it did not inline, a
   statement it cannot rewrite, and overloads its split makes collide. *)
let tuple_phase_a_tests () =
  let unsupported_error code =
    match
      (Stanli_pipeline.compile_portable ~model_name:"phase_a_test" code).result
    with
    | Error (Stanli_pipeline.Unsupported message) -> message
    | Error (Stanli_pipeline.Internal_error message) ->
        failwith ("an internal error instead of a compile error: " ^ message)
    | Error (Frontend_error _) -> failwith "a frontend error"
    | Ok _ -> failwith "compiled" in
  let promoted = Wire.read (compile_portable {|
    functions {
      tuple(int, real) g(real x) { return (2, x); }
      real f(real x) {
        tuple(real, real) u = g(x);
        tuple(real, real) w;
        w = g(x + 1);
        return u.1 * u.2 + w.1 * w.2;
      }
      tuple(real, real) h(real x) { return g(x); }
    }
    parameters { real x; }
    model { target += f(x); }
  |}) in
  require (not (Wire.has_unsupported promoted))
    "a promotion around a tuple call left an unsupported node";
  List.iter (fun passes ->
  match (compile_result ~passes {|
    functions {
      tuple(int, real) g(real x) { return (2, x); }
    }
    parameters { real x; }
    model { tuple(real, real) u = g(x); target += u.1 * u.2; }
    generated quantities { tuple(real, real) q = g(x); }
  |}).result with
  | Ok mir -> require (not (Wire.has_unsupported (Wire.read (encode mir))))
      ("a promotion around a tuple call left an unsupported node: " ^ Sexplib0.Sexp.to_string_hum (Program.Typed.sexp_of_t mir))
  | Error _ -> failwith "promotion model did not compile") [budget 0; no_budget];
  (* A statement Phase A cannot rewrite: a check of a whole tuple. Over a
     split tuple it names the variable; over a tuple expression it is left
     for the runtime to refuse. *)
  let mir = compile_upstream_o0 {|
    parameters { real x; }
    model { tuple(real, real) t = (x, x); target += t.1; }
  |} in
  let tuple_type = UnsizedType.UTuple [UReal; UReal] in
  let tuple_expr pattern : Expr.Typed.t =
    { pattern
    ; meta=
        { Expr.Typed.Meta.empty with
          type_= tuple_type
        ; adlevel= TupleAD [AutoDiffable; AutoDiffable] } } in
  let check var : Stmt.Located.t =
    { pattern=
        NRFunApp
          ( CompilerInternal
              (FnCheck {trans= Transformation.Lower (Expr.Helpers.int 0); var_name= "t"; var})
          , [Expr.Helpers.int 0] )
    ; meta= Location_span.empty } in
  let with_check var =
    let rec add (stmt : Stmt.Located.t) =
      match stmt.pattern with
      | Block body -> {stmt with pattern= Block (List.map add body @ [check var])}
      | _ -> stmt in
    {mir with log_prob= List.map add mir.log_prob} in
  (match Stanli_tuples.scalarize (with_check (tuple_expr (Var "t"))) with
  | _ -> failwith "a check of a split tuple was left unrewritten"
  | exception Portable_mir.Not_supported message ->
      require
        (String.equal message
           "stanli does not support checking a whole tuple (t) yet")
        ("unexpected message: " ^ message));
  let x = {Expr.Typed.Meta.empty with type_= UReal; adlevel= AutoDiffable} in
  ignore
    (Stanli_tuples.scalarize
       (with_check
          (tuple_expr
             (FunApp
                ( CompilerInternal FnMakeTuple
                , [{pattern= Var "x"; meta= x}; {pattern= Var "x"; meta= x}] )))));
  let message = unsupported_error {|
    functions {
      real f(tuple(real, real) t) { return t.1; }
      real f(real a, real b) { return a + b; }
    }
    parameters { real x; }
    model { target += f((x, x)) + f(x, x); }
  |} in
  require
    (String.starts_with ~prefix:"user function f has overloads" message)
    ("unexpected overload message: " ^ message)

let has_user_call name (mir : Program.Typed.t) =
  let rec expr (e : Expr.Typed.t) =
    (match e.pattern with
     | Expr.Pattern.FunApp (Fun_kind.UserDefined (called, _), _) ->
         String.equal called name
     | _ -> false)
    || Expr.Pattern.fold (fun found e -> found || expr e) false e.pattern in
  let rec stmt (s : Stmt.Located.t) =
    Stmt.Pattern.fold (fun found e -> found || expr e)
      (fun found s -> found || stmt s) false s.pattern in
  List.exists stmt mir.log_prob

let parse_passes spec =
  match Stanli_pipeline.parse_pass_override spec with
  | Ok passes -> passes
  | Error message -> failwith ("--passes " ^ spec ^ ": " ^ message)

let pass_override_tests () =
  let code =
    {|
    functions { real sq(real x) { return x * x; } }
    data { int N; vector[N] y; }
    parameters { real mu; }
    model {
      real s = sq(mu);
      for (n in 1:N) y[n] ~ normal(mu, 1 + s);
    }
  |} in
  let mir passes =
    match
      Stanli_pipeline.compile_mir ~passes ~model_name:"pass_selection_test"
        code
    with
    | {result= Ok mir; diagnostics= []; _} -> mir
    | {result= Ok _; _} -> failwith "pass override emitted a diagnostic"
    | {result= Error _; _} -> failwith "pass override did not compile" in
  let portable ?passes () =
    match
      Stanli_pipeline.compile_portable ?passes ~model_name:"pass_selection_test"
        code
    with
    | {result= Ok encoded; _} -> encoded
    | {result= Error _; _} -> failwith "pass override did not compile" in
  require
    (String.equal (encode (mir (parse_passes "none")))
       (encode (compile_upstream_o0 code)))
    "--passes none is not the transformed O0 MIR";
  require
    (String.equal (portable ~passes:(parse_passes "o1") ()) (portable ()))
    "--passes o1 differs from the production output";
  let production_list =
    "function_inlining,constant_propagation,copy_propagation,\
     dead_code_elimination,vectorize_loops,list_collapsing,optimize_soa,\
     allow_uninitialized_decls,block_fixing" in
  require
    (String.equal
       (portable ~passes:(parse_passes production_list) ())
       (portable ()))
    "the production pass list differs from the production output";
  require
    (has_user_call "sq" (mir (parse_passes "none")))
    "the O0 model lost its user function call";
  let inlined = mir (parse_passes "function_inlining") in
  require (not (has_user_call "sq" inlined))
    "function_inlining alone left the user function call";
  require
    (count_log_prob_fors inlined = 1)
    "function_inlining alone vectorized or unrolled the loop";
  match Stanli_pipeline.parse_pass_override "function_inlining,bogus" with
  | Ok _ -> failwith "an unknown pass name was accepted"
  | Error message ->
      let mentions word =
        let n = String.length word in
        let rec at i =
          i + n <= String.length message
          && (String.equal (String.sub message i n) word || at (i + 1)) in
        at 0 in
      require
        (mentions "bogus" && mentions "lazy_code_motion"
       && mentions "optimize_soa")
        ("the unknown-pass error does not name the pass and the valid names: "
       ^ message)

(* Each f_k calls f_(k-1) twice, so inlining f_depth copies f0 2^depth times. *)
let call_chain depth =
  let fn k =
    Printf.sprintf "real f%d(real x) { return f%d(x) + f%d(x + 1); }" k (k - 1)
      (k - 1) in
  Printf.sprintf
    "functions { real f0(real x) { return x; } %s } parameters { real y; } \
     model { target += f%d(y); }"
    (String.concat " " (List.init depth (fun k -> fn (k + 1))))
    depth

(* Long function bodies that the inliner leaves as calls: overloads, the
   recursive call inside a recursive function, and calls in parameter bounds.
   Counting any of them as inlined would put this program over the budget. *)
let uninlined_calls =
  let body = String.concat " " (List.init 60 (fun _ -> "t = t + 1;")) in
  let repeat n f = String.concat " " (List.init n f) in
  Printf.sprintf
    "functions { real overloaded(real x) { real t = 0; %s return x; } \
     real overloaded(int x) { real t = 0; %s return x; } \
     real recursive(real x) { real t = 0; %s \
     if (x > 10) return x; return recursive(x + 1); } \
     real bound() { real t = 0; %s return 0; } } \
     parameters { real y; %s } model { %s target += recursive(y); }"
    body body body body
    (repeat 400 (Printf.sprintf "real<lower=bound()> p%d;"))
    (repeat 200 (fun _ -> "target += overloaded(y) + overloaded(1);"))

let inlining_estimate_test () =
  let deep = call_chain 14 in
  let production = Stanli_pipeline.default_pass_selection in
  let compiled = compile_result ~passes:production deep in
  require
    (match compiled.result with
    | Ok mir -> String.equal (encode mir) (encode (compile_upstream_o0 deep))
    | Error _ -> false)
    "a deep call chain did not fall back to transformed O0 MIR";
  require
    (match compiled.diagnostics with
    | [Stanli_pipeline.O1_budget_exceeded {checked= Before_inlining; _}] ->
        true
    | _ -> false)
    "a deep call chain was inlined before the budget check";
  let shallow = compile_result ~passes:production uninlined_calls in
  require
    (List.is_empty shallow.diagnostics)
    "the estimate counted calls that the inliner leaves alone";
  require
    (match shallow.result with
    | Ok mir ->
        String.equal (encode mir)
          (encode (compile ~passes:no_budget uninlined_calls))
    | Error _ -> false)
    "a program under the budget did not get O1"

let () =
  inlining_estimate_test ();
  pass_override_tests ();
  tuple_wire_tests ();
  tuple_print_test ();
  tuple_print_source_test ();
  split_overload_test ();
  tuple_phase_a_tests ()
