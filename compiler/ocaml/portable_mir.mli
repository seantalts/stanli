val encode : Middle.Program.Typed.t -> string
(** Encode the backend-transformed, optimized stanc3 MIR consumed by stanli as
    compact portable MIR v4. The canonical result is a C-string-safe ASCII
    envelope containing a length-delimited binary payload. *)
