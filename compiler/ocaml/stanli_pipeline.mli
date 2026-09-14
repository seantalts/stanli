type error =
  | Frontend_error of Frontend.Errors.t
  | Internal_error of string

type diagnostic =
  | O1_budget_exceeded of
      { cost: int
      ; budget: int
      ; statements: int
      ; max_control_depth: int }

val diagnostic_message : diagnostic -> string

type 'a compilation =
  { result: ('a, error) result
  ; warnings: Frontend.Warnings.t list
  ; diagnostics: diagnostic list }

(** Source-level MIR passes that stanli may add to upstream's O1 policy. *)
type pass_selection =
  { vectorize_loops: bool
  ; max_o1_statement_depth_cost: int option }

val default_pass_selection : pass_selection
(** The shipping selection: upstream O1 plus loop vectorization, guarded by a
    deterministic pre-dataflow structural budget. *)

val compile_mir :
     ?include_source:Frontend.Include_files.t
  -> ?model_only:bool
  -> model_name:string
  -> string
  -> Middle.Program.Typed.t compilation
(** Parse, typecheck, apply the Stan Math MIR transform, and run the exact
    optimization policy selected by stanli. *)

val compile_mir_with_passes :
     ?include_source:Frontend.Include_files.t
  -> ?cache_signatures:bool
  -> ?prune_unused_sections:bool
  -> ?model_only:bool
  -> passes:pass_selection
  -> model_name:string
  -> string
  -> Middle.Program.Typed.t compilation
(** Internal pass-selection entrypoint. It obtains backend-transformed MIR at
    O0, then applies upstream O1 with only the selected additive passes. *)

val compile_portable :
     ?include_source:Frontend.Include_files.t
  -> ?model_only:bool
  -> model_name:string
  -> string
  -> string compilation
(** Compile Stan source and encode canonical compact portable MIR v2.
    [model_only] permits removal of functions unreachable from the model's
    entry points. The default preserves every exported function for the
    public function API. *)
