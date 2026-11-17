val encode : Middle.Program.Typed.t -> string
(** Encode the backend-transformed, optimized stanc3 MIR consumed by stanli as
    compact portable MIR v5 (compiler/portable_ir/SCHEMA.md). The canonical
    result is a C-string-safe ASCII envelope containing a length-delimited
    binary payload. Raises [Failure] when two user-function overloads take the
    same arguments once their tuple arguments are split into components. *)

val tuple_leaf_name : string -> int list -> string
(** [tuple_leaf_name base components] is the variable holding one leaf of the
    tuple variable [base], selected by each 1-based component index from the
    outermost tuple level: [tuple_leaf_name "x" [2; 1]] is ["x.2.1"], the
    CmdStan name of that leaf. *)
