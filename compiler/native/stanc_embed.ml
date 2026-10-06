(* stanli embedding entry point: Stan code -> compact portable MIR (upstream O1
   plus loop vectorization). Registered for C via Callback; built with
   -output-complete-obj so the OCaml runtime rides inside one object file linked
   into libstanli. Return protocol: "OK<portable>" or "ERR<message>". *)

let compile_tmir ~model_only ~fast_math ?(include_paths = [||]) (code : string)
    : string =
  let compilation =
    Stanli_pipeline.compile_portable ~model_only ~fast_math
      ~model_name:"embedded_model" code
      ~include_source:
        (Frontend.Include_files.FileSystemPaths (Array.to_list include_paths)) in
  List.iter
    (fun diagnostic ->
      prerr_endline (Stanli_pipeline.diagnostic_message diagnostic))
    compilation.diagnostics;
  match compilation.result with
  | Error (Stanli_pipeline.Internal_error message) -> "ERR" ^ message
  | Error (Stanli_pipeline.Frontend_error error) ->
      "ERR"
      ^ Fmt.str "%a"
          (Frontend.Errors.pp ?printed_filename:None ~code)
          error
  | Ok encoded -> "OK" ^ encoded

let () =
  ignore (Thread.self ());
  let register name ~model_only ~fast_math =
    Stdlib.Callback.register name (fun code ->
        compile_tmir ~model_only ~fast_math code) in
  register "stanc_compile_tmir" ~model_only:false ~fast_math:false;
  register "stanc_compile_model_tmir" ~model_only:true ~fast_math:false;
  register "stanc_compile_tmir_fast" ~model_only:false ~fast_math:true;
  register "stanc_compile_model_tmir_fast" ~model_only:true ~fast_math:true;
  Stdlib.Callback.register "stanc_compile_tmir_with_includes"
    (fun code include_paths ->
      compile_tmir ~model_only:false ~fast_math:false ~include_paths code);
  Stdlib.Callback.register "stanc_compile_tmir_with_includes_fast"
    (fun code include_paths ->
      compile_tmir ~model_only:false ~fast_math:true ~include_paths code)
