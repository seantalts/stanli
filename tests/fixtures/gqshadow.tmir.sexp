((functions_block ()) (input_vars ()) (prepare_data ())
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id mu) (decl_type (Sized SReal))
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
         (Decl (decl_adtype AutoDiffable) (decl_id x)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable x) ()) UVector
          ((pattern
            (FunApp (StanLib rep_vector FnPlain AoS)
             (((pattern (Var mu))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 3))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (For (loopvar i)
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
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var x))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var i))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id mu) (decl_type (Sized SReal))
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
         (Decl (decl_adtype AutoDiffable) (decl_id x)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable x) ()) UVector
          ((pattern
            (FunApp (StanLib rep_vector FnPlain AoS)
             (((pattern (Var mu))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 3))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (For (loopvar i)
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
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib std_normal_lpdf (FnLpdf true) SoA)
                     (((pattern
                        (Indexed
                         ((pattern (Var x))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var i))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id mu) (decl_type (Sized SReal))
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
         ((pattern (Var mu)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype DataOnly) (decl_id x)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment
      ((LVariable x)
       ((Single
         ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      UVector
      ((pattern
        (FunApp (StanLib Times__ FnPlain AoS)
         (((pattern
            (Promotion
             ((pattern (Lit Int 10))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var mu)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Assignment
      ((LVariable x)
       ((Single
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      UVector
      ((pattern
        (FunApp (StanLib Times__ FnPlain AoS)
         (((pattern
            (Promotion
             ((pattern (Lit Int 20))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var mu)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var x)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (transform_inits
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id mu) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable mu) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str mu))
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
         ((pattern (Var mu)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id mu) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable mu) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var mu)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((mu <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (x <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name gqshadow_model) (prog_path tests/fixtures/gqshadow.stan))