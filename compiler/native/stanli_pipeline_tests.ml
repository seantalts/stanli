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

let compile_upstream_o1 code =
  let flags =
    { Driver.Flags.default with
      optimization_level= Analysis_and_optimization.Optimize.O1 } in
  match
    Driver.Entry.stan2mir "pass_selection_test" (`Code code) flags (fun _ -> ())
  with
  | Ok mir -> mir
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
              ( Fun_kind.StanLib (_, (Fun_kind.FnLpdf _ | FnLpmf _), _)
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
      let upstream_o1_bytes = encode (compile_upstream_o1 code) in
      require
        (String.equal pass_off_bytes upstream_o1_bytes)
        ("pass-off output differs from upstream O1 for " ^ name);
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
