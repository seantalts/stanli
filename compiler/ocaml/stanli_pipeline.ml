type error =
  | Frontend_error of Frontend.Errors.t
  | Internal_error of string
  | Unsupported of string

type budget_check = Before_inlining | After_inlining

type diagnostic =
  | O1_budget_exceeded of
      { checked: budget_check
      ; cost: int
      ; budget: int
      ; statements: int
      ; max_control_depth: int }

let diagnostic_message = function
  | O1_budget_exceeded
      {checked= Before_inlining; cost; budget; statements; max_control_depth}
    ->
      Fmt.str
        "stanli compiler: skipped O1 because inlining functions would give \
         the transformed MIR a structural cost of at least %d, over the \
         budget of %d (at least %d statements, control depth at least %d); \
         using transformed O0 MIR. Set STANLI_NO_O1_FALLBACK=1 to force O1."
        cost budget statements max_control_depth
  | O1_budget_exceeded
      {checked= After_inlining; cost; budget; statements; max_control_depth}
    ->
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

let o1_fallback compilation =
  List.find_map
    (fun (O1_budget_exceeded _ as d) -> Some (diagnostic_message d))
    compilation.diagnostics

type pass_selection =
  { vectorize_loops: bool
  ; partial_evaluation: bool
  ; max_o1_statement_depth_cost: int option }

(* A flat procedure has cost equal to its statement count.  Nesting weights
   each statement by one plus the number of enclosing if/loop nodes.  The
   ctsem reproducer is intentionally far beyond this budget; ordinary corpus
   models remain far below it. *)
let default_o1_statement_depth_budget = 20_000

