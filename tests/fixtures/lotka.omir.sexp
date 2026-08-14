((functions_block
  (((fdrt (ReturnType (UArray UReal))) (fdname dz_dt) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable z (UArray UReal))
      (AutoDiffable theta (UArray UReal)) (AutoDiffable x_r (UArray UReal))
      (AutoDiffable x_i (UArray UInt))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id u) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable u) ()) UReal
             ((pattern
               (Indexed
                ((pattern (Var z))
                 (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                ((Single
                  ((pattern (Lit Int 1))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id v) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable v) ()) UReal
             ((pattern
               (Indexed
                ((pattern (Var z))
                 (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                ((Single
                  ((pattern (Lit Int 2))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id alpha) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable alpha) ()) UReal
             ((pattern
               (Indexed
                ((pattern (Var theta))
                 (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                ((Single
                  ((pattern (Lit Int 1))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id beta) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable beta) ()) UReal
             ((pattern
               (Indexed
                ((pattern (Var theta))
                 (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                ((Single
                  ((pattern (Lit Int 2))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id gamma) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable gamma) ()) UReal
             ((pattern
               (Indexed
                ((pattern (Var theta))
                 (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                ((Single
                  ((pattern (Lit Int 3))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id delta) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable delta) ()) UReal
             ((pattern
               (Indexed
                ((pattern (Var theta))
                 (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                ((Single
                  ((pattern (Lit Int 4))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id du_dt) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable du_dt) ()) UReal
             ((pattern
               (FunApp (StanLib Times__ FnPlain AoS)
                (((pattern
                   (FunApp (StanLib Minus__ FnPlain AoS)
                    (((pattern (Var alpha))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern (Var beta))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern (Var v))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern (Var u))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id dv_dt) (decl_type (Sized SReal))
             (initialize Uninit)))
           (meta <opaque>))
          ((pattern
            (Assignment ((LVariable dv_dt) ()) UReal
             ((pattern
               (FunApp (StanLib Times__ FnPlain AoS)
                (((pattern
                   (FunApp (StanLib fma FnPlain AoS)
                    (((pattern (Var delta))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern (Var u))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (FunApp (StanLib PMinus__ FnPlain AoS)
                        (((pattern (Var gamma))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern (Var v))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Var du_dt))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var dv_dt))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((N <opaque> SInt)
   (ts <opaque>
    (SArray SReal
     ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (y_init <opaque>
    (SArray SReal
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (y <opaque>
    (SArray
     (SArray SReal
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
      (((pattern (Lit Str ts)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id ts)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable ts) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id y_init)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable y_init) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str y_init))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str y)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id y)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id y_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable y_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str y))
               (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly)))))))
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
                   ((pattern (Var N))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable y)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray (UArray UReal))
                          ((pattern
                            (Indexed
                             ((pattern (Var y_flat__))
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
       (FnCheck
        (trans
         (Lower
          ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (var_name y)
        (var
         ((pattern (Var y))
          (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly)))))))
      (((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str z)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str y_rep))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 4))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_init)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id sigma)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z) ()) (UArray (UArray UReal))
      ((pattern
        (FunApp (StanLib integrate_ode_rk45 FnPlain AoS)
         (((pattern (Var dz_dt))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable (UArray UReal))
                 (AutoDiffable (UArray UReal)) (AutoDiffable (UArray UReal))
                 (AutoDiffable (UArray UInt)))
                (ReturnType (UArray UReal)) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var z_init))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var theta))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (FunApp (StanLib rep_array FnPlain AoS)
             (((pattern (Lit Real 0.0))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (StanLib rep_array FnPlain AoS)
             (((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-5))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-3))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 5e2))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf true) AoS)
             (((pattern
                (Indexed
                 ((pattern (Var theta))
                  (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                 ((MultiIndex
                   ((pattern
                     (FunApp (CompilerInternal FnMakeArray)
                      (((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 3))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 1))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Real 0.5))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf true) AoS)
             (((pattern
                (Indexed
                 ((pattern (Var theta))
                  (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                 ((MultiIndex
                   ((pattern
                     (FunApp (CompilerInternal FnMakeArray)
                      (((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 4))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Real 0.05))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Real 0.05))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var sigma))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int -1))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var z_init))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib log FnPlain AoS)
                 (((pattern (Lit Int 10))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
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
         (For (loopvar k)
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
                    (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var y_init))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                         ((Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (FunApp (StanLib log FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var z_init))
                              (meta
                               ((type_ (UArray UReal)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Var k))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var sigma))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>))
               ((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var y))
                          (meta
                           ((type_ (UArray (UArray UReal))) (loc <opaque>)
                            (adlevel DataOnly))))
                         (All
                          (Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (FunApp (StanLib log FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var z))
                              (meta
                               ((type_ (UArray (UArray UReal))) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             (All
                              (Single
                               ((pattern (Var k))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta
                            ((type_ (UArray UReal)) (loc <opaque>)
                             (adlevel AutoDiffable)))))))
                       (meta
                        ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var sigma))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 4))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern SoA)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_init)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern SoA)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id sigma)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern SoA)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z) ()) (UArray (UArray UReal))
      ((pattern
        (FunApp (StanLib integrate_ode_rk45 FnPlain AoS)
         (((pattern (Var dz_dt))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable (UArray UReal))
                 (AutoDiffable (UArray UReal)) (AutoDiffable (UArray UReal))
                 (AutoDiffable (UArray UInt)))
                (ReturnType (UArray UReal)) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var z_init))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var theta))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (FunApp (StanLib rep_array FnPlain AoS)
             (((pattern (Lit Real 0.0))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (StanLib rep_array FnPlain AoS)
             (((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-5))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-3))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 5e2))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf true) SoA)
             (((pattern
                (Indexed
                 ((pattern (Var theta))
                  (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                 ((MultiIndex
                   ((pattern
                     (FunApp (CompilerInternal FnMakeArray)
                      (((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 3))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int 1))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                 UReal DataOnly))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Real 0.5))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib normal_lpdf (FnLpdf true) SoA)
             (((pattern
                (Indexed
                 ((pattern (Var theta))
                  (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                 ((MultiIndex
                   ((pattern
                     (FunApp (CompilerInternal FnMakeArray)
                      (((pattern (Lit Int 2))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                       ((pattern (Lit Int 4))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                    (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Real 0.05))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Real 0.05))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) SoA)
             (((pattern (Var sigma))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Promotion
                 ((pattern (Lit Int -1))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) SoA)
             (((pattern (Var z_init))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib log FnPlain SoA)
                 (((pattern (Lit Int 10))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
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
         (For (loopvar k)
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
                    (FunApp (StanLib lognormal_lpdf (FnLpdf true) SoA)
                     (((pattern
                        (Indexed
                         ((pattern (Var y_init))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                         ((Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (FunApp (StanLib log FnPlain SoA)
                         (((pattern
                            (Indexed
                             ((pattern (Var z_init))
                              (meta
                               ((type_ (UArray UReal)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Var k))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var sigma))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>))
               ((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib lognormal_lpdf (FnLpdf true) SoA)
                     (((pattern
                        (Indexed
                         ((pattern (Var y))
                          (meta
                           ((type_ (UArray (UArray UReal))) (loc <opaque>)
                            (adlevel DataOnly))))
                         (All
                          (Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (FunApp (StanLib log FnPlain SoA)
                         (((pattern
                            (Indexed
                             ((pattern (Var z))
                              (meta
                               ((type_ (UArray (UArray UReal))) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             (All
                              (Single
                               ((pattern (Var k))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta
                            ((type_ (UArray UReal)) (loc <opaque>)
                             (adlevel AutoDiffable)))))))
                       (meta
                        ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Indexed
                         ((pattern (Var sigma))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
                         ((Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id theta)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 4))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z_init)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id sigma)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam
             (constrain
              (Lower
               ((pattern (Lit Int 0))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (dims
              (((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var theta))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var z_init))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var sigma))
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
     (Assignment ((LVariable z) ()) (UArray (UArray UReal))
      ((pattern
        (FunApp (StanLib integrate_ode_rk45 FnPlain AoS)
         (((pattern (Var dz_dt))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable (UArray UReal))
                 (AutoDiffable (UArray UReal)) (AutoDiffable (UArray UReal))
                 (AutoDiffable (UArray UInt)))
                (ReturnType (UArray UReal)) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var z_init))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             UReal DataOnly))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var theta))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (FunApp (StanLib rep_array FnPlain AoS)
             (((pattern (Lit Real 0.0))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (StanLib rep_array FnPlain AoS)
             (((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-5))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-3))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 5e2))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (IfElse
      ((pattern (Var emit_transformed_parameters__))
       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern
        (Block
         (((pattern
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
                      ((pattern (Var N))
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
                                   ((pattern (Var z))
                                    (meta
                                     ((type_ (UArray (UArray UReal))) 
                                      (loc <opaque>) (adlevel DataOnly))))
                                   ((Single
                                     ((pattern (Var sym2__))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                    (Single
                                     ((pattern (Var sym1__))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                 (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                             ()))
                           (meta <opaque>)))))
                       (meta <opaque>)))))
                   (meta <opaque>)))))
               (meta <opaque>)))))
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
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id y_init_rep)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id y_rep)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
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
             (Assignment
              ((LVariable y_init_rep)
               ((Single
                 ((pattern (Var k))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
              (UArray UReal)
              ((pattern
                (FunApp (StanLib lognormal_rng FnRng AoS)
                 (((pattern
                    (FunApp (StanLib log FnPlain AoS)
                     (((pattern
                        (Indexed
                         ((pattern (Var z_init))
                          (meta
                           ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                         ((Single
                           ((pattern (Var k))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (Indexed
                     ((pattern (Var sigma))
                      (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                     ((Single
                       ((pattern (Var k))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
            (meta <opaque>))
           ((pattern
             (For (loopvar n)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var N))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (Assignment
                      ((LVariable y_rep)
                       ((Single
                         ((pattern (Var n))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                        (Single
                         ((pattern (Var k))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      (UArray (UArray UReal))
                      ((pattern
                        (FunApp (StanLib lognormal_rng FnRng AoS)
                         (((pattern
                            (FunApp (StanLib log FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z))
                                  (meta
                                   ((type_ (UArray (UArray UReal))) (loc <opaque>)
                                    (adlevel DataOnly))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (Single
                                   ((pattern (Var k))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Indexed
                             ((pattern (Var sigma))
                              (meta
                               ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                             ((Single
                               ((pattern (Var k))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var y_init_rep))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var N))
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
                            ((pattern (Var y_rep))
                             (meta
                              ((type_ (UArray (UArray UReal))) (loc <opaque>)
                               (adlevel DataOnly))))
                            ((Single
                              ((pattern (Var sym2__))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                             (Single
                              ((pattern (Var sym1__))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                      ()))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (transform_inits
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable theta) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str theta))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var theta))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_init)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_init) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str z_init))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var z_init))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id sigma)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable sigma) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str sigma))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var sigma))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id theta)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable theta) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var theta))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_init)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_init) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var z_init))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id sigma)
      (decl_type
       (Sized
        (SArray SReal
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable sigma) ()) (UArray UReal)
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var sigma))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((theta <opaque>
    ((out_unconstrained_st
      (SArray SReal
       ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray SReal
       ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters)
     (out_trans
      (Lower
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
   (z_init <opaque>
    ((out_unconstrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters)
     (out_trans
      (Lower
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
   (sigma <opaque>
    ((out_unconstrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters)
     (out_trans
      (Lower
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
   (z <opaque>
    ((out_unconstrained_st
      (SArray
       (SArray SReal
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SArray SReal
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block TransformedParameters) (out_trans Identity)))
   (y_init_rep <opaque>
    ((out_unconstrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block GeneratedQuantities) (out_trans Identity)))
   (y_rep <opaque>
    ((out_unconstrained_st
      (SArray
       (SArray SReal
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SArray SReal
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name lotka_volterra_model) (prog_path web/models/lotka_volterra.stan))