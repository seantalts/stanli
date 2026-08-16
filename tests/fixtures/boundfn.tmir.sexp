((functions_block
  (((fdrt (ReturnType UReal)) (fdname bounds_jacobian) (fdsuffix FnJacobian)
    (fdargs
     ((AutoDiffable a UVector) (AutoDiffable lo UVector) (AutoDiffable hi UVector)
      (AutoDiffable s UReal) (AutoDiffable m UMatrix) (AutoDiffable na (UArray UVector))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id c) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern
                   (Promotion
                    ((pattern (Lit Int 0))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                    UReal AutoDiffable))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                        (((pattern (Var a))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var s))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib lower_bound_jacobian FnJacobian AoS)
                        (((pattern (Var a))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var lo))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib lower_bound_unconstrain FnPlain AoS)
                        (((pattern
                           (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                            (((pattern (Var a))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                             ((pattern (Var s))
                              (meta
                               ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var s))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id uc)
             (decl_type
              (Sized
               (SMatrix AoS
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable uc) ()) UMatrix
             ((pattern
               (FunApp (StanLib upper_bound_constrain FnPlain AoS)
                (((pattern (Var m))
                  (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern (Var s))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern (Var uc))
                      (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib upper_bound_jacobian FnJacobian AoS)
                        (((pattern (Var a))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var hi))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib upper_bound_unconstrain FnPlain AoS)
                        (((pattern
                           (FunApp (StanLib upper_bound_constrain FnPlain AoS)
                            (((pattern (Var a))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                             ((pattern (Var s))
                              (meta
                               ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var s))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                        (((pattern (Var a))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var lo))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var hi))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib lower_upper_bound_jacobian FnJacobian AoS)
                        (((pattern (Var a))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Lit Real -3.))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Lit Real 3.0))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib lower_upper_bound_unconstrain FnPlain AoS)
                        (((pattern
                           (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                            (((pattern (Var a))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                             ((pattern (Var lo))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                             ((pattern (Var hi))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var lo))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var hi))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id oc)
             (decl_type
              (Sized
               (SArray
                (SVector AoS
                 ((pattern (Lit Int 2))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable oc) ()) (UArray UVector)
             ((pattern
               (FunApp (StanLib offset_multiplier_constrain FnPlain AoS)
                (((pattern (Var na))
                  (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern (Var s))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern (Lit Real 2.5))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
              (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (Indexed
                        ((pattern (Var oc))
                         (meta
                          ((type_ (UArray UVector)) (loc <opaque>)
                           (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (Indexed
                        ((pattern (Var oc))
                         (meta
                          ((type_ (UArray UVector)) (loc <opaque>)
                           (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib offset_multiplier_jacobian FnJacobian AoS)
                        (((pattern (Var a))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var lo))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var hi))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable c) ()) UReal
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern (Var c))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib sum FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib offset_multiplier_unconstrain FnPlain AoS)
                        (((pattern (Var a))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var lo))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var hi))
                          (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var c))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((md <opaque>
    (SMatrix AoS
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
     (Decl (decl_adtype DataOnly) (decl_id md)
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
         (Decl (decl_adtype AutoDiffable) (decl_id md_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable md_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str md))
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
                          ((LVariable md)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          UMatrix
                          ((pattern
                            (Indexed
                             ((pattern (Var md_flat__))
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
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id s) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id total) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_return_sym11__)
      (decl_type (Sized SReal)) (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_c_sym12__)
          (decl_type (Sized SReal)) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                 UReal AutoDiffable))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern (Var s))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_uc_sym13__)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_uc_sym13__) ()) UMatrix
          ((pattern
            (FunApp (StanLib upper_bound_constrain FnPlain AoS)
             (((pattern (Var md))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern (Var inline_bounds_jacobian_uc_sym13__))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib upper_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib upper_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib upper_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern (Var s))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Lit Real -3.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 3.0))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Minus__ FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 4))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib exp FnPlain AoS)
                                 (((pattern (Var a))
                                   (meta
                                    ((type_ UVector) (loc <opaque>)
                                     (adlevel AutoDiffable)))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_oc_sym14__)
          (decl_type
           (Sized
            (SArray
             (SVector AoS
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_oc_sym14__) ()) 
          (UArray UVector)
          ((pattern
            (FunApp (StanLib offset_multiplier_constrain FnPlain AoS)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Var a))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var a))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Real 2.5))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var inline_bounds_jacobian_oc_sym14__))
                      (meta
                       ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var inline_bounds_jacobian_oc_sym14__))
                      (meta
                       ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib offset_multiplier_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym12__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym12__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib offset_multiplier_unconstrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_return_sym11__) ()) UReal
          ((pattern (Var inline_bounds_jacobian_c_sym12__))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable total) ()) UReal
      ((pattern (Var inline_bounds_jacobian_return_sym11__))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern (Var total))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id s) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id total) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_return_sym6__)
      (decl_type (Sized SReal)) (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_c_sym7__)
          (decl_type (Sized SReal)) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                 UReal AutoDiffable))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern (Var s))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_uc_sym8__)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_uc_sym8__) ()) UMatrix
          ((pattern
            (FunApp (StanLib upper_bound_constrain FnPlain AoS)
             (((pattern (Var md))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern (Var inline_bounds_jacobian_uc_sym8__))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib upper_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib upper_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib upper_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern (Var s))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Lit Real -3.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 3.0))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Minus__ FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 4))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib exp FnPlain AoS)
                                 (((pattern (Var a))
                                   (meta
                                    ((type_ UVector) (loc <opaque>)
                                     (adlevel AutoDiffable)))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_oc_sym9__)
          (decl_type
           (Sized
            (SArray
             (SVector AoS
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_oc_sym9__) ()) 
          (UArray UVector)
          ((pattern
            (FunApp (StanLib offset_multiplier_constrain FnPlain AoS)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Var a))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var a))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Real 2.5))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var inline_bounds_jacobian_oc_sym9__))
                      (meta
                       ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var inline_bounds_jacobian_oc_sym9__))
                      (meta
                       ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib offset_multiplier_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym7__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain SoA)
             (((pattern (Var inline_bounds_jacobian_c_sym7__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib offset_multiplier_unconstrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_return_sym6__) ()) UReal
          ((pattern (Var inline_bounds_jacobian_c_sym7__))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable total) ()) UReal
      ((pattern (Var inline_bounds_jacobian_return_sym6__))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern (Var total))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id a)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id s) (decl_type (Sized SReal))
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
     (Decl (decl_adtype DataOnly) (decl_id total) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var a)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var s)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype DataOnly) (decl_id inline_bounds_jacobian_return_sym1__)
      (decl_type (Sized SReal)) (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_c_sym2__)
          (decl_type (Sized SReal)) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (Promotion
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
                 UReal AutoDiffable))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib lower_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern (Var s))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_uc_sym3__)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_uc_sym3__) ()) UMatrix
          ((pattern
            (FunApp (StanLib upper_bound_constrain FnPlain AoS)
             (((pattern (Var md))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern (Var inline_bounds_jacobian_uc_sym3__))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib upper_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib upper_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib upper_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern (Var s))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var s))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Lit Real -3.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 3.0))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib lower_upper_bound_unconstrain FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib lower_upper_bound_constrain FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Minus__ FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 4))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib exp FnPlain AoS)
                                 (((pattern (Var a))
                                   (meta
                                    ((type_ UVector) (loc <opaque>)
                                     (adlevel AutoDiffable)))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id inline_bounds_jacobian_oc_sym4__)
          (decl_type
           (Sized
            (SArray
             (SVector AoS
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_oc_sym4__) ()) 
          (UArray UVector)
          ((pattern
            (FunApp (StanLib offset_multiplier_constrain FnPlain AoS)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Var a))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var a))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Real 2.5))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var inline_bounds_jacobian_oc_sym4__))
                      (meta
                       ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var inline_bounds_jacobian_oc_sym4__))
                      (meta
                       ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib offset_multiplier_jacobian FnJacobian AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_c_sym2__) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var inline_bounds_jacobian_c_sym2__))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib offset_multiplier_unconstrain FnPlain AoS)
                     (((pattern (Var a))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Minus__ FnPlain AoS)
                         (((pattern (Var a))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib exp FnPlain AoS)
                             (((pattern (Var a))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable inline_bounds_jacobian_return_sym1__) ()) UReal
          ((pattern (Var inline_bounds_jacobian_c_sym2__))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable total) ()) UReal
      ((pattern (Var inline_bounds_jacobian_return_sym1__))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (IfElse
      ((pattern (Var emit_transformed_parameters__))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern
        (Block
         (((pattern
            (NRFunApp
             (CompilerInternal
              (FnWriteParam (unconstrain_opt ())
               (var
                ((pattern (Var total))
                 (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
             ()))
           (meta <opaque>)))))
       (meta <opaque>))
      ()))
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
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable pos__) ()) UInt
      ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id a_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable a_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str a))
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
           ((pattern (Lit Int 2))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (Assignment
                  ((LVariable a)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var a_flat__))
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
         ((pattern (Var a)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id s) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable s) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str s))
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
         ((pattern (Var s)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable a) ()) UVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var a)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id s) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable s) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var s)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((a <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))
   (s <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (total <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal)
     (out_block TransformedParameters) (out_trans Identity)))))
 (prog_name boundfn_model) (prog_path tests/fixtures/boundfn.stan))