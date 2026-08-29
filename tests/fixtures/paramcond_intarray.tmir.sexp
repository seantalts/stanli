((functions_block
  (((fdrt (ReturnType (UArray (UArray UInt)))) (fdname echo_int_matrix)
    (fdsuffix FnPlain) (fdargs ((AutoDiffable x (UArray (UArray UInt)))))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern (Var x))
               (meta ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>))))
 (input_vars
  ((idx <opaque>
    (SArray
     (SArray SInt
      ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
     ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
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
     (Decl (decl_adtype DataOnly) (decl_id idx)
      (decl_type
       (Sized
        (SArray
         (SArray SInt
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id idx_flat__)
          (decl_type (Unsized (UArray UInt))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable idx_flat__) ()) (UArray UInt)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str idx))
               (meta ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
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
                   ((pattern (Lit Int 3))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable idx)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray (UArray UInt))
                          ((pattern
                            (Indexed
                             ((pattern (Var idx_flat__))
                              (meta
                               ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                             ((Single
                               ((pattern (Var pos__))
                                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
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
         (IfElse
          ((pattern
            (FunApp (StanLib Greater__ FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Block
             (((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id total) (decl_type (Sized SInt))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable total) ()) UInt
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (For (loopvar r)
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
                        (IfElse
                         ((pattern
                           (FunApp (StanLib Equals__ FnPlain AoS)
                            (((pattern
                               (Indexed
                                ((pattern (Var idx))
                                 (meta
                                  ((type_ (UArray (UArray UInt))) (loc <opaque>)
                                   (adlevel DataOnly))))
                                ((Single
                                  ((pattern (Var r))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                 (Single
                                  ((pattern (Lit Int 2))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern (Lit Int 5))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern
                           (Block
                            (((pattern
                               (Assignment ((LVariable total) ()) UInt
                                ((pattern
                                  (FunApp (StanLib Plus__ FnPlain AoS)
                                   (((pattern (Var total))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                    ((pattern
                                      (Indexed
                                       ((pattern (Var idx))
                                        (meta
                                         ((type_ (UArray (UArray UInt)))
                                          (loc <opaque>) (adlevel DataOnly))))
                                       ((Single
                                         ((pattern (Var r))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))
                                        (Single
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))))))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                              (meta <opaque>)))))
                          (meta <opaque>))
                         ()))
                       (meta <opaque>)))))
                   (meta <opaque>)))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id m) (decl_type (Sized SInt))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable m) ()) UInt
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib size FnPlain AoS)
                        (((pattern
                           (FunApp (CompilerInternal FnMakeArray)
                            (((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern (Lit Int 5))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern (Lit Int 6))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta
                           ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern
                       (Indexed
                        ((pattern
                          (FunApp (CompilerInternal FnMakeArray)
                           (((pattern (Lit Int 4))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 5))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 6))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id row)
                 (decl_type
                  (Sized
                   (SArray SInt
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable row) ()) (UArray UInt)
                 ((pattern
                   (Indexed
                    ((pattern (Var idx))
                     (meta
                      ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly))))
                    ((Single
                      ((pattern (Lit Int 2))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                     (Between
                      ((pattern (Lit Int 1))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Int 2))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                  (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id copied)
                 (decl_type
                  (Sized
                   (SArray
                    (SArray SInt
                     ((pattern (Lit Int 2))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                    ((pattern (Lit Int 3))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable copied) ()) (UArray (UArray UInt))
                 ((pattern (Var idx))
                  (meta
                   ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Assignment
                 ((LVariable copied)
                  ((Single
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                   (Between
                    ((pattern (Lit Int 1))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (UArray (UArray UInt))
                 ((pattern
                   (FunApp (CompilerInternal FnMakeArray)
                    (((pattern (Lit Int 9))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 8))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id echoed_rows)
                 (decl_type (Sized SInt)) (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable echoed_rows) ()) UInt
                 ((pattern
                   (FunApp (StanLib size FnPlain AoS)
                    (((pattern
                       (FunApp (UserDefined echo_int_matrix FnPlain)
                        (((pattern (Var copied))
                          (meta
                           ((type_ (UArray (UArray UInt))) (loc <opaque>)
                            (adlevel DataOnly)))))))
                      (meta
                       ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id echoed_cells)
                 (decl_type (Sized SInt)) (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable echoed_cells) ()) UInt
                 ((pattern
                   (FunApp (StanLib num_elements FnPlain AoS)
                    (((pattern
                       (FunApp (UserDefined echo_int_matrix FnPlain)
                        (((pattern (Var copied))
                          (meta
                           ((type_ (UArray (UArray UInt))) (loc <opaque>)
                            (adlevel DataOnly)))))))
                      (meta
                       ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (TargetPE
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern
                       (Promotion
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
                                           (((pattern (Var total))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly))))
                                            ((pattern
                                              (FunApp (StanLib Times__ FnPlain AoS)
                                               (((pattern (Lit Int 10))
                                                 (meta
                                                  ((type_ UInt) (loc <opaque>)
                                                   (adlevel DataOnly))))
                                                ((pattern (Var m))
                                                 (meta
                                                  ((type_ UInt) (loc <opaque>)
                                                   (adlevel DataOnly)))))))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly)))))))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly))))
                                        ((pattern
                                          (FunApp (StanLib Times__ FnPlain AoS)
                                           (((pattern (Lit Int 100))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly))))
                                            ((pattern
                                              (FunApp (StanLib sum FnPlain AoS)
                                               (((pattern (Var row))
                                                 (meta
                                                  ((type_ (UArray UInt))
                                                   (loc <opaque>) (adlevel DataOnly)))))))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly)))))))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                    ((pattern
                                      (FunApp (StanLib Times__ FnPlain AoS)
                                       (((pattern (Lit Int 1000))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly))))
                                        ((pattern
                                          (FunApp (StanLib sum FnPlain AoS)
                                           (((pattern
                                              (Indexed
                                               ((pattern (Var copied))
                                                (meta
                                                 ((type_ (UArray (UArray UInt)))
                                                  (loc <opaque>) (adlevel DataOnly))))
                                               ((Single
                                                 ((pattern (Lit Int 2))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))
                                                (Between
                                                 ((pattern (Lit Int 1))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Lit Int 2))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))))))
                                             (meta
                                              ((type_ (UArray UInt))
                                               (loc <opaque>) (adlevel DataOnly)))))))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern
                                  (FunApp (StanLib Times__ FnPlain AoS)
                                   (((pattern (Lit Int 10000))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                    ((pattern (Var echoed_rows))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern
                              (FunApp (StanLib Times__ FnPlain AoS)
                               (((pattern (Lit Int 100000))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern (Var echoed_cells))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        UReal DataOnly))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Var theta))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>)))))
           (meta <opaque>))
          (((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern (Var theta))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
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
         (IfElse
          ((pattern
            (FunApp (StanLib Greater__ FnPlain AoS)
             (((pattern (Var theta))
               (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
              ((pattern (Lit Int 0))
               (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
           (meta ((type_ UInt) (loc <opaque>) (adlevel AutoDiffable))))
          ((pattern
            (Block
             (((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id total) (decl_type (Sized SInt))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable total) ()) UInt
                 ((pattern (Lit Int 0))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (For (loopvar r)
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
                        (IfElse
                         ((pattern
                           (FunApp (StanLib Equals__ FnPlain AoS)
                            (((pattern
                               (Indexed
                                ((pattern (Var idx))
                                 (meta
                                  ((type_ (UArray (UArray UInt))) (loc <opaque>)
                                   (adlevel DataOnly))))
                                ((Single
                                  ((pattern (Var r))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                 (Single
                                  ((pattern (Lit Int 2))
                                   (meta
                                    ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern (Lit Int 5))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         ((pattern
                           (Block
                            (((pattern
                               (Assignment ((LVariable total) ()) UInt
                                ((pattern
                                  (FunApp (StanLib Plus__ FnPlain AoS)
                                   (((pattern (Var total))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                    ((pattern
                                      (Indexed
                                       ((pattern (Var idx))
                                        (meta
                                         ((type_ (UArray (UArray UInt)))
                                          (loc <opaque>) (adlevel DataOnly))))
                                       ((Single
                                         ((pattern (Var r))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly)))))
                                        (Single
                                         ((pattern (Lit Int 1))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))))))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
                              (meta <opaque>)))))
                          (meta <opaque>))
                         ()))
                       (meta <opaque>)))))
                   (meta <opaque>)))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id m) (decl_type (Sized SInt))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable m) ()) UInt
                 ((pattern
                   (FunApp (StanLib Plus__ FnPlain AoS)
                    (((pattern
                       (FunApp (StanLib size FnPlain AoS)
                        (((pattern
                           (FunApp (CompilerInternal FnMakeArray)
                            (((pattern (Lit Int 4))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern (Lit Int 5))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                             ((pattern (Lit Int 6))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (meta
                           ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly)))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern
                       (Indexed
                        ((pattern
                          (FunApp (CompilerInternal FnMakeArray)
                           (((pattern (Lit Int 4))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 5))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 6))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))
                        ((Single
                          ((pattern (Lit Int 2))
                           (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id row)
                 (decl_type
                  (Sized
                   (SArray SInt
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable row) ()) (UArray UInt)
                 ((pattern
                   (Indexed
                    ((pattern (Var idx))
                     (meta
                      ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly))))
                    ((Single
                      ((pattern (Lit Int 2))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                     (Between
                      ((pattern (Lit Int 1))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                      ((pattern (Lit Int 2))
                       (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                  (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id copied)
                 (decl_type
                  (Sized
                   (SArray
                    (SArray SInt
                     ((pattern (Lit Int 2))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                    ((pattern (Lit Int 3))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable copied) ()) (UArray (UArray UInt))
                 ((pattern (Var idx))
                  (meta
                   ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Assignment
                 ((LVariable copied)
                  ((Single
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                   (Between
                    ((pattern (Lit Int 1))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (UArray (UArray UInt))
                 ((pattern
                   (FunApp (CompilerInternal FnMakeArray)
                    (((pattern (Lit Int 9))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Lit Int 8))
                      (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ (UArray UInt)) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id echoed_rows)
                 (decl_type (Sized SInt)) (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable echoed_rows) ()) UInt
                 ((pattern
                   (FunApp (StanLib size FnPlain AoS)
                    (((pattern
                       (FunApp (UserDefined echo_int_matrix FnPlain)
                        (((pattern (Var copied))
                          (meta
                           ((type_ (UArray (UArray UInt))) (loc <opaque>)
                            (adlevel DataOnly)))))))
                      (meta
                       ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (Decl (decl_adtype AutoDiffable) (decl_id echoed_cells)
                 (decl_type (Sized SInt)) (initialize Default)))
               (meta <opaque>))
              ((pattern
                (Assignment ((LVariable echoed_cells) ()) UInt
                 ((pattern
                   (FunApp (StanLib num_elements FnPlain AoS)
                    (((pattern
                       (FunApp (UserDefined echo_int_matrix FnPlain)
                        (((pattern (Var copied))
                          (meta
                           ((type_ (UArray (UArray UInt))) (loc <opaque>)
                            (adlevel DataOnly)))))))
                      (meta
                       ((type_ (UArray (UArray UInt))) (loc <opaque>) (adlevel DataOnly)))))))
                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
               (meta <opaque>))
              ((pattern
                (TargetPE
                 ((pattern
                   (FunApp (StanLib Times__ FnPlain AoS)
                    (((pattern
                       (Promotion
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
                                           (((pattern (Var total))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly))))
                                            ((pattern
                                              (FunApp (StanLib Times__ FnPlain AoS)
                                               (((pattern (Lit Int 10))
                                                 (meta
                                                  ((type_ UInt) (loc <opaque>)
                                                   (adlevel DataOnly))))
                                                ((pattern (Var m))
                                                 (meta
                                                  ((type_ UInt) (loc <opaque>)
                                                   (adlevel DataOnly)))))))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly)))))))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly))))
                                        ((pattern
                                          (FunApp (StanLib Times__ FnPlain AoS)
                                           (((pattern (Lit Int 100))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly))))
                                            ((pattern
                                              (FunApp (StanLib sum FnPlain AoS)
                                               (((pattern (Var row))
                                                 (meta
                                                  ((type_ (UArray UInt))
                                                   (loc <opaque>) (adlevel DataOnly)))))))
                                             (meta
                                              ((type_ UInt) (loc <opaque>)
                                               (adlevel DataOnly)))))))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                    ((pattern
                                      (FunApp (StanLib Times__ FnPlain AoS)
                                       (((pattern (Lit Int 1000))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly))))
                                        ((pattern
                                          (FunApp (StanLib sum FnPlain AoS)
                                           (((pattern
                                              (Indexed
                                               ((pattern (Var copied))
                                                (meta
                                                 ((type_ (UArray (UArray UInt)))
                                                  (loc <opaque>) (adlevel DataOnly))))
                                               ((Single
                                                 ((pattern (Lit Int 2))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly)))))
                                                (Between
                                                 ((pattern (Lit Int 1))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 ((pattern (Lit Int 2))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))))))
                                             (meta
                                              ((type_ (UArray UInt))
                                               (loc <opaque>) (adlevel DataOnly)))))))
                                         (meta
                                          ((type_ UInt) (loc <opaque>)
                                           (adlevel DataOnly)))))))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern
                                  (FunApp (StanLib Times__ FnPlain AoS)
                                   (((pattern (Lit Int 10000))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                    ((pattern (Var echoed_rows))
                                     (meta
                                      ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern
                              (FunApp (StanLib Times__ FnPlain AoS)
                               (((pattern (Lit Int 100000))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                ((pattern (Var echoed_cells))
                                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        UReal DataOnly))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                     ((pattern (Var theta))
                      (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                  (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
               (meta <opaque>)))))
           (meta <opaque>))
          (((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern (Var theta))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
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
 (prog_name paramcond_intarray_model) (prog_path tests/fixtures/paramcond_intarray.stan))
