(* stanli embedding entry point: Stan code -> optimized-MIR sexp string
   (stanc3 --O1: inlining, constant/copy propagation, dead code
   elimination, partial evaluation). Registered for C via Callback; built
   with -output-complete-obj so the OCaml runtime rides inside one object
   file linked into libstanli.
   Return protocol: "OK<sexp>" or "ERR<message>". *)
open Core

let compile_tmir (code : string) : string =
  let captured = ref [] in
  let flags =
    { Driver.Flags.default with
      optimization_level= Analysis_and_optimization.Optimize.O1
    ; debug_settings =
        { Driver.Flags.default.debug_settings with
          print_optimized_mir= Driver.Flags.Basic } } in
  let output : Driver.Entry.other_output -> unit = function
    | DebugOutput s -> captured := s :: !captured
    | _ -> () in
  let res =
    Common.ICE.with_exn_message (fun () ->
        Driver.Entry.stan2cpp "embedded_model" (`Code code) flags output) in
  match res with
  | Error internal -> "ERR" ^ internal
  | Ok _ when not (List.is_empty !captured) ->
      "OK" ^ String.concat ~sep:"" (List.rev !captured)
  | Ok (Error e) ->
      "ERR" ^ Fmt.str "%a" (Frontend.Errors.pp ?printed_filename:None ?code:None) e
  | Ok (Ok _) -> "ERRinternal: no optimized MIR captured"

let () = Stdlib.Callback.register "stanc_compile_tmir" compile_tmir
