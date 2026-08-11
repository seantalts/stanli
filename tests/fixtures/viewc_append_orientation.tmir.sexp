((functions_block ()) (input_vars ()) (prepare_data ())
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id q)
      (decl_type
       (Sized
        (SVector AoS
         ((pattern (Lit Int 10)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 10))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r2)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r2) ()) URowVector
          ((pattern
            (FunApp (CompilerInternal FnMakeRowVec)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 1))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r3)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r3) ()) URowVector
          ((pattern
            (FunApp (CompilerInternal FnMakeRowVec)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 3))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 4))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 5))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v2)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v2) ()) UVector
          ((pattern
            (FunApp (StanLib Transpose__ FnPlain AoS)
             (((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 6))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 7))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v3)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v3) ()) UVector
          ((pattern
            (FunApp (StanLib Transpose__ FnPlain AoS)
             (((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 8))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 9))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 10))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 5))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r) ()) URowVector
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var r2))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var r3))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 5))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v) ()) UVector
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var v2))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var v3))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (Promotion
                     ((pattern
                       (FunApp (StanLib Plus__ FnPlain AoS)
                        (((pattern
                           (FunApp (StanLib Plus__ FnPlain AoS)
                            (((pattern
                               (FunApp (StanLib Plus__ FnPlain AoS)
                                (((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 100))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib rows FnPlain AoS)
                                        (((pattern (Var r))
                                          (meta
                                           ((type_ URowVector) (loc <opaque>)
                                            (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib cols FnPlain AoS)
                                        (((pattern (Var r))
                                          (meta
                                           ((type_ URowVector) (loc <opaque>)
                                            (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern
                               (FunApp (StanLib Times__ FnPlain AoS)
                                (((pattern (Lit Int 1000))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (FunApp (StanLib rows FnPlain AoS)
                                    (((pattern (Var v))
                                      (meta
                                       ((type_ UVector) (loc <opaque>)
                                        (adlevel AutoDiffable)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern
                           (FunApp (StanLib cols FnPlain AoS)
                            (((pattern (Var v))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal DataOnly))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (StanLib dot_product FnPlain AoS)
                     (((pattern (Var r))
                       (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Transpose__ FnPlain AoS)
                         (((pattern
                            (FunApp (CompilerInternal FnMakeRowVec)
                             (((pattern
                                (Promotion
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 2))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 4))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 8))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 16))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib dot_product FnPlain AoS)
                 (((pattern (Var v))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (CompilerInternal FnMakeRowVec)
                     (((pattern
                        (Promotion
                         ((pattern (Lit Int 32))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 64))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 128))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 256))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 512))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
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
         ((pattern (Lit Int 10)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 10))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r2)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r2) ()) URowVector
          ((pattern
            (FunApp (CompilerInternal FnMakeRowVec)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 1))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 2))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r3)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r3) ()) URowVector
          ((pattern
            (FunApp (CompilerInternal FnMakeRowVec)
             (((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 3))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 4))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (Indexed
                 ((pattern (Var q))
                  (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                 ((Single
                   ((pattern (Lit Int 5))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v2)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v2) ()) UVector
          ((pattern
            (FunApp (StanLib Transpose__ FnPlain AoS)
             (((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 6))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 7))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v3)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 3))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v3) ()) UVector
          ((pattern
            (FunApp (StanLib Transpose__ FnPlain AoS)
             (((pattern
                (FunApp (CompilerInternal FnMakeRowVec)
                 (((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 8))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 9))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (Indexed
                     ((pattern (Var q))
                      (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                     ((Single
                       ((pattern (Lit Int 10))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id r)
          (decl_type
           (Sized
            (SRowVector AoS
             ((pattern (Lit Int 5))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable r) ()) URowVector
          ((pattern
            (FunApp (StanLib append_col FnPlain AoS)
             (((pattern (Var r2))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var r3))
               (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id v)
          (decl_type
           (Sized
            (SVector AoS
             ((pattern (Lit Int 5))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable v) ()) UVector
          ((pattern
            (FunApp (StanLib append_row FnPlain AoS)
             (((pattern (Var v2))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Var v3))
               (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern
                (FunApp (StanLib Plus__ FnPlain AoS)
                 (((pattern
                    (Promotion
                     ((pattern
                       (FunApp (StanLib Plus__ FnPlain AoS)
                        (((pattern
                           (FunApp (StanLib Plus__ FnPlain AoS)
                            (((pattern
                               (FunApp (StanLib Plus__ FnPlain AoS)
                                (((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 100))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib rows FnPlain AoS)
                                        (((pattern (Var r))
                                          (meta
                                           ((type_ URowVector) (loc <opaque>)
                                            (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (FunApp (StanLib Times__ FnPlain AoS)
                                    (((pattern (Lit Int 10))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                     ((pattern
                                       (FunApp (StanLib cols FnPlain AoS)
                                        (((pattern (Var r))
                                          (meta
                                           ((type_ URowVector) (loc <opaque>)
                                            (adlevel AutoDiffable)))))))
                                      (meta
                                       ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern
                               (FunApp (StanLib Times__ FnPlain AoS)
                                (((pattern (Lit Int 1000))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 ((pattern
                                   (FunApp (StanLib rows FnPlain AoS)
                                    (((pattern (Var v))
                                      (meta
                                       ((type_ UVector) (loc <opaque>)
                                        (adlevel AutoDiffable)))))))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern
                           (FunApp (StanLib cols FnPlain AoS)
                            (((pattern (Var v))
                              (meta
                               ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     UReal DataOnly))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                  ((pattern
                    (FunApp (StanLib dot_product FnPlain AoS)
                     (((pattern (Var r))
                       (meta ((type_ URowVector) (loc <opaque>) (adlevel AutoDiffable))))
                      ((pattern
                        (FunApp (StanLib Transpose__ FnPlain AoS)
                         (((pattern
                            (FunApp (CompilerInternal FnMakeRowVec)
                             (((pattern
                                (Promotion
                                 ((pattern (Lit Int 1))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 2))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 4))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 8))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Promotion
                                 ((pattern (Lit Int 16))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                           (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
                       (meta ((type_ UVector) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib dot_product FnPlain AoS)
                 (((pattern (Var v))
                   (meta ((type_ UVector) (loc <opaque>) (adlevel AutoDiffable))))
                  ((pattern
                    (FunApp (CompilerInternal FnMakeRowVec)
                     (((pattern
                        (Promotion
                         ((pattern (Lit Int 32))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 64))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 128))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 256))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern
                        (Promotion
                         ((pattern (Lit Int 512))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ URowVector) (loc <opaque>) (adlevel DataOnly)))))))
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
         ((pattern (Lit Int 10)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Identity)
             (dims
              (((pattern (Lit Int 10))
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
         ((pattern (Lit Int 10)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
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
           ((pattern (Lit Int 10))
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
         ((pattern (Lit Int 10)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable q) ()) UVector
      ((pattern
        (FunApp (CompilerInternal FnReadDeserializer)
         (((pattern (Lit Int 10))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
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
       ((pattern (Lit Int 10)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SVector AoS
       ((pattern (Lit Int 10)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Identity)))))
 (prog_name viewc_append_orientation_model)
 (prog_path tests/fixtures/viewc_append_orientation.stan))