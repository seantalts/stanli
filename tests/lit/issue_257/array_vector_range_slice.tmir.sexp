((functions_block ()) (input_vars ((K <opaque> SInt) (TT <opaque> SInt)))
 (prepare_data
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id K) (decl_type (Sized SInt))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable K) ()) UInt
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str K))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
         ((Single
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id TT) (decl_type (Sized SInt))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable TT) ()) UInt
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str TT))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
         ((Single
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str eta)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str eta)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id eta)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var TT))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (For (loopvar j)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var eta))
                          (meta
                           ((type_ (UArray UVector)) (loc <opaque>)
                            (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var j))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>))
               ((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib normal_lpdf (FnLpdf true) AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var eta))
                          (meta
                           ((type_ (UArray UVector)) (loc <opaque>)
                            (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var j))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Between
                           ((pattern (Lit Int 2))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                           ((pattern (Var TT))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var eta))
                          (meta
                           ((type_ (UArray UVector)) (loc <opaque>)
                            (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var j))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Between
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                           ((pattern
                             (FunApp (StanLib Minus__ FnPlain AoS)
                              (((pattern (Var TT))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                               ((pattern (Lit Int 1))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 1))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id eta)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var TT))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (For (loopvar j)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib std_normal_lpdf (FnLpdf true) SoA)
                     (((pattern
                        (Indexed
                         ((pattern (Var eta))
                          (meta
                           ((type_ (UArray UVector)) (loc <opaque>)
                            (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var j))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>))
               ((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib normal_lpdf (FnLpdf true) AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var eta))
                          (meta
                           ((type_ (UArray UVector)) (loc <opaque>)
                            (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var j))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Between
                           ((pattern (Lit Int 2))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                           ((pattern (Var TT))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var eta))
                          (meta
                           ((type_ (UArray UVector)) (loc <opaque>)
                            (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var j))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Between
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                           ((pattern
                             (FunApp (StanLib Minus__ FnPlain AoS)
                              (((pattern (Var TT))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                               ((pattern (Lit Int 1))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 1))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id eta)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var TT))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (NRFunApp
                      (CompilerInternal
                       (FnWriteParam (unconstrain_opt ())
                        (var
                         ((pattern
                           (Indexed
                            ((pattern (Var eta))
                             (meta
                              ((type_ (UArray UVector)) (loc <opaque>)
                               (adlevel DataOnly))))
                            ((Single
                              ((pattern (Var sym2__))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                             (Single
                              ((pattern (Var sym1__))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                      ()))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
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
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id eta)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id eta_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable eta_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str eta))
               (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
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
           ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (For (loopvar sym2__)
                  (lower
                   ((pattern (Lit Int 1))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (upper
                   ((pattern (Var K))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable eta)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray UVector)
                          ((pattern
                            (Indexed
                             ((pattern (Var eta_flat__))
                              (meta
                               ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
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
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var eta))
          (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id eta)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (Assignment
                      ((LVariable eta)
                       ((Single
                         ((pattern (Var sym2__))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                        (Single
                         ((pattern (Var sym1__))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      UVector
                      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var eta))
          (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((eta <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Var TT)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))))
 (prog_name array_vector_range_slice_model)
 (prog_path tests/lit/issue_257/array_vector_range_slice.stan))
