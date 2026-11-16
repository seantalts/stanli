open Conversion
open Js_of_ocaml

let compile_portable ~model_only ~fast_math name code includes filename :
    stancReturn Js.t =
  let includes, include_reader_warnings = get_includes_lenient includes in
  let flags =
    match Js.Optdef.to_option filename with
    | Some file ->
        Js.array
          [|Js.string "O1"; Js.string ("filename-in-msg=" ^ Js.to_string file)|]
    | None -> Js.array [|Js.string "O1"|] in
  let flags = Js.some flags in
  match process_flags name code flags includes with
  | Error message ->
      wrap_error ~color_output:false ~warnings:include_reader_warnings message
  | Ok {name; code; driver_flags; color_output} ->
      let compilation =
        Stanli_pipeline.compile_portable ~model_only ~fast_math ~model_name:name code
          ~include_source:driver_flags.include_source in
      let warnings = include_reader_warnings @ compilation.warnings in
      let printed_filename = driver_flags.filename_in_msg in
      let returned =
        match compilation.result with
        | Ok encoded ->
            wrap_result ?printed_filename ~code ~color_output ~warnings
              (Ok encoded)
        | Error (Stanli_pipeline.Frontend_error error) ->
            wrap_result ?printed_filename ~code ~color_output ~warnings
              (Error error)
        | Error (Stanli_pipeline.Internal_error message) ->
            wrap_error ?printed_filename ~code ~color_output ~warnings message
      in
      Js.Unsafe.set returned (Js.string "o1_fallback")
        ( match Stanli_pipeline.o1_fallback compilation with
        | Some message -> Js.Unsafe.inject (Js.string message)
        | None -> Js.Unsafe.inject Js.null );
      returned

let dump_stan_math_signatures () =
  Js.string @@ Fmt.str "%a" Stan_math_signatures.pretty_print_all_math_sigs ()

let dump_stan_math_distributions () =
  Js.string
  @@ Fmt.str "%a" Stan_math_signatures.pretty_print_all_math_distributions ()

let () =
  Js.export "dump_stan_math_signatures"
    (Js.Unsafe.callback dump_stan_math_signatures);
  Js.export "dump_stan_math_distributions"
    (Js.Unsafe.callback dump_stan_math_distributions);
  let export_portable js_name ~model_only ~fast_math =
    Js.export js_name
      (Js.Unsafe.callback_with_arity 4
         (compile_portable ~model_only ~fast_math)) in
  export_portable "stanli_compile" ~model_only:false ~fast_math:false;
  export_portable "stanli_compile_model" ~model_only:true ~fast_math:false;
  export_portable "stanli_compile_fast" ~model_only:false ~fast_math:true;
  export_portable "stanli_compile_model_fast" ~model_only:true ~fast_math:true
