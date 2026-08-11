((functions_block ()) (input_vars ()) (prepare_data ())
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
         (Decl (decl_adtype AutoDiffable) (decl_id A)
          (decl_type
           (Sized
            (SArray
             (SArray SReal
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable A) ()) (UArray (UArray UReal))
          ((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Var q))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern (Lit Int 2))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (Promotion
                     ((pattern (Lit Int 3))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern (Lit Int 4))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (Promotion
                     ((pattern (Lit Int 5))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern (Lit Int 6))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id B)
          (decl_type
           (Sized
            (SArray
             (SArray SReal
              ((pattern (Lit Int 3))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable B) ()) (UArray (UArray UReal))
          ((pattern (Var A))
           (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (Indexed
             ((pattern (Var B))
              (meta
               ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))
             ((Single
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (Single
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
         (Decl (decl_adtype AutoDiffable) (decl_id A)
          (decl_type
           (Sized
            (SArray
             (SArray SReal
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable A) ()) (UArray (UArray UReal))
          ((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Var q))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern (Lit Int 2))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (Promotion
                     ((pattern (Lit Int 3))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern (Lit Int 4))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (Promotion
                     ((pattern (Lit Int 5))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern (Lit Int 6))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal AutoDiffable))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id B)
          (decl_type
           (Sized
            (SArray
             (SArray SReal
              ((pattern (Lit Int 3))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable B) ()) (UArray (UArray UReal))
          ((pattern (Var A))
           (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (Indexed
             ((pattern (Var B))
              (meta
               ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))
             ((Single
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (Single
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
 (prog_name viewc_array_bind_mismatch_model)
 (prog_path tests/fixtures/viewc_array_bind_mismatch.stan))