((functions_block
  (((fdrt (ReturnType UReal)) (fdname structured_value) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable theta UReal) (AutoDiffable x UMatrix)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id done) (decl_type (Sized SInt))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable done) ()) UInt
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id out) (decl_type (Sized SReal))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable out) ()) UReal
             ((pattern
               (Promotion
                ((pattern (Lit Int 0))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                UReal AutoDiffable))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (While
             ((pattern
               (FunApp (StanLib Greater__ FnPlain AoS)
                (((pattern (Var theta))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern (Var done))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             ((pattern
               (Block
                (((pattern
                   (Decl (decl_adtype AutoDiffable) (decl_id a)
                    (decl_type
                     (Sized
                      (SMatrix AoS
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (initialize Default)))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable a) ()) UMatrix
                    ((pattern
                      (FunApp (StanLib add_diag FnPlain AoS)
                       (((pattern
                          (FunApp (StanLib crossprod FnPlain AoS)
                           (((pattern (Var x))
                             (meta
                              ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                        ((pattern (Lit Real 2.0))
                         (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Decl (decl_adtype AutoDiffable) (decl_id row_diag)
                    (decl_type
                     (Sized
                      (SMatrix AoS
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (initialize Default)))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable row_diag) ()) UMatrix
                    ((pattern
                      (FunApp (StanLib add_diag FnPlain AoS)
                       (((pattern
                          (FunApp (StanLib crossprod FnPlain AoS)
                           (((pattern (Var x))
                             (meta
                              ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                        ((pattern
                          (FunApp (CompilerInternal FnMakeRowVec)
                           (((pattern (Lit Real 2.0))
                             (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Real 3.0))
                             (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Decl (decl_adtype AutoDiffable) (decl_id e)
                    (decl_type
                     (Sized
                      (SMatrix AoS
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (initialize Default)))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable e) ()) UMatrix
                    ((pattern
                      (FunApp (StanLib matrix_exp FnPlain AoS)
                       (((pattern
                          (FunApp (StanLib PMinus__ FnPlain AoS)
                           (((pattern (Var a))
                             (meta
                              ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                     (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Decl (decl_adtype AutoDiffable) (decl_id solved)
                    (decl_type
                     (Sized
                      (SMatrix AoS
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (initialize Default)))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable solved) ()) UMatrix
                    ((pattern
                      (FunApp (StanLib LDivide__ FnPlain AoS)
                       (((pattern (Var a))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                        ((pattern (Var x))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                     (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Decl (decl_adtype AutoDiffable) (decl_id right_spd)
                    (decl_type
                     (Sized
                      (SMatrix AoS
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (initialize Default)))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable right_spd) ()) UMatrix
                    ((pattern
                      (FunApp (StanLib mdivide_right_spd FnPlain AoS)
                       (((pattern (Var x))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                        ((pattern (Var a))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                     (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Decl (decl_adtype AutoDiffable) (decl_id q)
                    (decl_type
                     (Sized
                      (SMatrix AoS
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (initialize Default)))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable q) ()) UMatrix
                    ((pattern
                      (FunApp (StanLib quad_form_sym FnPlain AoS)
                       (((pattern (Var a))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                        ((pattern (Var x))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                     (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Decl (decl_adtype AutoDiffable) (decl_id tc)
                    (decl_type
                     (Sized
                      (SMatrix AoS
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (initialize Default)))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable tc) ()) UMatrix
                    ((pattern
                      (FunApp (StanLib tcrossprod FnPlain AoS)
                       (((pattern (Var x))
                         (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                     (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable out) ()) UReal
                    ((pattern
                      (FunApp (StanLib Plus__ FnPlain AoS)
                       (((pattern (Var out))
                         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                        ((pattern
                          (FunApp (StanLib Plus__ FnPlain AoS)
                           (((pattern
                              (FunApp (StanLib Minus__ FnPlain AoS)
                               (((pattern
                                  (FunApp (StanLib Plus__ FnPlain AoS)
                                   (((pattern
                                      (FunApp (StanLib Plus__ FnPlain AoS)
                                       (((pattern
                                          (FunApp (StanLib Plus__ FnPlain AoS)
                                           (((pattern
                                              (FunApp (StanLib Minus__ FnPlain AoS)
                                               (((pattern
                                                  (Indexed
                                                   ((pattern (Var e))
                                                    (meta
                                                     ((type_ UMatrix)
                                                      (loc <opaque>)
                                                      (adlevel AutoDiffable))))
                                                   ((Single
                                                     ((pattern (Lit Int 1))
                                                      (meta
                                                       ((type_ UInt)
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))
                                                    (Single
                                                     ((pattern (Lit Int 1))
                                                      (meta
                                                       ((type_ UInt)
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))))))
                                                 (meta
                                                  ((type_ UReal) (loc <opaque>)
                                                   (adlevel AutoDiffable))))
                                                ((pattern
                                                  (FunApp (StanLib Times__ FnPlain AoS)
                                                   (((pattern (Lit Real 0.7))
                                                     (meta
                                                      ((type_ UReal)
                                                       (loc <opaque>)
                                                       (adlevel DataOnly))))
                                                    ((pattern
                                                      (Indexed
                                                       ((pattern (Var e))
                                                        (meta
                                                         ((type_ UMatrix)
                                                          (loc <opaque>)
                                                          (adlevel AutoDiffable))))
                                                       ((Single
                                                         ((pattern (Lit Int 2))
                                                          (meta
                                                           ((type_ UInt)
                                                            (loc <opaque>)
                                                            (adlevel DataOnly)))))
                                                        (Single
                                                         ((pattern (Lit Int 1))
                                                          (meta
                                                           ((type_ UInt)
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))))))
                                                     (meta
                                                      ((type_ UReal)
                                                       (loc <opaque>)
                                                       (adlevel AutoDiffable)))))))
                                                 (meta
                                                  ((type_ UReal) (loc <opaque>)
                                                   (adlevel AutoDiffable)))))))
                                             (meta
                                              ((type_ UReal) (loc <opaque>)
                                               (adlevel AutoDiffable))))
                                            ((pattern
                                              (FunApp (StanLib Times__ FnPlain AoS)
                                               (((pattern (Lit Real 1.3))
                                                 (meta
                                                  ((type_ UReal) (loc <opaque>)
                                                   (adlevel DataOnly))))
                                                ((pattern
                                                  (Indexed
                                                   ((pattern (Var solved))
                                                    (meta
                                                     ((type_ UMatrix)
                                                      (loc <opaque>)
                                                      (adlevel AutoDiffable))))
                                                   ((Single
                                                     ((pattern (Lit Int 1))
                                                      (meta
                                                       ((type_ UInt)
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))
                                                    (Single
                                                     ((pattern (Lit Int 2))
                                                      (meta
                                                       ((type_ UInt)
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))))))
                                                 (meta
                                                  ((type_ UReal) (loc <opaque>)
                                                   (adlevel AutoDiffable)))))))
                                             (meta
                                              ((type_ UReal) (loc <opaque>)
                                               (adlevel AutoDiffable)))))))
                                         (meta
                                          ((type_ UReal) (loc <opaque>)
                                           (adlevel AutoDiffable))))
                                        ((pattern
                                          (FunApp (StanLib Times__ FnPlain AoS)
                                           (((pattern (Lit Real 0.6))
                                             (meta
                                              ((type_ UReal) (loc <opaque>)
                                               (adlevel DataOnly))))
                                            ((pattern
                                              (Indexed
                                               ((pattern (Var right_spd))
                                                (meta
                                                 ((type_ UMatrix) (loc <opaque>)
                                                  (adlevel AutoDiffable))))
                                               ((Single
                                                 ((pattern (Lit Int 2))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))
                                                (Single
                                                 ((pattern (Lit Int 1))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))))))
                                             (meta
                                              ((type_ UReal) (loc <opaque>)
                                               (adlevel AutoDiffable)))))))
                                         (meta
                                          ((type_ UReal) (loc <opaque>)
                                           (adlevel AutoDiffable)))))))
                                     (meta
                                      ((type_ UReal) (loc <opaque>)
                                       (adlevel AutoDiffable))))
                                    ((pattern
                                      (FunApp (StanLib Times__ FnPlain AoS)
                                       (((pattern (Lit Real 0.4))
                                         (meta
                                          ((type_ UReal) (loc <opaque>)
                                           (adlevel DataOnly))))
                                        ((pattern
                                          (Indexed
                                           ((pattern (Var q))
                                            (meta
                                             ((type_ UMatrix) (loc <opaque>)
                                              (adlevel AutoDiffable))))
                                           ((Single
                                             ((pattern (Lit Int 2))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))
                                            (Single
                                             ((pattern (Lit Int 2))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))))))
                                         (meta
                                          ((type_ UReal) (loc <opaque>)
                                           (adlevel AutoDiffable)))))))
                                     (meta
                                      ((type_ UReal) (loc <opaque>)
                                       (adlevel AutoDiffable)))))))
                                 (meta
                                  ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                                ((pattern
                                  (FunApp (StanLib Times__ FnPlain AoS)
                                   (((pattern (Lit Real 0.2))
                                     (meta
                                      ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                    ((pattern
                                      (Indexed
                                       ((pattern (Var tc))
                                        (meta
                                         ((type_ UMatrix) (loc <opaque>)
                                          (adlevel AutoDiffable))))
                                       ((Single
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))
                                        (Single
                                         ((pattern (Lit Int 2))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))))))
                                     (meta
                                      ((type_ UReal) (loc <opaque>)
                                       (adlevel AutoDiffable)))))))
                                 (meta
                                  ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                             (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                            ((pattern
                              (FunApp (StanLib Times__ FnPlain AoS)
                               (((pattern (Lit Real 0.05))
                                 (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern
                                  (Indexed
                                   ((pattern (Var row_diag))
                                    (meta
                                     ((type_ UMatrix) (loc <opaque>)
                                      (adlevel AutoDiffable))))
                                   ((Single
                                     ((pattern (Lit Int 2))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                    (Single
                                     ((pattern (Lit Int 2))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                 (meta
                                  ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                             (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                     (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                  (meta <opaque>))
                 ((pattern
                   (Assignment ((LVariable done) ()) UInt
                    ((pattern
                      (FunApp (StanLib Plus__ FnPlain AoS)
                       (((pattern (Var done))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        ((pattern (Lit Int 1))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                  (meta <opaque>)))))
              (meta <opaque>))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var out))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars ()) (prepare_data ())
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id x)
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
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (UserDefined structured_value FnPlain)
                 (((pattern (Var theta))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var x))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id x)
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
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (UserDefined structured_value FnPlain)
                 (((pattern (Var theta))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var x))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id theta) (decl_type (Sized SReal))
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
     (Decl (decl_adtype DataOnly) (decl_id x)
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var theta)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var x)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype DataOnly) (decl_id score) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable score) ()) UReal
      ((pattern
        (FunApp (UserDefined structured_value FnPlain)
         (((pattern (Var theta))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var x)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var score)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
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
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable theta) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str theta))
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
         ((pattern (Var theta)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id x)
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
         (Decl (decl_adtype AutoDiffable) (decl_id x_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable x_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str x))
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
                          ((LVariable x)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          UMatrix
                          ((pattern
                            (Indexed
                             ((pattern (Var x_flat__))
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
         ((pattern (Var x)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable theta) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var theta)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id x)
      (decl_type
       (Sized
        (SMatrix AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable x) ()) UMatrix
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
         ((pattern (Var x)) (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((theta <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (x <opaque>
    ((out_unconstrained_st
      (SMatrix AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SMatrix AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))
   (score <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal)
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name structured_matrix_ops_model)
 (prog_path tests/fixtures/structured_matrix_ops.stan))
