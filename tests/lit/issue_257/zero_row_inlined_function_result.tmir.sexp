((functions_block
  (((fdrt (ReturnType UMatrix)) (fdname pick) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable y UMatrix) (AutoDiffable k UInt) (AutoDiffable ref (UArray UInt))
      (AutoDiffable value UInt)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id n) (decl_type (Sized SInt))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable n) ()) UInt
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
           (meta <opaque>))
          ((pattern
            (For (loopvar ii)
             (lower
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (upper
              ((pattern
                (FunApp (StanLib size FnPlain AoS)
                 (((pattern (Var ref))
                   (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (body
              ((pattern
                (Block
                 (((pattern
                    (IfElse
                     ((pattern
                       (FunApp (StanLib Equals__ FnPlain AoS)
                        (((pattern
                           (Indexed
                            ((pattern (Var ref))
                             (meta
                              ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                            ((Single
                              ((pattern (Var ii))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Var value))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern
                       (Block
                        (((pattern
                           (Assignment ((LVariable n) ()) UInt
                            ((pattern
                              (FunApp (StanLib Plus__ FnPlain AoS)
                               (((pattern (Var n))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern (Lit Int 1))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                          (meta <opaque>)))))
                      (meta <opaque>))
                     ()))
                   (meta <opaque>)))))
               (meta <opaque>)))))
           (meta <opaque>))
          ((pattern
            (NRFunApp (CompilerInternal FnValidateSize)
             (((pattern (Lit Str res))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Str n))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var n)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta <opaque>))
          ((pattern
            (NRFunApp (CompilerInternal FnValidateSize)
             (((pattern (Lit Str res))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Str k))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var k)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id res)
             (decl_type
              (Sized
               (SMatrix AoS
                ((pattern (Var n))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Var k))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id jj) (decl_type (Sized SInt))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable jj) ()) UInt
             ((pattern (Lit Int 1))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
           (meta <opaque>))
          ((pattern
            (For (loopvar ii)
             (lower
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (upper
              ((pattern
                (FunApp (StanLib size FnPlain AoS)
                 (((pattern (Var ref))
                   (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             (body
              ((pattern
                (Block
                 (((pattern
                    (IfElse
                     ((pattern
                       (FunApp (StanLib Equals__ FnPlain AoS)
                        (((pattern
                           (Indexed
                            ((pattern (Var ref))
                             (meta
                              ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                            ((Single
                              ((pattern (Var ii))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Var value))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern
                       (Block
                        (((pattern
                           (For (loopvar kk)
                            (lower
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (upper
                             ((pattern (Var k))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (body
                             ((pattern
                               (Block
                                (((pattern
                                   (Assignment
                                    ((LVariable res)
                                     ((Single
                                       ((pattern (Var jj))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Var kk))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                    UMatrix
                                    ((pattern
                                      (Indexed
                                       ((pattern (Var y))
                                        (meta
                                         ((type_ UMatrix) (loc <opaque>)
                                          (adlevel AutoDiffable))))
                                       ((Single
                                         ((pattern (Var ii))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))
                                        (Single
                                         ((pattern (Var kk))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))))))
                                     (meta
                                      ((type_ UReal) (loc <opaque>)
                                       (adlevel AutoDiffable))))))
                                  (meta <opaque>)))))
                              (meta <opaque>)))))
                          (meta <opaque>))
                         ((pattern
                           (Assignment ((LVariable jj) ()) UInt
                            ((pattern
                              (FunApp (StanLib Plus__ FnPlain AoS)
                               (((pattern (Var jj))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern (Lit Int 1))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                          (meta <opaque>)))))
                      (meta <opaque>))
                     ()))
                   (meta <opaque>)))))
               (meta <opaque>)))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var res))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((K <opaque> SInt)
   (g <opaque>
    (SArray SInt
     ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
     (Decl (decl_adtype DataOnly) (decl_id g)
      (decl_type
       (Sized
        (SArray SInt
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable g) ()) (UArray UInt)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str g))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str X)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id X)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
             (((pattern
                (FunApp (StanLib to_vector FnPlain AoS)
                 (((pattern (Var X))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_return_sym8__)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Block
          (((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_n_sym9__)
              (decl_type (Sized SInt)) (initialize Uninit)))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_pick_n_sym9__) ()) UInt
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
            (meta <opaque>))
           ((pattern
             (For (loopvar inline_pick_ii_sym10__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern
                 (FunApp (StanLib size FnPlain AoS)
                  (((pattern (Var g))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (IfElse
                      ((pattern
                        (FunApp (StanLib Equals__ FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var g))
                              (meta
                               ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                             ((Single
                               ((pattern (Var inline_pick_ii_sym10__))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Block
                         (((pattern
                            (Assignment ((LVariable inline_pick_n_sym9__) ()) UInt
                             ((pattern
                               (FunApp (StanLib Plus__ FnPlain AoS)
                                (((pattern (Var inline_pick_n_sym9__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                           (meta <opaque>)))))
                       (meta <opaque>))
                      ()))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>))
           ((pattern
             (NRFunApp (CompilerInternal FnValidateSize)
              (((pattern (Lit Str res))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Str n))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var inline_pick_n_sym9__))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
            (meta <opaque>))
           ((pattern
             (NRFunApp (CompilerInternal FnValidateSize)
              (((pattern (Lit Str res))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Str k))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
            (meta <opaque>))
           ((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_res_sym11__)
              (decl_type
               (Sized
                (SMatrix AoS
                 ((pattern (Var inline_pick_n_sym9__))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 ((pattern (Var K))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
              (initialize Default)))
            (meta <opaque>))
           ((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_jj_sym12__)
              (decl_type (Sized SInt)) (initialize Uninit)))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_pick_jj_sym12__) ()) UInt
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
            (meta <opaque>))
           ((pattern
             (For (loopvar inline_pick_ii_sym10__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern
                 (FunApp (StanLib size FnPlain AoS)
                  (((pattern (Var g))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (IfElse
                      ((pattern
                        (FunApp (StanLib Equals__ FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var g))
                              (meta
                               ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                             ((Single
                               ((pattern (Var inline_pick_ii_sym10__))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Block
                         (((pattern
                            (For (loopvar inline_pick_kk_sym13__)
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
                                     ((LVariable inline_pick_res_sym11__)
                                      ((Single
                                        ((pattern (Var inline_pick_jj_sym12__))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))
                                       (Single
                                        ((pattern (Var inline_pick_kk_sym13__))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))))
                                     UMatrix
                                     ((pattern
                                       (Indexed
                                        ((pattern (Var X))
                                         (meta
                                          ((type_ UMatrix) (loc <opaque>)
                                           (adlevel AutoDiffable))))
                                        ((Single
                                          ((pattern (Var inline_pick_ii_sym10__))
                                           (meta
                                            ((type_ UInt) (loc <opaque>)
                                             (adlevel DataOnly)))))
                                         (Single
                                          ((pattern (Var inline_pick_kk_sym13__))
                                           (meta
                                            ((type_ UInt) (loc <opaque>)
                                             (adlevel DataOnly))))))))
                                      (meta
                                       ((type_ UReal) (loc <opaque>)
                                        (adlevel AutoDiffable))))))
                                   (meta <opaque>)))))
                               (meta <opaque>)))))
                           (meta <opaque>))
                          ((pattern
                            (Assignment ((LVariable inline_pick_jj_sym12__) ()) UInt
                             ((pattern
                               (FunApp (StanLib Plus__ FnPlain AoS)
                                (((pattern (Var inline_pick_jj_sym12__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                           (meta <opaque>)))))
                       (meta <opaque>))
                      ()))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_pick_return_sym8__) ()) UMatrix
              ((pattern (Var inline_pick_res_sym11__))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib sum FnPlain AoS)
             (((pattern
                (FunApp (StanLib Times__ FnPlain AoS)
                 (((pattern (Var inline_pick_return_sym8__))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib rep_vector FnPlain AoS)
                     (((pattern (Lit Real 1.0))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Var K))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id X)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
             (((pattern
                (FunApp (StanLib to_vector FnPlain AoS)
                 (((pattern (Var X))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_return_sym1__)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Block
          (((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_n_sym2__)
              (decl_type (Sized SInt)) (initialize Uninit)))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_pick_n_sym2__) ()) UInt
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
            (meta <opaque>))
           ((pattern
             (For (loopvar inline_pick_ii_sym3__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern
                 (FunApp (StanLib size FnPlain SoA)
                  (((pattern (Var g))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (IfElse
                      ((pattern
                        (FunApp (StanLib Equals__ FnPlain SoA)
                         (((pattern
                            (Indexed
                             ((pattern (Var g))
                              (meta
                               ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                             ((Single
                               ((pattern (Var inline_pick_ii_sym3__))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Block
                         (((pattern
                            (Assignment ((LVariable inline_pick_n_sym2__) ()) UInt
                             ((pattern
                               (FunApp (StanLib Plus__ FnPlain SoA)
                                (((pattern (Var inline_pick_n_sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                           (meta <opaque>)))))
                       (meta <opaque>))
                      ()))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>))
           ((pattern
             (NRFunApp (CompilerInternal FnValidateSize)
              (((pattern (Lit Str res))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Str n))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var inline_pick_n_sym2__))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
            (meta <opaque>))
           ((pattern
             (NRFunApp (CompilerInternal FnValidateSize)
              (((pattern (Lit Str res))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Str k))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
            (meta <opaque>))
           ((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_res_sym4__)
              (decl_type
               (Sized
                (SMatrix AoS
                 ((pattern (Var inline_pick_n_sym2__))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 ((pattern (Var K))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
              (initialize Default)))
            (meta <opaque>))
           ((pattern
             (Decl (decl_adtype AutoDiffable) (decl_id inline_pick_jj_sym5__)
              (decl_type (Sized SInt)) (initialize Uninit)))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_pick_jj_sym5__) ()) UInt
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
            (meta <opaque>))
           ((pattern
             (For (loopvar inline_pick_ii_sym3__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern
                 (FunApp (StanLib size FnPlain SoA)
                  (((pattern (Var g))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (IfElse
                      ((pattern
                        (FunApp (StanLib Equals__ FnPlain SoA)
                         (((pattern
                            (Indexed
                             ((pattern (Var g))
                              (meta
                               ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                             ((Single
                               ((pattern (Var inline_pick_ii_sym3__))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Block
                         (((pattern
                            (For (loopvar inline_pick_kk_sym6__)
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
                                     ((LVariable inline_pick_res_sym4__)
                                      ((Single
                                        ((pattern (Var inline_pick_jj_sym5__))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))
                                       (Single
                                        ((pattern (Var inline_pick_kk_sym6__))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))))
                                     UMatrix
                                     ((pattern
                                       (Indexed
                                        ((pattern (Var X))
                                         (meta
                                          ((type_ UMatrix) (loc <opaque>)
                                           (adlevel AutoDiffable))))
                                        ((Single
                                          ((pattern (Var inline_pick_ii_sym3__))
                                           (meta
                                            ((type_ UInt) (loc <opaque>)
                                             (adlevel DataOnly)))))
                                         (Single
                                          ((pattern (Var inline_pick_kk_sym6__))
                                           (meta
                                            ((type_ UInt) (loc <opaque>)
                                             (adlevel DataOnly))))))))
                                      (meta
                                       ((type_ UReal) (loc <opaque>)
                                        (adlevel AutoDiffable))))))
                                   (meta <opaque>)))))
                               (meta <opaque>)))))
                           (meta <opaque>))
                          ((pattern
                            (Assignment ((LVariable inline_pick_jj_sym5__) ()) UInt
                             ((pattern
                               (FunApp (StanLib Plus__ FnPlain SoA)
                                (((pattern (Var inline_pick_jj_sym5__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                           (meta <opaque>)))))
                       (meta <opaque>))
                      ()))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>))
           ((pattern
             (Assignment ((LVariable inline_pick_return_sym1__) ()) UMatrix
              ((pattern (Var inline_pick_res_sym4__))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib sum FnPlain AoS)
             (((pattern
                (FunApp (StanLib Times__ FnPlain AoS)
                 (((pattern (Var inline_pick_return_sym1__))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib rep_vector FnPlain AoS)
                     (((pattern (Lit Real 1.0))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Var K))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id X)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Var K))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var X)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
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
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id X)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id X_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable X_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str X))
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
           ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                          ((LVariable X)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          UMatrix
                          ((pattern
                            (Indexed
                             ((pattern (Var X_flat__))
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
         ((pattern (Var X)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id X)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable X) ()) UMatrix
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var X)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((X <opaque>
    ((out_unconstrained_st
      (SMatrix AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SMatrix AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var K)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))))
 (prog_name zero_row_inlined_function_result_model)
 (prog_path tests/lit/issue_257/zero_row_inlined_function_result.stan))
