(* stanli embedding entry point: Stan code -> portable-MIR JSON (stanc3 --O1:
   inlining, constant/copy propagation, dead code elimination, partial
   evaluation). Registered for C via Callback; built with -output-complete-obj
   so the OCaml runtime rides inside one object file linked into libstanli.
   Return protocol: "OK<json>" or "ERR<message>". *)

let compile_tmir (code : string) : string =
  let flags =
    { Driver.Flags.default with
      optimization_level= Analysis_and_optimization.Optimize.O1 } in
  let output : Driver.Entry.other_output -> unit = fun _ -> () in
  let res =
    Common.ICE.with_exn_message (fun () ->
        Stdlib.Result.map Portable_mir.encode
          (Driver.Entry.stan2mir "embedded_model" (`Code code) flags output))
  in
  match res with
  | Error internal -> "ERR" ^ internal
  | Ok (Error e) ->
      "ERR"
      ^ Fmt.str "%a" (Frontend.Errors.pp ?printed_filename:None ?code:None) e
  | Ok (Ok encoded) -> "OK" ^ encoded

let () = Stdlib.Callback.register "stanc_compile_tmir" compile_tmir
