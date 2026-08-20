((functions_block ())
 (input_vars
  ((d2 <opaque>
    (SArray
     (SArray SReal
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (d3 <opaque>
    (SArray
     (SArray
      (SArray SReal
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (d4 <opaque>
    (SArray
     (SArray
      (SArray
       (SArray SReal
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (dvv <opaque>
    (SArray
     (SArray
      (SVector AoS
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
   (dm <opaque>
    (SArray
     (SMatrix AoS
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
      ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
     (Decl (decl_adtype DataOnly) (decl_id d2)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id d2_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable d2_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str d2))
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable d2)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray (UArray UReal))
                          ((pattern
                            (Indexed
                             ((pattern (Var d2_flat__))
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
     (Decl (decl_adtype DataOnly) (decl_id d3)
      (decl_type
       (Sized
        (SArray
         (SArray
          (SArray SReal
           ((pattern (Lit Int 2))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id d3_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable d3_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str d3))
               (meta
                ((type_ (UArray (UArray (UArray UReal)))) (loc <opaque>)
                 (adlevel DataOnly)))))))
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
                                  ((LVariable d3)
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
                                  (UArray (UArray (UArray UReal)))
                                  ((pattern
                                    (Indexed
                                     ((pattern (Var d3_flat__))
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
     (Decl (decl_adtype DataOnly) (decl_id d4)
      (decl_type
       (Sized
        (SArray
         (SArray
          (SArray
           (SArray SReal
            ((pattern (Lit Int 2))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
           ((pattern (Lit Int 2))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id d4_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable d4_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str d4))
               (meta
                ((type_ (UArray (UArray (UArray (UArray UReal))))) (loc <opaque>)
                 (adlevel DataOnly)))))))
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
                                 (For (loopvar sym4__)
                                  (lower
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (upper
                                   ((pattern (Lit Int 2))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (body
                                   ((pattern
                                     (Block
                                      (((pattern
                                         (Assignment
                                          ((LVariable d4)
                                           ((Single
                                             ((pattern (Var sym4__))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))
                                            (Single
                                             ((pattern (Var sym3__))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))
                                            (Single
                                             ((pattern (Var sym2__))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))
                                            (Single
                                             ((pattern (Var sym1__))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))))
                                          (UArray (UArray (UArray (UArray UReal))))
                                          ((pattern
                                            (Indexed
                                             ((pattern (Var d4_flat__))
                                              (meta
                                               ((type_ (UArray UReal)) 
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var pos__))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))))
                                        (meta <opaque>))
                                       ((pattern
                                         (Assignment ((LVariable pos__) ()) UInt
                                          ((pattern
                                            (FunApp (StanLib Plus__ FnPlain AoS)
                                             (((pattern (Var pos__))
                                               (meta
                                                ((type_ UInt) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern (Lit Int 1))
                                               (meta
                                                ((type_ UInt) (loc <opaque>)
                                                 (adlevel DataOnly)))))))
                                           (meta
                                            ((type_ UInt) (loc <opaque>)
                                             (adlevel DataOnly))))))
                                        (meta <opaque>)))))
                                    (meta <opaque>)))))
                                (meta <opaque>)))))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id dvv)
      (decl_type
       (Sized
        (SArray
         (SArray
          (SVector AoS
           ((pattern (Lit Int 3))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id dvv_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable dvv_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str dvv))
               (meta
                ((type_ (UArray (UArray UVector))) (loc <opaque>) (adlevel DataOnly)))))))
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
                                  ((LVariable dvv)
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
                                  (UArray (UArray UVector))
                                  ((pattern
                                    (Indexed
                                     ((pattern (Var dvv_flat__))
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
     (Decl (decl_adtype DataOnly) (decl_id dm)
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
         (Decl (decl_adtype AutoDiffable) (decl_id dm_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable dm_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str dm))
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
                                  ((LVariable dm)
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
                                     ((pattern (Var dm_flat__))
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
     (Decl (decl_adtype DataOnly) (decl_id t2)
      (decl_type
       (Sized
        (SArray
         (SArray SReal
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable t2) ()) (UArray (UArray UReal))
      ((pattern
        (FunApp (CompilerInternal FnMakeArray)
         (((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern (Lit Real 11.))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Real 12.))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern (Lit Real 21.))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Real 22.))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id t3)
      (decl_type
       (Sized
        (SArray
         (SArray
          (SArray SReal
           ((pattern (Lit Int 2))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable t3) ()) (UArray (UArray (UArray UReal)))
      ((pattern
        (FunApp (CompilerInternal FnMakeArray)
         (((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Lit Real 111.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 112.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Lit Real 121.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 122.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Lit Real 211.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 212.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern (Lit Real 221.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 222.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly)))))))
       (meta
        ((type_ (UArray (UArray (UArray UReal)))) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id t4)
      (decl_type
       (Sized
        (SArray
         (SArray
          (SArray
           (SArray SReal
            ((pattern (Lit Int 2))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
           ((pattern (Lit Int 2))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable t4) ()) (UArray (UArray (UArray (UArray UReal))))
      ((pattern
        (FunApp (CompilerInternal FnMakeArray)
         (((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 1111.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 1112.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 1121.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 1122.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 1211.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 1212.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 1221.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 1222.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly)))))))
           (meta
            ((type_ (UArray (UArray (UArray UReal)))) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 2111.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 2112.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 2121.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 2122.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (CompilerInternal FnMakeArray)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 2211.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 2212.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (CompilerInternal FnMakeArray)
                     (((pattern (Lit Real 2221.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 2222.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ (UArray UReal)) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ (UArray (UArray UReal))) (loc <opaque>) (adlevel DataOnly)))))))
           (meta
            ((type_ (UArray (UArray (UArray UReal)))) (loc <opaque>) (adlevel DataOnly)))))))
       (meta
        ((type_ (UArray (UArray (UArray (UArray UReal))))) (loc <opaque>)
         (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id tvv)
      (decl_type
       (Sized
        (SArray
         (SArray
          (SVector AoS
           ((pattern (Lit Int 3))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable tvv) ()) (UArray (UArray UVector))
      ((pattern
        (FunApp (CompilerInternal FnMakeArray)
         (((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (StanLib Transpose__ FnPlain AoS)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeRowVec)
                     (((pattern (Lit Real 111.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 112.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 113.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (StanLib Transpose__ FnPlain AoS)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeRowVec)
                     (((pattern (Lit Real 121.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 122.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 123.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (CompilerInternal FnMakeArray)
             (((pattern
                (FunApp (StanLib Transpose__ FnPlain AoS)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeRowVec)
                     (((pattern (Lit Real 211.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 212.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 213.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (StanLib Transpose__ FnPlain AoS)
                 (((pattern
                    (FunApp (CompilerInternal FnMakeRowVec)
                     (((pattern (Lit Real 221.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 222.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 223.))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray (UArray UVector))) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id tm)
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
     (Assignment ((LVariable tm) ()) (UArray UMatrix)
      ((pattern
        (FunApp (CompilerInternal FnMakeArray)
         (((pattern
            (FunApp (CompilerInternal FnMakeRowVec)
             (((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern (Lit Real 111.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 112.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 113.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern (Lit Real 121.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 122.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 123.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (FunApp (CompilerInternal FnMakeRowVec)
             (((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern (Lit Real 211.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 212.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 213.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern (Lit Real 221.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 222.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern (Lit Real 223.))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id s3) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable s3) ()) UReal
      ((pattern
        (Promotion
         ((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
         UReal DataOnly))
       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (For (loopvar i)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar j)
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
                             (Assignment ((LVariable s3) ()) UReal
                              ((pattern
                                (FunApp (StanLib fma FnPlain AoS)
                                 (((pattern
                                    (Indexed
                                     ((pattern (Var t3))
                                      (meta
                                       ((type_ (UArray (UArray (UArray UReal))))
                                        (loc <opaque>) (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var i))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Var j))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Var k))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (Promotion
                                     ((pattern
                                       (FunApp (StanLib Plus__ FnPlain AoS)
                                        (((pattern
                                           (FunApp (StanLib fma FnPlain AoS)
                                            (((pattern (Lit Int 100))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             ((pattern (Var i))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             ((pattern
                                               (FunApp (StanLib Times__ FnPlain AoS)
                                                (((pattern (Lit Int 10))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var j))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly)))))))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         ((pattern (Var k))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern (Var s3))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id x) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id y) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id z) (decl_type (Sized SReal))
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
         (Decl (decl_adtype AutoDiffable) (decl_id diff) (decl_type (Sized SReal))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable diff) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id wt) (decl_type (Sized SReal))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable wt) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment ((LVariable diff) ()) UReal
                          ((pattern
                            (FunApp (StanLib fma FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib Minus__ FnPlain AoS)
                                 (((pattern
                                    (Indexed
                                     ((pattern (Var t2))
                                      (meta
                                       ((type_ (UArray (UArray UReal))) 
                                        (loc <opaque>) (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var i))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Var j))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (Indexed
                                     ((pattern (Var d2))
                                      (meta
                                       ((type_ (UArray (UArray UReal))) 
                                        (loc <opaque>) (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var i))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Var j))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib fma FnPlain AoS)
                                    (((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var i))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var j))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern (Var diff))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                        (meta <opaque>))
                       ((pattern
                         (Assignment ((LVariable wt) ()) UReal
                          ((pattern
                            (FunApp (StanLib fma FnPlain AoS)
                             (((pattern
                                (Indexed
                                 ((pattern (Var t2))
                                  (meta
                                   ((type_ (UArray (UArray UReal))) (loc <opaque>)
                                    (adlevel DataOnly))))
                                 ((Single
                                   ((pattern (Var i))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (Single
                                   ((pattern (Var j))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib fma FnPlain AoS)
                                    (((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var i))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var j))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern (Var wt))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
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
                                 (Assignment ((LVariable diff) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain AoS)
                                     (((pattern
                                        (FunApp (StanLib Minus__ FnPlain AoS)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var t3))
                                              (meta
                                               ((type_ (UArray (UArray (UArray UReal))))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Indexed
                                             ((pattern (Var d3))
                                              (meta
                                               ((type_ (UArray (UArray (UArray UReal))))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain AoS)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain AoS)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain AoS)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var diff))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                                (meta <opaque>))
                               ((pattern
                                 (Assignment ((LVariable wt) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain AoS)
                                     (((pattern
                                        (Indexed
                                         ((pattern (Var t3))
                                          (meta
                                           ((type_ (UArray (UArray (UArray UReal))))
                                            (loc <opaque>) (adlevel DataOnly))))
                                         ((Single
                                           ((pattern (Var i))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var j))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var k))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly))))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain AoS)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain AoS)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain AoS)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var wt))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
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
                                 (For (loopvar l)
                                  (lower
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (upper
                                   ((pattern (Lit Int 2))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (body
                                   ((pattern
                                     (Block
                                      (((pattern
                                         (Assignment ((LVariable diff) ()) UReal
                                          ((pattern
                                            (FunApp (StanLib fma FnPlain AoS)
                                             (((pattern
                                                (FunApp (StanLib Minus__ FnPlain AoS)
                                                 (((pattern
                                                    (Indexed
                                                     ((pattern (Var t4))
                                                      (meta
                                                       ((type_
                                                         (UArray
                                                          (UArray
                                                           (UArray (UArray UReal)))))
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((Single
                                                       ((pattern (Var i))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var j))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var k))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var l))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly))))))))
                                                   (meta
                                                    ((type_ UReal) (loc <opaque>)
                                                     (adlevel DataOnly))))
                                                  ((pattern
                                                    (Indexed
                                                     ((pattern (Var d4))
                                                      (meta
                                                       ((type_
                                                         (UArray
                                                          (UArray
                                                           (UArray (UArray UReal)))))
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((Single
                                                       ((pattern (Var i))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var j))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var k))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var l))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly))))))))
                                                   (meta
                                                    ((type_ UReal) (loc <opaque>)
                                                     (adlevel DataOnly)))))))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern
                                                (Promotion
                                                 ((pattern
                                                   (FunApp (StanLib Plus__ FnPlain AoS)
                                                    (((pattern
                                                       (FunApp (StanLib fma FnPlain AoS)
                                                        (((pattern (Lit Int 10))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern (Var k))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern
                                                           (FunApp
                                                            (StanLib fma FnPlain AoS)
                                                            (((pattern (Lit Int 1000))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern (Var i))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern
                                                               (FunApp
                                                                (StanLib Times__ FnPlain
                                                                 AoS)
                                                                (((pattern (Lit Int 100))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                 ((pattern (Var j))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly)))))))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly)))))))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var l))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 UReal DataOnly))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern (Var diff))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel AutoDiffable)))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel AutoDiffable))))))
                                        (meta <opaque>))
                                       ((pattern
                                         (Assignment ((LVariable wt) ()) UReal
                                          ((pattern
                                            (FunApp (StanLib fma FnPlain AoS)
                                             (((pattern
                                                (Indexed
                                                 ((pattern (Var t4))
                                                  (meta
                                                   ((type_
                                                     (UArray
                                                      (UArray (UArray (UArray UReal)))))
                                                    (loc <opaque>) (adlevel DataOnly))))
                                                 ((Single
                                                   ((pattern (Var i))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Var j))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Var k))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Var l))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly))))))))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern
                                                (Promotion
                                                 ((pattern
                                                   (FunApp (StanLib Plus__ FnPlain AoS)
                                                    (((pattern
                                                       (FunApp (StanLib fma FnPlain AoS)
                                                        (((pattern (Lit Int 10))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern (Var k))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern
                                                           (FunApp
                                                            (StanLib fma FnPlain AoS)
                                                            (((pattern (Lit Int 1000))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern (Var i))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern
                                                               (FunApp
                                                                (StanLib Times__ FnPlain
                                                                 AoS)
                                                                (((pattern (Lit Int 100))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                 ((pattern (Var j))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly)))))))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly)))))))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var l))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 UReal DataOnly))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern (Var wt))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel AutoDiffable)))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel AutoDiffable))))))
                                        (meta <opaque>)))))
                                    (meta <opaque>)))))
                                (meta <opaque>)))))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (For (loopvar k)
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
                                 (Assignment ((LVariable diff) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain AoS)
                                     (((pattern
                                        (FunApp (StanLib Minus__ FnPlain AoS)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var tvv))
                                              (meta
                                               ((type_ (UArray (UArray UVector)))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Indexed
                                             ((pattern (Var dvv))
                                              (meta
                                               ((type_ (UArray (UArray UVector)))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain AoS)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain AoS)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain AoS)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var diff))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                                (meta <opaque>))
                               ((pattern
                                 (Assignment ((LVariable wt) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain AoS)
                                     (((pattern
                                        (Indexed
                                         ((pattern (Var tvv))
                                          (meta
                                           ((type_ (UArray (UArray UVector)))
                                            (loc <opaque>) (adlevel DataOnly))))
                                         ((Single
                                           ((pattern (Var i))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var j))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var k))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly))))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain AoS)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain AoS)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain AoS)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var wt))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (For (loopvar k)
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
                                 (Assignment ((LVariable diff) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain AoS)
                                     (((pattern
                                        (FunApp (StanLib Minus__ FnPlain AoS)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var tm))
                                              (meta
                                               ((type_ (UArray UMatrix)) 
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Indexed
                                             ((pattern (Var dm))
                                              (meta
                                               ((type_ (UArray UMatrix)) 
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain AoS)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain AoS)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain AoS)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var diff))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                                (meta <opaque>))
                               ((pattern
                                 (Assignment ((LVariable wt) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain AoS)
                                     (((pattern
                                        (Indexed
                                         ((pattern (Var tm))
                                          (meta
                                           ((type_ (UArray UMatrix)) 
                                            (loc <opaque>) (adlevel DataOnly))))
                                         ((Single
                                           ((pattern (Var i))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var j))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var k))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly))))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain AoS)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain AoS)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain AoS)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var wt))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib fma FnPlain AoS)
             (((pattern (Var s3))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var z))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib fma FnPlain AoS)
                 (((pattern (Var diff))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var x))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib Times__ FnPlain AoS)
                     (((pattern (Var wt))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var y))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id x) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id y) (decl_type (Sized SReal))
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
     (Decl (decl_adtype AutoDiffable) (decl_id z) (decl_type (Sized SReal))
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
         (Decl (decl_adtype AutoDiffable) (decl_id diff) (decl_type (Sized SReal))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable diff) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id wt) (decl_type (Sized SReal))
          (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable wt) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment ((LVariable diff) ()) UReal
                          ((pattern
                            (FunApp (StanLib fma FnPlain SoA)
                             (((pattern
                                (FunApp (StanLib Minus__ FnPlain SoA)
                                 (((pattern
                                    (Indexed
                                     ((pattern (Var t2))
                                      (meta
                                       ((type_ (UArray (UArray UReal))) 
                                        (loc <opaque>) (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var i))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Var j))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (Indexed
                                     ((pattern (Var d2))
                                      (meta
                                       ((type_ (UArray (UArray UReal))) 
                                        (loc <opaque>) (adlevel DataOnly))))
                                     ((Single
                                       ((pattern (Var i))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                      (Single
                                       ((pattern (Var j))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib fma FnPlain SoA)
                                    (((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var i))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var j))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern (Var diff))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                        (meta <opaque>))
                       ((pattern
                         (Assignment ((LVariable wt) ()) UReal
                          ((pattern
                            (FunApp (StanLib fma FnPlain SoA)
                             (((pattern
                                (Indexed
                                 ((pattern (Var t2))
                                  (meta
                                   ((type_ (UArray (UArray UReal))) (loc <opaque>)
                                    (adlevel DataOnly))))
                                 ((Single
                                   ((pattern (Var i))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (Single
                                   ((pattern (Var j))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern
                                   (FunApp (StanLib fma FnPlain SoA)
                                    (((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var i))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern (Var j))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern (Var wt))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
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
                                 (Assignment ((LVariable diff) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain SoA)
                                     (((pattern
                                        (FunApp (StanLib Minus__ FnPlain SoA)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var t3))
                                              (meta
                                               ((type_ (UArray (UArray (UArray UReal))))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Indexed
                                             ((pattern (Var d3))
                                              (meta
                                               ((type_ (UArray (UArray (UArray UReal))))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain SoA)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain SoA)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain SoA)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var diff))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                                (meta <opaque>))
                               ((pattern
                                 (Assignment ((LVariable wt) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain SoA)
                                     (((pattern
                                        (Indexed
                                         ((pattern (Var t3))
                                          (meta
                                           ((type_ (UArray (UArray (UArray UReal))))
                                            (loc <opaque>) (adlevel DataOnly))))
                                         ((Single
                                           ((pattern (Var i))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var j))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var k))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly))))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain SoA)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain SoA)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain SoA)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var wt))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
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
                                 (For (loopvar l)
                                  (lower
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (upper
                                   ((pattern (Lit Int 2))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (body
                                   ((pattern
                                     (Block
                                      (((pattern
                                         (Assignment ((LVariable diff) ()) UReal
                                          ((pattern
                                            (FunApp (StanLib fma FnPlain SoA)
                                             (((pattern
                                                (FunApp (StanLib Minus__ FnPlain SoA)
                                                 (((pattern
                                                    (Indexed
                                                     ((pattern (Var t4))
                                                      (meta
                                                       ((type_
                                                         (UArray
                                                          (UArray
                                                           (UArray (UArray UReal)))))
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((Single
                                                       ((pattern (Var i))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var j))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var k))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var l))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly))))))))
                                                   (meta
                                                    ((type_ UReal) (loc <opaque>)
                                                     (adlevel DataOnly))))
                                                  ((pattern
                                                    (Indexed
                                                     ((pattern (Var d4))
                                                      (meta
                                                       ((type_
                                                         (UArray
                                                          (UArray
                                                           (UArray (UArray UReal)))))
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((Single
                                                       ((pattern (Var i))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var j))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var k))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly)))))
                                                      (Single
                                                       ((pattern (Var l))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly))))))))
                                                   (meta
                                                    ((type_ UReal) (loc <opaque>)
                                                     (adlevel DataOnly)))))))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern
                                                (Promotion
                                                 ((pattern
                                                   (FunApp (StanLib Plus__ FnPlain SoA)
                                                    (((pattern
                                                       (FunApp (StanLib fma FnPlain SoA)
                                                        (((pattern (Lit Int 10))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern (Var k))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern
                                                           (FunApp
                                                            (StanLib fma FnPlain SoA)
                                                            (((pattern (Lit Int 1000))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern (Var i))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern
                                                               (FunApp
                                                                (StanLib Times__ FnPlain
                                                                 SoA)
                                                                (((pattern (Lit Int 100))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                 ((pattern (Var j))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly)))))))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly)))))))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var l))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 UReal DataOnly))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern (Var diff))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel AutoDiffable)))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel AutoDiffable))))))
                                        (meta <opaque>))
                                       ((pattern
                                         (Assignment ((LVariable wt) ()) UReal
                                          ((pattern
                                            (FunApp (StanLib fma FnPlain SoA)
                                             (((pattern
                                                (Indexed
                                                 ((pattern (Var t4))
                                                  (meta
                                                   ((type_
                                                     (UArray
                                                      (UArray (UArray (UArray UReal)))))
                                                    (loc <opaque>) (adlevel DataOnly))))
                                                 ((Single
                                                   ((pattern (Var i))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Var j))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Var k))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Var l))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly))))))))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern
                                                (Promotion
                                                 ((pattern
                                                   (FunApp (StanLib Plus__ FnPlain SoA)
                                                    (((pattern
                                                       (FunApp (StanLib fma FnPlain SoA)
                                                        (((pattern (Lit Int 10))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern (Var k))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         ((pattern
                                                           (FunApp
                                                            (StanLib fma FnPlain SoA)
                                                            (((pattern (Lit Int 1000))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern (Var i))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly))))
                                                             ((pattern
                                                               (FunApp
                                                                (StanLib Times__ FnPlain
                                                                 SoA)
                                                                (((pattern (Lit Int 100))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                 ((pattern (Var j))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))))
                                                              (meta
                                                               ((type_ UInt)
                                                                (loc <opaque>)
                                                                (adlevel DataOnly)))))))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly)))))))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var l))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 UReal DataOnly))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern (Var wt))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel AutoDiffable)))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel AutoDiffable))))))
                                        (meta <opaque>)))))
                                    (meta <opaque>)))))
                                (meta <opaque>)))))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (For (loopvar k)
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
                                 (Assignment ((LVariable diff) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain SoA)
                                     (((pattern
                                        (FunApp (StanLib Minus__ FnPlain SoA)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var tvv))
                                              (meta
                                               ((type_ (UArray (UArray UVector)))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Indexed
                                             ((pattern (Var dvv))
                                              (meta
                                               ((type_ (UArray (UArray UVector)))
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain SoA)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain SoA)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain SoA)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var diff))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                                (meta <opaque>))
                               ((pattern
                                 (Assignment ((LVariable wt) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain SoA)
                                     (((pattern
                                        (Indexed
                                         ((pattern (Var tvv))
                                          (meta
                                           ((type_ (UArray (UArray UVector)))
                                            (loc <opaque>) (adlevel DataOnly))))
                                         ((Single
                                           ((pattern (Var i))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var j))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var k))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly))))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain SoA)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain SoA)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain SoA)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var wt))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
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
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (For (loopvar k)
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
                                 (Assignment ((LVariable diff) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain SoA)
                                     (((pattern
                                        (FunApp (StanLib Minus__ FnPlain SoA)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var tm))
                                              (meta
                                               ((type_ (UArray UMatrix)) 
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Indexed
                                             ((pattern (Var dm))
                                              (meta
                                               ((type_ (UArray UMatrix)) 
                                                (loc <opaque>) (adlevel DataOnly))))
                                             ((Single
                                               ((pattern (Var i))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var j))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly)))))
                                              (Single
                                               ((pattern (Var k))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain SoA)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain SoA)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain SoA)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var diff))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                                (meta <opaque>))
                               ((pattern
                                 (Assignment ((LVariable wt) ()) UReal
                                  ((pattern
                                    (FunApp (StanLib fma FnPlain SoA)
                                     (((pattern
                                        (Indexed
                                         ((pattern (Var tm))
                                          (meta
                                           ((type_ (UArray UMatrix)) 
                                            (loc <opaque>) (adlevel DataOnly))))
                                         ((Single
                                           ((pattern (Var i))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var j))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly)))))
                                          (Single
                                           ((pattern (Var k))
                                            (meta
                                             ((type_ UInt) (loc <opaque>)
                                              (adlevel DataOnly))))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern
                                           (FunApp (StanLib Plus__ FnPlain SoA)
                                            (((pattern
                                               (FunApp (StanLib fma FnPlain SoA)
                                                (((pattern (Lit Int 100))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Var i))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern
                                                   (FunApp (StanLib Times__ FnPlain SoA)
                                                    (((pattern (Lit Int 10))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly))))
                                                     ((pattern (Var j))
                                                      (meta
                                                       ((type_ UInt) 
                                                        (loc <opaque>)
                                                        (adlevel DataOnly)))))))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))))
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
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern (Var wt))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
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
         (TargetPE
          ((pattern
            (FunApp (StanLib fma FnPlain SoA)
             (((pattern (Var s3))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var z))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib fma FnPlain SoA)
                 (((pattern (Var diff))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern (Var x))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib Times__ FnPlain SoA)
                     (((pattern (Var wt))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern (Var y))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id x) (decl_type (Sized SReal))
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
     (Decl (decl_adtype DataOnly) (decl_id y) (decl_type (Sized SReal))
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
     (Decl (decl_adtype DataOnly) (decl_id z) (decl_type (Sized SReal))
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
         ((pattern (Var x)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var y)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var z)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype AutoDiffable) (decl_id x) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable x) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str x))
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
         ((pattern (Var x)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id y) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable y) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str y))
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
         ((pattern (Var y)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str z))
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
         ((pattern (Var z)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id x) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable x) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var x)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id y) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable y) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var y)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id z) (decl_type (Sized SReal))
      (initialize Uninit)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable z) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var z)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((x <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (y <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (z <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))))
 (prog_name ndlit_model) (prog_path tests/fixtures/ndlit.stan))