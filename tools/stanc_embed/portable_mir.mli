val encode : Middle.Program.Typed.t -> string
(** Encode the backend-transformed, optimized stanc3 MIR consumed by stanli.

    The result is the canonical compact UTF-8 JSON representation of stanli's
    portable MIR v1 envelope. *)
