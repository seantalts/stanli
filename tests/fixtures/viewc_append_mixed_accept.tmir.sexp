((functions_block ()) (input_vars ()) (prepare_data ())
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 9))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id s) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable s) ()) UReal
          ((pattern
            (Indexed
             ((pattern (Var q))
              (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
             ((Single
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v) ()) UVector
          ((pattern
            (Indexed
             ((pattern (Var q))
              (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
             ((Between
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r) ()) URowVector
          ((pattern
            (FunApp (StanLib Transpose__ FnPlain AoS)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Between
                   ((pattern (Lit Int 4))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                   ((pattern (Lit Int 5))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id M)
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
         (Assignment ((LVariable M) ()) UMatrix
          ((pattern
            (FunApp (StanLib to_matrix FnPlain AoS)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Between
                   ((pattern (Lit Int 6))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                   ((pattern (Lit Int 9))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ar1)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ar1) ()) UVector
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ar2)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ar2) ()) UVector
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ac1)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ac1) ()) URowVector
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var r))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ac2)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ac2) ()) URowVector
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var r))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id mr)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable mr) ()) UMatrix
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var M))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var r))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id mv)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable mv) ()) UMatrix
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var M))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib dot_product FnPlain AoS)
                                 (((pattern (Var ar1))
                                   (meta
                                    ((type_ UVector) (loc <opaque>)
                                     (adlevel AutoDiffable))))
                                  ((pattern
                                    (FunApp (StanLib Transpose__ FnPlain AoS)
                                     (((pattern
                                        (FunApp (CompilerInternal FnMakeRowVec)
                                         (((pattern
                                            (Promotion
                                             ((pattern (Lit Int 1))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 2))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 3))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ URowVector) (loc <opaque>)
                                         (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (FunApp (StanLib dot_product FnPlain AoS)
                                 (((pattern (Var ar2))
                                   (meta
                                    ((type_ UVector) (loc <opaque>)
                                     (adlevel AutoDiffable))))
                                  ((pattern
                                    (FunApp (StanLib Transpose__ FnPlain AoS)
                                     (((pattern
                                        (FunApp (CompilerInternal FnMakeRowVec)
                                         (((pattern
                                            (Promotion
                                             ((pattern (Lit Int 4))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 5))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 6))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ URowVector) (loc <opaque>)
                                         (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib dot_product FnPlain AoS)
                             (((pattern (Var ac1))
                               (meta
                                ((type_ URowVector) (loc <opaque>)
                                 (adlevel AutoDiffable))))
                              ((pattern
                                (FunApp (StanLib Transpose__ FnPlain AoS)
                                 (((pattern
                                    (FunApp (CompilerInternal FnMakeRowVec)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int 7))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 8))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 9))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ URowVector) (loc <opaque>)
                                     (adlevel DataOnly)))))))
                               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib dot_product FnPlain AoS)
                         (((pattern (Var ac2))
                           (meta
                            ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Transpose__ FnPlain AoS)
                             (((pattern
                                (FunApp (CompilerInternal FnMakeRowVec)
                                 (((pattern
                                    (Promotion
                                     ((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (Promotion
                                     ((pattern (Lit Int 11))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (Promotion
                                     ((pattern (Lit Int 12))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib dot_product FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib to_vector FnPlain AoS)
                         (((pattern (Var mr))
                           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Transpose__ FnPlain AoS)
                         (((pattern
                            (FunApp (CompilerInternal FnMakeRowVec)
                             (((pattern
                                (Promotion
                                 ((pattern (Lit Int 13))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 14))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 15))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 16))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 17))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 18))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib dot_product FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib to_vector FnPlain AoS)
                     (((pattern (Var mv))
                       (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib Transpose__ FnPlain AoS)
                     (((pattern
                        (FunApp (CompilerInternal FnMakeRowVec)
                         (((pattern
                            (Promotion
                             ((pattern (Lit Int 19))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 20))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 21))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 22))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 23))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 24))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 9))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id s) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable s) ()) UReal
          ((pattern
            (Indexed
             ((pattern (Var q))
              (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
             ((Single
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v) ()) UVector
          ((pattern
            (Indexed
             ((pattern (Var q))
              (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
             ((Between
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r) ()) URowVector
          ((pattern
            (FunApp (StanLib Transpose__ FnPlain AoS)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Between
                   ((pattern (Lit Int 4))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                   ((pattern (Lit Int 5))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id M)
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
         (Assignment ((LVariable M) ()) UMatrix
          ((pattern
            (FunApp (StanLib to_matrix FnPlain AoS)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Between
                   ((pattern (Lit Int 6))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                   ((pattern (Lit Int 9))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ar1)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ar1) ()) UVector
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ar2)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ar2) ()) UVector
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ac1)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ac1) ()) URowVector
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var r))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id ac2)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable ac2) ()) URowVector
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var r))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var s))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id mr)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable mr) ()) UMatrix
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var M))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var r))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id mv)
          (decl_type
           (Sized
            (SMatrix AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable mv) ()) UMatrix
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var M))
               (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var v))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib Plus__ FnPlain AoS)
                         (((pattern
                            (FunApp (StanLib Plus__ FnPlain AoS)
                             (((pattern
                                (FunApp (StanLib dot_product FnPlain AoS)
                                 (((pattern (Var ar1))
                                   (meta
                                    ((type_ UVector) (loc <opaque>)
                                     (adlevel AutoDiffable))))
                                  ((pattern
                                    (FunApp (StanLib Transpose__ FnPlain AoS)
                                     (((pattern
                                        (FunApp (CompilerInternal FnMakeRowVec)
                                         (((pattern
                                            (Promotion
                                             ((pattern (Lit Int 1))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 2))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 3))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ URowVector) (loc <opaque>)
                                         (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (FunApp (StanLib dot_product FnPlain AoS)
                                 (((pattern (Var ar2))
                                   (meta
                                    ((type_ UVector) (loc <opaque>)
                                     (adlevel AutoDiffable))))
                                  ((pattern
                                    (FunApp (StanLib Transpose__ FnPlain AoS)
                                     (((pattern
                                        (FunApp (CompilerInternal FnMakeRowVec)
                                         (((pattern
                                            (Promotion
                                             ((pattern (Lit Int 4))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 5))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly))))
                                          ((pattern
                                            (Promotion
                                             ((pattern (Lit Int 6))
                                              (meta
                                               ((type_ UInt) (loc <opaque>)
                                                (adlevel DataOnly))))
                                             UReal DataOnly))
                                           (meta
                                            ((type_ UReal) (loc <opaque>)
                                             (adlevel DataOnly)))))))
                                       (meta
                                        ((type_ URowVector) (loc <opaque>)
                                         (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib dot_product FnPlain AoS)
                             (((pattern (Var ac1))
                               (meta
                                ((type_ URowVector) (loc <opaque>)
                                 (adlevel AutoDiffable))))
                              ((pattern
                                (FunApp (StanLib Transpose__ FnPlain AoS)
                                 (((pattern
                                    (FunApp (CompilerInternal FnMakeRowVec)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int 7))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 8))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Promotion
                                         ((pattern (Lit Int 9))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                                   (meta
                                    ((type_ URowVector) (loc <opaque>)
                                     (adlevel DataOnly)))))))
                               (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib dot_product FnPlain AoS)
                         (((pattern (Var ac2))
                           (meta
                            ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Transpose__ FnPlain AoS)
                             (((pattern
                                (FunApp (CompilerInternal FnMakeRowVec)
                                 (((pattern
                                    (Promotion
                                     ((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (Promotion
                                     ((pattern (Lit Int 11))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                  ((pattern
                                    (Promotion
                                     ((pattern (Lit Int 12))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     UReal DataOnly))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                               (meta
                                ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib dot_product FnPlain AoS)
                     (((pattern
                        (FunApp (StanLib to_vector FnPlain AoS)
                         (((pattern (Var mr))
                           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Transpose__ FnPlain AoS)
                         (((pattern
                            (FunApp (CompilerInternal FnMakeRowVec)
                             (((pattern
                                (Promotion
                                 ((pattern (Lit Int 13))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 14))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 15))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 16))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 17))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 18))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib dot_product FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib to_vector FnPlain AoS)
                     (((pattern (Var mv))
                       (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (StanLib Transpose__ FnPlain AoS)
                     (((pattern
                        (FunApp (CompilerInternal FnMakeRowVec)
                         (((pattern
                            (Promotion
                             ((pattern (Lit Int 19))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 20))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 21))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 22))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 23))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                          ((pattern
                            (Promotion
                             ((pattern (Lit Int 24))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             UReal DataOnly))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id q)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 9))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var q)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Assignment ((LVariable pos__) ()) UInt
      ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id q_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable q_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str q))
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
           ((pattern (Lit Int 9))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (Assignment
                  ((LVariable q)
                   ((Single
                     ((pattern (Var sym1__))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  UVector
                  ((pattern
                    (Indexed
                     ((pattern (Var q_flat__))
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
         ((pattern (Var q)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable q) ()) UVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
       (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var q)) (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((q <opaque>
    ((out_unconstrained_st
      (SVector AoS
       ((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 9)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))))
 (prog_name viewc_append_mixed_accept_model)
 (prog_path tests/fixtures/viewc_append_mixed_accept.stan))