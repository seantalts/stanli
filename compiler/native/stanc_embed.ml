(* stanli embedding entry point: Stan code -> compact portable MIR (upstream O1
   plus loop vectorization). Registered for C via Callback; built with
   -output-complete-obj so the OCaml runtime rides inside one object file linked
   into libstanli. runtime/src/stanc_embed_c.cpp reads [embed_result] field by
   field, so the field order and types here are part of that contract. *)

let flag_model_only = 1
let flag_fast_math = 2

type embed_result =
  { outcome: (string, string) result  (** the portable MIR, or the error *)
  ; warnings: string array }

let compile_tmir ~model_only ~fast_math ?passes ~filename ~include_paths
    (code : string) : embed_result =
  let printed_filename = if filename = "" then None else Some filename in
  let compilation =
    Stanli_pipeline.compile_portable ~model_only ~fast_math ?passes
      ~model_name:"embedded_model" code
      ~include_source:
        (Frontend.Include_files.FileSystemPaths (Array.to_list include_paths)) in
  (* stanc3 closes an #include file only from a GC finaliser on its lexbuf. A
     one-shot compiler process exits first, but this runtime lives as long as
     its host, and Windows refuses to delete or replace a file that is still
     open. Collect now so every included file is closed before returning. *)
  if Frontend.Preprocessor.included_files () <> [] then Gc.full_major ();
  let warnings =
    Array.of_list
      (List.map
         (fun warning ->
           String.trim
             (Fmt.str "%a" (Frontend.Warnings.pp ?printed_filename ~code) warning))
         compilation.warnings) in
  let outcome =
    match compilation.result with
    | Error (Stanli_pipeline.Internal_error message)
     |Error (Stanli_pipeline.Unsupported message) ->
        Error message
    | Error (Stanli_pipeline.Frontend_error error) ->
        Error (Fmt.str "%a" (Frontend.Errors.pp ?printed_filename ~code) error)
    | Ok encoded -> Ok encoded in
  {outcome; warnings}

let () =
  ignore (Thread.self ());
  Stdlib.Callback.register "stanc_compile"
    (fun code include_paths filename flags passes ->
      let model_only = flags land flag_model_only <> 0 in
      let fast_math = flags land flag_fast_math <> 0 in
      (* [passes] is the CLI's --passes spec; "" selects the shipping passes. *)
      if passes = "" then
        compile_tmir ~model_only ~fast_math ~filename ~include_paths code
      else
        match Stanli_pipeline.parse_pass_override passes with
        | Ok passes ->
            compile_tmir ~model_only ~fast_math ~passes ~filename ~include_paths
              code
        | Error message -> {outcome= Error message; warnings= [||]})