let default_pass_selection =
  { vectorize_loops= true
  ; partial_evaluation= false
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

let saturating_mul a b =
  if a <> 0 && b > Stdlib.max_int / a then Stdlib.max_int else a * b

(* The stats of a statement or inlined body moved [depth] levels deeper. *)
let placed depth stats =
  { cost= saturating_add stats.cost (saturating_mul depth stats.statements)
  ; statements= stats.statements
  ; max_control_depth= saturating_add depth stats.max_control_depth }

let no_inlining (_ : string) = None

(* The structural cost of [stmt] at [depth]. [inlined name] gives the stats
   at depth 0 of the body that function_inlining puts in place of a call to
   [name]. The inliner keeps every statement of that body, puts it at least
   as deep as the call, and adds return wrappers and temporaries that are
   not counted here, so with inlining the result is a lower bound. *)
let rec statement_stats ?(inlined = no_inlining) depth
    (stmt : Middle.Stmt.Located.t) =
  let open Middle in
  let child_depth =
    match stmt.pattern with
    | IfElse _ | While _ | For _ -> saturating_add depth 1
    | _ -> depth in
  let own =
    { cost= saturating_add depth 1
    ; statements= 1
    ; max_control_depth= child_depth } in
  (* Like the inliner, look only at a call's arguments. Calls in the
     expressions of a compiler-internal function, such as a parameter's
     bounds, stay calls. *)
  let rec calls stats (e : Expr.Typed.t) =
    match e.pattern with
    | FunApp (kind, args) ->
        let stats =
          match kind with
          | UserDefined (name, _) -> (
            match inlined name with
            | Some body -> add_stats stats (placed depth body)
            | None -> stats )
          | _ -> stats in
        List.fold_left calls stats args
    | pattern -> Expr.Pattern.fold calls stats pattern in
  (* The expressions that Optimize.inline_function_statement inlines calls
     in. It leaves the sizes in a declaration's type alone. *)
  let own, exprs =
    match stmt.pattern with
    | NRFunApp (UserDefined (name, _), args) -> (
      match inlined name with
      | Some body -> (placed depth body, args)
      | None -> (own, args) )
    | NRFunApp (_, args) -> (own, args)
    | Assignment (lhs, _, rhs) ->
        (own, [Stmt.Helpers.expr_of_lvalue lhs ~meta:rhs.meta; rhs])
    | TargetPE e | JacobianPE e | Return (Some e) | IfElse (e, _, _)
     |While (e, _)
     |Decl {initialize= Assign e; _} ->
        (own, [e])
    | For {lower; upper; _} -> (own, [lower; upper])
    | Decl _ | Return None | Profile _ | Block _ | SList _ | Skip | Break
     |Continue ->
        (own, []) in
  let stats = List.fold_left calls own exprs in
  Stmt.Pattern.fold
    (fun stats (_ : Expr.Typed.t) -> stats)
    (fun stats child ->
      add_stats stats (statement_stats ~inlined child_depth child))
    stats stmt.pattern

let statement_list_stats ?inlined stmts =
  List.fold_left
    (fun stats stmt -> add_stats stats (statement_stats ?inlined 0 stmt))
    empty_stats stmts

let max_stats a b = if a.cost >= b.cost then a else b

(* function_inlining inlines every block but prepare_data. *)
let max_procedure_stats ?inlined (mir : Middle.Program.Typed.t) =
  let inlined_blocks =
    [ mir.log_prob; mir.reverse_mode_log_prob; mir.generate_quantities
    ; mir.transform_inits; mir.unconstrain_array ] in
  let block_max =
    List.fold_left
      (fun current block ->
        max_stats current (statement_list_stats ?inlined block))
      (statement_list_stats mir.prepare_data)
      inlined_blocks in
  List.fold_left
    (fun current (fn : Middle.Stmt.Located.t Middle.Program.fun_def) ->
      match fn.fdbody with
      | None -> current
      | Some body -> max_stats current (statement_stats 0 body))
    block_max mir.functions_block

module Function_stats = Map.Make (String)

(* A lower bound on [max_procedure_stats] of the program after
   function_inlining, computed without inlining. The inliner inlines a
   function only when it has one definition and no separate declaration,
   and inside a body only the functions defined before it, so recursive
   calls stay calls. Each body is measured once, with those earlier
   functions inlined. *)
let estimated_inlined_stats (mir : Middle.Program.Typed.t) =
  let definitions =
    List.fold_left
      (fun counts (fn : _ Middle.Program.fun_def) ->
        Function_stats.update fn.fdname
          (fun n -> Some (1 + Option.value n ~default:0))
          counts)
      Function_stats.empty mir.functions_block in
  let inlinable =
    List.fold_left
      (fun earlier (fn : Middle.Stmt.Located.t Middle.Program.fun_def) ->
        match fn.fdbody with
        | Some body when Function_stats.find fn.fdname definitions = 1 ->
            let inlined name = Function_stats.find_opt name earlier in
            Function_stats.add fn.fdname
              (statement_stats ~inlined 0 body)
              earlier
        | _ -> earlier)
      Function_stats.empty mir.functions_block in
  max_procedure_stats
    ~inlined:(fun name -> Function_stats.find_opt name inlinable)
    mir

let selected_default_passes ?(fast_math = false) () =
  let selection = {default_pass_selection with partial_evaluation= fast_math} in
  match Sys.getenv_opt "STANLI_NO_O1_FALLBACK" with
  | None -> selection
  | Some _ -> {selection with max_o1_statement_depth_cost= None}

module Function_names = Set.Make (String)

let referenced_kind names = function
  | Middle.Fun_kind.UserDefined (name, _)
   |Middle.Fun_kind.StanLib (name, _) -> Function_names.add name names
  | Operator _ | CompilerInternal _ -> names

let rec referenced_expr names (expr : Middle.Expr.Typed.t) =
  let names =
    match expr.pattern with
    | Var name ->
        (* A density passed as `foo_lupdf` names the definition foo_lpdf. *)
        Function_names.add name names
        |> Function_names.add (Middle.Utils.normalized_name name)
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
          Driver.Entry.stan2mir
            ~allowed_annotations:Stan_math_backend.Annotations.recognized_annotation
            model_name (`Code code) flags output)
    with
    | Error internal -> Error (Internal_error internal)
    | Ok (Error error) -> Error (Frontend_error error)
    | Ok (Ok mir) -> Ok mir in
  {result; warnings= !warnings; diagnostics= []}

(* Split tuples, reporting a construct the split cannot handle as a compile
   error rather than an internal one. *)
let split mir =
  match Stanli_tuples.scalarize mir with
  | split -> Ok split
  | exception Portable_mir.Not_supported message -> Error (Unsupported message)

let production_settings passes =
  { (Analysis_and_optimization.Optimize.level_optimizations O1) with
    vectorize_loops= passes.vectorize_loops
  ; preserve_stability= true
  ; partial_evaluation= passes.partial_evaluation }

(* [settings = None] returns the transformed O0 MIR. *)
let compile_with_settings ?include_source ?(prune_unused_sections = true)
    ?(model_only = false) ~settings ~budget ~model_name code =
  let compiled =
    compile_mir_at_level ?include_source
      ~optimization_level:Analysis_and_optimization.Optimize.O0 ~model_name code
  in
  let result, diagnostics =
    match (compiled.result, settings) with
    | Error error, _ -> (Error error, [])
    | Ok mir, None -> (split mir, [])
    | Ok mir, Some settings -> (
      let optimize candidate settings =
        Common.ICE.with_exn_message (fun () ->
            Analysis_and_optimization.Optimize.optimization_suite ~settings
              candidate) in
      let fallback checked stats budget =
        ( split mir
        , [ O1_budget_exceeded
              { checked
              ; cost= stats.cost
              ; budget
              ; statements= stats.statements
              ; max_control_depth= stats.max_control_depth } ] ) in
      (* Inline before dropping unused procedures: upstream uses a shared
         fresh-name supply, so skipping their inlining would rename later
         generated-quantities locals. Preserve the original structural-budget
         decision as well. The remaining dataflow passes need only the
         procedures the portable encoder actually consumes. *)
      let inline mir =
        if settings.Analysis_and_optimization.Optimize.function_inlining then
          Common.ICE.with_exn_message (fun () ->
              Analysis_and_optimization.Optimize.function_inlining mir)
        else Ok mir in
      (* Inlining a deep call chain can take minutes, so check the estimate
         first. *)
      let estimate = Option.map (fun _ -> estimated_inlined_stats mir) budget in
      match (budget, estimate) with
      | Some budget, Some estimate when estimate.cost > budget ->
          fallback Before_inlining estimate budget
      | _ -> (
        match inline mir with
        | Error internal -> (Error (Internal_error internal), [])
        | Ok inlined -> (
            let stats = max_procedure_stats inlined in
            match budget with
            | Some budget when stats.cost > budget ->
                fallback After_inlining stats budget
            | _ -> (
              (* Tuples are split before pruning, so pruned and unpruned
                 compiles name their temporaries alike. *)
              match split inlined with
              | Error error -> (Error error, [])
              | Ok inlined -> (
                  let candidate =
                    if prune_unused_sections then
                      if model_only then prune_portable_procedures inlined
                      else
                        { inlined with
                          reverse_mode_log_prob= []
                        ; unconstrain_array= [] }
                    else inlined in
                  let remaining_settings =
                    {settings with function_inlining= false} in
                  match optimize candidate remaining_settings with
                  | Error internal -> (Error (Internal_error internal), [])
                  | Ok optimized -> (Ok optimized, []) ) ) ) ) ) in
  {result; warnings= compiled.warnings; diagnostics}

let compile_mir_with_passes_uncached ?include_source ?prune_unused_sections
    ?model_only ~passes ~model_name code =
  compile_with_settings ?include_source ?prune_unused_sections ?model_only
    ~settings:(Some (production_settings passes))
    ~budget:passes.max_o1_statement_depth_cost ~model_name code

let compile_mir_with_passes ?include_source ?(cache_signatures = true)
    ?prune_unused_sections ?model_only
    ~passes ~model_name code =
  let compile () =
    compile_mir_with_passes_uncached ?include_source ?prune_unused_sections ?model_only
      ~passes ~model_name code in
  if cache_signatures then
    Frontend.SignatureMismatch.with_stanlib_cache compile
  else compile ()

type pass_override =
  | No_passes
  | O1_without_fallback
  | Only of Analysis_and_optimization.Optimize.optimization_settings

let pass_setters :
    (string
    * (   Analysis_and_optimization.Optimize.optimization_settings
       -> Analysis_and_optimization.Optimize.optimization_settings))
    list =
  [ ("function_inlining", fun s -> {s with function_inlining= true})
  ; ("constant_propagation", fun s -> {s with constant_propagation= true})
  ; ("copy_propagation", fun s -> {s with copy_propagation= true})
  ; ("dead_code_elimination", fun s -> {s with dead_code_elimination= true})
  ; ("vectorize_loops", fun s -> {s with vectorize_loops= true})
  ; ("one_step_loop_unrolling", fun s -> {s with one_step_loop_unrolling= true})
  ; ("expression_propagation", fun s -> {s with expression_propagation= true})
  ; ("partial_evaluation", fun s -> {s with partial_evaluation= true})
  ; ("lazy_code_motion", fun s -> {s with lazy_code_motion= true})
  ; ("static_loop_unrolling", fun s -> {s with static_loop_unrolling= true})
  ; ("list_collapsing", fun s -> {s with list_collapsing= true})
  ; ("optimize_ad_levels", fun s -> {s with optimize_ad_levels= true})
  ; ("optimize_soa", fun s -> {s with optimize_soa= true})
  ; ( "allow_uninitialized_decls"
    , fun s -> {s with allow_uninitialized_decls= true} )
  ; ("block_fixing", fun s -> {s with block_fixing= true})
  ; ("preserve_stability", fun s -> {s with preserve_stability= true}) ]

let pass_names = List.map fst pass_setters

let parse_pass_override = function
  | "none" -> Ok No_passes
  | "o1" -> Ok O1_without_fallback
  | spec ->
      let start =
        { Analysis_and_optimization.Optimize.no_optimizations with
          preserve_stability= true } in
      List.fold_left
        (fun settings name ->
          Result.bind settings (fun settings ->
              match List.assoc_opt name pass_setters with
              | Some set -> Ok (set settings)
              | None ->
                  Error
                    (Fmt.str
                       "unknown pass '%s'; expected none, o1, or a \
                        comma-separated list of: %s"
                       name
                       (String.concat ", " pass_names))))
        (Ok start)
        (String.split_on_char ',' spec)
      |> Result.map (fun settings -> Only settings)

let compile_mir ?include_source ?model_only ?fast_math ?passes ~model_name code
    =
  let production = selected_default_passes ?fast_math () in
  let without_budget settings =
    Frontend.SignatureMismatch.with_stanlib_cache (fun () ->
        compile_with_settings ?include_source ?model_only ~settings ~budget:None
          ~model_name code) in
  match passes with
  | None ->
      compile_mir_with_passes ?include_source ?model_only ~passes:production
        ~model_name code
  | Some O1_without_fallback ->
      compile_mir_with_passes ?include_source ?model_only
        ~passes:{production with max_o1_statement_depth_cost= None}
        ~model_name code
  | Some No_passes -> without_budget None
  | Some (Only settings) ->
      without_budget (Some {settings with preserve_stability= true})

let compile_portable ?include_source ?model_only ?fast_math ?passes ~model_name
    code =
  let compiled =
    compile_mir ?include_source ?model_only ?fast_math ?passes ~model_name code
  in
  let result =
    match compiled.result with
    | Error error -> Error error
    | Ok mir -> (
      match
        Common.ICE.with_exn_message (fun () ->
            try Ok (Portable_mir.encode mir)
            with Portable_mir.Not_supported message -> Error message)
      with
      | Error internal -> Error (Internal_error internal)
      | Ok (Error message) -> Error (Unsupported message)
      | Ok (Ok encoded) -> Ok encoded ) in
  { result
  ; warnings= compiled.warnings
  ; diagnostics= compiled.diagnostics }
