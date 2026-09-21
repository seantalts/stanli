let () =
  let model_only = ref false in
  let include_paths = ref [] in
  let model = ref None in
  let usage =
    "usage: stanli_compiler_cli [--model-only] [--include-path DIR]... MODEL.stan" in
  Arg.parse
    [ ("--model-only", Arg.Set model_only, "compile only model entry points")
    ; ( "--include-path"
      , Arg.String (fun path -> include_paths := path :: !include_paths)
      , "directory to search for Stan includes (repeatable)" ) ]
    (fun path ->
      match !model with
      | None -> model := Some path
      | Some _ -> raise (Arg.Bad "expected exactly one Stan source file"))
    usage;
  if Option.is_none !model then (
    prerr_endline usage;
    exit 2);
  let path = Option.get !model in
  let code = In_channel.with_open_bin path In_channel.input_all in
  let compilation =
    Stanli_pipeline.compile_portable ~model_only:!model_only ~model_name:"embedded_model" code
      ~include_source:
        (Frontend.Include_files.FileSystemPaths
           (List.rev !include_paths @ [Filename.dirname path]))
  in
  List.iter
    (fun warning ->
      Fmt.epr "%a@." (Frontend.Warnings.pp ?printed_filename:None) warning)
    compilation.warnings;
  List.iter
    (fun diagnostic ->
      prerr_endline (Stanli_pipeline.diagnostic_message diagnostic))
    compilation.diagnostics;
  match compilation.result with
  | Ok encoded -> print_string encoded
  | Error (Stanli_pipeline.Internal_error message) ->
      prerr_endline message;
      exit 1
  | Error (Stanli_pipeline.Frontend_error error) ->
      Fmt.epr "%a@."
        (Frontend.Errors.pp ?printed_filename:None ~code)
        error;
      exit 1
