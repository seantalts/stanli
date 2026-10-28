(* stanli embedding entry point: Stan code -> compact portable MIR (upstream O1
   plus loop vectorization). Registered for C via Callback; built with
   -output-complete-obj so the OCaml runtime rides inside one object file linked
   into libstanli. Return protocol: "W<n>:<n bytes of warnings>" followed by
   "OK<portable>" or "ERR<message>". *)

let flag_model_only = 1
let flag_fast_math = 2

let compile_tmir ~model_only ~fast_math ~filename ~include_paths (code : string)
    : string =
  let printed_filename = if filename = "" then None else Some filename in
  let compilation =
    Stanli_pipeline.compile_portable ~model_only ~fast_math
      ~model_name:"embedded_model" code
      ~include_source:
        (Frontend.Include_files.FileSystemPaths (Array.to_list include_paths)) in
  let warnings =
    String.concat "\n"
      (List.map
         (fun warning ->
           String.trim
             (Fmt.str "%a" (Frontend.Warnings.pp ?printed_filename) warning))
         compilation.warnings
      @ List.map Stanli_pipeline.diagnostic_message compilation.diagnostics) in
  let outcome =
    match compilation.result with
    | Error (Stanli_pipeline.Internal_error message) -> "ERR" ^ message
    | Error (Stanli_pipeline.Frontend_error error) ->
        "ERR"
        ^ Fmt.str "%a" (Frontend.Errors.pp ?printed_filename ~code) error
    | Ok encoded -> "OK" ^ encoded in
  Printf.sprintf "W%d:%s%s" (String.length warnings) warnings outcome

let () =
  ignore (Thread.self ());
  Stdlib.Callback.register "stanc_compile"
    (fun code include_paths filename flags ->
      compile_tmir
        ~model_only:(flags land flag_model_only <> 0)
        ~fast_math:(flags land flag_fast_math <> 0)
        ~filename ~include_paths code)
