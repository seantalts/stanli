((functions_block ()) (input_vars ((N <opaque> SInt)))
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
    (meta <opaque>))))
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
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id i) (decl_type (Sized SInt))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable i) ()) UInt
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id halves) (decl_type (Sized SInt))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable halves) ()) UInt
          ((pattern (Lit Int 5)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id idx)
          (decl_type
           (Sized
            (SArray SInt
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id acc) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable acc) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id position) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable position) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id step) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable step) ()) UReal
          ((pattern
            (FunApp (StanLib Divide__ FnPlain AoS)
             (((pattern (Lit Real 1.0))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (StanLib ceil FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Divide__ FnPlain AoS)
                     (((pattern
                        (Promotion
                         ((pattern (Var N))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 2.0))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id m)
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
         (Assignment ((LVariable m) ()) UMatrix
          ((pattern
            (FunApp (StanLib rep_matrix FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 3))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Leq__ FnPlain AoS)
             (((pattern (Var i)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Block
             (((pattern
                (Assignment ((LVariable acc) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var acc))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern
                           (Promotion
                            ((pattern (Var i))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            UReal DataOnly))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Var theta))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable i) ()) UInt
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var i))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 1))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>)))))
           (meta <opaque>))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Less__ FnPlain AoS)
             (((pattern (Var position))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Block
             (((pattern
                (Assignment ((LVariable acc) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var acc))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern (Var theta))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable position) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var position))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern (Var step))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>)))))
           (meta <opaque>))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Greater__ FnPlain AoS)
             (((pattern (Var halves))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Block
             (((pattern
                (Assignment ((LVariable halves) ()) UInt
                 ((pattern
                   (FunApp (StanLib IntDivide__ FnPlain AoS)
                    (((pattern (Var halves))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 2))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable acc) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var acc))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern
                           (Promotion
                            ((pattern (Var halves))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            UReal DataOnly))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Var theta))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>)))))
           (meta <opaque>))))
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
                 (Assignment
                  ((LVariable idx)
                   ((Single
                     ((pattern (Var k))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (UArray UInt)
                  ((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern (Var k))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Int 1))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable acc) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var acc))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var m))
                      (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                     ((MultiIndex
                       ((pattern (Var idx))
                        (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var acc))
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
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id i) (decl_type (Sized SInt))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable i) ()) UInt
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id halves) (decl_type (Sized SInt))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable halves) ()) UInt
          ((pattern (Lit Int 5)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id idx)
          (decl_type
           (Sized
            (SArray SInt
             ((pattern (Lit Int 2))
              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id acc) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable acc) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id position) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable position) ()) UReal
          ((pattern
            (Promotion
             ((pattern (Lit Int 0))
              (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
             UReal AutoDiffable))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id step) (decl_type (Sized SReal))
          (initialize Default)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable step) ()) UReal
          ((pattern
            (FunApp (StanLib Divide__ FnPlain AoS)
             (((pattern (Lit Real 1.0))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
              ((pattern
                (FunApp (StanLib ceil FnPlain AoS)
                 (((pattern
                    (FunApp (StanLib Divide__ FnPlain AoS)
                     (((pattern
                        (Promotion
                         ((pattern (Var N))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Real 2.0))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
        (meta <opaque>))
       ((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id m)
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
         (Assignment ((LVariable m) ()) UMatrix
          ((pattern
            (FunApp (StanLib rep_matrix FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 3))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 2))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Leq__ FnPlain AoS)
             (((pattern (Var i)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Var N)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Block
             (((pattern
                (Assignment ((LVariable acc) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var acc))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern
                           (Promotion
                            ((pattern (Var i))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            UReal DataOnly))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Var theta))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable i) ()) UInt
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var i))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 1))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>)))))
           (meta <opaque>))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Less__ FnPlain AoS)
             (((pattern (Var position))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Block
             (((pattern
                (Assignment ((LVariable acc) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var acc))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern (Var theta))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable position) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var position))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern (Var step))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>)))))
           (meta <opaque>))))
        (meta <opaque>))
       ((pattern
         (While
          ((pattern
            (FunApp (StanLib Greater__ FnPlain AoS)
             (((pattern (Var halves))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
              ((pattern (Lit Int 1))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern
            (Block
             (((pattern
                (Assignment ((LVariable halves) ()) UInt
                 ((pattern
                   (FunApp (StanLib IntDivide__ FnPlain AoS)
                    (((pattern (Var halves))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 2))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable acc) ()) UReal
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern (Var acc))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                     ((pattern
                       (FunApp (StanLib Times__ FnPlain AoS)
                        (((pattern
                           (Promotion
                            ((pattern (Var halves))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            UReal DataOnly))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern (Var theta))
                          (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>)))))
           (meta <opaque>))))
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
                 (Assignment
                  ((LVariable idx)
                   ((Single
                     ((pattern (Var k))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (UArray UInt)
                  ((pattern
                    (FunApp (StanLib Plus__ FnPlain AoS)
                     (((pattern (Var k))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Int 1))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                   (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable acc) ()) UReal
          ((pattern
            (FunApp (StanLib Plus__ FnPlain AoS)
             (((pattern (Var acc))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern
                (FunApp (StanLib sum FnPlain AoS)
                 (((pattern
                    (Indexed
                     ((pattern (Var m))
                      (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable))))
                     ((MultiIndex
                       ((pattern (Var idx))
                        (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))))
                   (meta ((type_ UMatrix) (loc <opaque>) (adlevel AutoDiffable)))))))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern
            (FunApp (StanLib std_normal_lpdf (FnLpdf true) AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var acc))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var theta)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
    (meta <opaque>))))
 (output_vars
  ((theta <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))))
 (prog_name whileloop_model) (prog_path tests/fixtures/whileloop.stan))
