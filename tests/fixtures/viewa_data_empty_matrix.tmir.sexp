((functions_block
  (((fdrt (ReturnType (UArray UMatrix))) (fdname pass_through) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable x (UArray UMatrix))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern (Var x))
               (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType UReal)) (fdname shape_code) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable x (UArray UMatrix)) (AutoDiffable q UReal)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib Plus__ FnPlain AoS)
                                 (((pattern (Var q))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                                  ((pattern
                                    (Promotion
                                     ((pattern
                                       (FunApp (StanLib Times__ FnPlain AoS)
                                        (((pattern (Lit Int 100))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         ((pattern
                                           (Indexed
                                            ((pattern
                                              (FunApp (StanLib dims FnPlain AoS)
                                               (((pattern (Var x))
                                                 (meta
                                                  ((type_ (UArray UMatrix))
                                                   (loc <opaque>) (adlevel AutoDiffable)))))))
                                             (meta
                                              ((type_ (UArray UInt))
                                               (loc <opaque>) (adlevel DataOnly))))
                                            ((Single
                                              ((pattern (Lit Int 1))
                                               (meta
                                                ((type_ UInt) (loc <opaque>)
                                                 (adlevel DataOnly))))))))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (Indexed
                                        ((pattern
                                          (FunApp (StanLib dims FnPlain AoS)
                                           (((pattern (Var x))
                                             (meta
                                              ((type_ (UArray UMatrix))
                                               (loc <opaque>) (adlevel AutoDiffable)))))))
                                         (meta
                                          ((type_ (UArray UInt)) (loc <opaque>)
                                           (adlevel DataOnly))))
                                        ((Single
                                          ((pattern (Lit Int 2))
                                           (meta
                                            ((type_ UInt) (loc <opaque>)
                                             (adlevel DataOnly))))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern
                               (Indexed
                                ((pattern
                                  (FunApp (StanLib dims FnPlain AoS)
                                   (((pattern (Var x))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel AutoDiffable)))))))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly))))
                                ((Single
                                  ((pattern (Lit Int 3))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern
                           (FunApp (StanLib Times__ FnPlain AoS)
                            (((pattern (Lit Int 1000))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern
                               (FunApp (StanLib size FnPlain AoS)
                                (((pattern (Var x))
                                  (meta
                                   ((type_ (UArray UMatrix)) (loc <opaque>)
                                    (adlevel AutoDiffable)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern (Lit Int 10000))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern
                           (FunApp (StanLib num_elements FnPlain AoS)
                            (((pattern (Var x))
                              (meta
                               ((type_ (UArray UMatrix)) (loc <opaque>)
                                (adlevel AutoDiffable)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal DataOnly))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((A <opaque>
    (SArray
     (SMatrix AoS
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
 (prepare_data
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id pos__) (decl_type (Sized SInt))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable pos__) ()) UInt
      ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id A)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id A_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable A_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str A))
               (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
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
           ((pattern (Lit Int 3))
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (For (loopvar sym3__)
                          (lower
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (upper
                           ((pattern (Lit Int 0))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable A)
                                   ((Single
                                     ((pattern (Var sym3__))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                    (Single
                                     ((pattern (Var sym2__))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                    (Single
                                     ((pattern (Var sym1__))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (UArray UMatrix)
                                  ((pattern
                                    (Indexed
                                     ((pattern (Var A_flat__))
                                      (meta
                                       ((type_ (UArray UReal)) (loc <opaque>)
                                        (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var pos__))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
                                (meta <opaque>))
                               ((pattern
                                 (Assignment ((LVariable pos__) ()) UInt
                                  ((pattern
                                    (FunApp (StanLib Plus__ FnPlain AoS)
                                     (((pattern (Var pos__))
                                       (meta
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Lit Int 1))
                                       (meta
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                                (meta <opaque>)))))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q) (decl_type (Sized SReal))
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
         (TargetPE
          ((pattern
            (FunApp (UserDefined shape_code FnPlain)
             (((pattern
                (FunApp (UserDefined pass_through FnPlain)
                 (((pattern (Var A))
                   (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var q))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q) (decl_type (Sized SReal))
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
         (TargetPE
          ((pattern
            (FunApp (UserDefined shape_code FnPlain)
             (((pattern
                (FunApp (UserDefined pass_through FnPlain)
                 (((pattern (Var A))
                   (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var q))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id q) (decl_type (Sized SReal))
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
         ((pattern (Var q)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype DataOnly) (decl_id B)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable B) ()) (UArray UMatrix)
      ((pattern
        (FunApp (UserDefined pass_through FnPlain)
         (((pattern (Var A))
           (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id proof) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable proof) ()) UReal
      ((pattern
        (FunApp (UserDefined shape_code FnPlain)
         (((pattern (Var B))
           (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
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
                     (For (loopvar sym3__)
                      (lower
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (upper
                       ((pattern (Lit Int 0))
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
                                    ((pattern (Var B))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
                                    ((Single
                                      ((pattern (Var sym3__))
                                       (meta
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                     (Single
                                      ((pattern (Var sym2__))
                                       (meta
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                     (Single
                                      ((pattern (Var sym1__))
                                       (meta
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var proof)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (transform_inits
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable q) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str q))
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
         ((pattern (Var q)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable q) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var q)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((q <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (B <opaque>
    ((out_unconstrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block GeneratedQuantities) (out_trans Identity)))
   (proof <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal)
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name viewa_data_empty_matrix_model)
 (prog_path tests/fixtures/viewa_data_empty_matrix.stan))