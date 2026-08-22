((functions_block ())
 (input_vars
  ((M <opaque> SInt)
   (idx <opaque>
    (SArray SInt
     ((pattern (Var M)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (Y <opaque>
    (SVector AoS
     ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
     (Decl (decl_adtype DataOnly) (decl_id M) (decl_type (Sized SInt))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable M) ()) UInt
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str M))
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
        (var_name M)
        (var ((pattern (Var M)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str idx)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str M)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var M)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id idx)
      (decl_type
       (Sized
        (SArray SInt
         ((pattern (Var M)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable idx) ()) (UArray UInt)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str idx))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id Y)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id Y_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable Y_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str Y))
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
                  ((LVariable Y)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var Y_flat__))
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
    (meta <opaque>))))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf false) AoS)
             (((pattern
                (Indexed
                 ((pattern (Var Y))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))
                 ((MultiIndex
                   ((pattern (Var idx))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var mu))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 1))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf false) AoS)
             (((pattern (Var mu))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 2))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf false) SoA)
             (((pattern
                (Indexed
                 ((pattern (Var Y))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))
                 ((MultiIndex
                   ((pattern (Var idx))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var mu))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 1))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf false) SoA)
             (((pattern (Var mu))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 2))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
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
     (Decl (decl_adtype DataOnly) (decl_id gsum) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable gsum) ()) UReal
      ((pattern
        (FunApp (StanLib sum FnPlain AoS)
         (((pattern
            (Indexed
             ((pattern (Var Y))
              (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))
             ((MultiIndex
               ((pattern (Var idx))
                (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var gsum)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
   (gsum <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal)
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name emptyidx_model) (prog_path tests/fixtures/emptyidx.stan))