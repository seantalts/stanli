((functions_block ())
 (input_vars
  ((N <opaque> SInt)
   (w <opaque>
    (SVector AoS
     ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
     (Decl (decl_adtype DataOnly) (decl_id N) (decl_type (Sized SInt))
      (initialize Uninit)))
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
      (((pattern (Lit Str w)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id w)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id w_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable w_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str w))
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
           ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (Assignment
                  ((LVariable w)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var w_flat__))
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
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str w2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id w2)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable w2) ()) UVector
      ((pattern
        (FunApp (StanLib Times__ FnPlain AoS)
         (((pattern
            (Promotion
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var w)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id ws) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable ws) ()) UReal
      ((pattern
        (FunApp (StanLib sum FnPlain AoS)
         (((pattern (Var w2)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id v)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id r)
      (decl_type
       (Sized
        (SRowVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id M)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern (Var v))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var r))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var M))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var a))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Times__ FnPlain AoS)
             (((pattern
                (Promotion
                 ((pattern (Lit Int 2))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id v)
      (decl_type
       (Sized
        (SVector SoA
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern SoA)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id r)
      (decl_type
       (Sized
        (SRowVector SoA
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern SoA)))
           ()))
         (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id M)
      (decl_type
       (Sized
        (SMatrix SoA
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern SoA)))
           ()))
         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern SoA)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern (Var v))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var r))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var M))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var a))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Times__ FnPlain SoA)
             (((pattern
                (Promotion
                 ((pattern (Lit Int 2))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id v)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id r)
      (decl_type
       (Sized
        (SRowVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id M)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id a)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var v)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var r))
          (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var M)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var a))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype DataOnly) (decl_id total) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable total) ()) UReal
      ((pattern
        (FunApp (StanLib Plus__ FnPlain AoS)
         (((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern (Var ws))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (FunApp (StanLib sum FnPlain AoS)
                         (((pattern (Var v))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (StanLib sum FnPlain AoS)
                     (((pattern (Var r))
                       (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern (Var M))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (StanLib sum FnPlain AoS)
             (((pattern (Var a))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var total)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
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
     (Decl (decl_adtype AutoDiffable) (decl_id v)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str v))
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
           ((pattern (Lit Int 3))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (Assignment
                  ((LVariable v)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var v_flat__))
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
         ((pattern (Var v)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id r)
      (decl_type
       (Sized
        (SRowVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str r))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
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
                 (Assignment
                  ((LVariable r)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  URowVector
                  ((pattern
                    (Indexed
                     ((pattern (Var r_flat__))
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
         ((pattern (Var r))
          (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id M)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id M_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable M_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str M))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable M)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          UMatrix
                          ((pattern
                            (Indexed
                             ((pattern (Var M_flat__))
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
         ((pattern (Var M)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable a) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str a))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var a))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id v)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable v) ()) UVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var v)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id r)
      (decl_type
       (Sized
        (SRowVector AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable r) ()) URowVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var r))
          (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id M)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable M) ()) UMatrix
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var M)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable a) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var a))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((v <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))
   (r <opaque>
    ((out_unconstrained_st
      (SRowVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SRowVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))
   (M <opaque>
    ((out_unconstrained_st
      (SMatrix AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SMatrix AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))
   (a <opaque>
    ((out_unconstrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))
   (total <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal)
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name tpecont_model) (prog_path tests/fixtures/tpecont.stan))