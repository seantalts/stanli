((functions_block
  (((fdrt (ReturnType UReal)) (fdname f) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable v UVector) (AutoDiffable idx (UArray UInt))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id s) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable s) ()) UReal
             ((pattern
               (Promotion
                ((pattern (Lit Int 0))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                UReal AutoDiffable))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (For (loopvar i)
             (lower
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (upper
              ((pattern
                (FunApp (StanLib size FnPlain AoS)
                 (((pattern (Var idx))
                   (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (body
              ((pattern
                (Block
                 (((pattern
                    (Assignment ((LVariable s) ()) UReal
                     ((pattern
                       (FunApp (StanLib Plus__ FnPlain AoS)
                        (((pattern (Var s))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern
                           (Indexed
                            ((pattern (Var v))
                             (meta
                              ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                            ((Single
                              ((pattern
                                (Indexed
                                 ((pattern (Var idx))
                                  (meta
                                   ((type_ (UArray UInt)) (loc <opaque>)
                                    (adlevel DataOnly))))
                                 ((Single
                                   ((pattern (Var i))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                   (meta <opaque>)))))
               (meta <opaque>)))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((winner <opaque>
    (SArray
     (SArray SInt
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
 (prepare_data
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id pos__) (decl_type (Sized SInt))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable pos__) ()) UInt
      ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id winner)
      (decl_type
       (Sized
        (SArray
         (SArray SInt
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id winner_flat__)
          (decl_type (Unsized (UArray UInt))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable winner_flat__) ()) (UArray UInt)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str winner))
               (meta ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable pos__) ()) UInt
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (For (loopvar sym1__)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Lit Int 2))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (For (loopvar sym2__)
                  (lower
                   ((pattern (Lit Int 1))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (upper
                   ((pattern (Lit Int 3))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable winner)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray (UArray UInt))
                          ((pattern
                            (Indexed
                             ((pattern (Var winner_flat__))
                              (meta
                               ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                             ((Single
                               ((pattern (Var pos__))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                        (meta <opaque>))
                       ((pattern
                         (Assignment ((LVariable pos__) ()) UInt
                          ((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern (Var pos__))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id elo)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 4))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
             (((pattern (Var elo))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_f_return_sym5__)
          (decl_type (Sized SReal)) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Block
          (((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_f_s_sym6__)
              (decl_type (Sized SReal)) (initialize Uninit)))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_f_s_sym6__) ()) UReal
              ((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                 UReal AutoDiffable))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
            (meta <opaque>))
           ((pattern
             (For (loopvar inline_f_i_sym7__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern
                 (FunApp (StanLib size FnPlain AoS)
                  (((pattern
                     (Indexed
                      ((pattern (Var winner))
                       (meta
                        ((type_ (UArray (UArray UInt))) (loc <opaque>)
                         (adlevel DataOnly))))
                      (All
                       (Single
                        ((pattern (Lit Int 1))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (Assignment ((LVariable inline_f_s_sym6__) ()) UReal
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern (Var inline_f_s_sym6__))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Indexed
                             ((pattern (Var elo))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                             ((Single
                               ((pattern
                                 (Indexed
                                  ((pattern
                                    (Indexed
                                     ((pattern (Var winner))
                                      (meta
                                       ((type_ (UArray (UArray UInt))) 
                                        (loc <opaque>) (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var inline_f_i_sym7__))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Lit Int 1))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ (UArray UInt)) (loc <opaque>)
                                     (adlevel DataOnly))))
                                  ()))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_f_return_sym5__) ()) UReal
              ((pattern (Var inline_f_s_sym6__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var inline_f_return_sym5__))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id elo)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 4))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
             (((pattern (Var elo))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_f_return_sym1__)
          (decl_type (Sized SReal)) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Block
          (((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_f_s_sym2__)
              (decl_type (Sized SReal)) (initialize Uninit)))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_f_s_sym2__) ()) UReal
              ((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                 UReal AutoDiffable))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
            (meta <opaque>))
           ((pattern
             (For (loopvar inline_f_i_sym3__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern
                 (FunApp (StanLib size FnPlain SoA)
                  (((pattern
                     (Indexed
                      ((pattern (Var winner))
                       (meta
                        ((type_ (UArray (UArray UInt))) (loc <opaque>)
                         (adlevel DataOnly))))
                      (All
                       (Single
                        ((pattern (Lit Int 1))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (Assignment ((LVariable inline_f_s_sym2__) ()) UReal
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain SoA)
                         (((pattern (Var inline_f_s_sym2__))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Indexed
                             ((pattern (Var elo))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                             ((Single
                               ((pattern
                                 (Indexed
                                  ((pattern
                                    (Indexed
                                     ((pattern (Var winner))
                                      (meta
                                       ((type_ (UArray (UArray UInt))) 
                                        (loc <opaque>) (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var inline_f_i_sym3__))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Lit Int 1))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ (UArray UInt)) (loc <opaque>)
                                     (adlevel DataOnly))))
                                  ()))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_f_return_sym1__) ()) UReal
              ((pattern (Var inline_f_s_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var inline_f_return_sym1__))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id elo)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 4))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var elo)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
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
    (meta <opaque>))))
 (transform_inits
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id pos__) (decl_type (Sized SInt))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable pos__) ()) UInt
      ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id elo)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id elo_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable elo_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str elo))
               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable pos__) ()) UInt
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (For (loopvar sym1__)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Lit Int 4))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (Assignment
                  ((LVariable elo)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var elo_flat__))
                      (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                     ((Single
                       ((pattern (Var pos__))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
                (meta <opaque>))
               ((pattern
                 (Assignment ((LVariable pos__) ()) UInt
                  ((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern (Var pos__))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Int 1))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var elo)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id elo)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable elo) ()) UVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var elo)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((elo <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))))
 (prog_name two_dimensional_int_column_slice_model)
 (prog_path tests/lit/issue_257/two_dimensional_int_column_slice.stan))
