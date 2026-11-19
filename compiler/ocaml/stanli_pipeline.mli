type error =
  | Frontend_error of Frontend.Errors.t
  | Internal_error of string
  | Unsupported of string
      (** a construct stanli's compiler does not support *)

(** [Before_inlining] means the estimate of the inlined cost, which never
    exceeds the real cost, was already over the budget. *)
type budget_check = Before_inlining | After_inlining

type diagnostic =
  | O1_budget_exceeded of
      { checked: budget_check
      ; cost: int
      ; budget: int
      ; statements: int
      ; max_control_depth: int }

val diagnostic_message : diagnostic -> string

type 'a compilation =
  { result: ('a, error) result
  ; warnings: Frontend.Warnings.t list
  ; diagnostics: diagnostic list }

val o1_fallback : 'a compilation -> string option
(** The notice for an O1 pass skipped for the structural budget, if any. *)

(** Source-level MIR passes added to upstream's stability-preserving O1 policy. *)
type pass_selection =
  { vectorize_loops: bool
  ; partial_evaluation: bool
  ; max_o1_statement_depth_cost: int option }

val default_pass_selection : pass_selection
(** The shipping selection: upstream O1 with stability preservation, without
    partial evaluation, plus loop vectorization and one-time binding of
    repeated scalar call arguments. A deterministic pre-dataflow
    structural budget guards the dataflow passes. *)

val selected_default_passes : ?fast_math:bool -> unit -> pass_selection
(** The shipping selection with the explicit [STANLI_NO_O1_FALLBACK] override.
    [fast_math] (default false) turns partial evaluation on. *)

(** An explicit pass set that replaces the shipping policy, for testing the
    optimizer against unoptimized output. *)
type pass_override =
  | No_passes
      (** The transformed O0 MIR that the structural budget fallback returns,
          without its diagnostic. *)
  | O1_without_fallback
      (** The shipping selection with the structural budget turned off. *)
  | Only of Analysis_and_optimization.Optimize.optimization_settings
      (** These upstream settings with stability preserved and no budget. *)

val pass_names : string list
(** The [optimization_settings] field names that {!parse_pass_override}
    accepts, in the order [optimization_suite] first applies each one. *)

val parse_pass_override : string -> (pass_override, string) result
(** [none], [o1], or a comma-separated list of {!pass_names}. *)

val compile_mir :
     ?include_source:Frontend.Include_files.t
  -> ?model_only:bool
  -> ?fast_math:bool
  -> ?passes:pass_override
  -> model_name:string
  -> string
  -> Middle.Program.Typed.t compilation
(** Parse, typecheck, apply the Stan Math MIR transform, and run the exact
    optimization policy selected by stanli, or [passes] when given. *)

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
    O0, then applies the stability-preserving O1 selection described above. *)

val compile_portable :
     ?include_source:Frontend.Include_files.t
  -> ?model_only:bool
  -> ?fast_math:bool
  -> ?passes:pass_override
  -> model_name:string
  -> string
  -> string compilation
(** Compile Stan source and encode canonical compact portable MIR v3.
    [model_only] permits removal of functions unreachable from the model's
    entry points. The default preserves every exported function for the
    public function API. *)
