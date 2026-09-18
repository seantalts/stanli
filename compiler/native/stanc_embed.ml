(* stanli embedding entry point: Stan code -> compact portable MIR (upstream O1
   plus loop vectorization). Registered for C via Callback; built with
   -output-complete-obj so the OCaml runtime rides inside one object file linked
   into libstanli. Return protocol: "OK<portable>" or "ERR<message>". *)

let compile_tmir ~model_only (code : string) : string =
  let compilation =
    Stanli_pipeline.compile_portable ~model_only ~model_name:"embedded_model" code in
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
  Stdlib.Callback.register "stanc_compile_tmir" (compile_tmir ~model_only:false);
  Stdlib.Callback.register "stanc_compile_model_tmir" (compile_tmir ~model_only:true)
