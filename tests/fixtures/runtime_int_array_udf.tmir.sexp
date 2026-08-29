((functions_block
  (((fdrt (ReturnType (UArray UInt))) (fdname matching_indices) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable x (UArray UInt)) (AutoDiffable test UInt)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (NRFunApp (CompilerInternal FnValidateSize)
             (((pattern (Lit Str hit))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Str "size(x)"))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (StanLib size FnPlain AoS)
                 (((pattern (Var x))
                   (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id hit)
             (decl_type
              (Sized
               (SArray SInt
                ((pattern
                  (FunApp (StanLib size FnPlain AoS)
                   (((pattern (Var x))
                     (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (For (loopvar i)
             (lower
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (upper
              ((pattern
                (FunApp (StanLib size FnPlain AoS)
                 (((pattern (Var x))
                   (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (body
              ((pattern
                (Block
                 (((pattern
                    (Assignment
                     ((LVariable hit)
                      ((Single
                        ((pattern (Var i))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                     (UArray UInt)
                     ((pattern
                       (FunApp (StanLib Equals__ FnPlain AoS)
                        (((pattern
                           (Indexed
                            ((pattern (Var x))
                             (meta
                              ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                            ((Single
                              ((pattern (Var i))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Var test))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                   (meta <opaque>)))))
               (meta <opaque>)))))
           (meta <opaque>))
          ((pattern
            (NRFunApp (CompilerInternal FnValidateSize)
             (((pattern (Lit Str out))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Str "sum(hit)"))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern (Var hit))
                   (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id out)
             (decl_type
              (Sized
               (SArray SInt
                ((pattern
                  (FunApp (StanLib sum FnPlain AoS)
                   (((pattern (Var hit))
                     (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id at) (decl_type (Sized SInt))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable at) ()) UInt
             ((pattern (Lit Int 1))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
           (meta <opaque>))
          ((pattern
            (For (loopvar i)
             (lower
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (upper
              ((pattern
                (FunApp (StanLib size FnPlain AoS)
                 (((pattern (Var x))
                   (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (body
              ((pattern
                (Block
                 (((pattern
                    (IfElse
                     ((pattern
                       (Indexed
                        ((pattern (Var hit))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                        ((Single
                          ((pattern (Var i))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern
                       (Block
                        (((pattern
                           (Assignment
                            ((LVariable out)
                             ((Single
                               ((pattern (Var at))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                            (UArray UInt)
                            ((pattern (Var i))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                          (meta <opaque>))
                         ((pattern
                           (Assignment ((LVariable at) ()) UInt
                            ((pattern
                              (FunApp (StanLib Plus__ FnPlain AoS)
                               (((pattern (Var at))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern (Lit Int 1))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                          (meta <opaque>)))))
                      (meta <opaque>))
                     ()))
                   (meta <opaque>)))))
               (meta <opaque>)))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var out))
               (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((N <opaque> SInt)
   (x <opaque>
    (SArray SInt
     ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
 (prepare_data
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id N) (decl_type (Sized SInt))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable N) ()) UInt
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str N))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
         ((Single
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnCheck
        (trans
         (Lower
          ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (var_name N)
        (var ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str x)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id x)
      (decl_type
       (Sized
        (SArray SInt
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable x) ()) (UArray UInt)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str x))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id selected_1dim__) (decl_type (Sized SInt))
      (initialize
       (Assign
        ((pattern
          (FunApp (StanLib size FnPlain AoS)
           (((pattern
              (FunApp (UserDefined matching_indices FnPlain)
               (((pattern (Var x))
                 (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Lit Int 1))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str selected))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str "size(matching_indices(x, 1))"))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var selected_1dim__))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity) (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id done) (decl_type (Sized SInt))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable done) ()) UInt
          ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Greater__ FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var done))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Block
             (((pattern
                (NRFunApp (CompilerInternal FnValidateSize)
                 (((pattern (Lit Str selected))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Str "size(matching_indices(x, 1))"))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (StanLib size FnPlain AoS)
                     (((pattern
                        (FunApp (UserDefined matching_indices FnPlain)
                         (((pattern (Var x))
                           (meta
                            ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id selected)
                 (decl_type
                  (Sized
                   (SArray SInt
                    ((pattern
                      (FunApp (StanLib size FnPlain AoS)
                       (((pattern
                          (FunApp (UserDefined matching_indices FnPlain)
                           (((pattern (Var x))
                             (meta
                              ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 1))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable selected) ()) (UArray UInt)
                 ((pattern
                   (FunApp (UserDefined matching_indices FnPlain)
                    (((pattern (Var x))
                      (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 1))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (TargetPE
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern
                       (Promotion
                        ((pattern
                          (FunApp (StanLib Plus__ FnPlain AoS)
                           (((pattern
                              (FunApp (StanLib size FnPlain AoS)
                               (((pattern (Var selected))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern
                              (FunApp (StanLib sum FnPlain AoS)
                               (((pattern (Var selected))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        UReal DataOnly))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Var theta))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable done) ()) UInt
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var done))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 1))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>)))))
           (meta <opaque>))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var theta))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity) (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id done) (decl_type (Sized SInt))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable done) ()) UInt
          ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Greater__ FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var done))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Block
             (((pattern
                (NRFunApp (CompilerInternal FnValidateSize)
                 (((pattern (Lit Str selected))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Str "size(matching_indices(x, 1))"))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (StanLib size FnPlain AoS)
                     (((pattern
                        (FunApp (UserDefined matching_indices FnPlain)
                         (((pattern (Var x))
                           (meta
                            ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id selected)
                 (decl_type
                  (Sized
                   (SArray SInt
                    ((pattern
                      (FunApp (StanLib size FnPlain AoS)
                       (((pattern
                          (FunApp (UserDefined matching_indices FnPlain)
                           (((pattern (Var x))
                             (meta
                              ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 1))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable selected) ()) (UArray UInt)
                 ((pattern
                   (FunApp (UserDefined matching_indices FnPlain)
                    (((pattern (Var x))
                      (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 1))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (TargetPE
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern
                       (Promotion
                        ((pattern
                          (FunApp (StanLib Plus__ FnPlain AoS)
                           (((pattern
                              (FunApp (StanLib size FnPlain AoS)
                               (((pattern (Var selected))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern
                              (FunApp (StanLib sum FnPlain AoS)
                               (((pattern (Var selected))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        UReal DataOnly))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Var theta))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable done) ()) UInt
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var done))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 1))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>)))))
           (meta <opaque>))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var theta))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id theta) (decl_type (Sized SReal))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity) (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var theta)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (IfElse
      ((pattern
        (FunApp (StanLib PNot__ FnPlain AoS)
         (((pattern
            (EOr
             ((pattern (Var emit_transformed_parameters__))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Var emit_generated_quantities__))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern (Block (((pattern (Return ())) (meta <opaque>))))) (meta <opaque>)) ()))
    (meta <opaque>))
   ((pattern
     (IfElse
      ((pattern
        (FunApp (StanLib PNot__ FnPlain AoS)
         (((pattern (Var emit_generated_quantities__))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern (Block (((pattern (Return ())) (meta <opaque>))))) (meta <opaque>)) ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id selected)
      (decl_type
       (Sized
        (SArray SInt
         ((pattern (Var selected_1dim__))
          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable selected) ()) (UArray UInt)
      ((pattern
        (FunApp (UserDefined matching_indices FnPlain)
         (((pattern (Var x))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id selected_count) (decl_type (Sized SInt))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable selected_count) ()) UInt
      ((pattern
        (FunApp (StanLib size FnPlain AoS)
         (((pattern (Var selected))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id selected_sum) (decl_type (Sized SInt))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable selected_sum) ()) UInt
      ((pattern
        (FunApp (StanLib sum FnPlain AoS)
         (((pattern (Var selected))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var selected))
          (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var selected_count))
          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var selected_sum))
          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (transform_inits
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable theta) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str theta))
              (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
         ((Single
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var theta)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable theta) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var theta)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((theta <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (selected <opaque>
    ((out_unconstrained_st
      (SArray SInt
       ((pattern (Var selected_1dim__))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray SInt
       ((pattern (Var selected_1dim__))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block GeneratedQuantities) (out_trans Identity)))
   (selected_count <opaque>
    ((out_unconstrained_st SInt) (out_constrained_st SInt)
     (out_block GeneratedQuantities) (out_trans Identity)))
   (selected_sum <opaque>
    ((out_unconstrained_st SInt) (out_constrained_st SInt)
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name runtime_int_array_udf_model)
 (prog_path tests/fixtures/runtime_int_array_udf.stan))
