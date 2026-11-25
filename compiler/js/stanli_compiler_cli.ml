let () =
  let model_only = ref false in
  let fast_math = ref false in
  let include_paths = ref [] in
  let filename = ref None in
  let messages_json = ref None in
  let model = ref None in
  let passes = ref None in
  let usage =
    "usage: stanli_compiler_cli [--model-only] [--fast-math] [--passes SPEC] [--include-path DIR]... [--filename NAME] [--messages-json PATH] MODEL.stan" in
  let set_passes spec =
    match Stanli_pipeline.parse_pass_override spec with
    | Ok selected -> passes := Some selected
    | Error message -> raise (Arg.Bad ("--passes: " ^ message)) in
  Arg.parse
    [ ("--model-only", Arg.Set model_only, "compile only model entry points")
    ; ("--fast-math", Arg.Set fast_math, "enable partial evaluation")
    ; ( "--passes"
      , Arg.String set_passes
      , "none, o1 (the shipping passes), or a comma-separated list of \
         optimization_settings field names to run instead of the shipping \
         passes" )
    ; ( "--include-path"
      , Arg.String (fun path -> include_paths := path :: !include_paths)
      , "directory to search for Stan includes (repeatable)" )
    ; ( "--filename"
      , Arg.String (fun name -> filename := Some name)
      , "file name to print in diagnostics instead of 'string'" )
    ; ( "--messages-json"
      , Arg.String (fun path -> messages_json := Some path)
      , "also write the warnings to PATH as JSON" )
    ]
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
    Stanli_pipeline.compile_portable ~model_only:!model_only ~fast_math:!fast_math ?passes:!passes ~model_name:"embedded_model" code
      ~include_source:
        (Frontend.Include_files.FileSystemPaths
           (List.rev !include_paths @ [Filename.dirname path]))
  in
  List.iter
    (fun warning ->
      Fmt.epr "%a@." (Frontend.Warnings.pp ?printed_filename:!filename ~code) warning)
    compilation.warnings;
  Option.iter
    (fun path ->
      let warning w =
        `String
          (String.trim
             (Fmt.str "%a"
                (Frontend.Warnings.pp ?printed_filename:!filename ~code)
                w)) in
      let json =
        `Assoc [("warnings", `List (List.map warning compilation.warnings))]
      in
      Out_channel.with_open_bin path (fun out ->
          Yojson.Basic.to_channel out json;
          Out_channel.output_char out '\n'))
    !messages_json;
  match compilation.result with
  | Ok encoded -> print_string encoded
  | Error (Stanli_pipeline.Internal_error message)
   |Error (Stanli_pipeline.Unsupported message) ->
      prerr_endline message;
      exit 1
  | Error (Stanli_pipeline.Frontend_error error) ->
      Fmt.epr "%a@."
        (Frontend.Errors.pp ?printed_filename:!filename ~code)
        error;
      exit 1
