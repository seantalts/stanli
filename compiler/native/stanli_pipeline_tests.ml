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
    | Error (Stanli_pipeline.Internal_error m) -> failwith ("budgeted tuple model: " ^ m)
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
