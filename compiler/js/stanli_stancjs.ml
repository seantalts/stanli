open Std
open Frontend
open Conversion
open Js_of_ocaml

let invoke_driver model_name model flags =
  let warnings = ref [] in
  let compilation_result =
    Return.with_return @@ fun return ->
    let output_callback : Driver.Entry.other_output -> unit = function
      | Warnings w -> warnings := !warnings @ w
      | Formatted s
       |DebugOutput s
       |Memory_patterns s
       |Info s
       |Version s
       |Generated s ->
          return (Ok s) in
    Driver.Entry.stan2cpp model_name (`Code model) flags output_callback in
  (compilation_result, !warnings)

let stan2cpp_wrapped name code flags includes : stancReturn Js.t =
  let includes, include_reader_warnings = get_includes_lenient includes in
  let compilation_result =
    let open Result.Syntax in
    let* {name; code; driver_flags; color_output} =
      process_flags name code flags includes in
    let+ result, warnings =
      Common.ICE.with_exn_message (fun () ->
          invoke_driver name code driver_flags) in
    (result, warnings, driver_flags.filename_in_msg, code, color_output) in
  match compilation_result with
  | Ok (result, warnings, printed_filename, code, color_output) ->
      let warnings =
        include_reader_warnings
        @ List.map
            ~f:(str_color ~color_output "%a" (Warnings.pp ?printed_filename))
            warnings in
      wrap_result ?printed_filename ~color_output ~code result ~warnings
  | Error non_compilation_error ->
      wrap_error ~warnings:include_reader_warnings non_compilation_error

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
  | Error message -> wrap_error ~warnings:include_reader_warnings message
  | Ok {name; code; driver_flags; color_output} ->
      let compilation =
        Stanli_pipeline.compile_portable ~model_only ~fast_math ~model_name:name code
          ~include_source:driver_flags.include_source in
      let warnings =
        include_reader_warnings
        @ List.map compilation.warnings ~f:(fun warning ->
              str_color ~color_output "%a"
                (Warnings.pp ?printed_filename:driver_flags.filename_in_msg)
                warning)
        @ List.map compilation.diagnostics
            ~f:Stanli_pipeline.diagnostic_message in
      (match compilation.result with
      | Ok encoded ->
          wrap_result ?printed_filename:driver_flags.filename_in_msg ~code
            ~color_output ~warnings (Ok encoded)
      | Error (Stanli_pipeline.Frontend_error error) ->
          wrap_result ?printed_filename:driver_flags.filename_in_msg ~code
            ~color_output ~warnings (Error error)
      | Error (Stanli_pipeline.Internal_error message) ->
          wrap_error ~warnings message)

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
  Js.export "stanc" (Js.Unsafe.callback stan2cpp_wrapped);
  let export_portable js_name ~model_only ~fast_math =
    Js.export js_name
      (Js.Unsafe.callback_with_arity 4
         (compile_portable ~model_only ~fast_math)) in
  export_portable "stanli_compile" ~model_only:false ~fast_math:false;
  export_portable "stanli_compile_model" ~model_only:true ~fast_math:false;
  export_portable "stanli_compile_fast" ~model_only:false ~fast_math:true;
  export_portable "stanli_compile_model_fast" ~model_only:true ~fast_math:true
