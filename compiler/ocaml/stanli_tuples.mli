val scalarize : Middle.Program.Typed.t -> Middle.Program.Typed.t
(** Split every tuple in the procedure and function bodies into one variable
    per leaf. A tuple-returning call that was not inlined is bound once to a
    temporary of the flat leaf tuple and read through its projections; a
    function returning a tuple returns the flat leaf tuple; an array of
    tuples printed whole is the tuple of its leaf arrays, typed as the
    original. input_vars and output_vars keep their types. A statement the
    pass cannot rewrite is left as it was, unless it names a tuple already
    split into leaves; then [Portable_mir.Not_supported] is raised. *)
