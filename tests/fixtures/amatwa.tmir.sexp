((functions_block ())
 (input_vars
  ((d <opaque>
    (SArray
     (SMatrix AoS
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
     (Decl (decl_adtype DataOnly) (decl_id d)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id d_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable d_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str d))
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
                           ((pattern (Lit Int 2))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable d)
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
                                     ((pattern (Var d_flat__))
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
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id m)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var m))
                      (meta
                       ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var m))
                      (meta
                       ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id m)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var m))
                      (meta
                       ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var m))
                      (meta
                       ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id m)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
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
                       ((pattern (Lit Int 2))
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
                                    ((pattern (Var m))
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
     (Decl (decl_adtype DataOnly) (decl_id g)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id gd)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable gd) ()) (UArray UMatrix)
      ((pattern (Var d))
       (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (For (loopvar k)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
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
                     (For (loopvar j)
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
                              ((LVariable g)
                               ((Single
                                 ((pattern (Var k))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var i))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var j))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (UArray UMatrix)
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib Plus__ FnPlain AoS)
                                    (((pattern
                                       (FunApp (StanLib Plus__ FnPlain AoS)
                                        (((pattern
                                           (FunApp (StanLib Times__ FnPlain AoS)
                                            (((pattern (Lit Int 100))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             ((pattern (Var k))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         ((pattern
                                           (FunApp (StanLib Times__ FnPlain AoS)
                                            (((pattern (Lit Int 10))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             ((pattern (Var i))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var j))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
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
                       ((pattern (Lit Int 2))
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
                                    ((pattern (Var g))
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
                       ((pattern (Lit Int 2))
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
                                    ((pattern (Var gd))
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
    (meta <opaque>))))
 (transform_inits
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id pos__) (decl_type (Sized SInt))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable pos__) ()) UInt
      ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id m)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id m_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable m_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str m))
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
                           ((pattern (Lit Int 2))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable m)
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
                                     ((pattern (Var m_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var m))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id m)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
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
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable m)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
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
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var m))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((m <opaque>
    ((out_unconstrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))
   (g <opaque>
    ((out_unconstrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block GeneratedQuantities) (out_trans Identity)))
   (gd <opaque>
    ((out_unconstrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name amatwa_model) (prog_path tests/fixtures/amatwa.stan))