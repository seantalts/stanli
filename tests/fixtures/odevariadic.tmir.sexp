((functions_block
  (((fdrt (ReturnType UVector)) (fdname rhs) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable y UVector) (AutoDiffable a UReal)
      (AutoDiffable b UReal)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id dy)
             (decl_type
              (Sized
               (SVector AoS
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (Assignment
             ((LVariable dy)
              ((Single
                ((pattern (Lit Int 1))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             UVector
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib PMinus__ FnPlain AoS)
                        (((pattern (Var a))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (Indexed
                        ((pattern (Var y))
                         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern (Var b))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (Indexed
                        ((pattern (Var y))
                         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment
             ((LVariable dy)
              ((Single
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             UVector
             ((pattern
               (FunApp (StanLib Minus__ FnPlain AoS)
                (((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern (Var a))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (Indexed
                        ((pattern (Var y))
                         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern (Var b))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (Indexed
                        ((pattern (Var y))
                         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var dy))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType UVector)) (fdname rhs_mixed) (fdsuffix FnPlain)
    (fdargs
     ((AutoDiffable t UReal) (AutoDiffable y UVector) (AutoDiffable a UReal)
      (AutoDiffable p UVector) (AutoDiffable d UReal) (AutoDiffable k (UArray UInt))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Decl (decl_adtype AutoDiffable) (decl_id dy)
             (decl_type
              (Sized
               (SVector AoS
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (initialize Default)))
           (meta <opaque>))
          ((pattern
            (Assignment
             ((LVariable dy)
              ((Single
                ((pattern (Lit Int 1))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             UVector
             ((pattern
               (FunApp (StanLib Plus__ FnPlain AoS)
                (((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern
                           (FunApp (StanLib PMinus__ FnPlain AoS)
                            (((pattern (Var a))
                              (meta
                               ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern
                           (Indexed
                            ((pattern (Var y))
                             (meta
                              ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                            ((Single
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern
                           (Indexed
                            ((pattern (Var p))
                             (meta
                              ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                            ((Single
                              ((pattern (Lit Int 1))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                         ((pattern
                           (Indexed
                            ((pattern (Var y))
                             (meta
                              ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                            ((Single
                              ((pattern (Lit Int 2))
                               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern (Var d))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (Promotion
                        ((pattern
                          (Indexed
                           ((pattern (Var k))
                            (meta
                             ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                           ((Single
                             ((pattern (Lit Int 1))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        UReal DataOnly))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Assignment
             ((LVariable dy)
              ((Single
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             UVector
             ((pattern
               (FunApp (StanLib Minus__ FnPlain AoS)
                (((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern (Var a))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (Indexed
                        ((pattern (Var y))
                         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 1))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern
                       (Indexed
                        ((pattern (Var p))
                         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (Indexed
                        ((pattern (Var y))
                         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
              (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var dy))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((N <opaque> SInt)
   (ts <opaque>
    (SArray SReal
     ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (d_real <opaque> SReal)
   (d_int <opaque>
    (SArray SInt
     ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
 (prepare_data
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id N) (decl_type (Sized SInt))
      (initialize Default)))
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
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (var_name N)
        (var ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype DataOnly) (decl_id d_real) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable d_real) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str d_real))
              (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
          (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
         ((Single
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id d_int)
      (decl_type
       (Sized
        (SArray SInt
         ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable d_int) ()) (UArray UInt)
      ((pattern
        (FunApp (CompilerInternal FnReadData)
         (((pattern (Lit Str d_int))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str z_rk45))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str z_bdf))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str z_adams))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str z_ckrk))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str z_tol))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str z_mixed))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a) (decl_type (Sized SReal))
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
             (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id b) (decl_type (Sized SReal))
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
             (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id p)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id y0)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_rk45)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_rk45) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45 FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_bdf)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_bdf) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_bdf FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_adams)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_adams) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_adams FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_ckrk)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_ckrk) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_ckrk FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_tol)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_tol) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45_tol FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-8))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-8))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 100000))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_mixed)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_mixed) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45 FnPlain AoS)
         (((pattern (Var rhs_mixed))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable (UArray UInt)))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var p))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var d_real))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var d_int))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var a))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var b))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var p))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var y0))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
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
         (For (loopvar n)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_rk45))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_bdf))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib sum FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var z_adams))
                              (meta
                               ((type_ (UArray UVector)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Var n))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>))
               ((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_ckrk))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_tol))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib sum FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var z_mixed))
                              (meta
                               ((type_ (UArray UVector)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Var n))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a) (decl_type (Sized SReal))
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
             (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id b) (decl_type (Sized SReal))
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
             (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id p)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id y0)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_rk45)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_rk45) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45 FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_bdf)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_bdf) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_bdf FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_adams)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_adams) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_adams FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_ckrk)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_ckrk) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_ckrk FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_tol)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_tol) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45_tol FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-8))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-8))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 100000))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z_mixed)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_mixed) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45 FnPlain AoS)
         (((pattern (Var rhs_mixed))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable (UArray UInt)))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var p))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var d_real))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var d_int))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var a))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var b))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var p))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib lognormal_lpdf (FnLpdf true) AoS)
             (((pattern (Var y0))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
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
         (For (loopvar n)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_rk45))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_bdf))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib sum FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var z_adams))
                              (meta
                               ((type_ (UArray UVector)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Var n))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>))
               ((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_ckrk))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib sum FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var z_tol))
                                  (meta
                                   ((type_ (UArray UVector)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var n))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib sum FnPlain AoS)
                         (((pattern
                            (Indexed
                             ((pattern (Var z_mixed))
                              (meta
                               ((type_ (UArray UVector)) (loc <opaque>)
                                (adlevel AutoDiffable))))
                             ((Single
                               ((pattern (Var n))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id a) (decl_type (Sized SReal))
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
             (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id b) (decl_type (Sized SReal))
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
             (dims ()) (mem_pattern AoS)))
           ()))
         (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id p)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id y0)
      (decl_type
       (Sized
        (SVector AoS
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
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z_rk45)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z_bdf)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z_adams)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z_ckrk)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z_tol)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id z_mixed)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var a)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var b)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var p)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var y0)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Assignment ((LVariable z_rk45) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45 FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_bdf) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_bdf FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_adams) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_adams FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_ckrk) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_ckrk FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_tol) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45_tol FnPlain AoS)
         (((pattern (Var rhs))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UReal))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-8))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Real 1e-8))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 100000))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var b))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z_mixed) ()) (UArray UVector)
      ((pattern
        (FunApp (StanLib ode_rk45 FnPlain AoS)
         (((pattern (Var rhs_mixed))
           (meta
            ((type_
              (UFun
               (((AutoDiffable UReal) (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable UVector) (AutoDiffable UReal)
                 (AutoDiffable (UArray UInt)))
                (ReturnType UVector) FnPlain AoS)))
             (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var y0))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Lit Real 0.0))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var ts))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var a))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var p))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern (Var d_real))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Var d_int))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))
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
                                   ((pattern (Var z_rk45))
                                    (meta
                                     ((type_ (UArray UVector)) (loc <opaque>)
                                      (adlevel DataOnly))))
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
                            (NRFunApp
                             (CompilerInternal
                              (FnWriteParam (unconstrain_opt ())
                               (var
                                ((pattern
                                  (Indexed
                                   ((pattern (Var z_bdf))
                                    (meta
                                     ((type_ (UArray UVector)) (loc <opaque>)
                                      (adlevel DataOnly))))
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
                            (NRFunApp
                             (CompilerInternal
                              (FnWriteParam (unconstrain_opt ())
                               (var
                                ((pattern
                                  (Indexed
                                   ((pattern (Var z_adams))
                                    (meta
                                     ((type_ (UArray UVector)) (loc <opaque>)
                                      (adlevel DataOnly))))
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
                            (NRFunApp
                             (CompilerInternal
                              (FnWriteParam (unconstrain_opt ())
                               (var
                                ((pattern
                                  (Indexed
                                   ((pattern (Var z_ckrk))
                                    (meta
                                     ((type_ (UArray UVector)) (loc <opaque>)
                                      (adlevel DataOnly))))
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
                            (NRFunApp
                             (CompilerInternal
                              (FnWriteParam (unconstrain_opt ())
                               (var
                                ((pattern
                                  (Indexed
                                   ((pattern (Var z_tol))
                                    (meta
                                     ((type_ (UArray UVector)) (loc <opaque>)
                                      (adlevel DataOnly))))
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
                            (NRFunApp
                             (CompilerInternal
                              (FnWriteParam (unconstrain_opt ())
                               (var
                                ((pattern
                                  (Indexed
                                   ((pattern (Var z_mixed))
                                    (meta
                                     ((type_ (UArray UVector)) (loc <opaque>)
                                      (adlevel DataOnly))))
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
     (Decl (decl_adtype AutoDiffable) (decl_id a) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable a) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str a))
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
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var a)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id b) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable b) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str b))
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
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var b)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id p)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id p_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable p_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str p))
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
                  ((LVariable p)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var p_flat__))
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
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var p)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id y0)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id y0_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable y0_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str y0))
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
                  ((LVariable y0)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var y0_flat__))
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
       (FnWriteParam
        (unconstrain_opt
         ((Lower
           ((pattern (Lit Int 0))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
        (var
         ((pattern (Var y0)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id a) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable a) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
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
         ((pattern (Var a)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id b) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable b) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
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
         ((pattern (Var b)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id p)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable p) ()) UVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
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
         ((pattern (Var p)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id y0)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable y0) ()) UVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
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
         ((pattern (Var y0)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((a <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans
      (Lower
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
   (b <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans
      (Lower
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
   (p <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters)
     (out_trans
      (Lower
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
   (y0 <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters)
     (out_trans
      (Lower
       ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
   (z_rk45 <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block TransformedParameters) (out_trans Identity)))
   (z_bdf <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block TransformedParameters) (out_trans Identity)))
   (z_adams <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block TransformedParameters) (out_trans Identity)))
   (z_ckrk <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block TransformedParameters) (out_trans Identity)))
   (z_tol <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block TransformedParameters) (out_trans Identity)))
   (z_mixed <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block TransformedParameters) (out_trans Identity)))))
 (prog_name odevariadic_model) (prog_path tests/fixtures/odevariadic.stan))