type error =
  | Frontend_error of Frontend.Errors.t
  | Internal_error of string

type diagnostic =
  | O1_budget_exceeded of
      { cost: int
      ; budget: int
      ; statements: int
      ; max_control_depth: int }

let diagnostic_message = function
  | O1_budget_exceeded {cost; budget; statements; max_control_depth} ->
      Fmt.str
        "stanli compiler: skipped O1 because the inlined transformed MIR \
         structural cost %d exceeds budget %d (%d statements, maximum control \
         depth %d); using transformed O0 MIR. Set STANLI_NO_O1_FALLBACK=1 to \
         force O1."
        cost budget statements max_control_depth

type 'a compilation =
  { result: ('a, error) result
  ; warnings: Frontend.Warnings.t list
  ; diagnostics: diagnostic list }

type pass_selection =
  { vectorize_loops: bool
  ; max_o1_statement_depth_cost: int option }

(* A flat procedure has cost equal to its statement count.  Nesting weights
   each statement by one plus the number of enclosing if/loop nodes.  The
   ctsem reproducer is intentionally far beyond this budget; ordinary corpus
   models remain far below it. *)
let default_o1_statement_depth_budget = 20_000

let default_pass_selection =
  { vectorize_loops= true
  ; max_o1_statement_depth_cost= Some default_o1_statement_depth_budget }

type structural_stats =
  { cost: int
  ; statements: int
  ; max_control_depth: int }

let empty_stats = {cost= 0; statements= 0; max_control_depth= 0}

let saturating_add a b =
  if a >= Stdlib.max_int - b then Stdlib.max_int else a + b

let add_stats a b =
  { cost= saturating_add a.cost b.cost
  ; statements= saturating_add a.statements b.statements
  ; max_control_depth= Int.max a.max_control_depth b.max_control_depth }

let rec statement_stats depth (stmt : Middle.Stmt.Located.t) =
  let controls_children =
    match stmt.pattern with
    | Middle.Stmt.Pattern.IfElse _ | While _ | For _ -> true
    | _ -> false in
  let child_depth =
    if controls_children then saturating_add depth 1 else depth in
  let own =
    { cost= saturating_add depth 1
    ; statements= 1
    ; max_control_depth= child_depth } in
  Middle.Stmt.Pattern.fold
    (fun stats (_ : Middle.Expr.Typed.t) -> stats)
    (fun stats child -> add_stats stats (statement_stats child_depth child))
    own stmt.pattern

let statement_list_stats stmts =
  List.fold_left
    (fun stats stmt -> add_stats stats (statement_stats 0 stmt))
    empty_stats stmts

let max_stats a b = if a.cost >= b.cost then a else b

let max_procedure_stats (mir : Middle.Program.Typed.t) =
  let blocks =
    [ mir.prepare_data; mir.log_prob; mir.reverse_mode_log_prob
    ; mir.generate_quantities; mir.transform_inits; mir.unconstrain_array ] in
  let block_max =
    List.fold_left
      (fun current block -> max_stats current (statement_list_stats block))
      empty_stats blocks in
  List.fold_left
    (fun current (fn : Middle.Stmt.Located.t Middle.Program.fun_def) ->
      match fn.fdbody with
      | None -> current
      | Some body -> max_stats current (statement_stats 0 body))
    block_max mir.functions_block

let selected_default_passes () =
  match Sys.getenv_opt "STANLI_NO_O1_FALLBACK" with
  | None -> default_pass_selection
  | Some _ -> {default_pass_selection with max_o1_statement_depth_cost= None}

module Function_names = Set.Make (String)

let referenced_kind names = function
  | Middle.Fun_kind.UserDefined (name, _)
   |Middle.Fun_kind.StanLib (name, _, _) -> Function_names.add name names
  | CompilerInternal _ -> names

let rec referenced_expr names (expr : Middle.Expr.Typed.t) =
  let names =
    match expr.pattern with
    | Var name -> Function_names.add name names
    | FunApp (kind, _) -> referenced_kind names kind
    | _ -> names in
  Middle.Expr.Pattern.fold referenced_expr names expr.pattern

let rec referenced_stmt names (stmt : Middle.Stmt.Located.t) =
  let names =
    match stmt.pattern with
    | NRFunApp (kind, _) -> referenced_kind names kind
    | _ -> names in
  Middle.Stmt.Pattern.fold referenced_expr referenced_stmt names stmt.pattern

let prune_portable_procedures (mir : Middle.Program.Typed.t) =
  (* Function-valued variables passed to higher-order functions are roots
     too. Keep all overloads of every reached name and follow their bodies
     transitively, including recursion and void/effectful calls. A local
     variable sharing a function name can conservatively retain extra code. *)
  let roots =
    List.fold_left (List.fold_left referenced_stmt) Function_names.empty
      [mir.prepare_data; mir.log_prob; mir.generate_quantities; mir.transform_inits] in
  let rec closure names =
    let next =
      List.fold_left
        (fun acc (fn : Middle.Stmt.Located.t Middle.Program.fun_def) ->
          if Function_names.mem fn.fdname names then
            match fn.fdbody with
            | Some body -> referenced_stmt acc body
            | None -> acc
          else acc)
        names mir.functions_block in
    if Function_names.equal names next then names else closure next in
  let reachable = closure roots in
  { mir with
    reverse_mode_log_prob= []
  ; unconstrain_array= []
  ; functions_block=
      List.filter
        (fun (fn : Middle.Stmt.Located.t Middle.Program.fun_def) ->
          Function_names.mem fn.fdname reachable)
        mir.functions_block }

let compile_mir_at_level
    ?(include_source = Driver.Flags.default.include_source) ~optimization_level
    ~model_name (code : string) =
  let flags =
    {Driver.Flags.default with optimization_level; include_source} in
  let warnings = ref [] in
  let output : Driver.Entry.other_output -> unit = function
    | Warnings emitted -> warnings := !warnings @ emitted
    | Formatted _ | DebugOutput _ | Memory_patterns _ | Info _ | Version _
     |Generated _ ->
        () in
  let result =
    match
      Common.ICE.with_exn_message (fun () ->
          Driver.Entry.stan2mir model_name (`Code code) flags output)
    with
    | Error internal -> Error (Internal_error internal)
    | Ok (Error error) -> Error (Frontend_error error)
    | Ok (Ok mir) -> Ok mir in
  {result; warnings= !warnings; diagnostics= []}

let compile_mir_with_passes_uncached ?include_source ?(prune_unused_sections = true)
    ?(model_only = false) ~passes ~model_name code =
  let compiled =
    compile_mir_at_level ?include_source
      ~optimization_level:Analysis_and_optimization.Optimize.O0 ~model_name code
  in
  let result, diagnostics =
    match compiled.result with
    | Error error -> (Error error, [])
    | Ok mir -> (
      let settings =
        { (Analysis_and_optimization.Optimize.level_optimizations O1) with
          vectorize_loops= passes.vectorize_loops
        ; preserve_stability= true
        ; partial_evaluation= false } in
      (* Keep upstream's dataflow optimizations without rewriting the source
         arithmetic into fused operations. The standalone partial evaluator
         does not accept preserve_stability, so it must stay disabled too.
         A one-ULP intermediate change can become hundreds in a gradient. *)
      let optimize candidate settings =
        Common.ICE.with_exn_message (fun () ->
            Analysis_and_optimization.Optimize.optimization_suite ~settings
              candidate) in
      (* Inline before dropping unused procedures: upstream uses a shared
         fresh-name supply, so skipping their inlining would rename later
         generated-quantities locals. Preserve the original structural-budget
         decision as well. The remaining dataflow passes need only the
         procedures the portable encoder actually consumes. *)
      match
        Common.ICE.with_exn_message (fun () ->
            Analysis_and_optimization.Optimize.function_inlining
              ~bind_repeated_scalar_args:true mir)
      with
      | Error internal -> (Error (Internal_error internal), [])
      | Ok inlined ->
          let stats = max_procedure_stats inlined in
          match passes.max_o1_statement_depth_cost with
          | Some budget when stats.cost > budget ->
              ( Ok mir
              , [O1_budget_exceeded
                   { cost= stats.cost
                   ; budget
                   ; statements= stats.statements
                   ; max_control_depth= stats.max_control_depth }] )
          | _ ->
              let candidate =
                if prune_unused_sections then
                  if model_only then prune_portable_procedures inlined
                  else {inlined with reverse_mode_log_prob= []; unconstrain_array= []}
                else inlined in
              let remaining_settings = {settings with function_inlining= false} in
              (match optimize candidate remaining_settings with
              | Error internal -> (Error (Internal_error internal), [])
              | Ok optimized -> (Ok optimized, [])) ) in
  {result; warnings= compiled.warnings; diagnostics}

let compile_mir_with_passes ?include_source ?(cache_signatures = true)
    ?prune_unused_sections ?model_only
    ~passes ~model_name code =
  let compile () =
    compile_mir_with_passes_uncached ?include_source ?prune_unused_sections ?model_only
      ~passes ~model_name code in
  if cache_signatures then
    Frontend.SignatureMismatch.with_stanlib_cache compile
  else compile ()

let compile_mir ?include_source ?model_only ~model_name code =
  compile_mir_with_passes ?include_source ?model_only ~passes:(selected_default_passes ())
    ~model_name code

let compile_portable ?include_source ?model_only ~model_name code =
  let compiled = compile_mir ?include_source ?model_only ~model_name code in
  let result =
    match compiled.result with
    | Error error -> Error error
    | Ok mir -> (
      match
        Common.ICE.with_exn_message (fun () -> Portable_mir.encode mir)
      with
      | Error internal -> Error (Internal_error internal)
      | Ok encoded -> Ok encoded ) in
  { result
  ; warnings= compiled.warnings
  ; diagnostics= compiled.diagnostics }
