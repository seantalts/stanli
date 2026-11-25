type error =
  | Frontend_error of Frontend.Errors.t
  | Internal_error of string
  | Unsupported of string

type 'a compilation =
  { result: ('a, error) result
  ; warnings: Frontend.Warnings.t list }

type pass_selection = {vectorize_loops: bool; partial_evaluation: bool}

let default_pass_selection = {vectorize_loops= true; partial_evaluation= false}

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
  let bodies = Hashtbl.create 16 in
  List.iter
    (fun (fn : Middle.Stmt.Located.t Middle.Program.fun_def) ->
      Option.iter (Hashtbl.add bodies fn.fdname) fn.fdbody)
    mir.functions_block;
  (* Each reached name's bodies are read once. *)
  let rec closure reached = function
    | [] -> reached
    | name :: rest when Function_names.mem name reached -> closure reached rest
    | name :: rest ->
        let found =
          List.fold_left referenced_stmt Function_names.empty
            (Hashtbl.find_all bodies name) in
        closure
          (Function_names.add name reached)
          (List.rev_append (Function_names.elements found) rest) in
  let reachable = closure Function_names.empty (Function_names.elements roots) in
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
  {result; warnings= !warnings}

(* Split tuples, reporting a construct the split cannot handle as a compile
   error rather than an internal one. *)
let split mir =
  match Stanli_tuples.scalarize mir with
  | split -> Ok split
  | exception Portable_mir.Not_supported message -> Error (Unsupported message)

(* Upstream O1 with stability preserved and loop vectorization, but without
   function inlining: the runtime inlines every call itself when it lowers
   the model, with the data in hand.

   Three O1 passes are off. Turning them off changed no byte of portable MIR
   for any of the 2,873 corpus and test models, in the default, fast-math
   and model-only modes. optimize_soa rewrites only reverse_mode_log_prob,
   which is dropped before the suite runs. allow_uninitialized_decls turns
   Default into Uninit, and the encoder writes both the same way. For
   block_fixing the evidence is only that sweep. *)
let production_settings passes =
  { (Analysis_and_optimization.Optimize.level_optimizations O1) with
    function_inlining= false
  ; vectorize_loops= passes.vectorize_loops
  ; preserve_stability= true
  ; partial_evaluation= passes.partial_evaluation
  ; optimize_soa= false
  ; allow_uninitialized_decls= false
  ; block_fixing= false }

(* [settings = None] returns the transformed O0 MIR. *)
let compile_with_settings ?include_source ?(prune_unused_sections = true)
    ?(model_only = false) ~settings ~model_name code =
  let compiled =
    compile_mir_at_level ?include_source
      ~optimization_level:Analysis_and_optimization.Optimize.O0 ~model_name code
  in
  let result =
    match (compiled.result, settings) with
    | Error error, _ -> Error error
    | Ok mir, None -> split mir
    | Ok mir, Some settings -> (
      (* Tuples are split before pruning, so pruned and unpruned compiles
         name their temporaries alike. *)
      match split mir with
      | Error error -> Error error
      | Ok split -> (
          let candidate =
            if prune_unused_sections then
              if model_only then prune_portable_procedures split
              else {split with reverse_mode_log_prob= []; unconstrain_array= []}
            else split in
          match
            Common.ICE.with_exn_message (fun () ->
                Analysis_and_optimization.Optimize.optimization_suite ~settings
                  candidate)
          with
          | Error internal -> Error (Internal_error internal)
          | Ok optimized -> Ok optimized ) ) in
  {result; warnings= compiled.warnings}

let compile_mir_with_passes_uncached ?include_source ?prune_unused_sections
    ?model_only ~passes ~model_name code =
  compile_with_settings ?include_source ?prune_unused_sections ?model_only
    ~settings:(Some (production_settings passes)) ~model_name code

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
  | Shipping
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
  | "o1" -> Ok Shipping
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

let compile_mir ?include_source ?model_only ?(fast_math = false) ?passes
    ~model_name code =
  let shipping () =
    compile_mir_with_passes ?include_source ?model_only
      ~passes:{default_pass_selection with partial_evaluation= fast_math}
      ~model_name code in
  let only settings =
    Frontend.SignatureMismatch.with_stanlib_cache (fun () ->
        compile_with_settings ?include_source ?model_only ~settings ~model_name
          code) in
  match passes with
  | None | Some Shipping -> shipping ()
  | Some No_passes -> only None
  | Some (Only settings) -> only (Some {settings with preserve_stability= true})

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
  {result; warnings= compiled.warnings}
