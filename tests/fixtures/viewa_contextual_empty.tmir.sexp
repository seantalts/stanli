((functions_block
  (((fdrt (ReturnType UReal)) (fdname vector_shape) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable x (UArray UVector)) (AutoDiffable q UReal)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern (Var q))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 1000))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib size FnPlain AoS)
                                        (((pattern (Var x))
                                          (meta
                                           ((type_ (UArray UVector))
                                            (loc <opaque>) (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern
                               (FunApp (StanLib Times__ FnPlain AoS)
                                (((pattern (Lit Int 100))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (FunApp (StanLib num_elements FnPlain AoS)
                                    (((pattern (Var x))
                                      (meta
                                       ((type_ (UArray UVector)) (loc <opaque>)
                                        (adlevel AutoDiffable)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern
                           (FunApp (StanLib Times__ FnPlain AoS)
                            (((pattern (Lit Int 10))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern
                               (Indexed
                                ((pattern
                                  (FunApp (StanLib dims FnPlain AoS)
                                   (((pattern (Var x))
                                     (meta
                                      ((type_ (UArray UVector)) (loc <opaque>)
                                       (adlevel AutoDiffable)))))))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly))))
                                ((Single
                                  ((pattern (Lit Int 1))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern
                       (Indexed
                        ((pattern
                          (FunApp (StanLib dims FnPlain AoS)
                           (((pattern (Var x))
                             (meta
                              ((type_ (UArray UVector)) (loc <opaque>)
                               (adlevel AutoDiffable)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal DataOnly))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType UReal)) (fdname row_vector_shape) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable x (UArray URowVector)) (AutoDiffable q UReal)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern (Var q))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 1000))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib size FnPlain AoS)
                                        (((pattern (Var x))
                                          (meta
                                           ((type_ (UArray URowVector))
                                            (loc <opaque>) (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern
                               (FunApp (StanLib Times__ FnPlain AoS)
                                (((pattern (Lit Int 100))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (FunApp (StanLib num_elements FnPlain AoS)
                                    (((pattern (Var x))
                                      (meta
                                       ((type_ (UArray URowVector)) (loc <opaque>)
                                        (adlevel AutoDiffable)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern
                           (FunApp (StanLib Times__ FnPlain AoS)
                            (((pattern (Lit Int 10))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern
                               (Indexed
                                ((pattern
                                  (FunApp (StanLib dims FnPlain AoS)
                                   (((pattern (Var x))
                                     (meta
                                      ((type_ (UArray URowVector)) (loc <opaque>)
                                       (adlevel AutoDiffable)))))))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly))))
                                ((Single
                                  ((pattern (Lit Int 1))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern
                       (Indexed
                        ((pattern
                          (FunApp (StanLib dims FnPlain AoS)
                           (((pattern (Var x))
                             (meta
                              ((type_ (UArray URowVector)) (loc <opaque>)
                               (adlevel AutoDiffable)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal DataOnly))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType UReal)) (fdname matrix_shape) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable x (UArray UMatrix)) (AutoDiffable q UReal)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib Plus__ FnPlain AoS)
                                 (((pattern (Var q))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                                  ((pattern
                                    (Promotion
                                     ((pattern
                                       (FunApp (StanLib Times__ FnPlain AoS)
                                        (((pattern (Lit Int 10000))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         ((pattern
                                           (FunApp (StanLib size FnPlain AoS)
                                            (((pattern (Var x))
                                              (meta
                                               ((type_ (UArray UMatrix))
                                                (loc <opaque>) (adlevel AutoDiffable)))))))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 1000))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib num_elements FnPlain AoS)
                                        (((pattern (Var x))
                                          (meta
                                           ((type_ (UArray UMatrix))
                                            (loc <opaque>) (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern
                               (FunApp (StanLib Times__ FnPlain AoS)
                                (((pattern (Lit Int 100))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (Indexed
                                    ((pattern
                                      (FunApp (StanLib dims FnPlain AoS)
                                       (((pattern (Var x))
                                         (meta
                                          ((type_ (UArray UMatrix)) (loc <opaque>)
                                           (adlevel AutoDiffable)))))))
                                     (meta
                                      ((type_ (UArray UInt)) (loc <opaque>)
                                       (adlevel DataOnly))))
                                    ((Single
                                      ((pattern (Lit Int 1))
                                       (meta
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern
                           (FunApp (StanLib Times__ FnPlain AoS)
                            (((pattern (Lit Int 10))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern
                               (Indexed
                                ((pattern
                                  (FunApp (StanLib dims FnPlain AoS)
                                   (((pattern (Var x))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel AutoDiffable)))))))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly))))
                                ((Single
                                  ((pattern (Lit Int 2))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern
                       (Indexed
                        ((pattern
                          (FunApp (StanLib dims FnPlain AoS)
                           (((pattern (Var x))
                             (meta
                              ((type_ (UArray UMatrix)) (loc <opaque>)
                               (adlevel AutoDiffable)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                        ((Single
                          ((pattern (Lit Int 3))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal DataOnly))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))
   ((fdrt (ReturnType UReal)) (fdname scalar_array_shape) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable x (UArray (UArray UReal))) (AutoDiffable q UReal)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern (Var q))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 1000))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib size FnPlain AoS)
                                        (((pattern (Var x))
                                          (meta
                                           ((type_ (UArray (UArray UReal)))
                                            (loc <opaque>) (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (Promotion
                             ((pattern
                               (FunApp (StanLib Times__ FnPlain AoS)
                                (((pattern (Lit Int 100))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (FunApp (StanLib num_elements FnPlain AoS)
                                    (((pattern (Var x))
                                      (meta
                                       ((type_ (UArray (UArray UReal)))
                                        (loc <opaque>) (adlevel AutoDiffable)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (Promotion
                         ((pattern
                           (FunApp (StanLib Times__ FnPlain AoS)
                            (((pattern (Lit Int 10))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern
                               (Indexed
                                ((pattern
                                  (FunApp (StanLib dims FnPlain AoS)
                                   (((pattern (Var x))
                                     (meta
                                      ((type_ (UArray (UArray UReal)))
                                       (loc <opaque>) (adlevel AutoDiffable)))))))
                                 (meta
                                  ((type_ (UArray UInt)) (loc <opaque>)
                                   (adlevel DataOnly))))
                                ((Single
                                  ((pattern (Lit Int 1))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Promotion
                     ((pattern
                       (Indexed
                        ((pattern
                          (FunApp (StanLib dims FnPlain AoS)
                           (((pattern (Var x))
                             (meta
                              ((type_ (UArray (UArray UReal))) (loc <opaque>)
                               (adlevel AutoDiffable)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal DataOnly))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((vectors <opaque>
    (SArray
     (SVector AoS
      ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (rows <opaque>
    (SArray
     (SRowVector AoS
      ((pattern (Lit Int 5)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (matrices <opaque>
    (SArray
     (SMatrix AoS
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (scalars <opaque>
    (SArray
     (SArray SReal
      ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
     (Decl (decl_adtype DataOnly) (decl_id vectors)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 4)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id vectors_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable vectors_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str vectors))
               (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
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
                 (For (loopvar sym2__)
                  (lower
                   ((pattern (Lit Int 1))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (upper
                   ((pattern (Lit Int 0))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable vectors)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray UVector)
                          ((pattern
                            (Indexed
                             ((pattern (Var vectors_flat__))
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
     (Decl (decl_adtype DataOnly) (decl_id rows)
      (decl_type
       (Sized
        (SArray
         (SRowVector AoS
          ((pattern (Lit Int 5)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id rows_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable rows_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str rows))
               (meta ((type_ (UArray URowVector)) (loc <opaque>) (adlevel DataOnly)))))))
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
           ((pattern (Lit Int 5))
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
                   ((pattern (Lit Int 0))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable rows)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray URowVector)
                          ((pattern
                            (Indexed
                             ((pattern (Var rows_flat__))
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
     (Decl (decl_adtype DataOnly) (decl_id matrices)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id matrices_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable matrices_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str matrices))
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
                           ((pattern (Lit Int 0))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable matrices)
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
                                     ((pattern (Var matrices_flat__))
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
     (Decl (decl_adtype DataOnly) (decl_id scalars)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id scalars_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable scalars_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str scalars))
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
                   ((pattern (Lit Int 0))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable scalars)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray (UArray UReal))
                          ((pattern
                            (Indexed
                             ((pattern (Var scalars_flat__))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (UserDefined vector_shape FnPlain)
                         (((pattern (Var vectors))
                           (meta
                            ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Var q))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (UserDefined row_vector_shape FnPlain)
                         (((pattern (Var rows))
                           (meta
                            ((type_ (UArray URowVector)) (loc <opaque>)
                             (adlevel DataOnly))))
                          ((pattern (Var q))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (UserDefined matrix_shape FnPlain)
                     (((pattern (Var matrices))
                       (meta
                        ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Var q))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (UserDefined scalar_array_shape FnPlain)
                 (((pattern (Var scalars))
                   (meta
                    ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Var q))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (UserDefined vector_shape FnPlain)
                         (((pattern (Var vectors))
                           (meta
                            ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern (Var q))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (UserDefined row_vector_shape FnPlain)
                         (((pattern (Var rows))
                           (meta
                            ((type_ (UArray URowVector)) (loc <opaque>)
                             (adlevel DataOnly))))
                          ((pattern (Var q))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (UserDefined matrix_shape FnPlain)
                     (((pattern (Var matrices))
                       (meta
                        ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Var q))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (UserDefined scalar_array_shape FnPlain)
                 (((pattern (Var scalars))
                   (meta
                    ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Var q))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
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
 (prog_name viewa_contextual_empty_model)
 (prog_path tests/fixtures/viewa_contextual_empty.stan))