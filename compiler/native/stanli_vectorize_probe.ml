let usage =
  "usage: stanli-vectorize-probe --vectorize-loops off|on --output OUT \
   [--legacy-output OUT] MODEL.stan"

let fail_usage message =
  prerr_endline message;
  prerr_endline usage;
  exit 2

let enabled_value option = function
  | "off" -> false
  | "on" -> true
  | value -> fail_usage ("unknown " ^ option ^ " value: " ^ value)

let rec parse_args index vectorize output legacy_output model =
  if index = Array.length Sys.argv then (vectorize, output, legacy_output, model)
  else
    match Sys.argv.(index) with
    | "--vectorize-loops" when index + 1 < Array.length Sys.argv ->
        let enabled = enabled_value "vectorize-loops" Sys.argv.(index + 1) in
        parse_args (index + 2) (Some enabled) output legacy_output model
    | "--output" when index + 1 < Array.length Sys.argv ->
        parse_args (index + 2) vectorize (Some Sys.argv.(index + 1)) legacy_output
          model
    | "--legacy-output" when index + 1 < Array.length Sys.argv ->
        parse_args (index + 2) vectorize output (Some Sys.argv.(index + 1)) model
    | ("--vectorize-loops" | "--output" | "--legacy-output") as option ->
        fail_usage ("missing value for " ^ option)
    | argument when String.starts_with ~prefix:"-" argument ->
        fail_usage ("unknown option: " ^ argument)
    | path -> (
        match model with
        | None -> parse_args (index + 1) vectorize output legacy_output (Some path)
        | Some _ -> fail_usage "more than one model path was provided")

let () =
  let vectorize, output, legacy_output, model =
    parse_args 1 None None None None in
  let vectorize =
    match vectorize with
    | Some enabled -> enabled
    | None -> fail_usage "--vectorize-loops is required" in
  let output =
    match output with
    | Some path -> path
    | None -> fail_usage "--output is required" in
  let model =
    match model with
    | Some path -> path
    | None -> fail_usage "MODEL.stan is required" in
  let code =
    try In_channel.with_open_bin model In_channel.input_all
    with Sys_error message ->
      prerr_endline message;
      exit 1 in
  let compilation =
    Stanli_pipeline.compile_mir_with_passes
      ~passes:{vectorize_loops= vectorize; max_o1_statement_depth_cost= None}
      ~model_name:"embedded_model" code
      ~include_source:
        (Frontend.Include_files.FileSystemPaths [Filename.dirname model]) in
  List.iter
    (fun warning ->
      Fmt.epr "%a@." (Frontend.Warnings.pp ?printed_filename:None) warning)
    compilation.warnings;
  match compilation.result with
  | Error (Stanli_pipeline.Internal_error message) ->
      prerr_endline message;
      exit 1
  | Error (Stanli_pipeline.Frontend_error error) ->
      Fmt.epr "%a@." (Frontend.Errors.pp ?printed_filename:None ~code) error;
      exit 1
  | Ok mir -> (
      match
        Common.ICE.with_exn_message (fun () -> Portable_mir.encode mir)
      with
      | Error message ->
          prerr_endline message;
          exit 1
      | Ok encoded -> (
          try
            Out_channel.with_open_bin output (fun channel ->
                output_string channel encoded);
            Option.iter
              (fun path ->
                Out_channel.with_open_bin path (fun channel ->
                    mir |> Middle.Program.Typed.sexp_of_t
                    |> Sexplib0.Sexp.to_string_hum |> output_string channel))
              legacy_output
          with Sys_error message ->
            prerr_endline message;
            exit 1))
