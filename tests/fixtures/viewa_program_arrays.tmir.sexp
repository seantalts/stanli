((functions_block
  (((fdrt (ReturnType (UArray UReal))) (fdname rhs_flat) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable y (UArray UReal))
      (AutoDiffable theta (UArray UReal)) (AutoDiffable x_r (UArray UReal))
      (AutoDiffable x_i (UArray UInt))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Times__ FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var theta))
                                  (meta
                                   ((type_ (UArray UReal)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Indexed
                                 ((pattern (Var y))
                                  (meta
                                   ((type_ (UArray UReal)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Indexed
                             ((pattern (Var x_r))
                              (meta
                               ((type_ (UArray UReal)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Lit Int 1))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern
                           (Indexed
                            ((pattern (Var x_i))
                             (meta
                              ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                            ((Single
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType (UArray UReal))) (fdname rhs_depth2) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable y (UArray UReal))
      (AutoDiffable theta (UArray (UArray UReal))) (AutoDiffable x_r (UArray UReal))
      (AutoDiffable x_i (UArray UInt))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (StanLib Times__ FnPlain AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var theta))
                          (meta
                           ((type_ (UArray (UArray UReal))) (loc <opaque>)
                            (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var y))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType (UArray UReal))) (fdname rhs_depth2_int) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable y (UArray UReal))
      (AutoDiffable theta (UArray UReal)) (AutoDiffable x_r (UArray UReal))
      (AutoDiffable x_i (UArray (UArray UInt)))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Times__ FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var theta))
                              (meta
                               ((type_ (UArray UReal)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Lit Int 1))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Indexed
                             ((pattern (Var y))
                              (meta
                               ((type_ (UArray UReal)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Lit Int 1))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern
                           (Indexed
                            ((pattern (Var x_i))
                             (meta
                              ((type_ (UArray (UArray UInt))) (loc <opaque>)
                               (adlevel DataOnly))))
                            ((Single
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                             (Single
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType (UArray UReal))) (fdname rhs_vectors) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable y (UArray UReal))
      (AutoDiffable theta (UArray UVector)) (AutoDiffable x_r (UArray UReal))
      (AutoDiffable x_i (UArray UInt))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (StanLib Times__ FnPlain AoS)
                     (((pattern
                        (Indexed
                         ((pattern
                           (Indexed
                            ((pattern (Var theta))
                             (meta
                              ((type_ (UArray UVector)) (loc <opaque>)
                               (adlevel AutoDiffable))))
                            ((Single
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var y))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType (UArray UReal))) (fdname rhs_matrices) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable y (UArray UReal))
      (AutoDiffable theta (UArray UMatrix)) (AutoDiffable x_r (UArray UReal))
      (AutoDiffable x_i (UArray UInt))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (StanLib Times__ FnPlain AoS)
                     (((pattern
                        (Indexed
                         ((pattern
                           (Indexed
                            ((pattern (Var theta))
                             (meta
                              ((type_ (UArray UMatrix)) (loc <opaque>)
                               (adlevel AutoDiffable))))
                            ((Single
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var y))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars ()) (prepare_data ())
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
          ((pattern (Var q))
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
          ((pattern (Var q))
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
     (out_trans Identity)))))
 (prog_name viewa_program_arrays_model)
 (prog_path tests/fixtures/viewa_program_arrays.stan))