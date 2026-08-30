((functions_block ()) (input_vars ()) (prepare_data ())
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id sigma) (decl_type (Sized SReal))
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
            (FunApp (StanLib normal_lpdf (FnLpdf true) AoS)
             (((pattern (Var sigma))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 1))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (IfElse
          ((pattern
            (FunApp (StanLib Less__ FnPlain AoS)
             (((pattern (Var sigma))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int -1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Block
             (((pattern
                (TargetPE
                 ((pattern (FunApp (CompilerInternal FnNegInf) ()))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>)))))
           (meta <opaque>))
          (((pattern
             (Block
              (((pattern
                 (IfElse
                  ((pattern
                    (FunApp (StanLib Greater__ FnPlain AoS)
                     (((pattern (Var sigma))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Lit Int 1))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (Block
                     (((pattern
                        (TargetPE
                         ((pattern (FunApp (CompilerInternal FnNegInf) ()))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
                       (meta <opaque>)))))
                   (meta <opaque>))
                  (((pattern
                     (Block
                      (((pattern
                         (TargetPE
                          ((pattern
                            (FunApp (StanLib PMinus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib log_diff_exp FnPlain AoS)
                                 (((pattern
                                    (FunApp (StanLib normal_lcdf FnPlain AoS)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 0))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (FunApp (StanLib normal_lcdf FnPlain AoS)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int -1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 0))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id sigma) (decl_type (Sized SReal))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity) (dims ()) (mem_pattern SoA)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf true) SoA)
             (((pattern (Var sigma))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 1))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (IfElse
          ((pattern
            (FunApp (StanLib Less__ FnPlain SoA)
             (((pattern (Var sigma))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int -1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Block
             (((pattern
                (TargetPE
                 ((pattern (FunApp (CompilerInternal FnNegInf) ()))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>)))))
           (meta <opaque>))
          (((pattern
             (Block
              (((pattern
                 (IfElse
                  ((pattern
                    (FunApp (StanLib Greater__ FnPlain SoA)
                     (((pattern (Var sigma))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Lit Int 1))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (Block
                     (((pattern
                        (TargetPE
                         ((pattern (FunApp (CompilerInternal FnNegInf) ()))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
                       (meta <opaque>)))))
                   (meta <opaque>))
                  (((pattern
                     (Block
                      (((pattern
                         (TargetPE
                          ((pattern
                            (FunApp (StanLib PMinus__ FnPlain SoA)
                             (((pattern
                                (FunApp (StanLib log_diff_exp FnPlain AoS)
                                 (((pattern
                                    (FunApp (StanLib normal_lcdf FnPlain AoS)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 0))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (FunApp (StanLib normal_lcdf FnPlain AoS)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int -1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 0))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id sigma) (decl_type (Sized SReal))
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
         ((pattern (Var sigma)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype AutoDiffable) (decl_id sigma) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable sigma) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str sigma))
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
         ((pattern (Var sigma)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id sigma) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable sigma) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var sigma)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((sigma <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))))
 (prog_name parameter_truncation_two_sided_model)
 (prog_path tests/lit/issue_257/parameter_truncation_two_sided.stan))
